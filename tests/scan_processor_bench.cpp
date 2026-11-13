// Scan Processor render timing. Offline, no Rack. Build and run with `make bench`.
// Times one renderBus() of a Scan Processor node over a grid source, for each scanline count, at two sizes.
// Deflection is X and Y at 1 kHz (audio rate, read at each point's scan time). Median of 15 frames, in ms.
//
// Usage: scan_processor_bench
// The 256-line row at 448 x 640 is the target (under about 6 ms per frame).

#include "signal_ring.hpp"
#include "video_bus.hpp"
#include "video_engine.hpp"
#include "video_external_frame.hpp"
#include "video_graph_renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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

int main() {
	const double pi = 3.14159265358979323846;
	const float rate = 48000.f;
	const int sizes[][2] = {{448, 640}, {576, 720}};
	const int lineCounts[] = {128, 256, 525, 1050};

	const int slot = signalRingAllocate();
	VideoGraphRenderer renderer;
	std::printf("size        lines   median ms   min ms\n");
	for (const auto& size : sizes) {
		const int w = size[0];
		const int h = size[1];
		const Frame source = makeGridSource(w, h);
		const int frameId = videoExternalFrameAllocate();
		videoExternalFramePublish(frameId, source.data(), w, h, 0.0);
		videoBusPublishExternalFrame(40, frameId);

		for (int lines : lineCounts) {
			// Two field periods of samples, so the ring always has a complete field to read.
			signalRingSetSampleRate(slot, rate);
			const double fieldSamples = crt::fieldSeconds(lines) * rate;
			const int total = static_cast<int>(fieldSamples * 2.0) + 64;
			for (int n = 0; n < total; ++n) {
				const double ph = 2.0 * pi * 1000.0 * n / rate;
				const float v[kSignalRingChannels] = {static_cast<float>(0.04 * std::sin(ph)), static_cast<float>(0.04 * std::cos(ph)), 0.f};
				signalRingPush(slot, v);
			}
			ScanProcessorParams p;
			p.lines = lines;
			p.signalSlotId = slot;
			videoBusPublishScanProcessor(41, 40, p);

			renderer.renderBus(41, w, h);  // warm up buffers
			std::vector<double> ms;
			for (int run = 0; run < 15; ++run) {
				const auto t0 = std::chrono::steady_clock::now();
				renderer.renderBus(41, w, h);
				const auto t1 = std::chrono::steady_clock::now();
				ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
			}
			std::sort(ms.begin(), ms.end());
			std::printf("%4dx%-5d %6d %11.2f %9.2f\n", w, h, lines, ms[ms.size() / 2], ms.front());
			videoBusClear(41);
		}
		videoBusClear(40);
		videoExternalFrameRelease(frameId);
	}
	signalRingRelease(slot);
	return 0;
}
