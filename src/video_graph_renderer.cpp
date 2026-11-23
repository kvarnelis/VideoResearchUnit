#include "video_graph_renderer.hpp"
#include "video_clock.hpp"
#include "video_bus.hpp"
#include "video_external_frame.hpp"
#include "freeze_frame_store.hpp"
#include "signal_ring.hpp"
#include "inverse_map.hpp"
#include "cv_param.hpp"
#include "deflection_yoke_math.hpp"
#include "focus_filter.hpp"
#include "raster_scan.hpp"
#include "render_pool.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>

namespace {
constexpr float kPi = 3.14159265358979323846f;
// Slope of the tone curve for Scope Trace.
constexpr float kScopeToneK = 1.6f;
// Energy gain of the Scope Trace beam. BEAM sets the width of the spot and nothing else, so the gain
// is one fixed number. It was chosen so that a horizontal or vertical line at the default BEAM (0.4,
// sigma 1.5 px) has about the brightest pixel the old gain (0.6 + 0.8 x BEAM = 0.92, two-tap linear
// spot) gave it, and that is all it holds. tests/bus_test.cpp draws both and compares the mean over
// 16 sub-pixel positions of the brightest pixel: at BEAM 0.4 the new line is 1.013 (horizontal) and
// 1.003 (vertical) times the old one, and a diagonal is 0.81. Per pixel of path the new line carries
// 2.6 / (0.6 + 0.8 x BEAM) times the old light: 2.8 times at the default.
constexpr float kScopeBeamGain = 2.6f;
}

namespace {
// The nodes that read their source through the frame helpers and so take a float picture as it is. Every other node
// is given bytes. (Part of the list in docs/architecture.md, "Precision".)
bool acceptsFloat(VideoBusNodeType type) {
	switch (type) {
	case VideoBusNodeType::ColorLab:
	case VideoBusNodeType::Colorizer:
	case VideoBusNodeType::Keyer:
	case VideoBusNodeType::WaveShaper:
	case VideoBusNodeType::FreezeFrame:
	case VideoBusNodeType::Rotator:
	case VideoBusNodeType::Feedback:
	case VideoBusNodeType::DeflectionYoke:
	case VideoBusNodeType::LineField:
	case VideoBusNodeType::Focus:
		return true;
	default:
		return false;
	}
}
}

VideoGraphRenderer::VideoGraphRenderer() : pool(static_cast<size_t>((kMaxDepth + 2) * kSlotsPerDepth)) {}

const VideoGraphRenderer::Frame& VideoGraphRenderer::renderBus(int busId, int width, int height) {
	signal = false;
	cycle = false;
	readContended = false;
	if (width <= 0 || height <= 0) {
		return none;
	}

	beginPass();
	deepest = 0;
	const Status status = renderNode(busId, width, height, 0, work);
	readContended = status == Status::Contended;
	if (delayJobCount > 0) {
		// A Frame Delay was met. Its source may be this very picture (a loop), so keep the root as a finished tap
		// for drawDelaySources() to reuse instead of drawing it again.
		if (status == Status::Ok && memoPass[static_cast<size_t>(busId)] != passId) {
			memo[static_cast<size_t>(busId)].copyFrom(work);
			memoPass[static_cast<size_t>(busId)] = passId;
			memoHeight[static_cast<size_t>(busId)] = deepest;
		}
		drawDelaySources(width, height);
	}
	if (status == Status::Ok) {
		// A picture leaves the graph as 8 bits (a float one is rounded here, half up). Swap instead of copy: work
		// becomes scratch for the next pass.
		work.ensureU8();
		lastGood.swap(work);
		lastGoodBus = busId;
		lastGoodWidth = width;
		lastGoodHeight = height;
		signal = true;
		return lastGood;
	}

	// A read the writer kept interrupting says nothing about the bus: hold the last picture for this frame.
	if (status == Status::Cycle || status == Status::Contended) {
		cycle = status == Status::Cycle;
		if (busId == lastGoodBus && width == lastGoodWidth && height == lastGoodHeight && !lastGood.empty()) {
			signal = true;
			return lastGood;
		}
	}
	return none;
}

void VideoGraphRenderer::beginPass() {
	++passId;
	passFloat = fullPrecision.load(std::memory_order_relaxed);
	renderCounts.fill(0);
	delayJobCount = 0;
	delayJobNext = 0;
}

VideoGraphRenderer::Status VideoGraphRenderer::renderBusInPass(int busId, int width, int height, Frame& out, bool keepFloatPlane) {
	signal = false;
	cycle = false;
	readContended = false;
	if (width <= 0 || height <= 0) {
		return Status::Empty;
	}
	deepest = 0;
	memoizeRoots = true;
	const Status status = renderNode(busId, width, height, 0, work);
	readContended = status == Status::Contended;
	memoizeRoots = false;
	if (delayJobNext < delayJobCount) {
		drawDelaySources(width, height);
	}
	if (status == Status::Ok) {
		// Same swap renderBus() makes with lastGood: `out` takes the picture, work becomes scratch. A float picture
		// is rounded to 8 bits once, here, straight into `out`, so the service's buffers never hold a float plane.
		bool keptPlane = false;
		if (work.isFloat()) {
			out.beginU8(work.f.size());
			floatToBytes(work.f.data(), out.data(), work.f.size());
			if (keepFloatPlane) {
				// Hand the float plane on as well: a swap, so there is no copy; `work` takes the old plane as scratch.
				out.precise.swap(work.f);
				keptPlane = true;
			}
		}
		else {
			out.swap(work);
		}
		if (!keptPlane) {
			out.precise.clear();  // capacity stays; the render service frees it (RenderService::acquireBuffer)
		}
		signal = true;
	}
	else if (status == Status::Cycle) {
		cycle = true;
	}
	return status;
}

VideoGraphRenderer::Status VideoGraphRenderer::renderNode(int busId, int width, int height, int depth, Frame& out) {
	if (depth > kMaxDepth) {
		return Status::Cycle;
	}

	// Shared taps: a bus already finished in this pass is copied, not rendered again. Its stateful nodes
	// (Feedback, Freeze Frame, Scope Trace and the CRT beam nodes) therefore advance once per pass.
	const bool inRange = busId >= 0 && busId <= kVideoBusCount;
	if (inRange && memoPass[static_cast<size_t>(busId)] == passId && !onStack[static_cast<size_t>(busId)]) {
		const size_t index = static_cast<size_t>(busId);
		// A fresh render would have walked memoHeight levels further down from here.
		if (depth + memoHeight[index] > kMaxDepth) {
			return Status::Cycle;
		}
		deepest = std::max(deepest, depth + memoHeight[index]);
		out.copyFrom(memo[index]);
		return Status::Ok;
	}

	VideoBusNode node;
	bool contended = false;
	if (!videoBusReadNode(busId, node, &contended)) {
		return contended ? Status::Contended : Status::Empty;
	}

	if (onStack[static_cast<size_t>(busId)]) {
		return Status::Cycle;
	}

	onStack[static_cast<size_t>(busId)] = true;
	const int outerDeepest = deepest;
	deepest = depth;
	const Status status = renderNodeBody(busId, node, width, height, depth, out);
	const int height2 = deepest - depth;
	deepest = std::max(outerDeepest, deepest);
	onStack[static_cast<size_t>(busId)] = false;
	++renderCounts[static_cast<size_t>(busId)];
	// The top bus of renderBus() has no second tap in this pass, so it is not copied. A root of renderBusInPass()
	// might be read by a later root, so it is.
	if (status == Status::Ok && (depth > 0 || memoizeRoots)) {
		const size_t index = static_cast<size_t>(busId);
		memo[index].copyFrom(out);
		memoPass[index] = passId;
		memoHeight[index] = height2;
	}
	return status;
}

// Picture as modulation (docs/architecture.md, "Picture as modulation"). Draws `modBusId` through the ordinary walk, at
// this node's size and one level below it, so it is counted in the depth limit, caught by the cycle check and drawn
// once per pass when something else reads it too. Then it keeps the brightness of every pixel in modLuma, 0..1
// (Rec. 709 weights as the Keyer's, read from a byte or a float picture; a float picture is not rounded).
//
// `luma` is left null, and the caller runs its own code unchanged, when there is no modulation: no bus, the node's
// own bus, an amount of zero, a bus that has no picture, or a picture of the wrong size. Cycles and contended reads propagate to the root. The buffer is shared by every node: it is filled after the nested draw is
// finished and used before the caller returns, so no other node writes it in between.
VideoGraphRenderer::Status VideoGraphRenderer::renderModulation(int ownBusId, int modBusId, float amount, int width, int height, int depth, int slot, const float*& luma) {
	luma = nullptr;
	if (modBusId <= 0 || modBusId == ownBusId || !(std::fabs(amount) > 1e-6f)) {
		return Status::Ok;
	}
	Frame& picture = scratch(depth, slot);
	const Status status = renderNode(modBusId, width, height, depth + 1, picture);
	if (status == Status::Cycle || status == Status::Contended) {
		return status;
	}
	const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
	if (status != Status::Ok || picture.count() != pixels * 4u) {
		return Status::Ok;
	}
	modLuma.resize(pixels);
	withSource(picture, [&](auto src) {
		for (size_t p = 0; p < pixels; ++p) {
			const size_t i = p * 4u;
			modLuma[p] = clamp01((src[i + 0] * 0.2126f + src[i + 1] * 0.7152f + src[i + 2] * 0.0722f) / 255.f);
		}
	});
	luma = modLuma.data();
	return Status::Ok;
}

VideoGraphRenderer::Status VideoGraphRenderer::renderNodeBody(int busId, const VideoBusNode& node, int width, int height, int depth, Frame& out) {
	switch (node.type) {
	case VideoBusNodeType::Generator: {
		const size_t index = static_cast<size_t>(busId);
		if (!generatorEngines[index]) {
			generatorEngines[index] = std::make_unique<PreviewVideoEngine>();
		}
		PreviewVideoEngine& engine = *generatorEngines[index];
		// A generator can be reached twice in one pass (e.g. both inputs of a Bus Mixer).
		// Reuse its frame for that pass so it advances only once per render, and both paths agree.
		if (generatorPass[index] != passId) {
			// FM IN (Video Oscillator): a picture whose brightness bends oscillator A. Drawn first, through the normal walk.
			const float* fmLuma = nullptr;
			const Status modulation = renderModulation(busId, node.generator.fmBusId, node.generator.fmAmount, width, height, depth, 0, fmLuma);
			if (modulation != Status::Ok) {
				return modulation;
			}
			engine.render(width, height, node.generator, passFloat, fmLuma);
		}
		generatorPass[index] = passId;
		if (engine.outputIsFloat()) {
			out.beginF32(engine.outputF().size());
			std::copy(engine.outputF().begin(), engine.outputF().end(), out.f.begin());
			return Status::Ok;
		}
		const std::vector<unsigned char>& rendered = engine.output();
		out.assign(rendered.begin(), rendered.end());
		return rendered.empty() ? Status::Empty : Status::Ok;
	}

	case VideoBusNodeType::Ramps: {
		// MOD IN: a picture whose brightness offsets layer A's phase.
		const float* modLuma = nullptr;
		const Status modulation = renderModulation(busId, node.ramps.modBusId, node.ramps.modAmount, width, height, depth, 0, modLuma);
		if (modulation != Status::Ok) {
			return modulation;
		}
		renderRamps(out, width, height, node.ramps, passFloat, modLuma, &rampsGeometry);
		return Status::Ok;
	}

	case VideoBusNodeType::Shapes:
		renderShapes(out, width, height, node.shapes, passFloat);
		return Status::Ok;

	case VideoBusNodeType::Raster: {
		// A picture built from the module's signal ring: the newest complete field of the video clock (VRU-29).
		// A read that fails (no field yet, or the writer wrapped onto the copy) keeps the last good picture.
		if (node.raster.slow > 1) {
			// SLOW: each pass paints the samples that arrived since the last one into the held picture.
			raster_scan::Held& held = rasterSlow[static_cast<size_t>(busId) % rasterSlow.size()];
			if (signalRingReadSince(node.raster.slotId, held.lastFrame, 8192, scopeWindow)) {
				raster_scan::paintSince(held, width, height, scopeWindow, node.raster);
			}
			else {
				raster_scan::paintSince(held, width, height, SignalWindow(), node.raster);
			}
			out.assign(held.pic.begin(), held.pic.end());
			return Status::Ok;
		}
		std::vector<unsigned char>& hold = rasterHold[static_cast<size_t>(busId) % rasterHold.size()];
		const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
		if (signalRingReadField(node.raster.slotId, scopeWindow, std::max(node.raster.lines, 1))) {
			raster_scan::render(hold, width, height, scopeWindow, node.raster);
		}
		else if (hold.size() != bytes) {
			raster_scan::render(hold, width, height, SignalWindow(), node.raster);
		}
		out.assign(hold.begin(), hold.end());
		return Status::Ok;
	}

	case VideoBusNodeType::ExternalFrame: {
		if (!videoExternalFrameRead(node.externalFrameId, externalFrame)) {
			return Status::Empty;
		}
		if (externalFrame.width == width && externalFrame.height == height) {
			out.swap(externalFrame.rgba);
		}
		else {
			resampleInto(externalFrame.rgba, externalFrame.width, externalFrame.height, out, width, height);
		}
		return out.empty() ? Status::Empty : Status::Ok;
	}

	case VideoBusNodeType::ScopeTrace:
		return renderScopeTraceFrame(busId, width, height, node.scopeTraceSlotId, node.scopeTrace, out) ? Status::Ok : Status::Empty;

	case VideoBusNodeType::DeflectionYoke:
	case VideoBusNodeType::LineField:
	case VideoBusNodeType::ColorLab:
	case VideoBusNodeType::Colorizer:
	case VideoBusNodeType::Keyer:
	case VideoBusNodeType::WaveShaper:
	case VideoBusNodeType::Feedback:
	case VideoBusNodeType::Rotator:
	case VideoBusNodeType::Focus:
	case VideoBusNodeType::FreezeFrame: {
		// Single-input processors: the source renders into this depth's scratch frame.
		Frame& src = scratch(depth, 0);
		const Status child = renderNode(node.sourceBusId, width, height, depth + 1, src);
		if (child != Status::Ok) {
			return child;
		}
		// A node that has not been converted to read through the frame helpers gets bytes (round half up).
		if (!acceptsFloat(node.type)) {
			src.ensureU8();
		}
		// MOD IN (Color Lab, Keyer): a second picture whose brightness, at each pixel, offsets one control.
		const float* modLuma = nullptr;
		if (node.type == VideoBusNodeType::ColorLab || node.type == VideoBusNodeType::Keyer) {
			const bool lab = node.type == VideoBusNodeType::ColorLab;
			const Status modulation = renderModulation(busId, lab ? node.colorLab.modBusId : node.keyer.modBusId, lab ? node.colorLab.modAmount : node.keyer.modAmount, width, height, depth, 1, modLuma);
			if (modulation != Status::Ok) {
				return modulation;
			}
		}
		switch (node.type) {
		case VideoBusNodeType::DeflectionYoke: renderDeflectionYokeFrame(busId, src, out, width, height, node.deflectionYoke); break;
		case VideoBusNodeType::LineField: renderLineFieldFrame(src, out, width, height, node.lineField); break;
		case VideoBusNodeType::ColorLab: renderColorLabFrame(src, out, width, height, node.colorLab, modLuma); break;
		case VideoBusNodeType::Colorizer: renderColorizerFrame(src, out, width, height, node.colorizer); break;
		case VideoBusNodeType::Keyer: renderKeyerFrame(src, out, width, height, node.keyer, modLuma); break;
		case VideoBusNodeType::WaveShaper: renderWaveShaperFrame(src, out, width, height, node.waveShaper); break;
		case VideoBusNodeType::Feedback: renderFeedbackFrame(src, out, width, height, node.feedback); break;
		case VideoBusNodeType::Rotator: renderRotatorFrame(src, out, width, height, node.rotator); break;
		case VideoBusNodeType::Focus: renderFocusFrame(src, out, width, height, node.focus); break;
		case VideoBusNodeType::FreezeFrame: renderFreezeFrame(src, out, width, height, node.freezeFrame); break;
		default: return Status::Empty;
		}
		return Status::Ok;
	}

	case VideoBusNodeType::ScanProcessor: {
		Frame& src = scratch(depth, 0);
		const Status child = renderNode(node.sourceBusId, width, height, depth + 1, src);
		if (child != Status::Ok) {
			return child;
		}
		// With "Full precision between modules" on, the picture (and the lift) are read as floats, unrounded. Off, bytes.
		if (!passFloat) {
			src.ensureU8();
		}
		// LIFT IN: a second picture that supplies the lift. Absent, empty or not rendering: the main picture lifts itself.
		Frame* lift = nullptr;
		if (node.scanProcessor.liftBusId > 0) {
			Frame& l = scratch(depth, 1);
			const Status liftStatus = renderNode(node.scanProcessor.liftBusId, width, height, depth + 1, l);
			if (liftStatus == Status::Cycle || liftStatus == Status::Contended) {
				return liftStatus;
			}
			if (liftStatus == Status::Ok) {
				if (!passFloat) {
					l.ensureU8();
				}
				lift = &l;
			}
		}
		renderScanProcessorFrame(busId, src, lift, out, width, height, node.scanProcessor);
		return Status::Ok;
	}

	case VideoBusNodeType::BusMixer: {
		const BusMixerParams& mixer = node.busMixer;
		Frame& a = scratch(depth, 0);
		const Status aStatus = renderNode(node.sourceBusId, width, height, depth + 1, a);
		if (aStatus != Status::Ok) {
			return aStatus;
		}

		// B absent (unpatched, empty or 0): the output is A alone.
		Frame& b = scratch(depth, 1);
		const Status bStatus = mixer.busB > 0 ? renderNode(mixer.busB, width, height, depth + 1, b) : Status::Empty;
		if (bStatus == Status::Cycle || bStatus == Status::Contended) {
			return bStatus;
		}
		if (bStatus != Status::Ok) {
			out.swap(a);
			return Status::Ok;
		}

		const bool needMatte = mixer.matteBus > 0 && (mixer.mode == 6 || mixer.matteAmount > 0.0001f);
		const Frame* matte = nullptr;
		if (needMatte) {
			Frame& m = scratch(depth, 2);
			const Status mStatus = renderNode(mixer.matteBus, width, height, depth + 1, m);
			if (mStatus == Status::Cycle || mStatus == Status::Contended) {
				return mStatus;
			}
			if (mStatus == Status::Ok) {
				matte = &m;
			}
		}

		renderBusMixerFrame(a, b, matte, out, width, height, mixer);
		return Status::Ok;
	}

	case VideoBusNodeType::VideoMixer:
		return renderVideoMixerNode(node.videoMixer, width, height, depth, out);

	case VideoBusNodeType::FrameDelay: {
		// A leaf: the picture comes from the ring, so the walk does not go into the source. The source is drawn
		// after the root, once per pass per delay, and stored for later ticks.
		if (delayQueued[static_cast<size_t>(busId)] != passId && delayJobCount < static_cast<int>(delayJobs.size())) {
			delayQueued[static_cast<size_t>(busId)] = passId;
			DelayJob& job = delayJobs[static_cast<size_t>(delayJobCount++)];
			job.busId = busId;
			job.sourceBusId = node.sourceBusId;
			job.params = node.frameDelay;
		}
		renderFrameDelayLeaf(node.frameDelay, out, width, height);
		return Status::Ok;
	}

	default:
		return Status::Empty;
	}
}

