#pragma once

#include "video_bus.hpp"
#include "video_engine.hpp"
#include "raster_scan.hpp"
#include "freeze_frame_store.hpp"
#include "frame_ring.hpp"
#include "video_external_frame.hpp"
#include "scope_timebase.hpp"
#include "signal_ring.hpp"
#include "crt_beam.hpp"
#include "video_frame.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

// Renders a bus graph to RGBA on the calling (UI) thread. See docs/architecture.md,
// "Renderer recursion and cycles".
//
// renderBus() walks the graph from the requested bus down to its generators. Each level
// of the walk writes its inputs into scratch frames owned by that depth, so a nested render
// never overwrites a frame that a parent still needs. Results are swapped, not copied.
//
// A Frame Delay (VRU-27) is a leaf of the walk: it returns a stored past picture of its source and does not walk
// into the source, so a loop that goes through one is not a cycle and does not count toward kMaxDepth. Its source
// is drawn separately, at depth 0, after the root of the pass (see drawDelaySources()), and stored for later ticks.
//
// A cycle, or a graph deeper than kMaxDepth, is not rendered. renderBus() then returns the
// last good frame for the same bus and size, and hasCycle() reports true.
class VideoGraphRenderer {
public:
	using Frame = VideoFrame;
	// Lets tests/bus_test.cpp call the private Scope Trace window reader directly.
	friend struct VideoGraphRendererTestAccess;

	VideoGraphRenderer();

	enum class Status { Ok, Empty, Cycle };

	// One picture of one root bus. It is beginPass() followed by renderBusInPass(), and the returned frame stays
	// valid until the next call. It is empty when nothing renders.
	const Frame& renderBus(int busId, int width, int height);

	// Several root buses in ONE pass (see src/render_service.hpp). beginPass() starts a pass: stateful nodes
	// (generators, Feedback, Freeze Frame, Scope Trace, the CRT beam nodes) advance at most once in it, however many
	// roots reach them. renderBusInPass() renders one root into `out` (a swap, so `out` takes the picture and its old
	// storage becomes scratch) and returns how it went; `out` is left alone unless the result is Ok. Every root of
	// the pass is also kept as a finished tap, so a later root that reads it copies it instead of drawing it again.
	// hasSignal(), hasCycle() and wasContended() describe the last root. The last-good hold is the caller's job.
	void beginPass();
	// Full precision between modules (docs/architecture.md, "Precision"). Off (the default): every picture a node
	// hands on is 8-bit, as always. On: the nodes that have been converted hand on float pictures and round to 8 bits
	// only where a picture leaves the graph. The setting is read once, at beginPass() (renderBus() calls it), so a
	// pass is never half one and half the other.
	void setFullPrecision(bool on) {
		fullPrecision.store(on, std::memory_order_relaxed);
	}
	bool fullPrecisionSetting() const {
		return fullPrecision.load(std::memory_order_relaxed);
	}
	// `keepFloatPlane`: when full precision is on and the root picture is a float picture, `out` also gets
	// `out.precise`, the float plane the bytes were rounded from (0..255 units). The bytes in `out` are the same
	// either way, so every byte consumer gets exactly the picture it always got. In every other case
	// `out.precise` is left empty (cleared; the caller frees its capacity if it wants the memory back), and a float reader falls back to the bytes.
	Status renderBusInPass(int busId, int width, int height, Frame& out, bool keepFloatPlane = false);
	// A bus read gave up in the last root because the writer kept interrupting it; the caller should hold its last picture.
	bool wasContended() const {
		return readContended;
	}
	bool hasSignal() const {
		return signal;
	}
	bool hasCycle() const {
		return cycle;
	}
	// How many times the last renderBus() rendered this bus's node (0 when it was not reached, 1 when it was,
	// however many inputs read it). A debug counter for tests/bus_test.cpp.
	int busRenderCount(int busId) const {
		return busId >= 0 && busId <= kVideoBusCount ? renderCounts[static_cast<size_t>(busId)] : 0;
	}

private:
	static constexpr int kMaxDepth = 16;
	// Scratch frames per recursion depth. Bus Mixer: 0 = source, 1 = bus B, 2 = matte. Video Mixer: 0 = live,
	// 1 = next, 2 = pattern, 3 = key, 4 = matte. Video Mixer ALL: 0 to 3 = pictures A to D, 4 = key, 5 = matte.
	static constexpr int kSlotsPerDepth = 6;

