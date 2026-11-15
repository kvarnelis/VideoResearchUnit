// Shared taps render timing. Offline, no Rack. Build and run with `make bench`.
// Builds a diamond graph N levels deep: bus 1 is a Video Oscillator, bus k+1 is a Bus Mixer whose A and B
// inputs both read bus k. Times one renderBus() of the top bus. If a picture that feeds two inputs is rendered
// once per input, the time doubles with every level. If it is rendered once per pass, the time grows by one
// mixer per level. Median of 7 frames, in ms, at 320 x 240.
//
// Usage: shared_taps_bench

#include "video_bus.hpp"
#include "video_engine.hpp"
#include "video_graph_renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

int main() {
	const int w = 320;
	const int h = 240;
	const int maxLevels = 12;
	VideoParams osc;
	osc.pattern = 1;
	osc.feedback = 0.f;
	videoBusPublish(1, osc);

	std::printf("levels   median ms   min ms   ratio to previous\n");
	double previous = 0.0;
	for (int levels = 0; levels <= maxLevels; ++levels) {
		if (levels > 0) {
			BusMixerParams m;
			m.busB = levels;
			m.mode = (levels % 2) ? 2 : 4;
			m.mix = 0.6f;
			videoBusPublishBusMixer(levels + 1, levels, m);
		}
		VideoGraphRenderer renderer;
		renderer.renderBus(levels + 1, w, h);  // warm up buffers
		std::vector<double> ms;
		for (int run = 0; run < 7; ++run) {
			const auto t0 = std::chrono::steady_clock::now();
			renderer.renderBus(levels + 1, w, h);
			const auto t1 = std::chrono::steady_clock::now();
			ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
		}
		std::sort(ms.begin(), ms.end());
		const double median = ms[ms.size() / 2];
		std::printf("%6d %11.2f %8.2f %12.2f\n", levels, median, ms.front(), previous > 0.0 ? median / previous : 0.0);
		previous = median;
		std::fflush(stdout);
	}
	for (int k = 1; k <= maxLevels + 1; ++k) {
		videoBusClear(k);
	}
	return 0;
}