void VideoGraphRenderer::renderFrameDelayLeaf(const FrameDelayParams& p, Frame& out, int width, int height) {
	frameRingClear(p.ringSlotId, p.clearCounter);
	const int frames = std::clamp(p.frames, 1, kFrameDelayMaxFrames);
	// The newest `frames` ticks plus the one being stored now.
	frameRingSetCapacity(p.ringSlotId, frames + 1);
	// A picture FRAMES ticks old, or, when FRAMES has just been made longer than the history kept, the oldest one.
	if (frameRingReadDelayed(p.ringSlotId, p.frameCounter, frames, delayPrev)) {
		const bool stored32 = !delayPrev.rgbaF.empty() && delayPrev.rgba.empty();
		if (stored32 && passFloat) {
			// A picture stored as floats comes back as floats: the delay passes it on with nothing rounded.
			out.format = FrameFormat::F32;
			static_cast<std::vector<unsigned char>&>(out).clear();
			if (delayPrev.width == width && delayPrev.height == height) out.f.swap(delayPrev.rgbaF);
			else resampleIntoF(delayPrev.rgbaF, delayPrev.width, delayPrev.height, out.f, width, height);
			return;
		}
		if (stored32) {
			delayPrev.rgba.resize(delayPrev.rgbaF.size());
			floatToBytes(delayPrev.rgbaF.data(), delayPrev.rgba.data(), delayPrev.rgbaF.size());
		}
		if (delayPrev.width == width && delayPrev.height == height) out.swap(delayPrev.rgba);
		else resampleInto(delayPrev.rgba, delayPrev.width, delayPrev.height, out, width, height);
		return;
	}
	// Nothing is stored that has ever been old enough (start, a clear, or a source that has only just appeared): black.
	out.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
	for (size_t i = 3; i < out.size(); i += 4u) {
		out[i] = 255;
	}
}

void VideoGraphRenderer::drawDelaySources(int width, int height) {
	const bool outerMemoize = memoizeRoots;
	const int outerDeepest = deepest;
	memoizeRoots = true;
	// The list may grow while it is worked through: a source can hold other delays.
	while (delayJobNext < delayJobCount) {
		const DelayJob job = delayJobs[static_cast<size_t>(delayJobNext++)];
		const FrameDelayParams& p = job.params;
		frameRingClear(p.ringSlotId, p.clearCounter);
		// A float store only while the module's whole ring stays within the budget (frame_ring.hpp).
		const bool floatFits = frameDelayFloatStoreFits(p.frames, width, height);
		frameRingSetByteFallback(p.ringSlotId, passFloat && !floatFits);
		// One store per tick per delay: another pass, size or renderer may have stored this tick's picture already.
		if (frameRingIsStored(p.ringSlotId, p.frameCounter)) {
			continue;
		}
		const int source = job.sourceBusId;
		if (source >= 0 && source <= kVideoBusCount && memoPass[static_cast<size_t>(source)] == passId && !onStack[static_cast<size_t>(source)]) {
			// The pass has already drawn the source (it is a root, or something reached it). Store that picture.
			const Frame& done = memo[static_cast<size_t>(source)];
			if (done.isFloat() && floatFits) {
				// The delay keeps the picture as floats too (full precision): rounding it to a byte at every store
				// would stop a decaying loop at a grey it cannot leave.
				frameRingPushFloatOnly(p.ringSlotId, p.frameCounter, done.f, width, height);
			}
			else if (done.isFloat()) {
				// Over the memory budget: store the picture rounded half up.
				done.copyBytesTo(delayBytes);
				frameRingPush(p.ringSlotId, p.frameCounter, delayBytes, width, height);
			}
			else {
				frameRingPush(p.ringSlotId, p.frameCounter, done, width, height);
			}
			continue;
		}
		deepest = 0;
		if (renderNode(source, width, height, 0, delaySource) == Status::Ok) {
			if (delaySource.isFloat() && floatFits) frameRingPushFloatOnly(p.ringSlotId, p.frameCounter, delaySource.f, width, height);
			else if (delaySource.isFloat()) {
				delaySource.copyBytesTo(delayBytes);
				frameRingPush(p.ringSlotId, p.frameCounter, delayBytes, width, height);
			}
			else frameRingPush(p.ringSlotId, p.frameCounter, delaySource, width, height);
		}
	}
	memoizeRoots = outerMemoize;
	deepest = outerDeepest;
}

void VideoGraphRenderer::resampleInto(const std::vector<unsigned char>& src, int srcWidth, int srcHeight, Frame& out, int width, int height) {
	prepare(out, width, height);
	for (int y = 0; y < height; ++y) {
		const float sourceY = height > 1 ? (static_cast<float>(y) / static_cast<float>(height - 1)) * static_cast<float>(srcHeight - 1) : 0.f;
		for (int x = 0; x < width; ++x) {
			const float sourceX = width > 1 ? (static_cast<float>(x) / static_cast<float>(width - 1)) * static_cast<float>(srcWidth - 1) : 0.f;
			const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
			sampleBilinear(src, srcWidth, srcHeight, sourceX, sourceY, &out[index]);
		}
	}
}

void VideoGraphRenderer::resampleIntoF(const std::vector<float>& src, int srcWidth, int srcHeight, std::vector<float>& out, int width, int height) {
	out.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
	for (int y = 0; y < height; ++y) {
		const float sourceY = height > 1 ? (static_cast<float>(y) / static_cast<float>(height - 1)) * static_cast<float>(srcHeight - 1) : 0.f;
		for (int x = 0; x < width; ++x) {
			const float sourceX = width > 1 ? (static_cast<float>(x) / static_cast<float>(width - 1)) * static_cast<float>(srcWidth - 1) : 0.f;
			const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
			sampleBilinearF(src, srcWidth, srcHeight, sourceX, sourceY, &out[index]);
		}
	}
}

