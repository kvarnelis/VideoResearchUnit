// DeflectionYoke render timing. Offline, no Rack. Build and run with `make bench`.
// Times one renderBus() of a DeflectionYoke node over a grid source, at each size and deflection rate.
// Prints the median of 15 frames (and the minimum) in milliseconds.
//
// Usage: deflection_yoke_bench [x|xy] [clamp]
//   x   X deflection only (full scale, 0.5 of the width)
//   xy  X plus a quadrature Y of the same amplitude
//   (no argument: both)
//   clamp  use the Clamp edge (extends the picture with its edge colour). Default: Black, the
//          module's default edge. Persistence and glow are the module defaults.

#include "signal_ring.hpp"
#include "video_bus.hpp"
#include "video_engine.hpp"
#include "video_external_frame.hpp"
#include "video_graph_renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using Frame = std::vector<unsigned char>;

Frame makeGridSource(int w, int h) {
	Frame f(static_cast<size_t>(w) * h * 4u, 0);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
			const bool line = (x % 40 == 0) || (y % 40 == 0);
			f[i] = line ? 235 : 40;
			f[i + 1] = line ? 235 : 40;
			f[i + 2] = line ? 80 : 90;
			f[i + 3] = 255;
		}
	}
	return f;
}

}  // namespace

int main(int argc, char** argv) {
	const double pi = 3.14159265358979323846;
	const float rate = 48000.f;
	const int samplesPerFrame = 800;
	const int sizes[][2] = {{448, 640}, {576, 720}, {600, 1080}};
	const double freqs[] = {200.0, 1000.0, 5000.0};

	const bool clampEdge = argc > 2 && std::strcmp(argv[2], "clamp") == 0;
	std::vector<bool> modes;  // true = X plus quadrature Y
	if (argc < 2) {
		modes = {false, true};
	}
	else if (std::strcmp(argv[1], "xy") == 0) {
		modes = {true};
	}
	else {
		modes = {false};
	}

	const int slot = signalRingAllocate();
	VideoGraphRenderer renderer;
	std::printf("deflection  size        freq     median ms   min ms\n");
	for (bool withY : modes) {
		for (const auto& size : sizes) {
			const int w = size[0];
			const int h = size[1];
			const Frame source = makeGridSource(w, h);
			const int frameId = videoExternalFrameAllocate();
			videoExternalFramePublish(frameId, source.data(), w, h, 0.0);
			videoBusPublishExternalFrame(40, frameId);

			for (double f : freqs) {
				signalRingSetSampleRate(slot, rate);
				const int total = samplesPerFrame * 6 + 35;
				for (int n = 0; n < total; ++n) {
					const double ph = 2.0 * pi * f * n / rate;
					float v[kSignalRingChannels] = {static_cast<float>(0.25 * std::sin(ph)), static_cast<float>(withY ? 0.25 * std::cos(ph) : 0.0), 0.f};
					signalRingPush(slot, v);
				}
				DeflectionYokeParams p;
				p.depth = 1.f;
				p.signalSlotId = slot;
				p.edge = static_cast<int>(clampEdge ? DeflectionYokeEdge::Clamp : DeflectionYokeEdge::Black);
				p.persist = 0.35f;
				p.glow = 0.4f;
				videoBusPublishDeflectionYoke(41, 40, p);

				renderer.renderBus(41, w, h);  // warm up buffers
				std::vector<double> ms;
				for (int run = 0; run < 15; ++run) {
					const auto t0 = std::chrono::steady_clock::now();
					renderer.renderBus(41, w, h);
					const auto t1 = std::chrono::steady_clock::now();
					ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
				}
				std::sort(ms.begin(), ms.end());
				std::printf("%-10s  %4dx%-5d %6.0f Hz %9.2f %9.2f\n", withY ? "X+Y" : "X", w, h, f, ms[ms.size() / 2], ms.front());
			}
			videoBusClear(41);
			videoBusClear(40);
			videoExternalFrameRelease(frameId);
		}
	}
	signalRingRelease(slot);
	return 0;
}
