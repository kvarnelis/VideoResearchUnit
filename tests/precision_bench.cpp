// Precision timing and memory. Offline, no Rack. Build and run with `make precision-bench`.
// A chain of ten converted nodes (Ramps, three Color Labs, two Wave Shapers, a Keyer, two Colorizers and a Bus
// Mixer that also reads the Ramps) is rendered with the full precision setting off and then on, at 640 x 480
// and 1440 x 1080. Prints the median of 15 frames in ms. With -DVRU_NO_PRECISION_API (used to time a build from
// before the setting existed) only the off column is printed.
//
// Usage: precision_bench

#include "video_bus.hpp"
#include "video_engine.hpp"
#include "video_graph_renderer.hpp"
#include "frame_ring.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include <sys/resource.h>
#include <cstdlib>

static void buildChain() {
	RampsParams ramps;
	ramps.axisA = 0;
	ramps.freqA = 1.f;
	ramps.mix = 0;
	videoBusPublishRamps(1, ramps);
	ColorLabParams lab;
	lab.hue = 0.1f;
	lab.saturation = 1.3f;
	lab.brightness = 0.9f;
	lab.contrast = 1.1f;
	videoBusPublishColorLab(2, 1, lab);
	WaveShaperParams shaper;
	shaper.drive = 1.4f;
	shaper.fold = 0.2f;
	videoBusPublishWaveShaper(3, 2, shaper);
	KeyerParams keyer;
	keyer.mode = 2;
	keyer.mix = 0.7f;
	videoBusPublishKeyer(4, 3, keyer);
	ColorizerParams colorizer;
	videoBusPublishColorizer(5, 4, colorizer);
	lab.hue = 0.3f;
	videoBusPublishColorLab(6, 5, lab);
	BusMixerParams mixer;
	mixer.busB = 1;
	mixer.mode = 2;
	mixer.mix = 0.5f;
	videoBusPublishBusMixer(7, 6, mixer);
	shaper.drive = 0.8f;
	videoBusPublishWaveShaper(8, 7, shaper);
	lab.hue = 0.5f;
	videoBusPublishColorLab(9, 8, lab);
	videoBusPublishColorizer(10, 9, colorizer);
}

static double timeChain(int w, int h, bool precision) {
	VideoGraphRenderer renderer;
#ifndef VRU_NO_PRECISION_API
	renderer.setFullPrecision(precision);
#else
	(void)precision;
#endif
	renderer.renderBus(10, w, h);
	std::vector<double> ms;
	for (int run = 0; run < 15; ++run) {
		const auto t0 = std::chrono::steady_clock::now();
		renderer.renderBus(10, w, h);
		const auto t1 = std::chrono::steady_clock::now();
		ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
	}
	std::sort(ms.begin(), ms.end());
	return ms[ms.size() / 2];
}

// Peak resident memory of one process that renders the chain: `precision_bench mem off|on WIDTH HEIGHT`.
static int memoryMode(int argc, char** argv) {
	if (argc < 5) {
		return 2;
	}
	const bool on = std::string(argv[2]) == "on";
	const int w = std::atoi(argv[3]);
	const int h = std::atoi(argv[4]);
	buildChain();
	{
		VideoGraphRenderer renderer;
#ifndef VRU_NO_PRECISION_API
		renderer.setFullPrecision(on);
#else
		(void)on;
#endif
		const int frames = argc >= 6 ? std::atoi(argv[5]) : 4;
		for (int i = 0; i < frames; ++i) {
			renderer.renderBus(10, w, h);
		}
	}
	rusage usage{};
	getrusage(RUSAGE_SELF, &usage);
	std::printf("%dx%d setting %s: peak resident %.1f MiB\n", w, h, on ? "on " : "off", static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0));
	return 0;
}

// Peak resident memory of a Frame Delay holding its whole ring: `precision_bench memdelay off|on WIDTH HEIGHT FRAMES`.
// Ramps -> Color Lab -> Frame Delay, rendered FRAMES + 2 ticks so every stored picture is in the ring.
static int memoryDelayMode(int argc, char** argv) {
	if (argc < 6) {
		return 2;
	}
	const bool on = std::string(argv[2]) == "on";
	const int w = std::atoi(argv[3]);
	const int h = std::atoi(argv[4]);
	const int frames = std::atoi(argv[5]);
	RampsParams ramps;
	ramps.axisA = 0;
	ramps.freqA = 1.f;
	ramps.mix = 0;
	videoBusPublishRamps(1, ramps);
	ColorLabParams lab;
	lab.brightness = 0.9f;
	videoBusPublishColorLab(2, 1, lab);
	const int slot = frameRingAllocateSlot();
	{
		VideoGraphRenderer renderer;
#ifndef VRU_NO_PRECISION_API
		renderer.setFullPrecision(on);
#else
		(void)on;
#endif
		for (int tick = 1; tick <= frames + 2; ++tick) {
			FrameDelayParams delay;
			delay.ringSlotId = slot;
			delay.frames = frames;
			delay.frameCounter = tick;
			videoBusPublishFrameDelay(3, 2, delay);
			renderer.renderBus(3, w, h);
		}
	}
	frameRingReleaseSlot(slot);
	rusage usage{};
	getrusage(RUSAGE_SELF, &usage);
	std::printf("frame delay %d frames at %dx%d setting %s: peak resident %.1f MiB\n", frames, w, h, on ? "on " : "off", static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0));
	return 0;
}

int main(int argc, char** argv) {
	if (argc >= 2 && std::string(argv[1]) == "mem") {
		return memoryMode(argc, argv);
	}
	if (argc >= 2 && std::string(argv[1]) == "memdelay") {
		return memoryDelayMode(argc, argv);
	}
	buildChain();
	std::printf("ten-node chain, median ms per picture\n");
	std::printf("size          off     on\n");
	const int sizes[2][2] = {{640, 480}, {1440, 1080}};
	for (const auto& size : sizes) {
		const double off = timeChain(size[0], size[1], false);
#ifndef VRU_NO_PRECISION_API
		const double on = timeChain(size[0], size[1], true);
#else
		const double on = 0.0;
#endif
		std::printf("%4dx%-5d %7.2f %7.2f\n", size[0], size[1], off, on);
	}
	for (int k = 1; k <= 10; ++k) {
		videoBusClear(k);
	}
	return 0;
}
