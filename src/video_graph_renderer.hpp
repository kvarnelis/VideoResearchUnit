#pragma once

#include "video_bus.hpp"
#include "video_engine.hpp"
#include "freeze_frame_store.hpp"
#include "frame_ring.hpp"
#include "video_external_frame.hpp"
#include "scope_timebase.hpp"
#include "signal_ring.hpp"
#include "crt_beam.hpp"

#include <array>
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
// A cycle, or a graph deeper than kMaxDepth, is not rendered. renderBus() then returns the
// last good frame for the same bus and size, and hasCycle() reports true.
class VideoGraphRenderer {
public:
	using Frame = std::vector<unsigned char>;
	// Lets tests/bus_test.cpp call the private Scope Trace window reader directly.
	friend struct VideoGraphRendererTestAccess;

	VideoGraphRenderer();

	// The returned frame stays valid until the next call. It is empty when nothing renders.
	const Frame& renderBus(int busId, int width, int height);
	bool hasSignal() const {
		return signal;
	}
	bool hasCycle() const {
		return cycle;
	}

private:
	enum class Status { Ok, Empty, Cycle };

	static constexpr int kMaxDepth = 16;
	// Scratch frames per recursion depth. Bus Mixer: 0 = source, 1 = bus B, 2 = matte. Video Mixer: 0 = live,
	// 1 = next, 2 = pattern, 3 = key, 4 = matte.
	static constexpr int kSlotsPerDepth = 5;

	Status renderNode(int busId, int width, int height, int depth, Frame& out);
	Status renderNodeBody(int busId, const VideoBusNode& node, int width, int height, int depth, Frame& out);
	Frame& scratch(int depth, int index) {
		return pool[static_cast<size_t>(depth * kSlotsPerDepth + index)];
	}

	bool renderScopeTraceFrame(int busId, int width, int height, int scopeTraceSlotId, const ScopeTraceParams& params, Frame& out);
	// Fills out with the window to draw. Oscilloscope and Dual start it at a trigger (see scopeFindTrigger).
	bool readScopeWindow(int scopeTraceSlotId, const ScopeTraceParams& params, SignalWindow& out);
	void depositScopeTrace(crt::EnergyField& energy, int width, int height, const ScopeTraceParams& params, const SignalWindow& window);
	void renderDeflectionYokeFrame(int busId, Frame& src, Frame& out, int width, int height, const DeflectionYokeParams& params);
	void renderScanProcessorFrame(int busId, Frame& src, Frame& out, int width, int height, const ScanProcessorParams& params);
	void renderLineFieldFrame(Frame& src, Frame& out, int width, int height, const LineFieldParams& params);
	void renderColorizerFrame(Frame& src, Frame& out, int width, int height, const ColorizerParams& params);
	void renderColorLabFrame(Frame& src, Frame& out, int width, int height, const ColorLabParams& params);
	void renderKeyerFrame(Frame& src, Frame& out, int width, int height, const KeyerParams& params);
	void renderBusMixerFrame(const Frame& a, const Frame& b, const Frame* matte, Frame& out, int width, int height, const BusMixerParams& params);
	void renderWaveShaperFrame(Frame& src, Frame& out, int width, int height, const WaveShaperParams& params);
	void renderFreezeFrame(Frame& src, Frame& out, int width, int height, const FreezeFrameParams& params);
	void renderFeedbackFrame(Frame& src, Frame& out, int width, int height, const FeedbackParams& params);
	void renderFocusFrame(Frame& src, Frame& out, int width, int height, const FocusParams& params);
	void renderRotatorFrame(Frame& src, Frame& out, int width, int height, const RotatorParams& params);
	Status renderVideoMixerNode(const VideoMixerParams& params, int width, int height, int depth, Frame& out);
	void renderVideoMixerFrame(const Frame* onAir, const Frame* next, const Frame* pattern, const Frame* key, const Frame* matte, Frame& out, int width, int height, const VideoMixerParams& params);
	void resampleInto(const Frame& src, int srcWidth, int srcHeight, Frame& out, int width, int height);

	static void prepare(Frame& frame, int width, int height);
	static void drawLine(Frame& dst, int width, int height, float x0, float y0, float x1, float y1, unsigned char r, unsigned char g, unsigned char b, float alpha, int thickness);
	static void plotLinePixel(Frame& dst, int width, int height, int x, int y, unsigned char r, unsigned char g, unsigned char b, float alpha);

	static float clamp01(float value);
	static float lumaAt(const Frame& frame, int width, int height, int x, int y);
	static float fract(float value);
	static void hsvToRgb(float h, float s, float v, unsigned char& r, unsigned char& g, unsigned char& b);
	static void rgbToHsv(float r, float g, float b, float& h, float& s, float& v);
	static void sampleBilinear(const Frame& frame, int width, int height, float x, float y, unsigned char* out);

	std::array<std::unique_ptr<PreviewVideoEngine>, kVideoBusCount + 1> generatorEngines;
	std::array<uint64_t, kVideoBusCount + 1> generatorPass {};
	std::array<bool, kVideoBusCount + 1> onStack {};
	uint64_t passId = 0;

	std::vector<Frame> pool;
	Frame work;
	Frame none;
	Frame lastGood;
	int lastGoodBus = 0;
	int lastGoodWidth = 0;
	int lastGoodHeight = 0;
	FreezeFrameData frozenData;
	Frame frozenResampled;
	FrameRingFrame feedbackPrev;
	Frame feedbackResampled;
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

	bool readContended = false;  // a bus read gave up because the writer kept interrupting it
	bool signal = false;
	bool cycle = false;
};