// The beam layer's output: the tone-mapped energy plus glow (in `glow`) as bytes, or as floats in full precision.
void VideoGraphRenderer::toneMapOutput(const crt::EnergyField& energy, float k, Frame& out, int width, int height) {
	if (passFloat) {
		out.beginF32(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
		crt::toneMapF(energy, &glow, k, out.f.data());
		return;
	}
	prepare(out, width, height);
	crt::toneMap(energy, &glow, k, out.data());
}

// Starts the picture a converted node produces: a float frame while the pass is in full precision, else bytes.
void VideoGraphRenderer::beginOutput(Frame& out, size_t count) const {
	if (passFloat) out.beginF32(count);
	else out.beginU8(count);
}

void VideoGraphRenderer::prepare(Frame& frame, int width, int height) {
	const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	if (frame.size() != byteCount) {
		frame.assign(byteCount, 0);
	}
}

void VideoGraphRenderer::renderColorLabFrame(Frame& source, Frame& out, int width, int height, const ColorLabParams& params, const float* modLuma) {
	const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	beginOutput(out, byteCount);
	const float mix = clamp01(params.mix);
	const float hueShift = params.hue;
	const float sat = std::clamp(params.saturation, 0.f, 2.f);
	const float bright = std::clamp(params.brightness, 0.f, 2.f);
	const float contrast = std::clamp(params.contrast, 0.f, 2.f);
	const float posterize = clamp01(params.posterize);
	const float solarize = clamp01(params.solarize);
	const int steps = std::clamp(static_cast<int>(2.f + posterize * 30.f), 2, 32);
	withSource(source, [&](auto src) {
		withDest<StoreRule::Truncate>(out, [&](auto dst) {
			// In a float picture the colour is not cut to a byte between the HSV step and the contrast step.
			constexpr bool toFloat = std::is_same<decltype(dst), FloatDest>::value;
			render_pool::parallelSpan(byteCount, 4u * 8192u, [&](size_t spanBegin, size_t spanEnd) {
			for (size_t i = spanBegin; i < spanEnd; i += 4u) {
				float r = src[i + 0] / 255.f;
				float g = src[i + 1] / 255.f;
				float b = src[i + 2] / 255.f;
				if (params.channelMode == 1) std::swap(r, g);
				else if (params.channelMode == 2) { float t = r; r = g; g = b; b = t; }
				else if (params.channelMode == 3) { float t = r; r = b; b = g; g = t; }
				float h = 0.f, sv = 0.f, v = 0.f;
				rgbToHsv(r, g, b, h, sv, v);
				// MOD IN: the picture shifts the hue, half a turn either way at full amount.
				h = modLuma != nullptr ? fract(h + hueShift + params.modAmount * (modLuma[i / 4u] - 0.5f)) : fract(h + hueShift);
				sv = clamp01(sv * sat);
				v = clamp01(v * bright);
				if (toFloat) {
					float rr = 0.f, gg = 0.f, bb = 0.f;
					hsvToRgb255(h, sv, v, rr, gg, bb);
					r = rr / 255.f; g = gg / 255.f; b = bb / 255.f;
				}
				else {
					unsigned char rr = 0, gg = 0, bb = 0;
					hsvToRgb(h, sv, v, rr, gg, bb);
					r = rr / 255.f; g = gg / 255.f; b = bb / 255.f;
				}
				r = clamp01((r - 0.5f) * contrast + 0.5f);
				g = clamp01((g - 0.5f) * contrast + 0.5f);
				b = clamp01((b - 0.5f) * contrast + 0.5f);
				if (posterize > 0.0001f) {
					r = std::round(r * static_cast<float>(steps - 1)) / static_cast<float>(steps - 1);
					g = std::round(g * static_cast<float>(steps - 1)) / static_cast<float>(steps - 1);
					b = std::round(b * static_cast<float>(steps - 1)) / static_cast<float>(steps - 1);
				}
				if (solarize > 0.0001f) {
					auto solar = [&](float c) { return c * (1.f - solarize) + (c > 0.5f ? 1.f - c : c) * solarize; };
					r = clamp01(solar(r)); g = clamp01(solar(g)); b = clamp01(solar(b));
				}
				if (params.invert) { r = 1.f - r; g = 1.f - g; b = 1.f - b; }
				dst.put(i + 0, src[i + 0] * (1.f - mix) + r * 255.f * mix);
				dst.put(i + 1, src[i + 1] * (1.f - mix) + g * 255.f * mix);
				dst.put(i + 2, src[i + 2] * (1.f - mix) + b * 255.f * mix);
				dst.put(i + 3, 255.f);
			}
			});
		});
	});
}

void VideoGraphRenderer::renderKeyerFrame(Frame& source, Frame& out, int width, int height, const KeyerParams& params, const float* modLuma) {
	const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	beginOutput(out, byteCount);
	const float threshold = clamp01(params.threshold);
	const float softness = std::clamp(params.softness, 0.001f, 1.f);
	const float gain = std::clamp(params.gain, 0.f, 2.f);
	const float mix = clamp01(params.mix);
	unsigned char kr = 0, kg = 0, kb = 0;
	hsvToRgb(params.hue, 0.9f, 1.f, kr, kg, kb);
	withSource(source, [&](auto src) {
		withDest<StoreRule::Truncate>(out, [&](auto dst) {
			for (size_t i = 0; i < byteCount; i += 4u) {
				const float l = clamp01((src[i + 0] * 0.2126f + src[i + 1] * 0.7152f + src[i + 2] * 0.0722f) / 255.f);
				// MOD IN: the picture moves the threshold, so the key opens at a different brightness in different places.
				const float thresholdHere = modLuma != nullptr ? clamp01(threshold + params.modAmount * 2.f * (modLuma[i / 4u] - 0.5f)) : threshold;
				float matte = clamp01((l - thresholdHere + softness * 0.5f) / softness) * gain;
				matte = clamp01(matte);
				if (params.invert) matte = 1.f - matte;
				float r = matte, g = matte, b = matte;
				if (params.mode == 1) { r = kr / 255.f * matte; g = kg / 255.f * matte; b = kb / 255.f * matte; }
				else if (params.mode == 2) { r = src[i + 0] / 255.f * matte; g = src[i + 1] / 255.f * matte; b = src[i + 2] / 255.f * matte; }
				dst.put(i + 0, src[i + 0] * (1.f - mix) + r * 255.f * mix);
				dst.put(i + 1, src[i + 1] * (1.f - mix) + g * 255.f * mix);
				dst.put(i + 2, src[i + 2] * (1.f - mix) + b * 255.f * mix);
				dst.put(i + 3, matte * 255.f);
			}
		});
	});
}

namespace {
// Bus Mixer and Wave Shaper look a byte picture up in a table when every output byte depends only on its input
// bytes. The table is used from this size up; below it, working the table out would cost more than the picture.
constexpr size_t kBusMixerTableMinBytes = 4u * 65536u;  // 65536 pixels
constexpr size_t kWaveShaperTableMinBytes = 4u * 1024u;  // 1024 pixels

// Two small byte pictures that between them hold every pair of byte values once: value n = pixel x 3 + channel has
// a = n / 256 in the first picture and b = n % 256 in the second (the last pixel's spare channels are 0, alpha is 255).
struct BytePairPictures {
	std::vector<unsigned char> a;
	std::vector<unsigned char> b;
};
const BytePairPictures& bytePairPictures() {
	static const BytePairPictures pictures = [] {
		BytePairPictures p;
		const size_t pixels = (65536u + 2u) / 3u;
		p.a.assign(pixels * 4u, 0);
		p.b.assign(pixels * 4u, 0);
		for (size_t n = 0; n < 65536u; ++n) {
			p.a[(n / 3u) * 4u + n % 3u] = static_cast<unsigned char>(n >> 8);
			p.b[(n / 3u) * 4u + n % 3u] = static_cast<unsigned char>(n & 255u);
		}
		for (size_t px = 0; px < pixels; ++px) {
			p.a[px * 4u + 3u] = 255;
			p.b[px * 4u + 3u] = 255;
		}
		return p;
	}();
	return pictures;
}

// One small byte picture that holds every byte value once: value n is channel n % 3 of pixel n / 3.
constexpr size_t kByteRampBytes = ((256u + 2u) / 3u) * 4u;
const std::array<unsigned char, kByteRampBytes>& byteRampPicture() {
	static const std::array<unsigned char, kByteRampBytes> picture = [] {
		std::array<unsigned char, kByteRampBytes> p{};
		for (size_t n = 0; n < 256u; ++n) {
			p[(n / 3u) * 4u + n % 3u] = static_cast<unsigned char>(n);
		}
		for (size_t px = 0; px < kByteRampBytes / 4u; ++px) {
			p[px * 4u + 3u] = 255;
		}
		return p;
	}();
	return picture;
}
}  // namespace

void VideoGraphRenderer::renderBusMixerFrame(const Frame& a, const Frame& b, const Frame* matte, Frame& out, int width, int height, const BusMixerParams& params) {
	const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	beginOutput(out, byteCount);
	const float baseMix = clamp01(params.mix);
	const float matteAmount = clamp01(params.matteAmount);
	const int mode = std::clamp(params.mode, 0, 6);
	const bool hasMatte = matte != nullptr && matte->count() == byteCount;
	// The pixel loop, over the bytes [spanBegin, spanEnd) of whatever pictures it is given.
	auto mixSpan = [&](auto srcA, auto srcB, auto srcM, auto dst, size_t spanBegin, size_t spanEnd) {
		for (size_t i = spanBegin; i < spanEnd; i += 4u) {
			// Factor is how much of B shows through. Matte mode (6) keys on the MAT BUS luma.
			// Without a MAT BUS, Matte mode falls back to the MIX knob.
			float factor = baseMix;
			if (hasMatte) {
				float m = (srcM[i + 0] * 0.2126f + srcM[i + 1] * 0.7152f + srcM[i + 2] * 0.0722f) / 255.f;
				if (params.invertMatte) m = 1.f - m;
				if (mode == 6) {
					factor = clamp01(m);
				}
				else if (matteAmount > 0.0001f) {
					factor = clamp01(baseMix * (1.f - matteAmount) + m * matteAmount);
				}
			}
			for (int c = 0; c < 3; ++c) {
				const float av = srcA[i + c] / 255.f;
				const float bv = srcB[i + c] / 255.f;
				float v = bv;
				if (mode == 1) v = clamp01(av + bv);
				else if (mode == 2) v = av * bv;
				else if (mode == 3) v = 1.f - (1.f - av) * (1.f - bv);
				else if (mode == 4) v = std::abs(av - bv);
				else if (mode == 5) v = std::max(av, bv);
				dst.put(i + c, (av * (1.f - factor) + v * factor) * 255.f);
			}
			dst.put(i + 3, 255.f);
		}
	};
	auto run = [&](auto srcA, auto srcB, auto srcM) {
		withDest<StoreRule::Truncate>(out, [&](auto dst) {
			render_pool::parallelSpan(byteCount, 4u * 16384u, [&](size_t spanBegin, size_t spanEnd) {
				mixSpan(srcA, srcB, srcM, dst, spanBegin, spanEnd);
			});
		});
	};
	const bool allBytes = !a.isFloat() && !b.isFloat() && (!hasMatte || !matte->isFloat());
	if (allBytes && !hasMatte && !out.isFloat() && byteCount >= kBusMixerTableMinBytes) {
		// Byte pictures and no matte: the factor is the same everywhere, so an output byte depends on its two input
		// bytes and nothing else. The answers for all 65536 pairs are made by the pixel loop itself, run over two small
		// pictures that hold every pair once (a table worked out by other code could differ in the last bit), and the
		// picture is then looked up. The table is kept while MODE and the mix stay as they were.
		if (!busMixerTableValid || busMixerTableMode != mode || busMixerTableMix != baseMix) {
			const BytePairPictures& pairs = bytePairPictures();
			busMixerTableWork.resize(pairs.a.size());
			const ByteDest<StoreRule::Truncate> work{busMixerTableWork.data()};
			render_pool::parallelSpan(pairs.a.size(), 4u * 2048u, [&](size_t spanBegin, size_t spanEnd) {
				mixSpan(ByteSource{pairs.a.data()}, ByteSource{pairs.b.data()}, ByteSource{nullptr}, work, spanBegin, spanEnd);
			});
			busMixerTable.resize(65536u);
			for (size_t n = 0; n < 65536u; ++n) {
				busMixerTable[n] = busMixerTableWork[(n / 3u) * 4u + n % 3u];
			}
			busMixerTableMode = mode;
			busMixerTableMix = baseMix;
			busMixerTableValid = true;
		}
		const unsigned char* pa = a.data();
		const unsigned char* pb = b.data();
		unsigned char* po = out.data();
		const unsigned char* table = busMixerTable.data();
		render_pool::parallelSpan(byteCount, 4u * 16384u, [=](size_t spanBegin, size_t spanEnd) {
			for (size_t i = spanBegin; i < spanEnd; i += 4u) {
				po[i + 0] = table[(static_cast<size_t>(pa[i + 0]) << 8) | pb[i + 0]];
				po[i + 1] = table[(static_cast<size_t>(pa[i + 1]) << 8) | pb[i + 1]];
				po[i + 2] = table[(static_cast<size_t>(pa[i + 2]) << 8) | pb[i + 2]];
				po[i + 3] = 255;
			}
		});
	}
	else if (allBytes) {
		run(ByteSource{a.data()}, ByteSource{b.data()}, ByteSource{hasMatte ? matte->data() : nullptr});
	}
	else {
		run(mixedSourceOf(a), mixedSourceOf(b), hasMatte ? mixedSourceOf(*matte) : MixedSource{nullptr, nullptr});
	}
}

void VideoGraphRenderer::renderWaveShaperFrame(Frame& source, Frame& out, int width, int height, const WaveShaperParams& params) {
	const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	beginOutput(out, byteCount);
	const float drive = std::clamp(params.drive, 0.f, 4.f);
	const float bias = std::clamp(params.bias, -1.f, 1.f);
	const float fold = clamp01(params.fold);
	const float quantize = clamp01(params.quantize);
	const float mirror = clamp01(params.mirror);
	const float contrast = std::clamp(params.contrast, 0.f, 2.f);
	const float mix = clamp01(params.mix);
	const int steps = std::clamp(static_cast<int>(2.f + quantize * 62.f), 2, 64);
	auto shape = [&](float c) {
		float v = (c - 0.5f) * drive + 0.5f + bias * 0.5f;
		if (fold > 0.0001f) v = v * (1.f - fold) + (1.f - std::abs(std::fmod(std::abs(v) * 2.f, 2.f) - 1.f)) * fold;
		if (mirror > 0.0001f) v = v * (1.f - mirror) + std::abs(v - 0.5f) * 2.f * mirror;
		v = clamp01((v - 0.5f) * contrast + 0.5f);
		if (quantize > 0.0001f) v = std::round(v * static_cast<float>(steps - 1)) / static_cast<float>(steps - 1);
		return clamp01(v);
	};
	// The pixel loop, over the bytes [spanBegin, spanEnd) of whatever picture it is given.
	auto shapeSpan = [&](auto src, auto dst, size_t spanBegin, size_t spanEnd) {
		for (size_t i = spanBegin; i < spanEnd; i += 4u) {
			if (params.mode == 1) {
				const float l = shape((src[i + 0] * 0.2126f + src[i + 1] * 0.7152f + src[i + 2] * 0.0722f) / 255.f);
				for (int c = 0; c < 3; ++c) dst.put(i + c, src[i + c] * (1.f - mix) + l * 255.f * mix);
			}
			else {
				for (int c = 0; c < 3; ++c) {
					const float v = shape(src[i + c] / 255.f);
					dst.put(i + c, src[i + c] * (1.f - mix) + v * 255.f * mix);
				}
			}
			dst.put(i + 3, 255.f);
		}
	};
	if (!source.isFloat() && !out.isFloat() && params.mode != 1 && byteCount >= kWaveShaperTableMinBytes) {
		// RGB mode on a byte picture: an output byte depends on its input byte and nothing else. The 256 answers are
		// made by the pixel loop itself, run over a small picture that holds every byte value once, and the picture is
		// then looked up.
		const std::array<unsigned char, kByteRampBytes>& ramp = byteRampPicture();
		std::array<unsigned char, kByteRampBytes> shaped;
		shapeSpan(ByteSource{ramp.data()}, ByteDest<StoreRule::Truncate>{shaped.data()}, 0u, kByteRampBytes);
		std::array<unsigned char, 256> table;
		for (size_t n = 0; n < 256u; ++n) {
			table[n] = shaped[(n / 3u) * 4u + n % 3u];
		}
		const unsigned char* ps = source.data();
		unsigned char* po = out.data();
		const unsigned char* t = table.data();
		render_pool::parallelSpan(byteCount, 4u * 16384u, [=](size_t spanBegin, size_t spanEnd) {
			for (size_t i = spanBegin; i < spanEnd; i += 4u) {
				po[i + 0] = t[ps[i + 0]];
				po[i + 1] = t[ps[i + 1]];
				po[i + 2] = t[ps[i + 2]];
				po[i + 3] = 255;
			}
		});
		return;
	}
	withSource(source, [&](auto src) {
		withDest<StoreRule::Truncate>(out, [&](auto dst) {
			render_pool::parallelSpan(byteCount, 4u * 16384u, [&](size_t spanBegin, size_t spanEnd) {
				shapeSpan(src, dst, spanBegin, spanEnd);
			});
		});
	});
}

void VideoGraphRenderer::renderFreezeFrame(Frame& source, Frame& out, int width, int height, const FreezeFrameParams& params) {
	// Clear first, then capture. A clear marks the current counter as handled, so the capture
	// that was cleared is not taken again.
	if (params.clear) {
		freezeFrameClear(params.freezeSlotId, params.captureCounter);
	}
	if (params.captureCounter != 0) {
		if (source.isFloat()) {
			// Full precision: a float picture is held as float32, not rounded (16 bytes a pixel; a byte picture is
			// already exact and is held as bytes).
			freezeFrameCaptureFloatIfNew(params.freezeSlotId, params.captureCounter, source.f, width, height);
		}
		else {
			freezeFrameCaptureIfNew(params.freezeSlotId, params.captureCounter, source, width, height);
		}
	}

	const float hold = clamp01(params.hold);
	const float blend = clamp01(params.blend);
	const float amount = clamp01(hold * blend);
	if (amount <= 0.0001f || !freezeFrameRead(params.freezeSlotId, frozenData)) {
		out.swap(source);
		return;
	}
	// The held picture is bytes, or floats when it was captured in full precision. A byte pass that meets a float
	// one (the setting was just switched off) rounds it half up.
	if (!passFloat && frozenData.rgba.empty() && !frozenData.rgbaF.empty()) {
		frozenData.rgba.resize(frozenData.rgbaF.size());
		floatToBytes(frozenData.rgbaF.data(), frozenData.rgba.data(), frozenData.rgbaF.size());
		std::vector<float>().swap(frozenData.rgbaF);
	}
	const bool frozenIsFloat = !frozenData.rgbaF.empty();
	const std::vector<unsigned char>* frozenFrame = &frozenData.rgba;
	const std::vector<float>* frozenFrameF = &frozenData.rgbaF;
	if (frozenData.width != width || frozenData.height != height) {
		if (frozenIsFloat) {
			resampleIntoF(frozenData.rgbaF, frozenData.width, frozenData.height, frozenResampledF, width, height);
			frozenFrameF = &frozenResampledF;
		}
		else {
			resampleInto(frozenData.rgba, frozenData.width, frozenData.height, frozenResampled, width, height);
			frozenFrame = &frozenResampled;
		}
	}

	const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	beginOutput(out, byteCount);
	auto mix = [&](auto frozen) {
		withSource(source, [&](auto src) {
			withDest<StoreRule::Truncate>(out, [&](auto dst) {
				for (size_t i = 0; i < byteCount; i += 4u) {
					dst.put(i + 0, src[i + 0] * (1.f - amount) + frozen[i + 0] * amount);
					dst.put(i + 1, src[i + 1] * (1.f - amount) + frozen[i + 1] * amount);
					dst.put(i + 2, src[i + 2] * (1.f - amount) + frozen[i + 2] * amount);
					dst.put(i + 3, 255.f);
				}
			});
		});
	};
	if (frozenIsFloat) mix(FloatSource{frozenFrameF->data()});
	else mix(ByteSource{frozenFrame->data()});
}

bool VideoGraphRenderer::renderScopeTraceFrame(int busId, int width, int height, int scopeTraceSlotId, const ScopeTraceParams& params, Frame& out) {
	// Persistence: this node's phosphor decays once per render pass and takes new trace energy.
	// If the same node is reached twice in one pass (e.g. both inputs of a Bus Mixer), it reuses
	// the frame from the first visit, so the trail advances once.
	BeamState& bs = beam[static_cast<size_t>(busId)];
	if (!bs.energy.matches(width, height)) {
		bs.energy.resize(width, height);
		bs.display.clear();
		bs.display.f.clear();
		bs.hasBeam = false;
	}
	if (bs.pass == passId && bs.display.hasPixels()) {
		out = bs.display;
		return true;
	}
	bs.pass = passId;

	// RESET on the module wipes the phosphor: the trail of earlier windows is dropped before the new
	// one is drawn. Only a change of the serial clears, so steady state never clears. The stored
	// serial belongs to the bus, so a replacement module that starts on a bus an earlier Scope Trace
	// used (it publishes serial 0 against the old count) clears once, on its first frame.
	if (params.clearSerial != bs.scopeClearSerial) {
		bs.scopeClearSerial = params.clearSerial;
		bs.energy.clear();
	}

	const float decay = clamp01(params.decay);
	bs.energy.decay(0.5f + 0.48f * decay);

	const bool haveSamples = readScopeWindow(scopeTraceSlotId, params, scopeWindow);
	if (haveSamples && scopeWindow.length >= 2) {
		depositScopeTrace(bs.energy, width, height, params, scopeWindow);
		bs.hasBeam = true;
	}
	if (!bs.hasBeam) {
		return false;
	}

	// Compose: tone-mapped phosphor plus glow, then the graticule on top (not persistent).
	glow.compute(bs.energy, std::clamp(params.bloom, 0.f, 2.f));
	toneMapOutput(bs.energy, kScopeToneK, out, width, height);

	const int gridLines = 4;
	for (int line = 1; line < gridLines; ++line) {
		const float x = static_cast<float>(line) * static_cast<float>(width - 1) / static_cast<float>(gridLines);
		const float y = static_cast<float>(line) * static_cast<float>(height - 1) / static_cast<float>(gridLines);
		drawLine(out, width, height, x, 0.f, x, static_cast<float>(height - 1), 24, 42, 32, 0.22f, 1);
		drawLine(out, width, height, 0.f, y, static_cast<float>(width - 1), y, 24, 42, 32, 0.22f, 1);
	}
	drawLine(out, width, height, 0.f, static_cast<float>(height - 1) * 0.5f, static_cast<float>(width - 1), static_cast<float>(height - 1) * 0.5f, 42, 62, 48, 0.24f, 1);
	drawLine(out, width, height, static_cast<float>(width - 1) * 0.5f, 0.f, static_cast<float>(width - 1) * 0.5f, static_cast<float>(height - 1), 42, 62, 48, 0.24f, 1);

	bs.display.copyFrom(out);  // the picture only; a large one is copied by the pool
	return true;
}

bool VideoGraphRenderer::readScopeWindow(int scopeTraceSlotId, const ScopeTraceParams& params, SignalWindow& out) {
	// The window is the newest W samples, W = TIME in samples. Time-based modes start it at a
	// trigger instead, so a periodic input lines up from frame to frame:
	// 1. TRIG cable patched: start at the newest TRIG edge whose window is complete.
	// 2. Otherwise: start at the latest rising crossing of the trigger level (0 V by default) by X,
	//    with hysteresis, that leaves room for the window inside the newest W + S samples,
	//    S = min(W, kSignalRingMaxCopy - W), so the span W + S fits in the longest read the ring gives
	//    (S = W unless the window is more than half of that).
	// 3. Auto: no trigger in range, so free-run on the newest W samples, as the XY and Polar modes do.
	const float rate = signalRingRate(scopeTraceSlotId);
	if (!(rate > 0.f)) {
		return false;
	}
	const double seconds = scopeWindowSeconds(params.timebase);
	const int W = std::clamp(static_cast<int>(std::lround(seconds * static_cast<double>(rate))), 2, kSignalRingMaxCopy);
	const int mode = std::clamp(params.mode, 0, 3);
	const bool timeBased = mode == static_cast<int>(ScopeTraceMode::Oscilloscope) || mode == static_cast<int>(ScopeTraceMode::DualTrace);
	if (!timeBased) {
		return signalRingReadLatest(scopeTraceSlotId, W, out);
	}

	if (params.triggerPatched && signalRingReadTriggered(scopeTraceSlotId, W, out)) {
		return true;
	}
	const int S = std::min(W, kSignalRingMaxCopy - W);
	if (S >= 1 && signalRingReadLatest(scopeTraceSlotId, W + S, scopeSpan) && scopeSpan.channels > 0) {
		const int p = scopeFindTrigger(scopeSpan.data.data(), scopeSpan.length, W, kScopeTriggerHysteresis, params.triggerLevel);
		if (p >= 0) {
			out.channels = scopeSpan.channels;
			out.length = W;
			out.data.resize(static_cast<size_t>(out.channels) * static_cast<size_t>(W));
			for (int ch = 0; ch < out.channels; ++ch) {
				const float* src = scopeSpan.data.data() + static_cast<size_t>(ch) * static_cast<size_t>(scopeSpan.length) + p;
				std::copy(src, src + W, out.data.begin() + static_cast<long>(static_cast<size_t>(ch) * static_cast<size_t>(W)));
			}
			out.framePeriodSamples = 0;
			out.sampleRate = scopeSpan.sampleRate;
			return true;
		}
	}
	return signalRingReadLatest(scopeTraceSlotId, W, out);
}

// Draws the window as a polyline of at most 4096 points, one stroke per segment, before the phosphor
// decays. DWELL sets how the energy is shared along the trace. For a stroke of len pixels:
//     perPx = base x pow(meanLen / max(len, 0.25), DWELL), at most 16 x base
//     total = perPx x max(len, 0.5)
// base is 0.25 x INTENSITY x Z x gain. meanLen is the mean length of the window's drawable segments
// (at least 0.5 px), taken per trace in Dual Trace. At DWELL 0 every pixel of path gets base, so the trace has an
// even brightness. At DWELL 1 every stroke gets the same energy whatever its length, which is a CRT
// beam at constant current: a slow stretch glows and a fast one is dim. The law is relative to the
// window's own mean, so running the whole figure faster leaves its average brightness per pixel of path
// where it was, and only the spread along the trace changes. The 0.5 px floor on total makes a stopped
// beam burn a dot, which energy in proportion to length never drew. The spot is round with a sigma set
// by BEAM (see scopeBeamSigmaPx), and the gain scales with the frame height so the picture looks the
// same at every size: a spot k times wider spreads each pixel of path over k times the rows, so the
// energy per pixel of path goes up k times to keep the peak where it was.
void VideoGraphRenderer::depositScopeTrace(crt::EnergyField& energy, int width, int height, const ScopeTraceParams& params, const SignalWindow& window) {
	constexpr float kBeamEnergyPerPixel = 0.25f;
	constexpr int kMaxPoints = 4096;
	constexpr float kMinDwellLength = 0.25f;  // px: segments shorter than this count as this long for DWELL
	constexpr float kMinStrokeLength = 0.5f;  // px: a stroke shorter than this is charged as this long
	constexpr float kMinMeanLength = 0.5f;    // px: floor of the window's mean segment length
	constexpr float kMaxDwellBoost = 16.f;    // most a slow stroke can outshine the even level
	const crt::BeamSpot spot(scopeBeamSigmaPx(params.thickness, height), crt::BeamSpot::Shape::Round);

	const int mode = std::clamp(params.mode, 0, 3);
	const bool dual = mode == static_cast<int>(ScopeTraceMode::DualTrace);
	const float intensity = std::clamp(params.intensity, 0.f, 2.f);
	const float dwell = std::clamp(params.dwell, 0.f, 1.f);
	const float gain = kScopeBeamGain * scopeBeamScale(height);
	const float rotation = rasterlab_cv::wrapTurn(params.rotation) * kPi;
	const float cr = std::cos(rotation);
	const float sr = std::sin(rotation);
	const float hueA = fract(params.hue);
	const float hueB = fract(params.hue + 0.33f);
	const int count = window.length;
	const int stride = std::max(1, (count + kMaxPoints - 1) / kMaxPoints);

	float rgbA[3] = {0.f, 0.f, 0.f};
	float rgbB[3] = {0.f, 0.f, 0.f};
	{
		unsigned char r = 0, g = 0, b = 0, r2 = 0, g2 = 0, b2 = 0;
		hsvToRgb(hueA, 0.92f, 1.f, r, g, b);
		hsvToRgb(hueB, 0.86f, 1.f, r2, g2, b2);
		rgbA[0] = r / 255.f; rgbA[1] = g / 255.f; rgbA[2] = b / 255.f;
		rgbB[0] = r2 / 255.f; rgbB[1] = g2 / 255.f; rgbB[2] = b2 / 255.f;
	}

	auto toScreen = [&](float x, float y, float& screenX, float& screenY) {
		x *= std::clamp(params.scaleX, 0.f, 2.f);
		y *= std::clamp(params.scaleY, 0.f, 2.f);
		const float rx = x * cr - y * sr;
		const float ry = x * sr + y * cr;
		const float px = std::clamp(rx + std::clamp(params.offsetX, -1.f, 1.f), -1.5f, 1.5f);
		const float py = std::clamp(ry + std::clamp(params.offsetY, -1.f, 1.f), -1.5f, 1.5f);
		screenX = (px * 0.5f + 0.5f) * static_cast<float>(width - 1);
		screenY = (1.f - (py * 0.5f + 0.5f)) * static_cast<float>(height - 1);
	};

	// One stroke of trace, spread by the shared beam layer. meanLen is the mean segment length of the
	// trace the stroke belongs to (see the law above). A stroke with no finite length is dropped.
	auto stroke = [&](float x0, float y0, float x1, float y1, float zMid, const float* rgb, float meanLen) {
		const float len = std::hypot(x1 - x0, y1 - y0);
		if (!std::isfinite(len)) {
			return;
		}
		const float base = kBeamEnergyPerPixel * intensity * zMid * gain;
		float perPx = base;
		if (dwell > 0.f) {
			perPx = std::min(base * std::pow(meanLen / std::max(len, kMinDwellLength), dwell), kMaxDwellBoost * base);
		}
		crt::depositPath(energy, spot, x0, y0, x1, y1, rgb, perPx * std::max(len, kMinStrokeLength), 0, height);
	};

	// Pass 1: map the decimated window to screen pixels and add up how long its segments are. A segment
	// with no finite length is never drawn, so it is left out of the mean as well.
	const float* chX = window.data.data();
	const float* chY = window.data.data() + count;
	const float* chZ = window.data.data() + 2 * static_cast<size_t>(count);
	scopePoints.clear();
	scopePoints.reserve(static_cast<size_t>((count + stride - 1) / stride));
	double lengthSum[2] = {0.0, 0.0};
	int lengthCount[2] = {0, 0};
	for (int i = 0; i < count; i += stride) {
		const float age = count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 1.f;
		const float timeX = age * 2.f - 1.f;
		const float sx = chX[i];
		const float sy = chY[i];
		const float z = std::clamp(chZ[i], 0.f, 2.f);
		float x = 0.f, y = 0.f, x2 = 0.f, y2 = 0.f;

		if (mode == static_cast<int>(ScopeTraceMode::Oscilloscope)) {
			x = timeX;
			y = sx;
		}
		else if (dual) {
			x = timeX;
			y = sx * 0.42f + 0.38f;
			x2 = timeX;
			y2 = sy * 0.42f - 0.38f;
		}
		else if (mode == static_cast<int>(ScopeTraceMode::Polar)) {
			const float angle = sx * kPi;
			const float radius = std::clamp(sy * 0.5f + 0.5f, 0.f, 1.25f);
			x = std::cos(angle) * radius;
			y = std::sin(angle) * radius;
		}
		else {
			x = sx;
			y = sy;
		}

		ScopePoint point {};
		toScreen(x, y, point.x, point.y);
		if (dual) {
			toScreen(x2, y2, point.x2, point.y2);
		}
		point.z = z;
		if (!scopePoints.empty()) {
			const ScopePoint& last = scopePoints.back();
			const float lenA = std::hypot(point.x - last.x, point.y - last.y);
			if (std::isfinite(lenA)) {
				lengthSum[0] += lenA;
				++lengthCount[0];
			}
			if (dual) {
				const float lenB = std::hypot(point.x2 - last.x2, point.y2 - last.y2);
				if (std::isfinite(lenB)) {
					lengthSum[1] += lenB;
					++lengthCount[1];
				}
			}
		}
		scopePoints.push_back(point);
	}

	const int segments = static_cast<int>(scopePoints.size()) - 1;
	if (segments < 1) {
		return;
	}
	const float meanLenA = std::max(static_cast<float>(lengthSum[0] / std::max(lengthCount[0], 1)), kMinMeanLength);
	const float meanLenB = std::max(static_cast<float>(lengthSum[1] / std::max(lengthCount[1], 1)), kMinMeanLength);

	// Pass 2: stroke every segment with the DWELL law.
	for (int i = 1; i <= segments; ++i) {
		const ScopePoint& a = scopePoints[static_cast<size_t>(i - 1)];
		const ScopePoint& b = scopePoints[static_cast<size_t>(i)];
		const float zMid = 0.5f * (a.z + b.z);
		stroke(a.x, a.y, b.x, b.y, zMid, rgbA, meanLenA);
		if (dual) {
			stroke(a.x2, a.y2, b.x2, b.y2, zMid, rgbB, meanLenB);
		}
	}
}

namespace {

// Phosphor: at PERSIST 1 each frame keeps this share of the energy.
constexpr float kWobblePersistMax = 0.9f;
// Energy per unit of scanline pitch for a full-white source. This sets the average brightness of
// the picture. The spot size only spreads that light across the line.
constexpr float kWobbleGain = 0.8f;
// Slope of the tone curve for the DeflectionYoke.
constexpr float kWobbleToneK = 1.6f;
// Zoom limits: near 0 collapses the raster toward a point, 4 overscans it.
constexpr float kZoomMin = 0.02f;
constexpr float kZoomMax = 4.f;
// Smallest spot, in pixels, so a dense raster still has a visible beam.
constexpr float kMinSpotPx = 0.3f;
// Largest signal magnitude the renderer believes (normalized units). Anything beyond is far off screen
// anyway; a non-finite value counts as 0. Values inside the limit pass through unchanged.
constexpr float kWobbleSignalLimit = 8.f;
// S coil signal below this (times kSCoilRadians) is under a thousandth of a pixel of movement: the
// frame is drawn as if the coil were off.
constexpr float kSCoilMinSignal = 1e-5f;
// Collapse brightness (see the coverage block in renderDeflectionYokeFrame): the occupancy grid cell size in
// pixels, the most samples taken along one cell's path, and a trim on the compensated gain.
constexpr float kCollapseCellPx = 4.f;
constexpr int kCollapseMaxSteps = 32;
constexpr float kCollapseBoost = 1.f;

float finiteOr(float value, float fallback) {
	return std::isfinite(value) ? value : fallback;
}

float sanitiseSignal(float value) {
	return std::isfinite(value) ? std::clamp(value, -kWobbleSignalLimit, kWobbleSignalLimit) : 0.f;
}

// Runs fn(begin, end) over [0, count) split into contiguous ranges, one per thread.
template <typename Fn>
void parallelRanges(int count, int threads, Fn fn) {
	if (threads <= 1 || count < 2) {
		fn(0, count);
		return;
	}
	// The same split as always, but run on the persistent helpers instead of threads started and joined every frame.
	render_pool::parallelChunks(threads, [&fn, count, threads](int t) {
		fn(count * t / threads, count * (t + 1) / threads);
	});
}

}  // namespace

void VideoGraphRenderer::renderDeflectionYokeFrame(int busId, Frame& source, Frame& out, int width, int height, const DeflectionYokeParams& params) {
	// Scan model (see docs/architecture.md, "DeflectionYoke"). The beam paints `lines` scanlines per field,
	// top to bottom. Each scanline is a left-to-right sweep of `width` source cells. The field has
	// NTSC-like timing (crt::fieldSeconds), so scanline k starts at (k + x / width) * line time, and
	// the beam is off during the flyback. A cell lands where deflection and zoom put it at its scan
	// time. Its energy is brightness times the scanline pitch, spread along its path, so a fast stroke
	// is dimmer and a slow one brighter. Strokes are splatted with a Gaussian spot across the line.
	// No raster signal yet: pass the source through.
	const int lines = std::clamp(params.lines, crt::kMinLines, crt::kMaxLines);
	if (!signalRingReadField(params.signalSlotId, wobbleSignal, lines) || !wobbleSignal.valid()) {
		out.swap(source);
		return;
	}

	BeamState& bs = beam[static_cast<size_t>(busId)];
	if (!bs.energy.matches(width, height)) {
		bs.energy.resize(width, height);
		bs.display.clear();
		bs.display.f.clear();
		bs.hasBeam = false;
	}
	if (bs.pass == passId && bs.display.hasPixels()) {
		out = bs.display;
		return;
	}
	bs.pass = passId;
	bs.energy.decay(kWobblePersistMax * clamp01(finiteOr(params.persist, 0.f)));

	const int N = lines;
	const int W = width;
	const float fw = static_cast<float>(width);
	const float fh = static_cast<float>(height);
	const float pitch = fh / static_cast<float>(N);
	const float depth = clamp01(finiteOr(params.depth, 0.f));
	const float lineCurl = std::clamp(finiteOr(params.lineCurl, 0.f), -deflection_yoke::kShapeLimit, deflection_yoke::kShapeLimit);
	const float twist = std::clamp(finiteOr(params.twist, 0.f), -deflection_yoke::kShapeLimit, deflection_yoke::kShapeLimit);
	// SIZE scales only the normal scan. Below 1 the raster is a real smaller raster (see the coverage block).
	const float hSize = clamp01(finiteOr(params.hSize, 1.f));
	const float vSize = clamp01(finiteOr(params.vSize, 1.f));
	const bool scaled = hSize < 1.f || vSize < 1.f;
	const int edge = std::clamp(params.edge, 0, 2);
	const bool clampEdge = edge == static_cast<int>(DeflectionYokeEdge::Clamp);
	const bool wrap = edge == static_cast<int>(DeflectionYokeEdge::Wrap);
	const bool twisted = twist * depth != 0.f;
	const bool curled = lineCurl * depth != 0.f;
	const int threads = render_pool::threadBudget();

	// Deflection and zoom over one field window. Table entry i is window sample i; the field begins
	// `startFraction` of a sample after entry 0, on the video clock (src/video_clock.hpp), and a line is
	// the clock's fractional number of samples long, so nothing here is fitted to a whole sample.
	const vclock::Clock fieldClock(static_cast<double>(wobbleSignal.sampleRate), lines);
	const double samplesPerLine = fieldClock.samplesPerLine();
	const float fieldStart = static_cast<float>(wobbleSignal.startFraction);
	const int tableLength = wobbleSignal.length;
	std::vector<float> xTable(static_cast<size_t>(tableLength));
	std::vector<float> yTable(static_cast<size_t>(tableLength));
	std::vector<float> zTable(static_cast<size_t>(tableLength));
	std::vector<float> sTable(static_cast<size_t>(tableLength));
	float maxDx = 0.f;
	float maxDy = 0.f;
	float maxS = 0.f;
	float zoomLo = 1.f;
	float zoomHi = 1.f;
	for (int i = 0; i < tableLength; ++i) {
		const size_t u = static_cast<size_t>(i);
		xTable[u] = sanitiseSignal(wobbleSignal.sampleAt(kDeflectionYokeDeflectX, i));
		yTable[u] = sanitiseSignal(wobbleSignal.sampleAt(kDeflectionYokeDeflectY, i));
		zTable[u] = sanitiseSignal(wobbleSignal.sampleAt(kDeflectionYokeZoom, i));
		sTable[u] = sanitiseSignal(wobbleSignal.sampleAt(kDeflectionYokeSCoil, i));
		maxDx = std::max(maxDx, std::abs(xTable[u]));
		maxDy = std::max(maxDy, std::abs(yTable[u]));
		maxS = std::max(maxS, std::abs(sTable[u]));
		const float zoom = std::clamp(1.f + zTable[u], kZoomMin, kZoomMax);
		zoomLo = std::min(zoomLo, zoom);
		zoomHi = std::max(zoomHi, zoom);
	}
	// The S coil rotates the total deflection. A window with the coil silent is drawn the way it always
	// was (no rotation arithmetic at all), so a patch that never touches the S coil renders as before.
	const bool rotating = maxS > kSCoilMinSignal;
	auto tableAt = [tableLength](const std::vector<float>& table, float pos) {
		const float clamped = std::clamp(pos, 0.f, static_cast<float>(tableLength - 2));
		const int i0 = static_cast<int>(clamped);
		const float frac = clamped - static_cast<float>(i0);
		const float a = table[static_cast<size_t>(i0)];
		return a + (table[static_cast<size_t>(i0) + 1] - a) * frac;
	};

	// Clamp extends the picture past each edge with the edge's own colour, and the beam keeps going.
	// That needs cells beyond the first and last column, and scanlines beyond the first and last line,
	// enough to cover the largest excursion of the picture. Black and wrap need none.
	int extraX = 0;
	int extraY = 0;
	if (clampEdge) {
		const float zoomSpread = std::max(std::abs(zoomHi - 1.f), std::abs(1.f - zoomLo));
		extraX = std::min(W, static_cast<int>(std::ceil(fw * (maxDx * zoomHi + zoomSpread))) + 2);
		extraY = std::min(N, static_cast<int>(std::ceil(static_cast<float>(N) * (maxDy * zoomHi + zoomSpread))) + 2);
		if (rotating) {
			// A rotation by theta swings the corners of the picture by up to R (sin theta + 1 - cos theta)
			// pixels (R is the half diagonal), past the edges in both directions. A cell is one pixel wide
			// and a scanline `pitch` pixels tall.
			const float halfDiagonal = 0.5f * std::sqrt(fw * fw + fh * fh);
			const float theta = std::min(1.5707963f, maxS * deflection_yoke::kSCoilRadians);
			const float reach = halfDiagonal * (std::sin(theta) + 1.f - std::cos(theta)) * zoomHi;
			extraX = std::min(W, extraX + static_cast<int>(std::ceil(reach)));
			extraY = std::min(N, extraY + static_cast<int>(std::ceil(reach / pitch)));
		}
		// A collapsed axis is a real smaller raster, not a picture to stretch past its edge.
		if (hSize < 1.f) {
			extraX = 0;
		}
		if (vSize < 1.f) {
			extraY = 0;
		}
	}

	// Colour of each scanline and cell: the average of the source rows its band covers, with the
	// flips applied. A raster with more lines than rows samples one row per line.
	wobbleColour.resize(static_cast<size_t>(N) * static_cast<size_t>(W) * 3u);  // every entry is written below
	if (source.isFloat()) {
		// Full precision: the same average over a float picture, summed in float (the level is not cut to a byte).
		std::vector<float> sums(static_cast<size_t>(W) * 3u);
		for (int k = 0; k < N; ++k) {
			const int band = params.flipV ? N - 1 - k : k;
			const int y0 = std::min(height - 1, band * height / N);
			const int y1 = std::max(y0 + 1, std::min(height, (band + 1) * height / N));
			std::fill(sums.begin(), sums.end(), 0.f);
			for (int y = y0; y < y1; ++y) {
				const float* row = &source.f[static_cast<size_t>(y) * static_cast<size_t>(W) * 4u];
				for (int x = 0; x < W; ++x) {
					float* s = &sums[static_cast<size_t>(x) * 3u];
					s[0] += row[x * 4 + 0];
					s[1] += row[x * 4 + 1];
					s[2] += row[x * 4 + 2];
				}
			}
			const float inv = 1.f / (255.f * static_cast<float>(y1 - y0));
			float* dst = wobbleColour.data() + static_cast<size_t>(k) * static_cast<size_t>(W) * 3u;
			for (int x = 0; x < W; ++x) {
				const int sx = params.flipH ? W - 1 - x : x;
				const float* s = &sums[static_cast<size_t>(sx) * 3u];
				dst[x * 3 + 0] = s[0] * inv;
				dst[x * 3 + 1] = s[1] * inv;
				dst[x * 3 + 2] = s[2] * inv;
			}
		}
	}
	else {
		std::vector<unsigned int> sums(static_cast<size_t>(W) * 3u);
		for (int k = 0; k < N; ++k) {
			const int band = params.flipV ? N - 1 - k : k;
			const int y0 = std::min(height - 1, band * height / N);
			const int y1 = std::max(y0 + 1, std::min(height, (band + 1) * height / N));
			std::fill(sums.begin(), sums.end(), 0u);
			for (int y = y0; y < y1; ++y) {
				const unsigned char* row = &source[static_cast<size_t>(y) * static_cast<size_t>(W) * 4u];
				for (int x = 0; x < W; ++x) {
					unsigned int* s = &sums[static_cast<size_t>(x) * 3u];
					s[0] += row[x * 4 + 0];
					s[1] += row[x * 4 + 1];
					s[2] += row[x * 4 + 2];
				}
			}
			const float inv = 1.f / (255.f * static_cast<float>(y1 - y0));
			float* dst = wobbleColour.data() + static_cast<size_t>(k) * static_cast<size_t>(W) * 3u;
			for (int x = 0; x < W; ++x) {
				const int sx = params.flipH ? W - 1 - x : x;
				const unsigned int* s = &sums[static_cast<size_t>(sx) * 3u];
				dst[x * 3 + 0] = static_cast<float>(s[0]) * inv;
				dst[x * 3 + 1] = static_cast<float>(s[1]) * inv;
				dst[x * 3 + 2] = static_cast<float>(s[2]) * inv;
			}
		}
	}

	// Phase 1: where the beam lands for each cell corner. Scanline s holds corners for x = -extraX
	// to W + extraX. Scanline s - extraY is the real line k, and the edge extension uses the timing
	// of the nearest real line.
	const int Ntot = N + 2 * extraY;
	const int Wc = W + 2 * extraX + 1;
	wobbleCorners.resize(static_cast<size_t>(Ntot) * static_cast<size_t>(Wc));
	wobbleBounds.resize(static_cast<size_t>(Ntot));

	// Coverage, for the brightness of a collapsed raster: a coarse grid over the screen, marked wherever a
	// stroke passes. Only kept when a size is below 1; a full-size raster never touches it.
	const int gridW = std::max(1, static_cast<int>(std::ceil(fw / kCollapseCellPx)));
	const int gridH = std::max(1, static_cast<int>(std::ceil(fh / kCollapseCellPx)));
	const size_t gridCells = static_cast<size_t>(gridW) * static_cast<size_t>(gridH);
	std::vector<unsigned char> occupancy;
	std::mutex occupancyMutex;
	if (scaled) {
		occupancy.assign(gridCells, 0);
	}
	auto markCell = [&](std::vector<unsigned char>& grid, float x, float y) {
		int cx = static_cast<int>(std::floor(x / kCollapseCellPx));
		int cy = static_cast<int>(std::floor(y / kCollapseCellPx));
		if (wrap) {
			cx = ((cx % gridW) + gridW) % gridW;
			cy = ((cy % gridH) + gridH) % gridH;
		}
		else if (cx < 0 || cx >= gridW || cy < 0 || cy >= gridH) {
			return;
		}
		grid[static_cast<size_t>(cy) * static_cast<size_t>(gridW) + static_cast<size_t>(cx)] = 1;
	};

	// The corner loop comes in two instantiations. A patch that uses neither the S coil nor a SIZE below 1
	// runs the one that holds only the original arithmetic, so such a patch renders exactly as it always
	// did, whatever the optimiser does with the general one.
	auto landRows = [&](auto general, int sBegin, int sEnd) {
		constexpr bool kGeneral = decltype(general)::value;
		std::vector<unsigned char> seen;
		if constexpr (kGeneral) {
			if (scaled) {
				seen.assign(gridCells, 0);
			}
		}
		for (int s = sBegin; s < sEnd; ++s) {
			const int k = s - extraY;
			const int kEdge = std::clamp(k, 0, N - 1);
			const float gy = (static_cast<float>(k) + 0.5f) / static_cast<float>(N);
			std::array<float, 2>* row = &wobbleCorners[static_cast<size_t>(s) * static_cast<size_t>(Wc)];
			float lo = std::numeric_limits<float>::max();
			float hi = -std::numeric_limits<float>::max();
			for (int c = 0; c < Wc; ++c) {
				const int x = c - extraX;
				const float gx0 = static_cast<float>(x) / fw;
				const float pos = static_cast<float>((static_cast<float>(kEdge) + gx0) * samplesPerLine) + fieldStart;
				const float dx = tableAt(xTable, pos);
				const float dy = tableAt(yTable, pos);
				const float zoom = std::clamp(1.f + tableAt(zTable, pos), kZoomMin, kZoomMax);

				// Static geometry on the landing position: twist and line curl, as before.
				float gx = gx0;
				float gyy = gy;
				if (twisted || curled) {
					const float cx0 = gx0 * 2.f - 1.f;
					const float cy0 = gy * 2.f - 1.f;
					if (twisted) {
						const float angle = -twist * depth * cy0 * 1.35f;
						const float ca = std::cos(angle);
						const float sa = std::sin(angle);
						gx = (cx0 * ca - cy0 * sa) * 0.5f + 0.5f;
						gyy = (cx0 * sa + cy0 * ca) * 0.5f + 0.5f;
					}
					else {
						gx = cx0 * 0.5f + 0.5f;
						gyy = cy0 * 0.5f + 0.5f;
					}
					gx -= depth * 0.08f * lineCurl * (cy0 * std::abs(cy0));
				}

				float px;
				float py;
				if constexpr (!kGeneral) {
					// Yoke zoom about the centre, then the beam displacement.
					px = fw * (0.5f + zoom * (gx - 0.5f + dx));
					py = fh * (0.5f + zoom * (gyy - 0.5f + dy));
				}
				else {
					// Total deflection from the centre, in pixels so a circle stays round: the normal scan
					// (scaled by SIZE) plus the H and V coils. The S coil rotates it, then the yoke zoom scales it.
					float ox = fw * ((gx - 0.5f) * hSize + dx);
					float oy = fh * ((gyy - 0.5f) * vSize + dy);
					if (rotating) {
						const float angle = tableAt(sTable, pos) * deflection_yoke::kSCoilRadians;
						const float ca = std::cos(angle);
						const float sa = std::sin(angle);
						const float rx = ox * ca - oy * sa;
						oy = ox * sa + oy * ca;
						ox = rx;
					}
					px = fw * 0.5f + zoom * ox;
					py = fh * 0.5f + zoom * oy;
				}
				row[c] = {px, py};
				lo = std::min(lo, py);
				hi = std::max(hi, py);
			}
			wobbleBounds[static_cast<size_t>(s)] = {lo, hi};
			if constexpr (kGeneral) {
				if (scaled) {
					// Mark the cells this scanline's strokes pass through. A long stroke is sampled along its
					// length, so a fast coil still counts the ground it covers.
					for (int c = 0; c + 1 < Wc; ++c) {
						const float ax = row[c][0];
						const float ay = row[c][1];
						const float bx = row[c + 1][0];
						const float by = row[c + 1][1];
						const float length = std::hypot(bx - ax, by - ay);
						const int steps = std::clamp(static_cast<int>(std::ceil(length / kCollapseCellPx)), 1, kCollapseMaxSteps);
						for (int i = 0; i < steps; ++i) {
							const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(steps);
							markCell(seen, ax + (bx - ax) * t, ay + (by - ay) * t);
						}
					}
				}
			}
		}
		if constexpr (kGeneral) {
			if (scaled) {
				std::lock_guard<std::mutex> lock(occupancyMutex);
				for (size_t i = 0; i < gridCells; ++i) {
					occupancy[i] |= seen[i];
				}
			}
		}
	};
	parallelRanges(Ntot, threads, [&](int sBegin, int sEnd) {
		if (rotating || scaled) {
			landRows(std::true_type {}, sBegin, sEnd);
		}
		else {
			landRows(std::false_type {}, sBegin, sEnd);
		}
	});

	// Collapse brightness. depositPath gives every cell the same energy whatever its path, so a raster
	// squeezed onto a line (or a point) stacks all its energy there and goes solid white. The share of the
	// screen the strokes actually cover gives the gain back: full compensation when a size is 0, none at 1,
	// continuous in between. A line is a few pixels wide in a 4 pixel cell, so it still reads bright.
	float coverageGain = 1.f;
	if (scaled) {
		size_t covered = 0;
		for (size_t i = 0; i < gridCells; ++i) {
			covered += occupancy[i];
		}
		const float coverage = static_cast<float>(covered) / static_cast<float>(gridCells);
		const float collapse = 1.f - std::min(hSize, vSize);
		coverageGain = (1.f + (coverage - 1.f) * collapse) * (1.f + (kCollapseBoost - 1.f) * collapse);
	}

	// Phase 2: deposit. Each thread owns a band of rows, so no two threads write one pixel. Every
	// thread visits the scanlines that reach its band and writes only those rows.
	const float spotFraction = std::clamp(finiteOr(params.spot, 0.2f), 0.05f, 2.f);
	const crt::BeamSpot spot(std::max(kMinSpotPx, spotFraction * pitch));
	float gain = kWobbleGain * pitch;  // energy per cell at rest (one pixel of path), per unit colour
	if (scaled) {
		gain *= coverageGain;
	}
	const float pad = static_cast<float>(spot.reach()) + 1.f;
	const float* colour = wobbleColour.data();
	const std::array<float, 2>* corners = wobbleCorners.data();
	const std::array<float, 2>* bounds = wobbleBounds.data();
	crt::EnergyField& energy = bs.energy;
	parallelRanges(height, threads > 1 ? threads * 4 : 1, [&](int bandY0, int bandY1) {
		const float y0 = static_cast<float>(bandY0);
		const float y1 = static_cast<float>(bandY1);
		for (int s = 0; s < Ntot; ++s) {
			const int k = s - extraY;
			const float lo = bounds[s][0];
			const float hi = bounds[s][1];
			// Wrap draws the line again at each whole-screen vertical offset that reaches this band.
			int kyLo = 0;
			int kyHi = 0;
			if (wrap) {
				kyLo = static_cast<int>(std::ceil((y0 - pad - hi) / fh));
				kyHi = static_cast<int>(std::ceil((y1 + pad - lo) / fh)) - 1;
			}
			else if (hi + pad < y0 || lo - pad >= y1) {
				continue;
			}
			for (int ky = kyLo; ky <= kyHi; ++ky) {
				const float ty = static_cast<float>(ky) * fh;
				for (int c = 0; c + 1 < Wc; ++c) {
					const int x = c - extraX;
					const float* rgb;
					if (clampEdge) {
						rgb = colour + (static_cast<size_t>(std::clamp(k, 0, N - 1)) * static_cast<size_t>(W) + static_cast<size_t>(std::clamp(x, 0, W - 1))) * 3u;
					}
					else {
						if (x < 0 || x >= W) {
							continue;
						}
						rgb = colour + (static_cast<size_t>(k) * static_cast<size_t>(W) + static_cast<size_t>(x)) * 3u;
					}
					if (rgb[0] <= 0.f && rgb[1] <= 0.f && rgb[2] <= 0.f) {
						continue;  // black adds no energy
					}
					const std::array<float, 2>& a = corners[static_cast<size_t>(s) * static_cast<size_t>(Wc) + static_cast<size_t>(c)];
					const std::array<float, 2>& b = corners[static_cast<size_t>(s) * static_cast<size_t>(Wc) + static_cast<size_t>(c) + 1u];
					if (!wrap) {
						// A stroke that misses this band adds nothing to it (the same test the wrap case makes below), so
						// it is not walked once per band.
						if (std::max(a[1], b[1]) + pad < y0 || std::min(a[1], b[1]) - pad >= y1) {
							continue;
						}
						crt::depositPath(energy, spot, a[0], a[1], b[0], b[1], rgb, gain, bandY0, bandY1);
						continue;
					}
					// A stroke that misses this band at this repeat adds nothing to it. Skipping the call here
					// costs nothing; depositPath would walk the whole path (up to 4096 steps) to find that out,
					// once per repeat, which is what made a wildly deflected wrap frame take minutes.
					const float minY = std::min(a[1], b[1]) + ty;
					const float maxY = std::max(a[1], b[1]) + ty;
					if (maxY + pad < y0 || minY - pad >= y1) {
						continue;
					}
					const float minX = std::min(a[0], b[0]);
					const float maxX = std::max(a[0], b[0]);
					const int kxLo = static_cast<int>(std::ceil((-pad - maxX) / fw));
					const int kxHi = static_cast<int>(std::ceil((fw + pad - minX) / fw)) - 1;
					for (int kx = kxLo; kx <= kxHi; ++kx) {
						const float tx = static_cast<float>(kx) * fw;
						crt::depositPath(energy, spot, a[0] + tx, a[1] + ty, b[0] + tx, b[1] + ty, rgb, gain, bandY0, bandY1);
					}
				}
			}
		}
	});

	// Glow and tone map: energy plus glow, through 1 - exp(-k E).
	bs.hasBeam = true;
	glow.compute(energy, std::clamp(finiteOr(params.glow, 0.f), 0.f, 2.f));
	toneMapOutput(energy, kWobbleToneK, out, width, height);
	bs.display.copyFrom(out);  // the picture only; a large one is copied by the pool
}

namespace {

// Phosphor and tone, as the DeflectionYoke (see docs/architecture.md, "Scan Processor").
constexpr float kScanProcessorPersistMax = 0.9f;
// Energy per scan column per unit of line pitch for a full-level beam. Spreads over the spot, so the
// picture's brightness is the same whatever the line count.
constexpr float kScanProcessorGain = 0.8f;
constexpr float kScanProcessorToneK = 1.6f;
// Spot sigma across the line, in units of the line pitch, with a floor in pixels.
constexpr float kScanProcessorSpot = 0.22f;
constexpr float kScanProcessorMinSpotPx = 0.3f;
// Green/white phosphor for the monochrome beam.
constexpr float kScanProcessorPhosphorR = 0.62f;
constexpr float kScanProcessorPhosphorG = 1.0f;
constexpr float kScanProcessorPhosphorB = 0.72f;

}  // namespace

void VideoGraphRenderer::renderScanProcessorFrame(int busId, Frame& source, Frame* liftSource, Frame& out, int width, int height, const ScanProcessorParams& params) {
	// Model (see docs/architecture.md, "Scan Processor"). Each scanline is one continuous beam stroke across the
	// source. For a point at ramp r along line k (line ramp L = (k + 0.5) / N):
	//   x = hPos + hWidth (r + DEFLECT X)
	//   y = vPos + vHeight (L + DEFLECT Y) - lumaGain x luma(r, L)
	// Luma is the source averaged over the rows of the line's band and bilinear in x, so there are no steps.
	// DEFLECT X, DEFLECT Y, BLANK and beam Z are read from the signal ring at each point's scan time, (k + r) x line time.
	const int lines = std::clamp(params.lines, 16, 2048);
	const bool haveSignal = signalRingReadField(params.signalSlotId, scanProcessorSignal, lines) && scanProcessorSignal.valid();

	BeamState& bs = beam[static_cast<size_t>(busId)];
	if (!bs.energy.matches(width, height)) {
		bs.energy.resize(width, height);
		bs.display.clear();
		bs.display.f.clear();
		bs.hasBeam = false;
	}
	if (bs.pass == passId && bs.display.hasPixels()) {
		out = bs.display;
		return;
	}
	bs.pass = passId;
	bs.energy.decay(kScanProcessorPersistMax * clamp01(params.persist));

	// Scan space: lines are rows and the sweep runs along columns. ROTATE 90 swaps the output axes at
	// the end, so the beam stroke and its spot are always drawn across rows.
	const bool rotate = params.rotate;
	const int SW = rotate ? height : width;
	const int SH = rotate ? width : height;
	const float fsw = static_cast<float>(SW);
	const float fsh = static_cast<float>(SH);
	const float pitch = fsh / static_cast<float>(lines);
	const crt::BeamSpot spot(std::max(kScanProcessorMinSpotPx, kScanProcessorSpot * pitch));
	const float gain = kScanProcessorGain * pitch * std::max(0.f, params.intensity);
	const float zi = clamp01(params.zIntensity);
	const bool colour = params.color;
	const float lumaGain = std::clamp(params.lumaGain, -1.f, 1.f);
	const float hPos = params.hPos;
	const float hWidth = params.hWidth;
	const float vPos = params.vPos;
	const float vHeight = params.vHeight;
	const int threads = render_pool::threadBudget();

	const int S = SW;      // one segment per scan column
	const int P = S + 1;   // points per line
	scanProcessorPoints.resize(static_cast<size_t>(lines) * static_cast<size_t>(P));
	scanProcessorBounds.resize(static_cast<size_t>(lines));
	const float lineSec = static_cast<float>(vclock::Clock(48000.0, lines).lineSeconds());  // the clock's line time; the rate does not enter it
	const float bandH = static_cast<float>(height) / static_cast<float>(lines);
	// LIFT IN picture, or null: then the main picture supplies the lift as well as the brightness and colour.
	// Either picture may be bytes or floats (0..255 units); the pixel readers below follow its format.
	const VideoFrame& pixels = source;
	const VideoFrame* liftPixels = (liftSource && liftSource->count() == source.count()) ? liftSource : nullptr;

	// Phase 1: points for each line. Each thread keeps its own row buffer.
	parallelRanges(lines, threads, [&](int kBegin, int kEnd) {
		std::vector<float> row(static_cast<size_t>(width) * 4u);  // R, G, B, luma per source column
		std::vector<float> liftRow(liftPixels ? static_cast<size_t>(width) * 4u : 0u);  // the LIFT IN picture, when patched
		// Fills one row buffer (R, G, B, luma) for line k from a picture.
		const auto fillRow = [&](const VideoFrame& frame, std::vector<float>& row, int k) {
		withSource(frame, [&](auto px) {
		// Source row for this line: an area average over its band, or bilinear when lines outnumber rows.
		if (bandH >= 1.f) {
			const int y0 = std::min(height - 1, static_cast<int>(std::floor(static_cast<float>(k) * bandH)));
			const int y1 = std::clamp(static_cast<int>(std::floor(static_cast<float>(k + 1) * bandH)), y0 + 1, height);
			std::fill(row.begin(), row.end(), 0.f);
			for (int y = y0; y < y1; ++y) {
				const size_t line = static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
				for (int x = 0; x < width; ++x) {
					float* d = &row[static_cast<size_t>(x) * 4u];
					d[0] += px[line + static_cast<size_t>(x) * 4u + 0u];
					d[1] += px[line + static_cast<size_t>(x) * 4u + 1u];
					d[2] += px[line + static_cast<size_t>(x) * 4u + 2u];
				}
			}
			const float inv = 1.f / (255.f * static_cast<float>(y1 - y0));
			for (int x = 0; x < width; ++x) {
				float* d = &row[static_cast<size_t>(x) * 4u];
				d[0] *= inv;
				d[1] *= inv;
				d[2] *= inv;
			}
		}
		else {
			const float fy = std::clamp((static_cast<float>(k) + 0.5f) * bandH - 0.5f, 0.f, static_cast<float>(height - 1));
			const int ya = static_cast<int>(fy);
			const int yb = std::min(ya + 1, height - 1);
			const float fr = fy - static_cast<float>(ya);
			const size_t la = static_cast<size_t>(ya) * static_cast<size_t>(width) * 4u;
			const size_t lb = static_cast<size_t>(yb) * static_cast<size_t>(width) * 4u;
			for (int x = 0; x < width; ++x) {
				float* d = &row[static_cast<size_t>(x) * 4u];
				for (int c = 0; c < 3; ++c) {
					d[c] = ((px[la + static_cast<size_t>(x) * 4u + static_cast<size_t>(c)] * (1.f - fr) + px[lb + static_cast<size_t>(x) * 4u + static_cast<size_t>(c)] * fr)) / 255.f;
				}
			}
		}
		for (int x = 0; x < width; ++x) {
			float* d = &row[static_cast<size_t>(x) * 4u];
			d[3] = 0.2126f * d[0] + 0.7152f * d[1] + 0.0722f * d[2];
		}
		});
		};
		for (int k = kBegin; k < kEnd; ++k) {
			fillRow(pixels, row, k);
			if (liftPixels) {
				fillRow(*liftPixels, liftRow, k);
			}
			const float L = (static_cast<float>(k) + 0.5f) / static_cast<float>(lines);
			ScanProcessorPoint* pts = &scanProcessorPoints[static_cast<size_t>(k) * static_cast<size_t>(P)];
			float lo = std::numeric_limits<float>::max();
			float hi = -std::numeric_limits<float>::max();
			for (int s = 0; s <= S; ++s) {
				const float r = static_cast<float>(s) / static_cast<float>(S);
				// Bilinear source sample at column r x width (pixel centres at x + 0.5).
				const float fx = std::clamp(r * static_cast<float>(width) - 0.5f, 0.f, static_cast<float>(width - 1));
				const int i0 = static_cast<int>(fx);
				const int i1 = std::min(i0 + 1, width - 1);
				const float fr = fx - static_cast<float>(i0);
				const float* a = &row[static_cast<size_t>(i0) * 4u];
				const float* b = &row[static_cast<size_t>(i1) * 4u];
				float v[4];
				for (int c = 0; c < 4; ++c) {
					v[c] = a[c] + (b[c] - a[c]) * fr;
				}
				// The lift comes from the LIFT IN picture when there is one; brightness and colour stay with v.
				float lift = v[3];
				if (liftPixels) {
					const float* la = &liftRow[static_cast<size_t>(i0) * 4u];
					const float* lb = &liftRow[static_cast<size_t>(i1) * 4u];
					lift = la[3] + (lb[3] - la[3]) * fr;
				}

				float dx = 0.f;
				float dy = 0.f;
				float blank = 0.f;
				float beamZ = 0.f;
				if (haveSignal) {
					const float t = (static_cast<float>(k) + r) * lineSec;
					dx = scanProcessorSignal.valueAtScanTime(kScanProcessorDeflectX, t);
					dy = scanProcessorSignal.valueAtScanTime(kScanProcessorDeflectY, t);
					blank = scanProcessorSignal.valueAtScanTime(kScanProcessorBlank, t) > 0.5f ? 1.f : 0.f;
					beamZ = scanProcessorSignal.valueAtScanTime(kScanProcessorZ, t);
				}

				const float xn = hPos + hWidth * (r + dx);
				const float yn = vPos + vHeight * (L + dy) - lumaGain * lift;
				ScanProcessorPoint& p = pts[s];
				p.x = xn * fsw;
				p.y = yn * fsh;
				// Z-INTENSITY: 1 + z (2 luma - 1), so a dark picture is dimmer and a bright one brighter.
				// Beam Z scales the whole beam: 0 leaves it alone, -1 turns it off, +1 doubles it.
				p.energy = blank > 0.5f ? 0.f : (1.f + zi * (2.f * v[3] - 1.f)) * std::max(0.f, 1.f + beamZ);
				if (colour) {
					p.r = v[0];
					p.g = v[1];
					p.b = v[2];
				}
				else {
					p.r = kScanProcessorPhosphorR;
					p.g = kScanProcessorPhosphorG;
					p.b = kScanProcessorPhosphorB;
				}
				lo = std::min(lo, p.y);
				hi = std::max(hi, p.y);
			}
			scanProcessorBounds[static_cast<size_t>(k)] = {lo, hi};
		}
	});

	// Phase 2: deposit. Each thread owns a band of scan rows, so no two threads write one pixel.
	scanField.resize(SW, SH);
	const int pad = spot.reach() + 1;
	const ScanProcessorPoint* points = scanProcessorPoints.data();
	const std::array<float, 2>* bounds = scanProcessorBounds.data();
	crt::EnergyField& field = scanField;
	parallelRanges(SH, threads > 1 ? threads * 4 : 1, [&](int bandY0, int bandY1) {
		const float y0 = static_cast<float>(bandY0);
		const float y1 = static_cast<float>(bandY1);
		for (int k = 0; k < lines; ++k) {
			const float lo = bounds[k][0];
			const float hi = bounds[k][1];
			if (hi + static_cast<float>(pad) < y0 || lo - static_cast<float>(pad) >= y1) {
				continue;
			}
			const ScanProcessorPoint* pts = points + static_cast<size_t>(k) * static_cast<size_t>(P);
			for (int s = 0; s < S; ++s) {
				const ScanProcessorPoint& a = pts[s];
				const ScanProcessorPoint& b = pts[s + 1];
				const float e = 0.5f * (a.energy + b.energy);
				if (e <= 0.f) {
					continue;  // blanked: no energy
				}
				// A stroke that misses this band adds nothing to it; do not walk it once per band.
				if (std::max(a.y, b.y) + static_cast<float>(pad) < y0 || std::min(a.y, b.y) - static_cast<float>(pad) >= y1) {
					continue;
				}
				const float rgb[3] = {0.5f * (a.r + b.r), 0.5f * (a.g + b.g), 0.5f * (a.b + b.b)};
				crt::depositPath(field, spot, a.x, a.y, b.x, b.y, rgb, gain * e, bandY0, bandY1);
			}
		}
	});

	// Add the scan into the node's energy. A rotated scan is transposed: scan row = output column.
	float* dst = bs.energy.data();
	const float* scan = scanField.data();
	// Every float of the energy gets exactly one add, so the pieces can go to the pool in any order.
	if (!rotate) {
		const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
		render_pool::parallelSpan(count, 3u * 32768u, [dst, scan](size_t begin, size_t end) {
			for (size_t i = begin; i < end; ++i) {
				dst[i] += scan[i];
			}
		});
	}
	else {
		// Bands of output rows. Inside a band the columns are the outer loop: a scan row is then read as one short
		// run (the band's rows are neighbours in it) instead of one float triple from each of `width` scan rows.
		render_pool::parallelSpan(static_cast<size_t>(height), 16u, [&](size_t rowBegin, size_t rowEnd) {
			for (int X = 0; X < width; ++X) {
				for (size_t Y = rowBegin; Y < rowEnd; ++Y) {
					const size_t d = (Y * static_cast<size_t>(width) + static_cast<size_t>(X)) * 3u;
					const size_t s = (static_cast<size_t>(X) * static_cast<size_t>(SW) + Y) * 3u;
					dst[d + 0] += scan[s + 0];
					dst[d + 1] += scan[s + 1];
					dst[d + 2] += scan[s + 2];
				}
			}
		});
	}

	bs.hasBeam = true;
	glow.compute(bs.energy, std::clamp(params.glow, 0.f, 2.f));
	toneMapOutput(bs.energy, kScanProcessorToneK, out, width, height);
	bs.display.copyFrom(out);  // the picture only; a large one is copied by the pool
}

void VideoGraphRenderer::renderLineFieldFrame(Frame& source, Frame& out, int width, int height, const LineFieldParams& params) {
	const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	const float mix = clamp01(params.mix);
	if (mix <= 0.0001f) {
		out.swap(source);
		return;
	}
	// Full precision: a float source is read as floats and the lines are drawn into a float picture (the line colours
	// are not cut to bytes, and the blends are not rounded). A byte source with the setting on is read the same way.
	const bool useFloat = passFloat || source.isFloat();
	if (useFloat) {
		source.ensureF32();
		out.beginF32(byteCount);
		std::fill(out.f.begin(), out.f.end(), 0.f);
		for (size_t i = 3; i < out.f.size(); i += 4u) {
			out.f[i] = 255.f;
		}
	}
	else {
		prepare(out, width, height);

		std::fill(out.begin(), out.end(), 0);
		for (size_t i = 3; i < out.size(); i += 4u) {
			out[i] = 255;
		}
	}
	const float* sf = useFloat ? source.f.data() : nullptr;
	// The same luma as lumaAt (clamped, Rec. 709), from either plane.
	auto lumaOf = [&](int x, int y) -> float {
		if (!sf) return lumaAt(source, width, height, x, y);
		if (x < 0 || y < 0 || x >= width || y >= height) return 0.f;
		const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
		return clamp01(sf[index + 0] / 255.f * 0.2126f + sf[index + 1] / 255.f * 0.7152f + sf[index + 2] / 255.f * 0.0722f);
	};
	auto sv = [&](size_t i) -> float { return sf ? sf[i] : static_cast<float>(source[i]); };

	const float depth = clamp01(params.depth) * 1.25f;
	const float xTilt = std::clamp(params.xTilt, -1.f, 1.f) * 0.95f;
	const float yTilt = std::clamp(params.yTilt, -1.f, 1.f) * 0.95f;
	const float zSpin = std::clamp(params.zSpin, -1.f, 1.f) * 3.14159265358979323846f;
	const float perspective = clamp01(params.perspective) * 1.85f;
	const float density = clamp01(params.lineDensity);
	const float gain = std::clamp(params.gain, 0.f, 2.f);
	const int lineCount = std::clamp(static_cast<int>(8.f + density * 56.f), 4, std::max(4, height));
	const int sampleStep = std::max(1, width / 128);
	const int thickness = density > 0.65f ? 1 : 2;

	const float sx = std::sin(xTilt);
	const float cx = std::cos(xTilt);
	const float sy = std::sin(yTilt);
	const float cy = std::cos(yTilt);
	const float sz = std::sin(zSpin);
	const float cz = std::cos(zSpin);
	const float imageScale = 0.78f;

	for (int line = 0; line < lineCount; ++line) {
		const float yNorm = lineCount > 1 ? static_cast<float>(line) / static_cast<float>(lineCount - 1) : 0.f;
		const int srcY = std::clamp(static_cast<int>(std::round(yNorm * static_cast<float>(height - 1))), 0, height - 1);
		bool hasPrevious = false;
		float previousX = 0.f;
		float previousY = 0.f;
		float previousR = 0.f;
		float previousG = 0.f;
		float previousB = 0.f;

		for (int srcX = 0; srcX < width; srcX += sampleStep) {
			const size_t srcIndex = (static_cast<size_t>(srcY) * static_cast<size_t>(width) + static_cast<size_t>(srcX)) * 4u;
			float luma = lumaOf(srcX, srcY);
			if (params.invert) {
				luma = 1.f - luma;
			}

			float px = width > 1 ? (static_cast<float>(srcX) / static_cast<float>(width - 1)) * 2.f - 1.f : 0.f;
			float py = yNorm * 2.f - 1.f;
			float pz = (luma - 0.5f) * depth;

			const float y1 = py * cx - pz * sx;
			const float z1 = py * sx + pz * cx;
			const float x2 = px * cy + z1 * sy;
			const float z2 = -px * sy + z1 * cy;
			const float x3 = x2 * cz - y1 * sz;
			const float y3 = x2 * sz + y1 * cz;
			const float z3 = z2;
			const float persp = 1.f / std::max(0.18f, 1.f + perspective * (z3 + 0.65f));
			const float screenX = (x3 * imageScale * persp * 0.5f + 0.5f) * static_cast<float>(width - 1);
			const float screenY = (y3 * imageScale * persp * 0.5f + 0.5f) * static_cast<float>(height - 1);

			const float shade = clamp01((0.25f + luma * 0.95f) * gain);
			// A byte picture's line colour is cut to a byte as it always was; a float picture's is not.
			auto lineColour = [&](size_t i) {
				const float v = std::clamp((sv(i) * 0.40f + 255.f * 0.60f) * shade, 0.f, 255.f);
				return sf ? v : static_cast<float>(static_cast<unsigned char>(v));
			};
			const float r = lineColour(srcIndex + 0);
			const float g = lineColour(srcIndex + 1);
			const float b = lineColour(srcIndex + 2);

			if (hasPrevious) {
				drawLine(out, width, height, previousX, previousY, screenX, screenY, previousR, previousG, previousB, 1.f, thickness);
			}
			hasPrevious = true;
			previousX = screenX;
			previousY = screenY;
			previousR = r;
			previousG = g;
			previousB = b;
		}
	}

	if (mix < 0.999f && sf) {
		for (size_t i = 0; i < byteCount; i += 4u) {
			for (size_t ch = 0; ch < 3u; ++ch) {
				out.f[i + ch] = std::clamp(sf[i + ch] * (1.f - mix) + out.f[i + ch] * mix, 0.f, 255.f);
			}
			out.f[i + 3] = 255.f;
		}
	}
	else if (mix < 0.999f) {
		for (size_t i = 0; i < byteCount; i += 4u) {
			out[i + 0] = static_cast<unsigned char>(std::clamp(source[i + 0] * (1.f - mix) + out[i + 0] * mix, 0.f, 255.f));
			out[i + 1] = static_cast<unsigned char>(std::clamp(source[i + 1] * (1.f - mix) + out[i + 1] * mix, 0.f, 255.f));
			out[i + 2] = static_cast<unsigned char>(std::clamp(source[i + 2] * (1.f - mix) + out[i + 2] * mix, 0.f, 255.f));
			out[i + 3] = 255;
		}
	}
}

void VideoGraphRenderer::drawLine(Frame& dst, int width, int height, float x0, float y0, float x1, float y1, float r, float g, float b, float alpha, int thickness) {
	const float dx = x1 - x0;
	const float dy = y1 - y0;
	const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::abs(dx), std::abs(dy)))));
	for (int step = 0; step <= steps; ++step) {
		const float t = static_cast<float>(step) / static_cast<float>(steps);
		const int px = static_cast<int>(std::round(x0 + dx * t));
		const int py = static_cast<int>(std::round(y0 + dy * t));
		for (int oy = -thickness + 1; oy < thickness; ++oy) {
			for (int ox = -thickness + 1; ox < thickness; ++ox) {
				const float falloff = 1.f - std::min(1.f, std::sqrt(static_cast<float>(ox * ox + oy * oy)) / static_cast<float>(std::max(1, thickness)));
				plotLinePixel(dst, width, height, px + ox, py + oy, r, g, b, alpha * std::max(0.28f, falloff));
			}
		}
	}
}

