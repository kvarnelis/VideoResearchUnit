// Scope Trace stroke timing. Offline, no Rack. Build and run with `make bench`.
// Times VideoGraphRenderer::depositScopeTrace (the beam stroke, before the glow and the tone map) over the
// longest TIME window: TIME 1.0 is 100 ms, 4800 samples at 48 kHz, drawn as 2400 points. The signal is a
// full-scale 1 kHz sine on X and a 1.51 kHz sine on Y, about the longest path a steady tone draws. Each
// row is one mode at one frame size, at BEAM 0, 0.4 (the default) and 1 (sigma 0.5 px, 1.5 px and 3 px at
// 480 rows, scaled with the height above that, see scopeBeamSigmaPx), with DWELL at its default. The frame
// sizes are 640 x 480, 1440 x 1080 and 1920 x 1080. 1440 x 1080 is the real worst case: the Display draws
// at most 1080 lines (kMaxRenderLines) and Syphon Out offers 1440 x 1080. 1920 x 1080 is a stress case
// beyond it. The last column times a whole renderBus() of the same trace (decay, stroke, glow, tone map,
// graticule) so the stroke can be read against the frame. Median of 15 runs, in ms.
//
// Usage: scope_trace_bench

#include "crt_beam.hpp"
#include "scope_timebase.hpp"
#include "signal_ring.hpp"
#include "video_bus.hpp"
#include "video_engine.hpp"
#include "video_graph_renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

// The renderer declares this a friend, so the bench can call the private stroke on its own.
struct VideoGraphRendererTestAccess {
	static void depositScopeTrace(VideoGraphRenderer& r, crt::EnergyField& energy, int width, int height, const ScopeTraceParams& params, const SignalWindow& window) {
		r.depositScopeTrace(energy, width, height, params, window);
	}
};

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kRate = 48000.f;
constexpr int kRuns = 15;
constexpr int kBus = 50;

struct Timing {
	double median = 0.0;
	double min = 0.0;
};

Timing summarise(std::vector<double>& ms) {
	std::sort(ms.begin(), ms.end());
	return {ms[ms.size() / 2], ms.front()};
}

// Sample n of the test signal: X and Y are the Lissajous pair, Z is full brightness.
void testSample(int n, float* values) {
	const double t = static_cast<double>(n) / static_cast<double>(kRate);
	values[0] = static_cast<float>(std::sin(2.0 * kPi * 1000.0 * t));
	values[1] = static_cast<float>(std::sin(2.0 * kPi * 1510.0 * t + 0.5 * kPi));
	values[2] = 1.f;
	values[3] = 0.f;
}

// The first `count` samples of the test signal as a window, channel-major like a ring read.
SignalWindow makeWindow(int count) {
	SignalWindow window;
	window.channels = kSignalRingChannels;
	window.length = count;
	window.sampleRate = kRate;
	window.data.assign(static_cast<size_t>(kSignalRingChannels) * static_cast<size_t>(count), 0.f);
	for (int n = 0; n < count; ++n) {
		float values[kSignalRingChannels];
		testSample(n, values);
		for (int ch = 0; ch < kSignalRingChannels; ++ch) {
			window.data[static_cast<size_t>(ch) * static_cast<size_t>(count) + static_cast<size_t>(n)] = values[ch];
		}
	}
	return window;
}

}  // namespace

int main() {
	const int sizes[][2] = {{640, 480}, {1440, 1080}, {1920, 1080}};
	struct ModeRow {
		const char* name;
		ScopeTraceMode mode;
	};
	const ModeRow modes[] = {
		{"Oscilloscope", ScopeTraceMode::Oscilloscope},
		{"XY", ScopeTraceMode::XY},
		{"Dual Trace", ScopeTraceMode::DualTrace},
	};
	const float beams[] = {0.f, 0.4f, 1.f};

	const int windowSamples = static_cast<int>(std::lround(static_cast<double>(scopeWindowSeconds(1.f)) * static_cast<double>(kRate)));
	const SignalWindow window = makeWindow(windowSamples);

	// A ring with three windows in it, so renderBus() has the window and the span the trigger search reads.
	const int slot = signalRingAllocate();
	signalRingSetSampleRate(slot, kRate);
	for (int n = 0; n < windowSamples * 3; ++n) {
		float values[kSignalRingChannels];
		testSample(n, values);
		signalRingPush(slot, values);
	}

	VideoGraphRenderer renderer;
	std::printf("window: %d samples (TIME 1.0 at %.0f Hz)\n", windowSamples, static_cast<double>(kRate));
	std::printf("size        mode           beam  sigma px   stroke median   stroke min   frame median\n");
	for (const auto& size : sizes) {
		const int w = size[0];
		const int h = size[1];
		for (const ModeRow& row : modes) {
			for (float beam : beams) {
				ScopeTraceParams params;
				params.mode = static_cast<int>(row.mode);
				params.timebase = 1.f;
				params.thickness = beam;

				// The stroke alone. The field is decayed between runs, outside the timer, so it is warm and
				// holding a trail as it does in a running patch.
				crt::EnergyField energy;
				energy.resize(w, h);
				VideoGraphRendererTestAccess::depositScopeTrace(renderer, energy, w, h, params, window);  // warm up buffers
				std::vector<double> stroke;
				for (int run = 0; run < kRuns; ++run) {
					energy.decay(0.5f);
					const auto t0 = std::chrono::steady_clock::now();
					VideoGraphRendererTestAccess::depositScopeTrace(renderer, energy, w, h, params, window);
					const auto t1 = std::chrono::steady_clock::now();
					stroke.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
				}
				const Timing strokeTime = summarise(stroke);

				// The whole frame.
				videoBusPublishScopeTrace(kBus, slot, params);
				renderer.renderBus(kBus, w, h);  // warm up buffers
				std::vector<double> frame;
				for (int run = 0; run < kRuns; ++run) {
					const auto t0 = std::chrono::steady_clock::now();
					renderer.renderBus(kBus, w, h);
					const auto t1 = std::chrono::steady_clock::now();
					frame.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
				}
				const Timing frameTime = summarise(frame);
				videoBusClear(kBus);

				std::printf("%4dx%-5d %-14s %4.1f %9.3f %12.2f ms %10.2f ms %10.2f ms\n", w, h, row.name, static_cast<double>(beam),
					static_cast<double>(scopeBeamSigmaPx(beam, h)), strokeTime.median, strokeTime.min, frameTime.median);
			}
		}
	}
	signalRingRelease(slot);
	return 0;
}
