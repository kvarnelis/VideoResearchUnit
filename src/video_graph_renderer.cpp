#include "video_graph_renderer.hpp"
#include "video_bus.hpp"
#include "video_external_frame.hpp"
#include "freeze_frame_store.hpp"
#include "signal_ring.hpp"
#include "inverse_map.hpp"
#include "cv_param.hpp"
#include "deflection_yoke_math.hpp"

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

VideoGraphRenderer::VideoGraphRenderer() : pool(static_cast<size_t>((kMaxDepth + 2) * kSlotsPerDepth)) {}

const VideoGraphRenderer::Frame& VideoGraphRenderer::renderBus(int busId, int width, int height) {
	signal = false;
	cycle = false;
	if (width <= 0 || height <= 0) {
		return none;
	}

	++passId;
	const Status status = renderNode(busId, width, height, 0, work);
	if (status == Status::Ok) {
		// Swap instead of copy: work becomes scratch for the next pass.
		lastGood.swap(work);
		lastGoodBus = busId;
		lastGoodWidth = width;
		lastGoodHeight = height;
		signal = true;
		return lastGood;
	}

	if (status == Status::Cycle) {
		cycle = true;
		if (busId == lastGoodBus && width == lastGoodWidth && height == lastGoodHeight && !lastGood.empty()) {
			signal = true;
			return lastGood;
		}
	}
	return none;
}