void VideoGraphRenderer::plotLinePixel(Frame& dst, int width, int height, int x, int y, float r, float g, float b, float alpha) {
	if (x < 0 || y < 0 || x >= width || y >= height || alpha <= 0.f) {
		return;
	}
	const float a = clamp01(alpha);
	const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
	if (dst.isFloat()) {
		// A float picture takes the blend unrounded.
		dst.f[index + 0] = std::clamp(dst.f[index + 0] * (1.f - a) + r * a, 0.f, 255.f);
		dst.f[index + 1] = std::clamp(dst.f[index + 1] * (1.f - a) + g * a, 0.f, 255.f);
		dst.f[index + 2] = std::clamp(dst.f[index + 2] * (1.f - a) + b * a, 0.f, 255.f);
		dst.f[index + 3] = 255.f;
		return;
	}
	dst[index + 0] = static_cast<unsigned char>(std::clamp(dst[index + 0] * (1.f - a) + r * a, 0.f, 255.f));
	dst[index + 1] = static_cast<unsigned char>(std::clamp(dst[index + 1] * (1.f - a) + g * a, 0.f, 255.f));
	dst[index + 2] = static_cast<unsigned char>(std::clamp(dst[index + 2] * (1.f - a) + b * a, 0.f, 255.f));
	dst[index + 3] = 255;
}