	Status renderNode(int busId, int width, int height, int depth, Frame& out);
	// Picture as modulation: draws the modulation bus at this node's size and fills modLuma (see the definition).
	Status renderModulation(int ownBusId, int modBusId, float amount, int width, int height, int depth, int slot, const float*& luma);
	Status renderNodeBody(int busId, const VideoBusNode& node, int width, int height, int depth, Frame& out);
	// Frame Delay: the stored picture `frames` ticks old, resampled to this size, or black when none is that old.
	void renderFrameDelayLeaf(const FrameDelayParams& params, Frame& out, int width, int height);
	// After a root has rendered: draws the source of every Frame Delay the pass met and stores it (once per tick).
	void drawDelaySources(int width, int height);
	Frame& scratch(int depth, int index) {
		return pool[static_cast<size_t>(depth * kSlotsPerDepth + index)];
	}

	bool renderScopeTraceFrame(int busId, int width, int height, int scopeTraceSlotId, const ScopeTraceParams& params, Frame& out);
	// Fills out with the window to draw. Oscilloscope and Dual start it at a trigger (see scopeFindTrigger).
	bool readScopeWindow(int scopeTraceSlotId, const ScopeTraceParams& params, SignalWindow& out);
	void depositScopeTrace(crt::EnergyField& energy, int width, int height, const ScopeTraceParams& params, const SignalWindow& window);
	void renderDeflectionYokeFrame(int busId, Frame& src, Frame& out, int width, int height, const DeflectionYokeParams& params);
	void renderScanProcessorFrame(int busId, Frame& src, Frame* liftSrc, Frame& out, int width, int height, const ScanProcessorParams& params);
	void renderLineFieldFrame(Frame& src, Frame& out, int width, int height, const LineFieldParams& params);
	void renderColorizerFrame(Frame& src, Frame& out, int width, int height, const ColorizerParams& params);
	void renderColorLabFrame(Frame& src, Frame& out, int width, int height, const ColorLabParams& params, const float* modLuma = nullptr);
	void renderKeyerFrame(Frame& src, Frame& out, int width, int height, const KeyerParams& params, const float* modLuma = nullptr);
	void renderBusMixerFrame(const Frame& a, const Frame& b, const Frame* matte, Frame& out, int width, int height, const BusMixerParams& params);
	void renderWaveShaperFrame(Frame& src, Frame& out, int width, int height, const WaveShaperParams& params);
	void renderFreezeFrame(Frame& src, Frame& out, int width, int height, const FreezeFrameParams& params);
	void renderFeedbackFrame(Frame& src, Frame& out, int width, int height, const FeedbackParams& params);
	void toneMapOutput(const crt::EnergyField& energy, float k, Frame& out, int width, int height);
	bool readFeedbackHistory(int slotId);
	void feedbackHistoryAsOutput(Frame& out, int width, int height);
	void renderFocusFrame(Frame& src, Frame& out, int width, int height, const FocusParams& params);
	void renderRotatorFrame(Frame& src, Frame& out, int width, int height, const RotatorParams& params);
	Status renderVideoMixerNode(const VideoMixerParams& params, int width, int height, int depth, Frame& out);
	void renderVideoMixerFrame(const Frame* onAir, const Frame* next, const Frame* pattern, const Frame* key, const Frame* matte, Frame& out, int width, int height, const VideoMixerParams& params, const Frame* const* pictures = nullptr);
	void resampleInto(const std::vector<unsigned char>& src, int srcWidth, int srcHeight, Frame& out, int width, int height);
	static void resampleIntoF(const std::vector<float>& src, int srcWidth, int srcHeight, std::vector<float>& out, int width, int height);