VideoGraphRenderer::Status VideoGraphRenderer::renderNode(int busId, int width, int height, int depth, Frame& out) {
	if (depth > kMaxDepth) {
		return Status::Cycle;
	}

	VideoBusNode node;
	if (!videoBusReadNode(busId, node)) {
		return Status::Empty;
	}

	if (onStack[static_cast<size_t>(busId)]) {
		return Status::Cycle;
	}

	onStack[static_cast<size_t>(busId)] = true;
	const Status status = renderNodeBody(busId, node, width, height, depth, out);
	onStack[static_cast<size_t>(busId)] = false;
	return status;
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
		const Frame& rendered = generatorPass[index] == passId ? engine.output() : engine.render(width, height, node.generator);
		generatorPass[index] = passId;
		out.assign(rendered.begin(), rendered.end());
		return rendered.empty() ? Status::Empty : Status::Ok;
	}

	case VideoBusNodeType::Ramps:
		renderRamps(out, width, height, node.ramps);
		return Status::Ok;

	case VideoBusNodeType::Shapes:
		renderShapes(out, width, height, node.shapes);
		return Status::Ok;

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
	case VideoBusNodeType::ScanProcessor:
	case VideoBusNodeType::LineField:
	case VideoBusNodeType::ColorLab:
	case VideoBusNodeType::Colorizer:
	case VideoBusNodeType::Keyer:
	case VideoBusNodeType::WaveShaper:
	case VideoBusNodeType::Feedback:
	case VideoBusNodeType::Rotator:
	case VideoBusNodeType::FreezeFrame: {
		// Single-input processors: the source renders into this depth's scratch frame.
		Frame& src = scratch(depth, 0);
		const Status child = renderNode(node.sourceBusId, width, height, depth + 1, src);
		if (child != Status::Ok) {
			return child;
		}
		switch (node.type) {
		case VideoBusNodeType::DeflectionYoke: renderDeflectionYokeFrame(busId, src, out, width, height, node.deflectionYoke); break;
		case VideoBusNodeType::ScanProcessor: renderScanProcessorFrame(busId, src, out, width, height, node.scanProcessor); break;
		case VideoBusNodeType::LineField: renderLineFieldFrame(src, out, width, height, node.lineField); break;
		case VideoBusNodeType::ColorLab: renderColorLabFrame(src, out, width, height, node.colorLab); break;
		case VideoBusNodeType::Colorizer: renderColorizerFrame(src, out, width, height, node.colorizer); break;
		case VideoBusNodeType::Keyer: renderKeyerFrame(src, out, width, height, node.keyer); break;
		case VideoBusNodeType::WaveShaper: renderWaveShaperFrame(src, out, width, height, node.waveShaper); break;
		case VideoBusNodeType::Feedback: renderFeedbackFrame(src, out, width, height, node.feedback); break;
		case VideoBusNodeType::Rotator: renderRotatorFrame(src, out, width, height, node.rotator); break;
		case VideoBusNodeType::FreezeFrame: renderFreezeFrame(src, out, width, height, node.freezeFrame); break;
		default: return Status::Empty;
		}
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
		if (bStatus == Status::Cycle) {
			return Status::Cycle;
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
			if (mStatus == Status::Cycle) {
				return Status::Cycle;
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

	default:
		return Status::Empty;
	}
}

void VideoGraphRenderer::resampleInto(const Frame& src, int srcWidth, int srcHeight, Frame& out, int width, int height) {
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

void VideoGraphRenderer::prepare(Frame& frame, int width, int height) {
	const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	if (frame.size() != byteCount) {
		frame.assign(byteCount, 0);
	}
}

void VideoGraphRenderer::renderColorLabFrame(Frame& source, Frame& out, int width, int height, const ColorLabParams& params) {
	const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	prepare(out, width, height);
	const float mix = clamp01(params.mix);
	const float hueShift = params.hue;
	const float sat = std::clamp(params.saturation, 0.f, 2.f);
	const float bright = std::clamp(params.brightness, 0.f, 2.f);
	const float contrast = std::clamp(params.contrast, 0.f, 2.f);
	const float posterize = clamp01(params.posterize);
	const float solarize = clamp01(params.solarize);
	const int steps = std::clamp(static_cast<int>(2.f + posterize * 30.f), 2, 32);
	for (size_t i = 0; i < byteCount; i += 4u) {
		float r = source[i + 0] / 255.f;
		float g = source[i + 1] / 255.f;
		float b = source[i + 2] / 255.f;
		if (params.channelMode == 1) std::swap(r, g);
		else if (params.channelMode == 2) { float t = r; r = g; g = b; b = t; }
		else if (params.channelMode == 3) { float t = r; r = b; b = g; g = t; }
		float h = 0.f, sv = 0.f, v = 0.f;
		rgbToHsv(r, g, b, h, sv, v);
		h = fract(h + hueShift);
		sv = clamp01(sv * sat);
		v = clamp01(v * bright);
		unsigned char rr = 0, gg = 0, bb = 0;
		hsvToRgb(h, sv, v, rr, gg, bb);
		r = rr / 255.f; g = gg / 255.f; b = bb / 255.f;
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
		out[i + 0] = static_cast<unsigned char>(std::clamp(source[i + 0] * (1.f - mix) + r * 255.f * mix, 0.f, 255.f));
		out[i + 1] = static_cast<unsigned char>(std::clamp(source[i + 1] * (1.f - mix) + g * 255.f * mix, 0.f, 255.f));
		out[i + 2] = static_cast<unsigned char>(std::clamp(source[i + 2] * (1.f - mix) + b * 255.f * mix, 0.f, 255.f));
		out[i + 3] = 255;
	}
}

void VideoGraphRenderer::renderKeyerFrame(Frame& source, Frame& out, int width, int height, const KeyerParams& params) {
	const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	prepare(out, width, height);
	const float threshold = clamp01(params.threshold);
	const float softness = std::clamp(params.softness, 0.001f, 1.f);
	const float gain = std::clamp(params.gain, 0.f, 2.f);
	const float mix = clamp01(params.mix);
	unsigned char kr = 0, kg = 0, kb = 0;
	hsvToRgb(params.hue, 0.9f, 1.f, kr, kg, kb);
	for (size_t i = 0; i < byteCount; i += 4u) {
		const float l = clamp01((source[i + 0] * 0.2126f + source[i + 1] * 0.7152f + source[i + 2] * 0.0722f) / 255.f);
		float matte = clamp01((l - threshold + softness * 0.5f) / softness) * gain;
		matte = clamp01(matte);
		if (params.invert) matte = 1.f - matte;
		float r = matte, g = matte, b = matte;
		if (params.mode == 1) { r = kr / 255.f * matte; g = kg / 255.f * matte; b = kb / 255.f * matte; }
		else if (params.mode == 2) { r = source[i + 0] / 255.f * matte; g = source[i + 1] / 255.f * matte; b = source[i + 2] / 255.f * matte; }
		out[i + 0] = static_cast<unsigned char>(std::clamp(source[i + 0] * (1.f - mix) + r * 255.f * mix, 0.f, 255.f));
		out[i + 1] = static_cast<unsigned char>(std::clamp(source[i + 1] * (1.f - mix) + g * 255.f * mix, 0.f, 255.f));
		out[i + 2] = static_cast<unsigned char>(std::clamp(source[i + 2] * (1.f - mix) + b * 255.f * mix, 0.f, 255.f));
		out[i + 3] = static_cast<unsigned char>(std::clamp(matte * 255.f, 0.f, 255.f));
	}
}

void VideoGraphRenderer::renderBusMixerFrame(const Frame& a, const Frame& b, const Frame* matte, Frame& out, int width, int height, const BusMixerParams& params) {
	const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	prepare(out, width, height);
	const float baseMix = clamp01(params.mix);
	const float matteAmount = clamp01(params.matteAmount);
	const int mode = std::clamp(params.mode, 0, 6);
	const bool hasMatte = matte != nullptr && matte->size() == byteCount;
	for (size_t i = 0; i < byteCount; i += 4u) {
		// Factor is how much of B shows through. Matte mode (6) keys on the MAT BUS luma.
		// Without a MAT BUS, Matte mode falls back to the MIX knob.
		float factor = baseMix;
		if (hasMatte) {
			float m = ((*matte)[i + 0] * 0.2126f + (*matte)[i + 1] * 0.7152f + (*matte)[i + 2] * 0.0722f) / 255.f;
			if (params.invertMatte) m = 1.f - m;
			if (mode == 6) {
				factor = clamp01(m);
			}
			else if (matteAmount > 0.0001f) {
				factor = clamp01(baseMix * (1.f - matteAmount) + m * matteAmount);
			}
		}
		for (int c = 0; c < 3; ++c) {
			const float av = a[i + c] / 255.f;
			const float bv = b[i + c] / 255.f;
			float v = bv;
			if (mode == 1) v = clamp01(av + bv);
			else if (mode == 2) v = av * bv;
			else if (mode == 3) v = 1.f - (1.f - av) * (1.f - bv);
			else if (mode == 4) v = std::abs(av - bv);
			else if (mode == 5) v = std::max(av, bv);
			out[i + c] = static_cast<unsigned char>(std::clamp((av * (1.f - factor) + v * factor) * 255.f, 0.f, 255.f));
		}
		out[i + 3] = 255;
	}
}

void VideoGraphRenderer::renderWaveShaperFrame(Frame& source, Frame& out, int width, int height, const WaveShaperParams& params) {
	const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	prepare(out, width, height);
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
	for (size_t i = 0; i < byteCount; i += 4u) {
		if (params.mode == 1) {
			const float l = shape((source[i + 0] * 0.2126f + source[i + 1] * 0.7152f + source[i + 2] * 0.0722f) / 255.f);
			for (int c = 0; c < 3; ++c) out[i + c] = static_cast<unsigned char>(std::clamp(source[i + c] * (1.f - mix) + l * 255.f * mix, 0.f, 255.f));
		}
		else {
			for (int c = 0; c < 3; ++c) {
				const float v = shape(source[i + c] / 255.f);
				out[i + c] = static_cast<unsigned char>(std::clamp(source[i + c] * (1.f - mix) + v * 255.f * mix, 0.f, 255.f));
			}
		}
		out[i + 3] = 255;
	}
}

void VideoGraphRenderer::renderFreezeFrame(Frame& source, Frame& out, int width, int height, const FreezeFrameParams& params) {
	// Clear first, then capture. A clear marks the current counter as handled, so the capture
	// that was cleared is not taken again.
	if (params.clear) {
		freezeFrameClear(params.freezeSlotId, params.captureCounter);
	}
	if (params.captureCounter != 0) {
		freezeFrameCaptureIfNew(params.freezeSlotId, params.captureCounter, source, width, height);
	}

	const float hold = clamp01(params.hold);
	const float blend = clamp01(params.blend);
	const float amount = clamp01(hold * blend);
	if (amount <= 0.0001f || !freezeFrameRead(params.freezeSlotId, frozenData) || frozenData.rgba.empty()) {
		out.swap(source);
		return;
	}

	const Frame* frozenFrame = &frozenData.rgba;
	if (frozenData.width != width || frozenData.height != height) {
		resampleInto(frozenData.rgba, frozenData.width, frozenData.height, frozenResampled, width, height);
		frozenFrame = &frozenResampled;
	}

	const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	prepare(out, width, height);
	for (size_t i = 0; i < byteCount; i += 4u) {
		out[i + 0] = static_cast<unsigned char>(std::clamp(source[i + 0] * (1.f - amount) + (*frozenFrame)[i + 0] * amount, 0.f, 255.f));
		out[i + 1] = static_cast<unsigned char>(std::clamp(source[i + 1] * (1.f - amount) + (*frozenFrame)[i + 1] * amount, 0.f, 255.f));
		out[i + 2] = static_cast<unsigned char>(std::clamp(source[i + 2] * (1.f - amount) + (*frozenFrame)[i + 2] * amount, 0.f, 255.f));
		out[i + 3] = 255;
	}
}

bool VideoGraphRenderer::renderScopeTraceFrame(int busId, int width, int height, int scopeTraceSlotId, const ScopeTraceParams& params, Frame& out) {
	// Persistence: this node's phosphor decays once per render pass and takes new trace energy.
	// If the same node is reached twice in one pass (e.g. both inputs of a Bus Mixer), it reuses
	// the frame from the first visit, so the trail advances once.
	BeamState& bs = beam[static_cast<size_t>(busId)];
	if (!bs.energy.matches(width, height)) {
		bs.energy.resize(width, height);
		bs.display.clear();
		bs.hasBeam = false;
	}
	if (bs.pass == passId && !bs.display.empty()) {
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
	prepare(out, width, height);
	glow.compute(bs.energy, std::clamp(params.bloom, 0.f, 2.f));
	crt::toneMap(bs.energy, glow.data(), kScopeToneK, out.data());

	const int gridLines = 4;
	for (int line = 1; line < gridLines; ++line) {
		const float x = static_cast<float>(line) * static_cast<float>(width - 1) / static_cast<float>(gridLines);
		const float y = static_cast<float>(line) * static_cast<float>(height - 1) / static_cast<float>(gridLines);
		drawLine(out, width, height, x, 0.f, x, static_cast<float>(height - 1), 24, 42, 32, 0.22f, 1);
		drawLine(out, width, height, 0.f, y, static_cast<float>(width - 1), y, 24, 42, 32, 0.22f, 1);
	}
	drawLine(out, width, height, 0.f, static_cast<float>(height - 1) * 0.5f, static_cast<float>(width - 1), static_cast<float>(height - 1) * 0.5f, 42, 62, 48, 0.24f, 1);
	drawLine(out, width, height, static_cast<float>(width - 1) * 0.5f, 0.f, static_cast<float>(width - 1) * 0.5f, static_cast<float>(height - 1), 42, 62, 48, 0.24f, 1);

	bs.display = out;
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
	std::vector<std::thread> pool;
	pool.reserve(static_cast<size_t>(threads));
	for (int t = 0; t < threads; ++t) {
		const int begin = count * t / threads;
		const int end = count * (t + 1) / threads;
		pool.emplace_back([&fn, begin, end] { fn(begin, end); });
	}
	for (std::thread& th : pool) {
		th.join();
	}
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
	if (!signalRingReadFrame(params.signalSlotId, wobbleSignal, crt::fieldSeconds(lines)) || !wobbleSignal.valid()) {
		out.swap(source);
		return;
	}

	BeamState& bs = beam[static_cast<size_t>(busId)];
	if (!bs.energy.matches(width, height)) {
		bs.energy.resize(width, height);
		bs.display.clear();
		bs.hasBeam = false;
	}
	if (bs.pass == passId && !bs.display.empty()) {
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
	const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);

	// Deflection and zoom over one field window. Table entry i is window sample i.
	const int fieldSamples = wobbleSignal.framePeriodSamples;
	const double samplesPerLine = static_cast<double>(fieldSamples) / (static_cast<double>(N) + crt::kBlankingLines);
	const int tableLength = fieldSamples + 2;
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
	{
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
				const float pos = static_cast<float>((static_cast<float>(kEdge) + gx0) * samplesPerLine);
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
	parallelRanges(height, threads, [&](int bandY0, int bandY1) {
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
	prepare(out, width, height);
	crt::toneMap(energy, glow.data(), kWobbleToneK, out.data());
	bs.display = out;
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

void VideoGraphRenderer::renderScanProcessorFrame(int busId, Frame& source, Frame& out, int width, int height, const ScanProcessorParams& params) {
	// Model (see docs/architecture.md, "Scan Processor"). Each scanline is one continuous beam stroke across the
	// source. For a point at ramp r along line k (line ramp L = (k + 0.5) / N):
	//   x = hPos + hWidth (r + DEFLECT X)
	//   y = vPos + vHeight (L + DEFLECT Y) - lumaGain x luma(r, L)
	// Luma is the source averaged over the rows of the line's band and bilinear in x, so there are no steps.
	// DEFLECT X, DEFLECT Y, BLANK and beam Z are read from the signal ring at each point's scan time, (k + r) x line time.
	const int lines = std::clamp(params.lines, 16, 2048);
	const bool haveSignal = signalRingReadFrame(params.signalSlotId, scanProcessorSignal, crt::fieldSeconds(lines)) && scanProcessorSignal.valid();

	BeamState& bs = beam[static_cast<size_t>(busId)];
	if (!bs.energy.matches(width, height)) {
		bs.energy.resize(width, height);
		bs.display.clear();
		bs.hasBeam = false;
	}
	if (bs.pass == passId && !bs.display.empty()) {
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
	const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);

	const int S = SW;      // one segment per scan column
	const int P = S + 1;   // points per line
	scanProcessorPoints.resize(static_cast<size_t>(lines) * static_cast<size_t>(P));
	scanProcessorBounds.resize(static_cast<size_t>(lines));
	const float lineSec = static_cast<float>(crt::lineSeconds());
	const float bandH = static_cast<float>(height) / static_cast<float>(lines);
	const unsigned char* pixels = source.data();

	// Phase 1: points for each line. Each thread keeps its own row buffer.
	parallelRanges(lines, threads, [&](int kBegin, int kEnd) {
		std::vector<float> row(static_cast<size_t>(width) * 4u);  // R, G, B, luma per source column
		for (int k = kBegin; k < kEnd; ++k) {
			// Source row for this line: an area average over its band, or bilinear when lines outnumber rows.
			if (bandH >= 1.f) {
				const int y0 = std::min(height - 1, static_cast<int>(std::floor(static_cast<float>(k) * bandH)));
				const int y1 = std::clamp(static_cast<int>(std::floor(static_cast<float>(k + 1) * bandH)), y0 + 1, height);
				std::fill(row.begin(), row.end(), 0.f);
				for (int y = y0; y < y1; ++y) {
					const unsigned char* line = pixels + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
					for (int x = 0; x < width; ++x) {
						float* d = &row[static_cast<size_t>(x) * 4u];
						d[0] += line[x * 4 + 0];
						d[1] += line[x * 4 + 1];
						d[2] += line[x * 4 + 2];
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
				const unsigned char* la = pixels + static_cast<size_t>(ya) * static_cast<size_t>(width) * 4u;
				const unsigned char* lb = pixels + static_cast<size_t>(yb) * static_cast<size_t>(width) * 4u;
				for (int x = 0; x < width; ++x) {
					float* d = &row[static_cast<size_t>(x) * 4u];
					for (int c = 0; c < 3; ++c) {
						d[c] = ((la[x * 4 + c] * (1.f - fr) + lb[x * 4 + c] * fr)) / 255.f;
					}
				}
			}
			for (int x = 0; x < width; ++x) {
				float* d = &row[static_cast<size_t>(x) * 4u];
				d[3] = 0.2126f * d[0] + 0.7152f * d[1] + 0.0722f * d[2];
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
				const float yn = vPos + vHeight * (L + dy) - lumaGain * v[3];
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
	parallelRanges(SH, threads, [&](int bandY0, int bandY1) {
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
				const float rgb[3] = {0.5f * (a.r + b.r), 0.5f * (a.g + b.g), 0.5f * (a.b + b.b)};
				crt::depositPath(field, spot, a.x, a.y, b.x, b.y, rgb, gain * e, bandY0, bandY1);
			}
		}
	});

	// Add the scan into the node's energy. A rotated scan is transposed: scan row = output column.
	float* dst = bs.energy.data();
	const float* scan = scanField.data();
	if (!rotate) {
		const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
		for (size_t i = 0; i < count; ++i) {
			dst[i] += scan[i];
		}
	}
	else {
		for (int Y = 0; Y < height; ++Y) {
			for (int X = 0; X < width; ++X) {
				const size_t d = (static_cast<size_t>(Y) * static_cast<size_t>(width) + static_cast<size_t>(X)) * 3u;
				const size_t s = (static_cast<size_t>(X) * static_cast<size_t>(SW) + static_cast<size_t>(Y)) * 3u;
				dst[d + 0] += scan[s + 0];
				dst[d + 1] += scan[s + 1];
				dst[d + 2] += scan[s + 2];
			}
		}
	}

	bs.hasBeam = true;
	glow.compute(bs.energy, std::clamp(params.glow, 0.f, 2.f));
	prepare(out, width, height);
	crt::toneMap(bs.energy, glow.data(), kScanProcessorToneK, out.data());
	bs.display = out;
}

void VideoGraphRenderer::renderLineFieldFrame(Frame& source, Frame& out, int width, int height, const LineFieldParams& params) {
	const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	const float mix = clamp01(params.mix);
	if (mix <= 0.0001f) {
		out.swap(source);
		return;
	}
	prepare(out, width, height);

	std::fill(out.begin(), out.end(), 0);
	for (size_t i = 3; i < out.size(); i += 4u) {
		out[i] = 255;
	}

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
		unsigned char previousR = 0;
		unsigned char previousG = 0;
		unsigned char previousB = 0;

		for (int srcX = 0; srcX < width; srcX += sampleStep) {
			const size_t srcIndex = (static_cast<size_t>(srcY) * static_cast<size_t>(width) + static_cast<size_t>(srcX)) * 4u;
			float luma = lumaAt(source, width, height, srcX, srcY);
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
			const unsigned char r = static_cast<unsigned char>(std::clamp((source[srcIndex + 0] * 0.40f + 255.f * 0.60f) * shade, 0.f, 255.f));
			const unsigned char g = static_cast<unsigned char>(std::clamp((source[srcIndex + 1] * 0.40f + 255.f * 0.60f) * shade, 0.f, 255.f));
			const unsigned char b = static_cast<unsigned char>(std::clamp((source[srcIndex + 2] * 0.40f + 255.f * 0.60f) * shade, 0.f, 255.f));

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

	if (mix < 0.999f) {
		for (size_t i = 0; i < byteCount; i += 4u) {
			out[i + 0] = static_cast<unsigned char>(std::clamp(source[i + 0] * (1.f - mix) + out[i + 0] * mix, 0.f, 255.f));
			out[i + 1] = static_cast<unsigned char>(std::clamp(source[i + 1] * (1.f - mix) + out[i + 1] * mix, 0.f, 255.f));
			out[i + 2] = static_cast<unsigned char>(std::clamp(source[i + 2] * (1.f - mix) + out[i + 2] * mix, 0.f, 255.f));
			out[i + 3] = 255;
		}
	}
}

void VideoGraphRenderer::drawLine(Frame& dst, int width, int height, float x0, float y0, float x1, float y1, unsigned char r, unsigned char g, unsigned char b, float alpha, int thickness) {
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

void VideoGraphRenderer::plotLinePixel(Frame& dst, int width, int height, int x, int y, unsigned char r, unsigned char g, unsigned char b, float alpha) {
	if (x < 0 || y < 0 || x >= width || y >= height || alpha <= 0.f) {
		return;
	}
	const float a = clamp01(alpha);
	const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
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

float VideoGraphRenderer::lumaAt(const Frame& frame, int width, int height, int x, int y) {
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
	r = static_cast<unsigned char>(std::clamp((rf + m) * 255.f, 0.f, 255.f));
	g = static_cast<unsigned char>(std::clamp((gf + m) * 255.f, 0.f, 255.f));
	b = static_cast<unsigned char>(std::clamp((bf + m) * 255.f, 0.f, 255.f));
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
void VideoGraphRenderer::sampleBilinear(const Frame& frame, int width, int height, float x, float y, unsigned char* out) {
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

void VideoGraphRenderer::renderColorizerFrame(Frame& source, Frame& out, int width, int height, const ColorizerParams& params) {
	prepare(out, width, height);
	// Hue interpolation and HSV conversion happen once per luma value, not per pixel.
	std::array<std::array<unsigned char, 3>, 256> table;
	float hue[4], level[4], delta[3];
	for (int i = 0; i < 4; ++i) {
		hue[i] = fract(params.hue[i]);
		level[i] = clamp01(params.level[i]);
	}
	for (int i = 0; i < 3; ++i) delta[i] = fract(hue[i + 1] - hue[i] + 0.5f) - 0.5f;
	const float saturation = clamp01(params.saturation);
	const int bands = std::clamp(params.bands, 0, 8);
	const float offset = fract(params.offset);
	for (int n = 0; n < 256; ++n) {
		// Luma runs from 0 to 1 inclusive. Only what the offset pushes past 1 wraps back to the bottom;
		// fract() would also send an exact 1 (grey 255 at offset 0) to 0, so white took black's colour.
		float l = n / 255.f + offset;
		if (l > 1.f) l -= 1.f;
		// The top band holds l = 1 too: floor(1 x bands) is bands, one past the last band.
		if (bands >= 2) l = (std::min(std::floor(l * bands), static_cast<float>(bands - 1)) + 0.5f) / bands;
		const int i = std::min(2, static_cast<int>(l * 3.f));
		const float t = l * 3.f - i;
		hsvToRgb(fract(hue[i] + delta[i] * t), saturation, level[i] + (level[i + 1] - level[i]) * t,
			table[n][0], table[n][1], table[n][2]);
	}
	const int threads = height >= 480 ? 4 : 1;
	parallelRanges(height, threads, [&](int begin, int end) {
		for (int y = begin; y < end; ++y) {
			for (int x = 0; x < width; ++x) {
				const size_t i = (static_cast<size_t>(y) * width + x) * 4u;
				const int luma = std::clamp(static_cast<int>(std::lround(source[i] * 0.2126f + source[i + 1] * 0.7152f + source[i + 2] * 0.0722f)), 0, 255);
				out[i] = table[luma][0];
				out[i + 1] = table[luma][1];
				out[i + 2] = table[luma][2];
				out[i + 3] = source[i + 3];
			}
		}
	});
}

void VideoGraphRenderer::renderFeedbackFrame(Frame& src, Frame& out, int width, int height, const FeedbackParams& p) {
	frameRingClear(p.ringSlotId, p.clearCounter);
	if (frameRingIsStored(p.ringSlotId, p.frameCounter)) {
		if (!frameRingRead(p.ringSlotId, 0, feedbackPrev)) {
			out.swap(src);
			return;
		}
		if (feedbackPrev.width == width && feedbackPrev.height == height) out = feedbackPrev.rgba;
		else resampleInto(feedbackPrev.rgba, feedbackPrev.width, feedbackPrev.height, out, width, height);
		return;
	}
	if (!frameRingRead(p.ringSlotId, 0, feedbackPrev)) {
		out.swap(src);
		frameRingPush(p.ringSlotId, p.frameCounter, out, width, height);
		return;
	}
	const Frame* prev = &feedbackPrev.rgba;
	if (feedbackPrev.width != width || feedbackPrev.height != height) {
		resampleInto(feedbackPrev.rgba, feedbackPrev.width, feedbackPrev.height, feedbackResampled, width, height);
		prev = &feedbackResampled;
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
	prepare(out, width, height);
	const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
	parallelRanges(height, threads, [&](int begin, int end) {
		for (int y = begin; y < end; ++y) {
			for (int x = 0; x < width; ++x) {
				float sx = 0.f, sy = 0.f;
				unsigned char rgba[4] {};
				if (inverseMapSource(map, width, height, static_cast<float>(x), static_cast<float>(y), sx, sy)) {
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
				float fb[3];
				for (int ch = 0; ch < 3; ++ch) {
					fb[ch] = clamp01((hue[ch][0]*rgba[0] + hue[ch][1]*rgba[1] + hue[ch][2]*rgba[2]) / 255.f);
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
					const float input = src[i+ch] / 255.f;
					const float v = p.mode == 1 ? input*(1.f-blend) + fb[ch]*blend : input + gain*fb[ch];
					out[i+ch] = static_cast<unsigned char>(clamp01(v)*255.f + 0.5f);
				}
				out[i+3] = 255;
			}
		}
	});
	if (!frameRingPush(p.ringSlotId, p.frameCounter, out, width, height) && frameRingRead(p.ringSlotId, 0, feedbackPrev)) {
		// A concurrent renderer may have won the write while this pass was being computed.
		if (feedbackPrev.width == width && feedbackPrev.height == height) out = feedbackPrev.rgba;
		else resampleInto(feedbackPrev.rgba, feedbackPrev.width, feedbackPrev.height, out, width, height);
	}
}

void VideoGraphRenderer::renderRotatorFrame(Frame& src, Frame& out, int width, int height, const RotatorParams& p) {
	const int folds = std::clamp(p.folds, 1, 12);
	const bool identity = std::abs(p.angle) < 1e-7f && std::abs(p.zoom - 1.f) < 1e-7f && p.offsetX == 0.f
		&& p.offsetY == 0.f && folds == 1;
	if (identity || src.empty()) {
		out.swap(src);
		return;
	}
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
	prepare(out, width, height);
	const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
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
		if (status == Status::Cycle) {
			return Status::Cycle;
		}
		// An absent or empty picture reads as black (and an empty matte as "nothing in MATTE").
		if (status == Status::Ok && slot.size() == byteCount) {
			frame[role] = &slot;
		}
	}
	renderVideoMixerFrame(frame[0], frame[1], frame[2], frame[3], frame[4], out, width, height, p);
	return Status::Ok;
}

void VideoGraphRenderer::renderVideoMixerFrame(const Frame* onAir, const Frame* next, const Frame* pattern, const Frame* key, const Frame* matte, Frame& out, int width, int height, const VideoMixerParams& p) {
	// Per pixel, in this order: colour each of the two pictures, mix them, lay the key over the mix, fade to black.
	// See docs/architecture.md, "Video Mixer", for the maths of each stage.
	prepare(out, width, height);
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
		float r = 1.f, g = 1.f, b = 1.f;
	};
	auto tintOf = [&](int index) {
		Tint tint;
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

	const unsigned char* airData = onAir != nullptr ? onAir->data() : nullptr;
	const unsigned char* nextData = next != nullptr ? next->data() : nullptr;
	const unsigned char* patternData = pattern != nullptr ? pattern->data() : nullptr;
	const unsigned char* keyData = key != nullptr ? key->data() : nullptr;
	const unsigned char* matteData = matteSource != nullptr ? matteSource->data() : nullptr;
	const bool showAir = airData != nullptr && fader < 1.f;
	const bool showNext = nextData != nullptr && fader > 0.f;

	auto grey = [](const unsigned char* px) {
		return (px[0] * 0.2126f + px[1] * 0.7152f + px[2] * 0.0722f) * (1.f / 255.f);
	};
	auto load = [&](const unsigned char* px, const Tint& tint, float* rgb) {
		if (tint.on) {
			const float l = grey(px);
			rgb[0] = l * tint.r;
			rgb[1] = l * tint.g;
			rgb[2] = l * tint.b;
		}
		else {
			rgb[0] = px[0] * (1.f / 255.f);
			rgb[1] = px[1] * (1.f / 255.f);
			rgb[2] = px[2] * (1.f / 255.f);
		}
	};

	const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
	parallelRanges(height, threads, [&](int begin, int end) {
		for (int y = begin; y < end; ++y) {
			for (int x = 0; x < width; ++x) {
				const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
				float a[3] = {0.f, 0.f, 0.f};
				float n[3] = {0.f, 0.f, 0.f};
				if (showAir) load(airData + i, airTint, a);
				if (showNext) load(nextData + i, nextTint, n);
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
					const float l = patternData != nullptr ? grey(patternData + i) : (nextData != nullptr ? grey(nextData + i) : 0.f);
					const float m = clamp01((l - edge) * invSoft + 0.5f);
					for (int c = 0; c < 3; ++c) v[c] = a[c] * (1.f - m) + n[c] * m;
				}
				if (keyActive) {
					float m = clamp01((grey(matteData + i) - keyThresh + keySoft * 0.5f) / keySoft);
					if (p.keyInvert) m = 1.f - m;
					for (int c = 0; c < 3; ++c) {
						const float fill = flatFill ? flat[c] : keyData[i + static_cast<size_t>(c)] * (1.f / 255.f);
						v[c] = v[c] * (1.f - m) + fill * m;
					}
				}
				if (black > 0.f) {
					for (int c = 0; c < 3; ++c) v[c] *= 1.f - black;
				}
				for (int c = 0; c < 3; ++c) {
					out[i + static_cast<size_t>(c)] = static_cast<unsigned char>(std::clamp(v[c] * 255.f + 0.5f, 0.f, 255.f));
				}
				out[i + 3u] = 255;
			}
		}
	});
}