float VideoGraphRenderer::clamp01(float value) {
	return std::clamp(value, 0.f, 1.f);
}

float VideoGraphRenderer::fract(float value) {
	return value - std::floor(value);
}

float VideoGraphRenderer::lumaAt(const std::vector<unsigned char>& frame, int width, int height, int x, int y) {
	if (frame.empty() || x < 0 || y < 0 || x >= width || y >= height) {
		return 0.f;
	}
	const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
	const float r = frame[index + 0] / 255.f;
	const float g = frame[index + 1] / 255.f;
	const float b = frame[index + 2] / 255.f;
	return clamp01(r * 0.2126f + g * 0.7152f + b * 0.0722f);
}

void VideoGraphRenderer::hsvToRgb(float h, float s, float v, unsigned char& r, unsigned char& g, unsigned char& b) {
	float rf = 0.f, gf = 0.f, bf = 0.f;
	hsvToRgb255(h, s, v, rf, gf, bf);
	r = static_cast<unsigned char>(rf);
	g = static_cast<unsigned char>(gf);
	b = static_cast<unsigned char>(bf);
}

// The colour in 0..255 units, clamped, not yet a byte. hsvToRgb() truncates this, as it always did.
void VideoGraphRenderer::hsvToRgb255(float h, float s, float v, float& r, float& g, float& b) {
	h = fract(h);
	s = clamp01(s);
	v = clamp01(v);
	const float c = v * s;
	const float hp = h * 6.f;
	const float x = c * (1.f - std::abs(std::fmod(hp, 2.f) - 1.f));
	float rf = 0.f;
	float gf = 0.f;
	float bf = 0.f;
	if (hp < 1.f) {
		rf = c;
		gf = x;
	}
	else if (hp < 2.f) {
		rf = x;
		gf = c;
	}
	else if (hp < 3.f) {
		gf = c;
		bf = x;
	}
	else if (hp < 4.f) {
		gf = x;
		bf = c;
	}
	else if (hp < 5.f) {
		rf = x;
		bf = c;
	}
	else {
		rf = c;
		bf = x;
	}
	const float m = v - c;
	r = std::clamp((rf + m) * 255.f, 0.f, 255.f);
	g = std::clamp((gf + m) * 255.f, 0.f, 255.f);
	b = std::clamp((bf + m) * 255.f, 0.f, 255.f);
}