	static void prepare(Frame& frame, int width, int height);
	void beginOutput(Frame& out, size_t count) const;
	static void drawLine(Frame& dst, int width, int height, float x0, float y0, float x1, float y1, float r, float g, float b, float alpha, int thickness);
	static void plotLinePixel(Frame& dst, int width, int height, int x, int y, float r, float g, float b, float alpha);

	using GradientTable = std::array<std::array<unsigned char, 3>, 256>;
	static void gradientTable(const float hue[4], const float level[4], const float sat[4], int bands, float offset, GradientTable& table);
	// The same table before it is cut to bytes: 0..255 units, clamped. gradientTable() truncates this.
	using GradientTableF = std::array<std::array<float, 3>, 256>;
	static void gradientTableF(const float hue[4], const float level[4], const float sat[4], int bands, float offset, GradientTableF& table);
	static float clamp01(float value);
	static float lumaAt(const std::vector<unsigned char>& frame, int width, int height, int x, int y);
	static float fract(float value);
	static void hsvToRgb(float h, float s, float v, unsigned char& r, unsigned char& g, unsigned char& b);
	static void hsvToRgb255(float h, float s, float v, float& r, float& g, float& b);
	static void rgbToHsv(float r, float g, float b, float& h, float& s, float& v);
	static void sampleBilinear(const std::vector<unsigned char>& frame, int width, int height, float x, float y, unsigned char* out);
	// The same lookup on a float picture (0..255 units); nothing is rounded.
	static void sampleBilinearF(const std::vector<float>& frame, int width, int height, float x, float y, float* out);

	std::array<std::unique_ptr<PreviewVideoEngine>, kVideoBusCount + 1> generatorEngines;
	std::array<uint64_t, kVideoBusCount + 1> generatorPass {};
	std::array<bool, kVideoBusCount + 1> onStack {};
	// One finished picture per bus for the current pass (shared taps). A bus that two inputs read is rendered once;
	// the later tap copies this frame. memoHeight is how many levels the bus's own subtree reaches below it, so a
	// reuse still honours kMaxDepth exactly as a fresh render would. The frames keep their capacity between passes.
	std::array<Frame, kVideoBusCount + 1> memo;
	std::array<uint64_t, kVideoBusCount + 1> memoPass {};
	std::array<int, kVideoBusCount + 1> memoHeight {};
	std::array<int, kVideoBusCount + 1> renderCounts {};
	// Frame Delays the pass has met and not yet stored a picture for: their own bus, source bus and settings. A delay
	// is queued once per pass (delayQueued); drawDelaySources() works through the list, which may grow as sources
	// that hold other delays are drawn. A bus can be queued once, so the list never outgrows the bus count.
	struct DelayJob {
		int busId = 0;
		int sourceBusId = 0;
		FrameDelayParams params;
	};
	std::array<DelayJob, kVideoBusCount + 1> delayJobs {};
	int delayJobCount = 0;
	int delayJobNext = 0;  // jobs before this index have had their source drawn in this pass
	std::array<uint64_t, kVideoBusCount + 1> delayQueued {};
	Frame delaySource;  // scratch for drawing a delay's source
	FrameRingFrame delayPrev;
	int deepest = 0;  // deepest level the walk has reached since it was last reset in renderNode
	uint64_t passId = 0;
	bool memoizeRoots = false;  // renderBusInPass: the top bus of a root is kept as a tap too