void VideoGraphRenderer::rgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
	r = clamp01(r); g = clamp01(g); b = clamp01(b);
	const float maxc = std::max(r, std::max(g, b));
	const float minc = std::min(r, std::min(g, b));
	const float delta = maxc - minc;
	v = maxc;
	s = maxc <= 0.f ? 0.f : delta / maxc;
	if (delta <= 0.000001f) h = 0.f;
	else if (maxc == r) h = fract((g - b) / delta / 6.f);
	else if (maxc == g) h = ((b - r) / delta + 2.f) / 6.f;
	else h = ((r - g) / delta + 4.f) / 6.f;
}

// Bilinear lookup of all four RGBA channels at once. Positions are in source pixels; the four
// neighbours are read once and blended, so a DeflectionYoke pixel costs one lookup, not three.
// Positions outside the frame return black.
void VideoGraphRenderer::sampleBilinear(const std::vector<unsigned char>& frame, int width, int height, float x, float y, unsigned char* out) {
	if (frame.empty() || x < 0.f || y < 0.f || x > static_cast<float>(width - 1) || y > static_cast<float>(height - 1)) {
		out[0] = out[1] = out[2] = out[3] = 0;
		return;
	}

	const int x0 = std::clamp(static_cast<int>(std::floor(x)), 0, width - 1);
	const int y0 = std::clamp(static_cast<int>(std::floor(y)), 0, height - 1);
	const int x1 = std::clamp(x0 + 1, 0, width - 1);
	const int y1 = std::clamp(y0 + 1, 0, height - 1);
	const float tx = x - static_cast<float>(x0);
	const float ty = y - static_cast<float>(y0);
	const float w00 = (1.f - tx) * (1.f - ty);
	const float w10 = tx * (1.f - ty);
	const float w01 = (1.f - tx) * ty;
	const float w11 = tx * ty;

	const size_t row0 = static_cast<size_t>(y0) * static_cast<size_t>(width);
	const size_t row1 = static_cast<size_t>(y1) * static_cast<size_t>(width);
	const size_t i00 = (row0 + static_cast<size_t>(x0)) * 4u;
	const size_t i10 = (row0 + static_cast<size_t>(x1)) * 4u;
	const size_t i01 = (row1 + static_cast<size_t>(x0)) * 4u;
	const size_t i11 = (row1 + static_cast<size_t>(x1)) * 4u;

	for (size_t c = 0; c < 4u; ++c) {
		const float v = frame[i00 + c] * w00 + frame[i10 + c] * w10 + frame[i01 + c] * w01 + frame[i11 + c] * w11;
		out[c] = static_cast<unsigned char>(std::clamp(v + 0.5f, 0.f, 255.f));
	}
}

// sampleBilinear on a float picture: the same four neighbours and weights, but the result is not rounded or clamped
// to a byte. Outside the frame it returns black.
void VideoGraphRenderer::sampleBilinearF(const std::vector<float>& frame, int width, int height, float x, float y, float* out) {
	if (frame.empty() || x < 0.f || y < 0.f || x > static_cast<float>(width - 1) || y > static_cast<float>(height - 1)) {
		out[0] = out[1] = out[2] = out[3] = 0.f;
		return;
	}
	const int x0 = std::clamp(static_cast<int>(std::floor(x)), 0, width - 1);
	const int y0 = std::clamp(static_cast<int>(std::floor(y)), 0, height - 1);
	const int x1 = std::clamp(x0 + 1, 0, width - 1);
	const int y1 = std::clamp(y0 + 1, 0, height - 1);
	const float tx = x - static_cast<float>(x0);
	const float ty = y - static_cast<float>(y0);
	const float w00 = (1.f - tx) * (1.f - ty);
	const float w10 = tx * (1.f - ty);
	const float w01 = (1.f - tx) * ty;
	const float w11 = tx * ty;
	const size_t row0 = static_cast<size_t>(y0) * static_cast<size_t>(width);
	const size_t row1 = static_cast<size_t>(y1) * static_cast<size_t>(width);
	const size_t i00 = (row0 + static_cast<size_t>(x0)) * 4u;
	const size_t i10 = (row0 + static_cast<size_t>(x1)) * 4u;
	const size_t i01 = (row1 + static_cast<size_t>(x0)) * 4u;
	const size_t i11 = (row1 + static_cast<size_t>(x1)) * 4u;
	for (size_t c = 0; c < 4u; ++c) {
		out[c] = frame[i00 + c] * w00 + frame[i10 + c] * w10 + frame[i01 + c] * w01 + frame[i11 + c] * w11;
	}
}

// The Colorizer's gradient, one colour for each of the 256 luma values. The Video Mixer's GRADIENT colouring calls
// this too, so the two cannot drift apart. Each stop has a hue, a level and its own saturation; the Colorizer gives
// every stop the same saturation, so its table is what it always was.
void VideoGraphRenderer::gradientTable(const float hueIn[4], const float levelIn[4], const float satIn[4], int bandsIn, float offsetIn, GradientTable& table) {
	GradientTableF exact;
	gradientTableF(hueIn, levelIn, satIn, bandsIn, offsetIn, exact);
	for (int n = 0; n < 256; ++n) {
		for (int c = 0; c < 3; ++c) table[n][c] = static_cast<unsigned char>(exact[n][c]);
	}
}

void VideoGraphRenderer::gradientTableF(const float hueIn[4], const float levelIn[4], const float satIn[4], int bandsIn, float offsetIn, GradientTableF& table) {
	// Hue interpolation and HSV conversion happen once per luma value, not per pixel.
	float hue[4], level[4], sat[4], delta[3];
	for (int i = 0; i < 4; ++i) {
		hue[i] = fract(hueIn[i]);
		level[i] = clamp01(levelIn[i]);
		sat[i] = clamp01(satIn[i]);
	}
	for (int i = 0; i < 3; ++i) delta[i] = fract(hue[i + 1] - hue[i] + 0.5f) - 0.5f;
	const int bands = std::clamp(bandsIn, 0, 8);
	const float offset = fract(offsetIn);
	for (int n = 0; n < 256; ++n) {
		// Luma runs from 0 to 1 inclusive. Only what the offset pushes past 1 wraps back to the bottom;
		// fract() would also send an exact 1 (grey 255 at offset 0) to 0, so white took black's colour.
		float l = n / 255.f + offset;
		if (l > 1.f) l -= 1.f;
		// The top band holds l = 1 too: floor(1 x bands) is bands, one past the last band.
		if (bands >= 2) l = (std::min(std::floor(l * bands), static_cast<float>(bands - 1)) + 0.5f) / bands;
		const int i = std::min(2, static_cast<int>(l * 3.f));
		const float t = l * 3.f - i;
		hsvToRgb255(fract(hue[i] + delta[i] * t), sat[i] + (sat[i + 1] - sat[i]) * t, level[i] + (level[i + 1] - level[i]) * t,
			table[n][0], table[n][1], table[n][2]);
	}
}

void VideoGraphRenderer::renderColorizerFrame(Frame& source, Frame& out, int width, int height, const ColorizerParams& params) {
	const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	beginOutput(out, byteCount);
	GradientTableF table;
	const float saturation = clamp01(params.saturation);
	const float sat[4] = {saturation, saturation, saturation, saturation};
	gradientTableF(params.hue, params.level, sat, params.bands, params.offset, table);
	// A float picture is coloured by the gradient at the exact luma (the two nearest entries blended) so the output
	// has no steps; with BANDS the gradient is steps by design and stays a lookup of the nearest entry.
	const bool smooth = params.bands < 2;
	withSource(source, [&](auto src) {
		withDest<StoreRule::Truncate>(out, [&](auto dst) {
			constexpr bool toFloat = std::is_same<decltype(dst), FloatDest>::value;
			// Rows are independent, so bands of rows go to the pool; every pixel is computed exactly as before.
			render_pool::parallelSpan(static_cast<size_t>(height), 8u, [&](size_t rowBegin, size_t rowEnd) {
				for (int y = static_cast<int>(rowBegin); y < static_cast<int>(rowEnd); ++y) {
					for (int x = 0; x < width; ++x) {
						const size_t i = (static_cast<size_t>(y) * width + x) * 4u;
						const float lumaValue = src[i] * 0.2126f + src[i + 1] * 0.7152f + src[i + 2] * 0.0722f;
						if (toFloat && smooth) {
							const float l = std::clamp(lumaValue, 0.f, 255.f);
							const int lo = std::min(static_cast<int>(l), 255);
							const int hi = std::min(lo + 1, 255);
							const float t = l - static_cast<float>(lo);
							dst.put(i, table[lo][0] + (table[hi][0] - table[lo][0]) * t);
							dst.put(i + 1, table[lo][1] + (table[hi][1] - table[lo][1]) * t);
							dst.put(i + 2, table[lo][2] + (table[hi][2] - table[lo][2]) * t);
						}
						else {
							const int luma = std::clamp(static_cast<int>(std::lround(lumaValue)), 0, 255);
							dst.put(i, table[luma][0]);
							dst.put(i + 1, table[luma][1]);
							dst.put(i + 2, table[luma][2]);
						}
						dst.put(i + 3, src[i + 3]);
					}
				}
			});
		});
	});
}

// With the fade option on, a stored level below this (in 0..255 units, so 1/1024 of one grey level) is stored as 0.
// It is a thousand times finer than the 8-bit step, so no picture can show it (the 8-bit output rounds anything
// under half a level to 0), and about thirty orders of magnitude above the smallest normal float, so a decaying
// trail ends at exact zero long before it could become a denormal.
constexpr float kFeedbackFadeSnapLevel = 1.f / 1024.f;

// Reads Feedback's newest stored picture into feedbackPrev, in the plane the pass needs. Full precision stores
// float-only entries (no byte plane); a byte pass that meets one rounds it half up, and a float pass that meets a byte
// entry widens it. With the setting off no entry is ever float-only, so the byte pass is unchanged.
bool VideoGraphRenderer::readFeedbackHistory(int slotId) {
	if (!frameRingRead(slotId, 0, feedbackPrev)) return false;
	if (passFloat) {
		if (feedbackPrev.rgbaF.empty()) feedbackPrev.rgbaF.assign(feedbackPrev.rgba.begin(), feedbackPrev.rgba.end());
	}
	else if (feedbackPrev.rgba.empty() && !feedbackPrev.rgbaF.empty()) {
		feedbackPrev.rgba.resize(feedbackPrev.rgbaF.size());
		floatToBytes(feedbackPrev.rgbaF.data(), feedbackPrev.rgba.data(), feedbackPrev.rgbaF.size());
	}
	return true;
}

// The stored picture as this node's output (another renderer already stored this tick's picture).
void VideoGraphRenderer::feedbackHistoryAsOutput(Frame& out, int width, int height) {
	if (passFloat) {
		out.beginF32(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
		if (feedbackPrev.width == width && feedbackPrev.height == height) out.f = feedbackPrev.rgbaF;
		else resampleIntoF(feedbackPrev.rgbaF, feedbackPrev.width, feedbackPrev.height, out.f, width, height);
		return;
	}
	if (feedbackPrev.width == width && feedbackPrev.height == height) static_cast<std::vector<unsigned char>&>(out) = feedbackPrev.rgba;
	else resampleInto(feedbackPrev.rgba, feedbackPrev.width, feedbackPrev.height, out, width, height);
}

// Feedback. With the plugin setting "Full precision between modules" on, the node reads a float source, mixes in
// float, writes a float picture and keeps a float-only history (as if "Fade trails fully to black" were on, but with
// no byte plane); that setting overrides the module option for the pass. With the plugin setting off, the module
// option means what it always did.
void VideoGraphRenderer::renderFeedbackFrame(Frame& src, Frame& out, int width, int height, const FeedbackParams& p) {
	frameRingClear(p.ringSlotId, p.clearCounter);
	if (passFloat) src.ensureF32();
	if (frameRingIsStored(p.ringSlotId, p.frameCounter)) {
		if (!readFeedbackHistory(p.ringSlotId)) {
			out.swap(src);
			return;
		}
		feedbackHistoryAsOutput(out, width, height);
		return;
	}
	const bool fade = p.fadeToBlack != 0 || passFloat;
	if (!fade) {
		// The float planes belong to the fade option; give their memory back when it is off.
		if (!feedbackPrevF.empty()) std::vector<float>().swap(feedbackPrevF);
		if (!feedbackResampledF.empty()) std::vector<float>().swap(feedbackResampledF);
		if (!feedbackOutF.empty()) std::vector<float>().swap(feedbackOutF);
		if (!feedbackPrev.rgbaF.empty()) std::vector<float>().swap(feedbackPrev.rgbaF);
	}
	if (!readFeedbackHistory(p.ringSlotId)) {
		out.swap(src);
		if (passFloat) {
			frameRingPushFloatOnly(p.ringSlotId, p.frameCounter, out.f, width, height);
		}
		else if (fade) {
			feedbackOutF.assign(out.begin(), out.end());
			frameRingPushFloat(p.ringSlotId, p.frameCounter, out, feedbackOutF, width, height);
		}
		else frameRingPush(p.ringSlotId, p.frameCounter, out, width, height);
		return;
	}
	const std::vector<unsigned char>* prev = nullptr;
	const std::vector<float>* prevF = nullptr;
	const bool sameSize = feedbackPrev.width == width && feedbackPrev.height == height;
	if (fade) {
		// The history is the float plane. A frame stored without one (the option was just switched on, or another
		// writer stored it) is read as its 8-bit picture.
		if (!passFloat && feedbackPrev.rgbaF.size() != feedbackPrev.rgba.size()) feedbackPrev.rgbaF.assign(feedbackPrev.rgba.begin(), feedbackPrev.rgba.end());
		prevF = &feedbackPrev.rgbaF;
		if (!sameSize) {
			resampleIntoF(feedbackPrev.rgbaF, feedbackPrev.width, feedbackPrev.height, feedbackResampledF, width, height);
			prevF = &feedbackResampledF;
		}
		if (!passFloat) feedbackOutF.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
	}
	else {
		prev = &feedbackPrev.rgba;
		if (!sameSize) {
			resampleInto(feedbackPrev.rgba, feedbackPrev.width, feedbackPrev.height, feedbackResampled, width, height);
			prev = &feedbackResampled;
		}
	}
	const InverseMap map = makeInverseMap(width, height, p.zoom, p.rotate, p.offsetX, p.offsetY,
		static_cast<InverseMapEdge>(p.edge));
	const float c = std::cos(p.hue * 2.f * kPi);
	const float s = std::sin(p.hue * 2.f * kPi);
	// Luma-preserving hue rotation: neutral greys stay neutral through every pass.
	const float hue[3][3] = {
		{0.299f + 0.701f*c + 0.168f*s, 0.587f - 0.587f*c + 0.330f*s, 0.114f - 0.114f*c - 0.498f*s},
		{0.299f - 0.299f*c - 0.328f*s, 0.587f + 0.413f*c + 0.035f*s, 0.114f - 0.114f*c + 0.293f*s},
		{0.299f - 0.299f*c + 1.250f*s, 0.587f - 0.587f*c - 1.050f*s, 0.114f + 0.886f*c - 0.200f*s}
	};
	const float gain = std::clamp(p.gain, 0.f, 1.2f);
	const float blend = std::min(gain, 1.f);
	const float key = clamp01(p.key);
	if (passFloat) out.beginF32(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
	else prepare(out, width, height);
	const float* srcF = passFloat ? src.f.data() : nullptr;
	float* outF = passFloat ? out.f.data() : nullptr;
	const int threads = render_pool::threadBudget();
	// The pixel loop is compiled twice, so the byte loop has no float tests in it (full precision is `kF`).
	auto pixels = [&](auto tag) {
	constexpr bool kF = decltype(tag)::value;
	parallelRanges(height, threads, [&](int begin, int end) {
		for (int y = begin; y < end; ++y) {
			for (int x = 0; x < width; ++x) {
				float sx = 0.f, sy = 0.f;
				unsigned char rgba[4] {};
				float px[4] {};
				if (fade) {
					if (inverseMapSource(map, width, height, static_cast<float>(x), static_cast<float>(y), sx, sy)) {
						if (p.edge == static_cast<int>(InverseMapEdge::Wrap) && (sx > width - 1 || sy > height - 1)) {
							const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
							const int x1 = (x0 + 1) % width, y1 = (y0 + 1) % height;
							const float tx = sx - x0, ty = sy - y0;
							for (int ch = 0; ch < 4; ++ch) {
								const float a = (*prevF)[(static_cast<size_t>(y0)*width+x0)*4+ch] * (1.f-tx) + (*prevF)[(static_cast<size_t>(y0)*width+x1)*4+ch] * tx;
								const float b = (*prevF)[(static_cast<size_t>(y1)*width+x0)*4+ch] * (1.f-tx) + (*prevF)[(static_cast<size_t>(y1)*width+x1)*4+ch] * tx;
								px[ch] = std::clamp(a*(1.f-ty)+b*ty, 0.f, 255.f);
							}
						}
						else sampleBilinearF(*prevF, width, height, sx, sy, px);
					}
				}
				else if (inverseMapSource(map, width, height, static_cast<float>(x), static_cast<float>(y), sx, sy)) {
					if (p.edge == static_cast<int>(InverseMapEdge::Wrap) && (sx > width - 1 || sy > height - 1)) {
						// Interpolate across the wrap seam instead of sampling outside the frame.
						const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
						const int x1 = (x0 + 1) % width, y1 = (y0 + 1) % height;
						const float tx = sx - x0, ty = sy - y0;
						for (int ch = 0; ch < 4; ++ch) {
							const float a = (*prev)[(static_cast<size_t>(y0)*width+x0)*4+ch] * (1.f-tx) + (*prev)[(static_cast<size_t>(y0)*width+x1)*4+ch] * tx;
							const float b = (*prev)[(static_cast<size_t>(y1)*width+x0)*4+ch] * (1.f-tx) + (*prev)[(static_cast<size_t>(y1)*width+x1)*4+ch] * tx;
							rgba[ch] = static_cast<unsigned char>(std::clamp(a*(1.f-ty)+b*ty+0.5f, 0.f, 255.f));
						}
					}
					else sampleBilinear(*prev, width, height, sx, sy, rgba);
				}
				if (!fade) {
					for (int ch = 0; ch < 4; ++ch) px[ch] = rgba[ch];
				}
				float fb[3];
				for (int ch = 0; ch < 3; ++ch) {
					fb[ch] = clamp01((hue[ch][0]*px[0] + hue[ch][1]*px[1] + hue[ch][2]*px[2]) / 255.f);
				}
				if (key > 0.001f) {
					const float luma = fb[0]*0.2126f + fb[1]*0.7152f + fb[2]*0.0722f;
					const float lo = std::max(0.f, key - 0.1f);
					const float t = clamp01((luma - lo) / (key - lo));
					const float k = t*t*(3.f - 2.f*t);
					for (float& v : fb) v *= k;
				}
				const size_t i = (static_cast<size_t>(y)*width+x)*4u;
				for (int ch = 0; ch < 3; ++ch) {
					const float input = (kF ? srcF[i+ch] : static_cast<float>(src[i+ch])) / 255.f;
					const float v = p.mode == 1 ? input*(1.f-blend) + fb[ch]*blend : input + gain*fb[ch];
					const float level = clamp01(v)*255.f;
					if constexpr (kF) {
						// Full precision: the picture is the float level, snapped like the history.
						outF[i+ch] = level < kFeedbackFadeSnapLevel ? 0.f : level;
						continue;
					}
					out[i+ch] = static_cast<unsigned char>(level + 0.5f);
					// The float history keeps the unrounded level, and snaps a vanishing one to exactly 0, so a
					// trail ends cleanly and never becomes a denormal.
					if (fade) feedbackOutF[i+ch] = level < kFeedbackFadeSnapLevel ? 0.f : level;
				}
				if constexpr (kF) {
					outF[i+3] = 255.f;
					continue;
				}
				out[i+3] = 255;
				if (fade) feedbackOutF[i+3] = 255.f;
			}
		}
	});
	};
	if (passFloat) pixels(std::true_type{});
	else pixels(std::false_type{});
	const bool stored = passFloat ? frameRingPushFloatOnly(p.ringSlotId, p.frameCounter, out.f, width, height)
		: fade ? frameRingPushFloat(p.ringSlotId, p.frameCounter, out, feedbackOutF, width, height)
		: frameRingPush(p.ringSlotId, p.frameCounter, out, width, height);
	if (!stored && readFeedbackHistory(p.ringSlotId)) {
		// A concurrent renderer may have won the write while this pass was being computed.
		feedbackHistoryAsOutput(out, width, height);
	}
}

// Focus (VRU-24): CONVERGE, then FOCUS and SMEAR, then SHARP, then MIX (see focus_filter.hpp). A control at its
// zero costs nothing; with every control at zero, or MIX at 0, the source is swapped into the output untouched.
void VideoGraphRenderer::renderFocusFrame(Frame& src, Frame& out, int width, int height, const FocusParams& params) {
	namespace ff = focus_filter;
	const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
	if (src.count() != pixels * 4u || width < 1 || height < 1) {
		out.swap(src);
		return;
	}
	const float mix = clamp01(params.mix);
	const float focusPx = ff::focusRadius(params.focus, width);
	const float smearPx = ff::smearLength(params.smear, width);
	const float shiftPx = ff::convergeShift(params.converge, width);
	const float sharp = clamp01(params.sharp);
	const bool doFocus = focusPx >= 0.05f;
	const bool doSmear = smearPx >= 0.05f;
	const bool doSharp = sharp > 0.001f;
	const bool doConverge = std::fabs(shiftPx) >= 0.01f;
	if (mix <= 0.0005f || !(doFocus || doSmear || doSharp || doConverge)) {
		out.swap(src);
		return;
	}
	// Full precision: the planes are float already, so the final cut to a byte is skipped and the picture is handed on
	// as floats. The input is read from whichever plane holds it.
	const bool floatOut = passFloat || src.isFloat();
	if (floatOut) out.beginF32(pixels * 4u);
	else prepare(out, width, height);

	// Scratch, sized only when the picture size changes. C is needed only for SHARP's blurred copy.
	const size_t planeCount = pixels;
	const int sets = doSharp ? 3 : 2;
	for (int s = 0; s < 3; ++s) {
		const size_t want = s < sets ? planeCount * 3u : 0u;
		if (focusPlanes[s].size() < want) {
			focusPlanes[s].resize(want);
		}
	}
	// The line pad is the largest window any pass here uses, plus its partial pixel.
	const float maxReach = std::max({focusPx * 0.5f, smearPx * 0.5f, sharp > 0.f ? std::max(1.f, ff::kSharpRadiusFraction * static_cast<float>(width)) : 0.f}) + 3.f;
	const size_t lineNeed = 3u * (static_cast<size_t>(width) + 2u * static_cast<size_t>(maxReach) + 8u);
	if (focusLine.size() < lineNeed) focusLine.resize(lineNeed);
	if (focusColumn.size() < static_cast<size_t>(width)) focusColumn.resize(static_cast<size_t>(width));
	if (focusRowA.size() < 3u * static_cast<size_t>(width)) focusRowA.resize(3u * static_cast<size_t>(width));
	if (focusRowB.size() < 3u * static_cast<size_t>(width)) focusRowB.resize(3u * static_cast<size_t>(width));
	float* A = focusPlanes[0].data();
	float* B = focusPlanes[1].data();
	float* C = doSharp ? focusPlanes[2].data() : nullptr;
	float* line = focusLine.data();
	float* column = focusColumn.data();

	// 1. Row by row, while the row is still in cache: load a channel (red slid right and blue slid left for
	// CONVERGE, linear between pixels), then FOCUS's sideways passes and SMEAR (sideways only), each two passes so
	// the kernel is a triangle. These sideways steps commute, so doing them per row is the same picture as doing
	// them on whole planes, with far less memory traffic.
	const float focusHalf = focusPx * 0.5f;  // two passes of this reach focusPx, the stated radius
	const ff::Window focusWin = ff::makeWindow(focusHalf, focusHalf);
	const float smearHalf = smearPx * 0.5f;  // two passes, so the tail is about the stated length
	const ff::Window smearWin = ff::makeWindow(smearHalf * ff::kSmearBehindShare, smearHalf * (1.f - ff::kSmearBehindShare));
	float* rowA[3] = {focusRowA.data(), focusRowA.data() + width, focusRowA.data() + 2 * static_cast<size_t>(width)};
	float* rowB[3] = {focusRowB.data(), focusRowB.data() + width, focusRowB.data() + 2 * static_cast<size_t>(width)};
	const float* inA[3] = {rowA[0], rowA[1], rowA[2]};
	const float* inB[3] = {rowB[0], rowB[1], rowB[2]};
	withSource(src, [&](auto rd) {
	for (int y = 0; y < height; ++y) {
		const size_t rowBase = static_cast<size_t>(y) * width * 4u;
		for (int c = 0; c < 3; ++c) {
			if (doConverge && c != 1) {
				const float shift = c == 0 ? -shiftPx : shiftPx;
				for (int x = 0; x < width; ++x) {
					const float at = std::clamp(static_cast<float>(x) + shift, 0.f, static_cast<float>(width - 1));
					const int i0 = static_cast<int>(at);
					const int i1 = std::min(i0 + 1, width - 1);
					const float f = at - static_cast<float>(i0);
					rowA[c][x] = rd[rowBase + i0 * 4 + c] * (1.f - f) + rd[rowBase + i1 * 4 + c] * f;
				}
			}
			else {
				for (int x = 0; x < width; ++x) {
					rowA[c][x] = rd[rowBase + x * 4 + c];
				}
			}
		}
		if (doFocus) {
			ff::boxRow3(inA, rowB, width, focusWin, line);
			ff::boxRow3(inB, rowA, width, focusWin, line);
		}
		if (doSmear) {
			ff::boxRow3(inA, rowB, width, smearWin, line);
			ff::boxRow3(inB, rowA, width, smearWin, line);
		}
		for (int c = 0; c < 3; ++c) {
			std::copy(rowA[c], rowA[c] + width, A + c * planeCount + static_cast<size_t>(y) * width);
		}
	}
	});

	// 2. FOCUS's up and down passes need whole planes.
	if (doFocus) {
		for (int c = 0; c < 3; ++c) {
			ff::twiceColumns(A + c * planeCount, B + c * planeCount, width, height, focusWin, column);
		}
	}

	// 3. SHARP: unsharp mask. C is a small blur of the picture; the difference is added back.
	if (doSharp) {
		const float r = std::max(1.f, ff::kSharpRadiusFraction * static_cast<float>(width)) * 0.5f;
		const ff::Window win = ff::makeWindow(r, r);
		for (int y = 0; y < height; ++y) {
			const size_t base = static_cast<size_t>(y) * width;
			const float* inRows[3] = {A + base, A + planeCount + base, A + 2 * planeCount + base};
			float* tmp[3] = {rowA[0], rowA[1], rowA[2]};
			float* dst[3] = {C + base, C + planeCount + base, C + 2 * planeCount + base};
			ff::boxRow3(inRows, tmp, width, win, line);
			ff::boxRow3(inA, dst, width, win, line);
		}
		for (int c = 0; c < 3; ++c) {
			ff::boxColumns(C + c * planeCount, B + c * planeCount, width, height, win, column);
			ff::boxColumns(B + c * planeCount, C + c * planeCount, width, height, win, column);
		}
	}

	// 4. Mix with the untouched source and write bytes (or floats, with the setting on).
	const float amount = sharp * ff::kSharpMaxAmount;
	withSource(src, [&](auto in) {
		withDest<StoreRule::RoundHalfUp>(out, [&](auto dst) {
			for (int y = 0; y < height; ++y) {
				const size_t base = static_cast<size_t>(y) * width;
				for (int ch = 0; ch < 3; ++ch) {
					const float* a = A + ch * planeCount + base;
					const float* d = doSharp ? C + ch * planeCount + base : nullptr;
					for (int x = 0; x < width; ++x) {
						float v = a[x];
						if (d) v += amount * (v - d[x]);
						const float dry = in[base * 4u + x * 4 + ch];
						v = dry + (v - dry) * mix;
						dst.put(base * 4u + x * 4 + ch, v);
					}
				}
				for (int x = 0; x < width; ++x) {
					dst.put(base * 4u + x * 4 + 3, 255.f);
				}
			}
		});
	});
}

void VideoGraphRenderer::renderRotatorFrame(Frame& src, Frame& out, int width, int height, const RotatorParams& p) {
	const int folds = std::clamp(p.folds, 1, 12);
	const bool identity = std::abs(p.angle) < 1e-7f && std::abs(p.zoom - 1.f) < 1e-7f && p.offsetX == 0.f
		&& p.offsetY == 0.f && folds == 1;
	if (identity || !src.hasPixels()) {
		out.swap(src);
		return;
	}
	// Full precision: sample the float picture and write floats, nothing rounded between passes. A byte source is
	// widened first (it is this node's own scratch copy).
	const bool useFloat = passFloat || src.isFloat();
	if (useFloat) src.ensureF32();
	static constexpr InverseMapEdge kEdges[4] = {InverseMapEdge::Black, InverseMapEdge::Clamp, InverseMapEdge::Wrap, InverseMapEdge::Mirror};
	const InverseMapEdge edge = kEdges[std::clamp(p.edge, 0, 3)];
	InverseMap map = makeInverseMap(width, height, std::clamp(p.zoom, 0.05f, 20.f), p.angle, p.offsetX, p.offsetY, edge);
	// A right angle is exact: cos and sin of a quarter turn come out as 4e-8, not 0, and that is enough to blur a
	// 90 degree turn by a hair. Snap them.
	const float quarters = p.angle * 4.f;
	if (std::abs(quarters - std::round(quarters)) < 1e-5f) {
		const int q = ((static_cast<int>(std::lround(quarters)) % 4) + 4) % 4;
		static constexpr float kCos[4] = {1.f, 0.f, -1.f, 0.f};
		static constexpr float kSin[4] = {0.f, 1.f, 0.f, -1.f};
		map.cosR = kCos[q];
		map.sinR = kSin[q];
	}
	// The map turns and zooms about its centre, so moving the centre moves the pivot. The picture's offset is
	// applied afterwards, so the pivot rides along with it.
	map.centreX += p.pivotX * width;
	map.centreY += p.pivotY * height;
	const float foldX = map.centreX + map.offsetX;
	const float foldY = map.centreY + map.offsetY;
	const float wedge = 2.f * kPi / static_cast<float>(folds);
	const int threads = render_pool::threadBudget();
	if (useFloat) {
		out.beginF32(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
		float* dst = out.f.data();
		parallelRanges(height, threads, [&](int begin, int end) {
			for (int y = begin; y < end; ++y) {
				for (int x = 0; x < width; ++x) {
					float px = static_cast<float>(x), py = static_cast<float>(y);
					if (folds > 1) {
						const float dx = px - foldX, dy = py - foldY;
						const float r = std::sqrt(dx * dx + dy * dy);
						float a = std::atan2(dy, dx);
						a -= std::floor(a / wedge) * wedge;
						if (a > wedge * 0.5f) a = wedge - a;
						px = foldX + r * std::cos(a);
						py = foldY + r * std::sin(a);
					}
					float sx = 0.f, sy = 0.f;
					float rgba[4] = {0.f, 0.f, 0.f, 255.f};
					if (inverseMapSource(map, width, height, px, py, sx, sy)) {
						if (edge == InverseMapEdge::Wrap && (sx > width - 1 || sy > height - 1)) {
							const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
							const int x1 = (x0 + 1) % width, y1 = (y0 + 1) % height;
							const float tx = sx - x0, ty = sy - y0;
							for (int ch = 0; ch < 4; ++ch) {
								const float a = src.f[(static_cast<size_t>(y0) * width + x0) * 4 + ch] * (1.f - tx) + src.f[(static_cast<size_t>(y0) * width + x1) * 4 + ch] * tx;
								const float b = src.f[(static_cast<size_t>(y1) * width + x0) * 4 + ch] * (1.f - tx) + src.f[(static_cast<size_t>(y1) * width + x1) * 4 + ch] * tx;
								rgba[ch] = std::clamp(a * (1.f - ty) + b * ty, 0.f, 255.f);
							}
						}
						else sampleBilinearF(src.f, width, height, sx, sy, rgba);
					}
					const size_t i = (static_cast<size_t>(y) * width + x) * 4u;
					dst[i] = rgba[0]; dst[i + 1] = rgba[1]; dst[i + 2] = rgba[2];
					dst[i + 3] = 255.f;
				}
			}
		});
		return;
	}
	prepare(out, width, height);
	parallelRanges(height, threads, [&](int begin, int end) {
		for (int y = begin; y < end; ++y) {
			for (int x = 0; x < width; ++x) {
				float px = static_cast<float>(x), py = static_cast<float>(y);
				if (folds > 1) {
					// Kaleidoscope: fold the screen angle about the pivot into one wedge and its mirror image.
					const float dx = px - foldX, dy = py - foldY;
					const float r = std::sqrt(dx * dx + dy * dy);
					float a = std::atan2(dy, dx);
					a -= std::floor(a / wedge) * wedge;
					if (a > wedge * 0.5f) a = wedge - a;
					px = foldX + r * std::cos(a);
					py = foldY + r * std::sin(a);
				}
				float sx = 0.f, sy = 0.f;
				unsigned char rgba[4] = {0, 0, 0, 255};
				if (inverseMapSource(map, width, height, px, py, sx, sy)) {
					if (edge == InverseMapEdge::Wrap && (sx > width - 1 || sy > height - 1)) {
						// Interpolate across the repeat seam instead of sampling outside the frame.
						const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
						const int x1 = (x0 + 1) % width, y1 = (y0 + 1) % height;
						const float tx = sx - x0, ty = sy - y0;
						for (int ch = 0; ch < 4; ++ch) {
							const float a = src[(static_cast<size_t>(y0) * width + x0) * 4 + ch] * (1.f - tx) + src[(static_cast<size_t>(y0) * width + x1) * 4 + ch] * tx;
							const float b = src[(static_cast<size_t>(y1) * width + x0) * 4 + ch] * (1.f - tx) + src[(static_cast<size_t>(y1) * width + x1) * 4 + ch] * tx;
							rgba[ch] = static_cast<unsigned char>(std::clamp(a * (1.f - ty) + b * ty + 0.5f, 0.f, 255.f));
						}
					}
					else sampleBilinear(src, width, height, sx, sy, rgba);
				}
				const size_t i = (static_cast<size_t>(y) * width + x) * 4u;
				out[i] = rgba[0]; out[i + 1] = rgba[1]; out[i + 2] = rgba[2];
				out[i + 3] = 255;
			}
		}
	});
}

VideoGraphRenderer::Status VideoGraphRenderer::renderVideoMixerNode(const VideoMixerParams& p, int width, int height, int depth, Frame& out) {
	const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	if (p.mode == 3) {
		// ALL reads every patched picture at a non-zero LEVEL (slots 0 to 3), the key (slot 4) and the matte (slot 5).
		// FADER, the next picture and the pattern are not read.
		const Frame* picture[4] = {nullptr, nullptr, nullptr, nullptr};
		for (int i = 0; i < 4; ++i) {
			if (p.inputBus[i] <= 0 || !(p.level[i] > 0.f)) {
				continue;
			}
			Frame& slot = scratch(depth, i);
			const Status status = renderNode(p.inputBus[i], width, height, depth + 1, slot);
			if (status == Status::Cycle || status == Status::Contended) {
				return status;
			}
			if (status == Status::Ok && slot.count() == byteCount) {
				picture[i] = &slot;
			}
		}
		const Frame* keyFrame = nullptr;
		const Frame* matteFrame = nullptr;
		if (p.keyOn && p.keyBus > 0) {
			Frame& slot = scratch(depth, 4);
			const Status status = renderNode(p.keyBus, width, height, depth + 1, slot);
			if (status == Status::Cycle || status == Status::Contended) {
				return status;
			}
			if (status == Status::Ok && slot.count() == byteCount) {
				keyFrame = &slot;
			}
		}
		if (p.keyOn && p.matteBus > 0) {
			Frame& slot = scratch(depth, 5);
			const Status status = renderNode(p.matteBus, width, height, depth + 1, slot);
			if (status == Status::Cycle || status == Status::Contended) {
				return status;
			}
			if (status == Status::Ok && slot.count() == byteCount) {
				matteFrame = &slot;
			}
		}
		renderVideoMixerFrame(nullptr, nullptr, nullptr, keyFrame, matteFrame, out, width, height, p, picture);
		return Status::Ok;
	}
	const int airIndex = std::clamp(p.onAir, 0, 3);
	const int nextIndex = std::clamp(p.next, 0, 3);
	const float fader = clamp01(p.fader);
	// The pictures this frame reads: 0 live, 1 next, 2 pattern, 3 key, 4 matte. A picture the frame does not
	// show is not rendered: the fader at an end reads one picture, the pattern only matters mid-wipe, and the key
	// layer reads nothing while it is off.
	const int bus[5] = {p.inputBus[airIndex], p.inputBus[nextIndex], p.patternBus, p.keyBus, p.matteBus};
	const bool needed[5] = {fader < 1.f, fader > 0.f, p.mode == 2 && fader > 0.f && fader < 1.f, p.keyOn, p.keyOn};
	const Frame* frame[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
	for (int role = 0; role < 5; ++role) {
		if (!needed[role] || bus[role] <= 0) {
			continue;
		}
		// The same bus in two roles (the next picture as its own pattern, one picture as key and matte) renders once.
		int same = -1;
		for (int earlier = 0; earlier < role; ++earlier) {
			if (needed[earlier] && bus[earlier] == bus[role]) {
				same = earlier;
				break;
			}
		}
		if (same >= 0) {
			frame[role] = frame[same];
			continue;
		}
		Frame& slot = scratch(depth, role);
		const Status status = renderNode(bus[role], width, height, depth + 1, slot);
		if (status == Status::Cycle || status == Status::Contended) {
			return status;
		}
		// An absent or empty picture reads as black (and an empty matte as "nothing in MATTE").
		if (status == Status::Ok && slot.count() == byteCount) {
			frame[role] = &slot;
		}
	}
	renderVideoMixerFrame(frame[0], frame[1], frame[2], frame[3], frame[4], out, width, height, p);
	return Status::Ok;
}

void VideoGraphRenderer::renderVideoMixerFrame(const Frame* onAir, const Frame* next, const Frame* pattern, const Frame* key, const Frame* matte, Frame& out, int width, int height, const VideoMixerParams& p, const Frame* const* pictures) {
	// Per pixel, in this order: colour each of the two pictures, mix them, lay the key over the mix, fade to black.
	// See docs/architecture.md, "Video Mixer", for the maths of each stage.
	beginOutput(out, static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
	const int airIndex = std::clamp(p.onAir, 0, 3);
	const int nextIndex = std::clamp(p.next, 0, 3);
	const float fader = clamp01(p.fader);
	const int mode = std::clamp(p.mode, 0, 2);
	const float soft = std::clamp(p.soft, 0.001f, 1.f);
	const float black = clamp01(p.black);
	const float sat = clamp01(p.sat);

	// Colour is one multiply for each coloured picture: its brightness times a tint, the hue pulled toward white
	// as SAT falls. Black stays black, white becomes the full tint, and at SAT 0 the tint is white (plain grey).
	struct Tint {
		bool on = false;
		bool gradient = false;
		float r = 1.f, g = 1.f, b = 1.f;
		GradientTable table;
		GradientTableF tableF;
	};
	auto tintOf = [&](int index) {
		Tint tint;
		if (p.gradientOn[index]) {
			// Black, the input's hue, the hue one SPAN further round, then white. Levels follow luma, so at SAT 0
			// the picture is its plain grey; SAT scales the three coloured stops and white keeps none.
			const float hue = p.hue[index];
			const float step = std::clamp(p.span, 0.f, 0.5f);
			const float hues[4] = {hue, hue, hue + step, hue + 2.f * step};
			const float levels[4] = {0.f, 1.f / 3.f, 2.f / 3.f, 1.f};
			const float sats[4] = {sat, sat, sat, 0.f};
			gradientTableF(hues, levels, sats, 0, 0.f, tint.tableF);
			for (int n = 0; n < 256; ++n) {
				for (int c = 0; c < 3; ++c) tint.table[n][c] = static_cast<unsigned char>(tint.tableF[n][c]);
			}
			tint.on = true;
			tint.gradient = true;
			return tint;
		}
		if (!p.colorOn[index]) {
			return tint;
		}
		unsigned char hr = 0, hg = 0, hb = 0;
		hsvToRgb(p.hue[index], 1.f, 1.f, hr, hg, hb);
		tint.on = true;
		tint.r = 1.f + (hr / 255.f - 1.f) * sat;
		tint.g = 1.f + (hg / 255.f - 1.f) * sat;
		tint.b = 1.f + (hb / 255.f - 1.f) * sat;
		return tint;
	};
	const Tint airTint = tintOf(airIndex);
	const Tint nextTint = tintOf(nextIndex);

	// ALL (mode 3): every picture that is patched and has a LEVEL above 0 is coloured as above, scaled by its LEVEL,
	// and added to the others. The sum is clamped at white, or divided by the number of patched pictures when
	// AVERAGE is set. The key and black then work as in the other modes; FADER does not reach this.
	if (p.mode == 3) {
		const Tint tints[4] = {tintOf(0), tintOf(1), tintOf(2), tintOf(3)};
		int connected = 0;
		for (int i = 0; i < 4; ++i) {
			connected += p.inputBus[i] > 0 ? 1 : 0;
		}
		const float scale = (p.average && connected > 0) ? 1.f / static_cast<float>(connected) : 1.f;
		const Frame* matteSource = matte != nullptr ? matte : key;
		const bool flatFill = p.keyFill == 1;
		const bool keyActive = p.keyOn && matteSource != nullptr && (flatFill || key != nullptr);
		const float keyThresh = clamp01(p.keyThresh);
		const float keySoft = std::clamp(p.keySoft, 0.001f, 1.f);
		unsigned char flatR = 0, flatG = 0, flatB = 0;
		hsvToRgb(p.keyHue, 0.9f, 1.f, flatR, flatG, flatB);
		const float flat[3] = {flatR / 255.f, flatG / 255.f, flatB / 255.f};
		const float black = clamp01(p.black);
		const bool toFloat = out.isFloat();

		bool has[4] = {false, false, false, false};
		MixedSource in[4] = {MixedSource{nullptr, nullptr}, MixedSource{nullptr, nullptr}, MixedSource{nullptr, nullptr}, MixedSource{nullptr, nullptr}};
		for (int i = 0; i < 4; ++i) {
			has[i] = pictures != nullptr && pictures[i] != nullptr && p.level[i] > 0.f;
			if (has[i]) {
				in[i] = mixedSourceOf(*pictures[i]);
			}
		}
		const MixedSource keyR = key != nullptr ? mixedSourceOf(*key) : MixedSource{nullptr, nullptr};
		const MixedSource matteR = matteSource != nullptr ? mixedSourceOf(*matteSource) : MixedSource{nullptr, nullptr};
		auto grey = [](const MixedSource& r, size_t j) {
			return (r[j + 0] * 0.2126f + r[j + 1] * 0.7152f + r[j + 2] * 0.0722f) * (1.f / 255.f);
		};
		// The same reading as the other modes: a gradient at the exact grey on a float picture, from its table on a byte one.
		auto load = [&](const MixedSource& r, size_t j, const Tint& tint, float* rgb) {
			if (tint.gradient) {
				if (toFloat) {
					const float l = std::clamp(grey(r, j) * 255.f, 0.f, 255.f);
					const int lo = std::min(static_cast<int>(l), 255);
					const int hi = std::min(lo + 1, 255);
					const float t = l - static_cast<float>(lo);
					rgb[0] = (tint.tableF[lo][0] + (tint.tableF[hi][0] - tint.tableF[lo][0]) * t) * (1.f / 255.f);
					rgb[1] = (tint.tableF[lo][1] + (tint.tableF[hi][1] - tint.tableF[lo][1]) * t) * (1.f / 255.f);
					rgb[2] = (tint.tableF[lo][2] + (tint.tableF[hi][2] - tint.tableF[lo][2]) * t) * (1.f / 255.f);
				}
				else {
					const int luma = std::clamp(static_cast<int>(std::lround(grey(r, j) * 255.f)), 0, 255);
					rgb[0] = tint.table[luma][0] * (1.f / 255.f);
					rgb[1] = tint.table[luma][1] * (1.f / 255.f);
					rgb[2] = tint.table[luma][2] * (1.f / 255.f);
				}
			}
			else if (tint.on) {
				const float l = grey(r, j);
				rgb[0] = l * tint.r;
				rgb[1] = l * tint.g;
				rgb[2] = l * tint.b;
			}
			else {
				rgb[0] = r[j + 0] * (1.f / 255.f);
				rgb[1] = r[j + 1] * (1.f / 255.f);
				rgb[2] = r[j + 2] * (1.f / 255.f);
			}
		};

		withDest<StoreRule::RoundHalfUp>(out, [&](auto dst) {
			const int threads = render_pool::threadBudget();
			parallelRanges(height, threads, [&](int begin, int end) {
				for (int y = begin; y < end; ++y) {
					for (int x = 0; x < width; ++x) {
						const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
						float v[3] = {0.f, 0.f, 0.f};
						for (int k = 0; k < 4; ++k) {
							if (!has[k]) {
								continue;
							}
							float rgb[3];
							load(in[k], i, tints[k], rgb);
							for (int c = 0; c < 3; ++c) {
								v[c] += rgb[c] * p.level[k];
							}
						}
						for (int c = 0; c < 3; ++c) {
							v[c] = clamp01(v[c] * scale);
						}
						if (keyActive) {
							float m = clamp01((grey(matteR, i) - keyThresh + keySoft * 0.5f) / keySoft);
							if (p.keyInvert) {
								m = 1.f - m;
							}
							for (int c = 0; c < 3; ++c) {
								const float fill = flatFill ? flat[c] : keyR[i + static_cast<size_t>(c)] * (1.f / 255.f);
								v[c] = v[c] * (1.f - m) + fill * m;
							}
						}
						if (black > 0.f) {
							for (int c = 0; c < 3; ++c) {
								v[c] *= 1.f - black;
							}
						}
						for (int c = 0; c < 3; ++c) {
							dst.putBiased(i + static_cast<size_t>(c), v[c] * 255.f + 0.5f);
						}
						dst.put(i + 3u, 255.f);
					}
				}
			});
		});
		return;
	}

	// Transition. A wipe's threshold runs from just above white (nothing has arrived) to just below black (all
	// of it has), with SOFT as the width of the edge in grey levels.
	const float addAir = std::min(1.f, 2.f * (1.f - fader));
	const float addNext = std::min(1.f, 2.f * fader);
	const float edge = (1.f + soft * 0.5f) + (-soft * 0.5f - (1.f + soft * 0.5f)) * fader;
	const float invSoft = 1.f / soft;

	// Key. The matte comes from MATTE when something is patched there, else from the KEY picture itself. A
	// layer with no matte source, or a picture fill with no KEY picture, does nothing.
	const Frame* matteSource = matte != nullptr ? matte : key;
	const bool flatFill = p.keyFill == 1;
	const bool keyActive = p.keyOn && matteSource != nullptr && (flatFill || key != nullptr);
	const float keyThresh = clamp01(p.keyThresh);
	const float keySoft = std::clamp(p.keySoft, 0.001f, 1.f);
	unsigned char flatR = 0, flatG = 0, flatB = 0;
	hsvToRgb(p.keyHue, 0.9f, 1.f, flatR, flatG, flatB);
	const float flat[3] = {flatR / 255.f, flatG / 255.f, flatB / 255.f};

	const bool showAir = onAir != nullptr && fader < 1.f;
	const bool showNext = next != nullptr && fader > 0.f;
	const bool havePattern = pattern != nullptr;
	const bool haveNext = next != nullptr;

	// One reader per input, one function for the whole loop. When every picture is bytes the readers are plain
	// pointers (the loop it always was); otherwise each read checks its picture's format.
	auto run = [&](auto airR, auto nextR, auto patternR, auto keyR, auto matteR) {
		withDest<StoreRule::RoundHalfUp>(out, [&](auto dst) {
			constexpr bool toFloat = std::is_same<decltype(dst), FloatDest>::value;
			auto grey = [](const auto& r, size_t j) {
				return (r[j + 0] * 0.2126f + r[j + 1] * 0.7152f + r[j + 2] * 0.0722f) * (1.f / 255.f);
			};
			auto load = [&](const auto& r, size_t j, const Tint& tint, float* rgb) {
				if (tint.gradient) {
					if (toFloat) {
						// The gradient at the exact grey, the two nearest entries blended, so there are no steps.
						const float l = std::clamp(grey(r, j) * 255.f, 0.f, 255.f);
						const int lo = std::min(static_cast<int>(l), 255);
						const int hi = std::min(lo + 1, 255);
						const float t = l - static_cast<float>(lo);
						rgb[0] = (tint.tableF[lo][0] + (tint.tableF[hi][0] - tint.tableF[lo][0]) * t) * (1.f / 255.f);
						rgb[1] = (tint.tableF[lo][1] + (tint.tableF[hi][1] - tint.tableF[lo][1]) * t) * (1.f / 255.f);
						rgb[2] = (tint.tableF[lo][2] + (tint.tableF[hi][2] - tint.tableF[lo][2]) * t) * (1.f / 255.f);
					}
					else {
						const int luma = std::clamp(static_cast<int>(std::lround(grey(r, j) * 255.f)), 0, 255);
						rgb[0] = tint.table[luma][0] * (1.f / 255.f);
						rgb[1] = tint.table[luma][1] * (1.f / 255.f);
						rgb[2] = tint.table[luma][2] * (1.f / 255.f);
					}
				}
				else if (tint.on) {
					const float l = grey(r, j);
					rgb[0] = l * tint.r;
					rgb[1] = l * tint.g;
					rgb[2] = l * tint.b;
				}
				else {
					rgb[0] = r[j + 0] * (1.f / 255.f);
					rgb[1] = r[j + 1] * (1.f / 255.f);
					rgb[2] = r[j + 2] * (1.f / 255.f);
				}
			};

			const int threads = render_pool::threadBudget();
			parallelRanges(height, threads, [&](int begin, int end) {
				for (int y = begin; y < end; ++y) {
					for (int x = 0; x < width; ++x) {
						const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
						float a[3] = {0.f, 0.f, 0.f};
						float n[3] = {0.f, 0.f, 0.f};
						if (showAir) load(airR, i, airTint, a);
						if (showNext) load(nextR, i, nextTint, n);
						float v[3];
						if (fader <= 0.f) {
							v[0] = a[0]; v[1] = a[1]; v[2] = a[2];
						}
						else if (fader >= 1.f) {
							v[0] = n[0]; v[1] = n[1]; v[2] = n[2];
						}
						else if (mode == 0) {
							for (int c = 0; c < 3; ++c) v[c] = a[c] * (1.f - fader) + n[c] * fader;
						}
						else if (mode == 1) {
							for (int c = 0; c < 3; ++c) v[c] = std::min(1.f, a[c] * addAir + n[c] * addNext);
						}
						else {
							// With no PATTERN the next picture's own greys, before it is coloured, shape the wipe.
							const float l = havePattern ? grey(patternR, i) : (haveNext ? grey(nextR, i) : 0.f);
							const float m = clamp01((l - edge) * invSoft + 0.5f);
							for (int c = 0; c < 3; ++c) v[c] = a[c] * (1.f - m) + n[c] * m;
						}
						if (keyActive) {
							float m = clamp01((grey(matteR, i) - keyThresh + keySoft * 0.5f) / keySoft);
							if (p.keyInvert) m = 1.f - m;
							for (int c = 0; c < 3; ++c) {
								const float fill = flatFill ? flat[c] : keyR[i + static_cast<size_t>(c)] * (1.f / 255.f);
								v[c] = v[c] * (1.f - m) + fill * m;
							}
						}
						if (black > 0.f) {
							for (int c = 0; c < 3; ++c) v[c] *= 1.f - black;
						}
						for (int c = 0; c < 3; ++c) {
							dst.putBiased(i + static_cast<size_t>(c), v[c] * 255.f + 0.5f);
						}
						dst.put(i + 3u, 255.f);
					}
				}
			});
		});
	};
	const Frame* all[5] = {onAir, next, pattern, key, matteSource};
	bool allBytes = true;
	for (const Frame* f : all) {
		if (f != nullptr && f->isFloat()) allBytes = false;
	}
	auto bytesOf = [](const Frame* f) { return ByteSource{f != nullptr ? f->data() : nullptr}; };
	auto mixedOf = [](const Frame* f) { return f != nullptr ? mixedSourceOf(*f) : MixedSource{nullptr, nullptr}; };
	if (allBytes) {
		run(bytesOf(onAir), bytesOf(next), bytesOf(pattern), bytesOf(key), bytesOf(matteSource));
	}
	else {
		run(mixedOf(onAir), mixedOf(next), mixedOf(pattern), mixedOf(key), mixedOf(matteSource));
	}
}