	std::vector<Frame> pool;
	std::vector<float> modLuma;  // brightness of the modulation picture, one value per pixel (renderModulation)
	Frame work;
	Frame none;
	Frame lastGood;
	int lastGoodBus = 0;
	int lastGoodWidth = 0;
	int lastGoodHeight = 0;
	FreezeFrameData frozenData;
	Frame frozenResampled;
	std::vector<unsigned char> delayBytes;  // a float picture rounded to bytes when a Frame Delay is over its float budget
	std::vector<float> frozenResampledF;
	FrameRingFrame feedbackPrev;
	Frame feedbackResampled;
	// Feedback's float history, used only while its fade option is on: the stored plane converted from 8-bit when
	// a frame has none, the plane resampled to this renderer's size, and the plane being written this tick.
	std::vector<float> feedbackPrevF;
	std::vector<float> feedbackResampledF;
	std::vector<float> feedbackOutF;
	// Focus scratch: three planar float pictures (R, G, B planes of width x height each), a padded line and a
	// column sum. Sized on the first frame and when the picture size changes, never per frame.
	std::vector<float> focusPlanes[3];
	std::vector<float> focusLine;
	std::vector<float> focusColumn;
	std::vector<float> focusRowA;
	std::vector<float> focusRowB;
	ExternalVideoFrame externalFrame;
	// CRT beam state (see crt_beam.hpp). Each beam node on a bus (Scope Trace, DeflectionYoke) has its own
	// energy field: it decays once per render and takes new beam energy on top, so persistence is a
	// per-node trail. Kept per renderer, so Display and Syphon Out each have their own trails.
	struct BeamState {
		crt::EnergyField energy;
		Frame display;      // last composed frame, reused when the same node renders twice in a pass
		uint64_t pass = 0;
		bool hasBeam = false;
		uint32_t scopeClearSerial = 0;  // Scope Trace RESET counter last seen (see ScopeTraceParams)
	};
	std::array<BeamState, kVideoBusCount + 1> beam;
	crt::Glow glow;
	SignalWindow scopeWindow;
	std::vector<raster_scan::Held> rasterSlow = std::vector<raster_scan::Held>(65);   // slow Raster pictures, painted a slice at a time
	std::vector<std::vector<unsigned char>> rasterHold = std::vector<std::vector<unsigned char>>(65);   // last good Raster picture per bus
	SignalWindow scopeSpan;  // trigger search span: the newest W + S samples
	// Scope Trace polyline scratch: the decimated window as screen pixels (trace A, trace B for Dual
	// Trace, and Z), so a first pass can measure the mean segment length before any stroke is drawn.
	struct ScopePoint {
		float x;
		float y;
		float x2;
		float y2;
		float z;
	};
	std::vector<ScopePoint> scopePoints;
	SignalWindow wobbleSignal;
	// DeflectionYoke scratch: scanline colours (lines x width x RGB), beam landing corners per scanline
	// (x, y), and the vertical extent of each scanline's corners (min, max).
	std::vector<float> wobbleColour;
	std::vector<std::array<float, 2>> wobbleCorners;
	std::vector<std::array<float, 2>> wobbleBounds;
	// Scan Processor scratch. The beam is drawn in scan space (lines are rows, the sweep runs along columns)
	// into scanField, then added to the node's energy. ROTATE 90 transposes that add. Each line keeps
	// S + 1 points (S = scan width): landed position, energy factor (0 while blanked, else the Z-INTENSITY
	// factor) and beam colour. scanProcessorBounds holds each line's vertical extent for band culling.
	struct ScanProcessorPoint {
		float x;
		float y;
		float energy;
		float r;
		float g;
		float b;
	};
	crt::EnergyField scanField;
	std::vector<ScanProcessorPoint> scanProcessorPoints;
	std::vector<std::array<float, 2>> scanProcessorBounds;
	SignalWindow scanProcessorSignal;

	std::atomic<bool> fullPrecision{false};  // the setting, as last set
	bool passFloat = false;                  // the setting as beginPass() read it: this pass's picture format
	bool readContended = false;  // a bus read gave up because the writer kept interrupting it
	bool signal = false;
	bool cycle = false;
};
