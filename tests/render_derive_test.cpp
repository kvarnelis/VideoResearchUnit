// A consumer that says mayDerive (Video Probe) gets the larger picture averaged down instead of a second render of
// the graph, in thread mode, while another consumer has the same bus drawn larger and in the same shape
// (src/render_service.hpp, touch(); src/frame_downsample.hpp). Offline, no Rack SDK.
//
// Checked here: the box average itself; that the small picture is exactly the large one averaged down and that the
// bus is rendered once a tick, not twice; that a larger picture of another shape, another bus, or another consumer
// that also says mayDerive is not used; that the small picture is rendered again when the large consumer goes; that
// a consumer that did not say mayDerive always gets its own render; and that the UI-driven mode ignores the flag.

#include "frame_downsample.hpp"
#include "render_service.hpp"
#include "video_bus.hpp"
#include "video_engine.hpp"
#include "video_graph_renderer.hpp"

#include <cstdio>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;

int failures = 0;
int checks = 0;

void check(bool ok, const char* what) {
	++checks;
	if (!ok) {
		++failures;
		std::printf("  FAIL: %s\n", what);
	}
}

double gTime = 1000.0;
double timeSource() {
	return gTime;
}

constexpr int kBus = 7;
constexpr int kOtherBus = 8;
constexpr int kSmallW = 320;
constexpr int kSmallH = 240;

RampsParams rampsA() {
	RampsParams p;
	p.axisA = 2;
	p.freqA = 3.f;
	p.waveA = 0.3f;
	p.axisB = 3;
	p.freqB = 5.f;
	p.waveB = 0.6f;
	p.mix = 1;
	return p;
}

Bytes nativeRender(int bus, int w, int h) {
	VideoGraphRenderer r;
	const VideoGraphRenderer::Frame& f = r.renderBus(bus, w, h);
	return Bytes(f.begin(), f.end());
}

Bytes averaged(const Bytes& large, int W, int H, int w, int h) {
	Bytes out(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
	frame_downsample::box(large.data(), W, H, out.data(), w, h);
	return out;
}

void testBox() {
	std::printf("Box average\n");
	// A copy at the same size.
	Bytes src(8u * 6u * 4u);
	for (size_t i = 0; i < src.size(); ++i) {
		src[i] = static_cast<unsigned char>((i * 37u + 11u) & 255u);
	}
	check(averaged(src, 8, 6, 8, 6) == src, "the same size is a copy");

	// Exactly 2 to 1: each output pixel is the mean of a 2 x 2 block, rounded to nearest.
	const Bytes half = averaged(src, 8, 6, 4, 3);
	bool exact = true;
	for (int y = 0; y < 3; ++y) {
		for (int x = 0; x < 4; ++x) {
			for (int c = 0; c < 4; ++c) {
				unsigned sum = 0;
				for (int dy = 0; dy < 2; ++dy) {
					for (int dx = 0; dx < 2; ++dx) {
						sum += src[(static_cast<size_t>(y * 2 + dy) * 8u + static_cast<size_t>(x * 2 + dx)) * 4u + static_cast<size_t>(c)];
					}
				}
				if (half[(static_cast<size_t>(y) * 4u + static_cast<size_t>(x)) * 4u + static_cast<size_t>(c)] != (sum + 2u) / 4u) {
					exact = false;
				}
			}
		}
	}
	check(exact, "2 to 1 is the rounded mean of each 2 x 2 block");

	// A ratio that is not whole: a flat picture stays flat, and the mean of a ramp is kept to within rounding.
	Bytes flat(969u * 727u * 4u, 0);
	for (size_t i = 0; i < flat.size(); i += 4u) {
		flat[i] = 200;
		flat[i + 1] = 7;
		flat[i + 2] = 255;
		flat[i + 3] = 255;
	}
	const Bytes flatSmall = averaged(flat, 969, 727, kSmallW, kSmallH);
	bool stillFlat = true;
	for (size_t i = 0; i < flatSmall.size(); i += 4u) {
		if (flatSmall[i] != 200 || flatSmall[i + 1] != 7 || flatSmall[i + 2] != 255 || flatSmall[i + 3] != 255) {
			stillFlat = false;
		}
	}
	check(stillFlat, "a flat picture stays flat at 969 x 727 to 320 x 240");

	Bytes ramp(969u * 727u * 4u, 255);
	double meanLarge = 0.0;
	for (int y = 0; y < 727; ++y) {
		for (int x = 0; x < 969; ++x) {
			const unsigned char v = static_cast<unsigned char>((x * 255) / 968);
			ramp[(static_cast<size_t>(y) * 969u + static_cast<size_t>(x)) * 4u] = v;
			meanLarge += v;
		}
	}
	meanLarge /= 969.0 * 727.0;
	const Bytes rampSmall = averaged(ramp, 969, 727, kSmallW, kSmallH);
	double meanSmall = 0.0;
	for (size_t i = 0; i < rampSmall.size(); i += 4u) {
		meanSmall += rampSmall[i];
	}
	meanSmall /= static_cast<double>(kSmallW) * kSmallH;
	std::printf("  ramp mean: large %.3f, small %.3f\n", meanLarge, meanSmall);
	check(std::abs(meanLarge - meanSmall) < 0.6, "the mean of a ramp is kept to within rounding");

	check(frame_downsample::sameShape(1440, 1080, 320, 240), "1440 x 1080 has the shape of 320 x 240");
	check(frame_downsample::sameShape(969, 727, 320, 240), "969 x 727 (a Display 727 rows tall) has the shape of 320 x 240");
	check(!frame_downsample::sameShape(1920, 1080, 320, 240), "1920 x 1080 does not have the shape of 320 x 240");
	check(!frame_downsample::sameShape(640, 360, 320, 240), "640 x 360 does not have the shape of 320 x 240");
}

struct Tick {
	RenderService& svc;
	void run() {
		gTime += 1.0 / 60.0;
		svc.runTickForTest();
	}
};

Bytes taken(RenderService& svc, const void* owner, int bus, int w, int h, bool mayDerive) {
	const RenderService::Result r = svc.frame(owner, bus, w, h, mayDerive);
	return r.signal ? Bytes(r.pixels->begin(), r.pixels->end()) : Bytes();
}

void testThreadMode() {
	std::printf("Thread mode: the small picture is the large one averaged down\n");
	videoBusPublishRamps(kBus, rampsA());
	RampsParams other = rampsA();
	other.freqA = 9.f;
	videoBusPublishRamps(kOtherBus, other);

	const Bytes nativeSmall = nativeRender(kBus, kSmallW, kSmallH);
	const Bytes nativeLarge = nativeRender(kBus, 640, 480);
	const Bytes expected = averaged(nativeLarge, 640, 480, kSmallW, kSmallH);
	check(!nativeSmall.empty() && nativeSmall != expected, "the graph drawn small and the large picture made small are different pictures");

	RenderService svc;
	svc.setTimeSource(timeSource);
	svc.setThreadAutoStart(false);
	svc.setThreadMode(true);
	Tick tick{svc};
	int display = 0, probe = 0, probe2 = 0, syphon = 0;

	// The probe alone: rendered at its own size.
	svc.touch(&probe, kBus, kSmallW, kSmallH, true);
	tick.run();
	check(taken(svc, &probe, kBus, kSmallW, kSmallH, true) == nativeSmall, "a probe alone gets the graph drawn at its own size");
	check(svc.derivedTotal() == 0, "nothing was averaged down");

	// A Display joins at 640 x 480: one render a tick, and the probe's picture is the Display's averaged down.
	uint64_t before = svc.renderTotal();
	for (int i = 0; i < 3; ++i) {
		svc.touch(&display, kBus, 640, 480);
		svc.touch(&probe, kBus, kSmallW, kSmallH, true);
		tick.run();
	}
	check(svc.renderTotal() - before == 3, "with a Display on the bus it is rendered once a tick, not twice");
	check(svc.derivedTotal() == 3, "the probe's picture was averaged down each tick");
	check(taken(svc, &display, kBus, 640, 480, false) == nativeLarge, "the Display's picture is its own render");
	check(taken(svc, &probe, kBus, kSmallW, kSmallH, true) == expected, "the probe's picture is the Display's averaged down, byte for byte");

	// A second probe on the same bus shares the picture; it does not stop the averaging.
	svc.touch(&display, kBus, 640, 480);
	svc.touch(&probe, kBus, kSmallW, kSmallH, true);
	svc.touch(&probe2, kBus, kSmallW, kSmallH, true);
	before = svc.renderTotal();
	tick.run();
	check(svc.renderTotal() - before == 1, "two probes and a Display: still one render");
	check(taken(svc, &probe2, kBus, kSmallW, kSmallH, true) == expected, "the second probe gets the same averaged picture");

	// A consumer at the small size that did not say mayDerive makes the small picture a render again, for everyone.
	svc.touch(&display, kBus, 640, 480);
	svc.touch(&probe, kBus, kSmallW, kSmallH, true);
	svc.touch(&syphon, kBus, kSmallW, kSmallH, false);
	before = svc.renderTotal();
	tick.run();
	check(svc.renderTotal() - before == 2, "a plain consumer at the small size brings its render back");
	check(taken(svc, &probe, kBus, kSmallW, kSmallH, true) == nativeSmall, "and the probe then gets the graph drawn small");
	svc.forget(&syphon);
	svc.forget(&probe2);

	// The large picture is on another bus: not used.
	svc.forget(&display);
	svc.touch(&display, kOtherBus, 640, 480);
	svc.touch(&probe, kBus, kSmallW, kSmallH, true);
	before = svc.renderTotal();
	tick.run();
	check(svc.renderTotal() - before == 2, "a larger picture of another bus is not used");
	check(taken(svc, &probe, kBus, kSmallW, kSmallH, true) == nativeSmall, "the probe gets its own render then");

	// The large picture has another shape (16:9): not used.
	svc.touch(&display, kBus, 640, 360);
	svc.touch(&probe, kBus, kSmallW, kSmallH, true);
	before = svc.renderTotal();
	tick.run();
	check(svc.renderTotal() - before == 2, "a larger picture of another shape is not used");
	check(taken(svc, &probe, kBus, kSmallW, kSmallH, true) == nativeSmall, "the probe gets its own render then");

	// The only larger consumer also says mayDerive: someone has to render, so both do.
	svc.forget(&display);
	svc.touch(&probe2, kBus, 640, 480, true);
	svc.touch(&probe, kBus, kSmallW, kSmallH, true);
	before = svc.renderTotal();
	tick.run();
	check(svc.renderTotal() - before == 2, "two consumers that both say mayDerive are both rendered");
	svc.forget(&probe2);

	// Back with the Display, then the Display stops asking: after its registration lapses the probe is rendered again.
	svc.touch(&display, kBus, 640, 480);
	svc.touch(&probe, kBus, kSmallW, kSmallH, true);
	tick.run();
	check(taken(svc, &probe, kBus, kSmallW, kSmallH, true) == expected, "with the Display back the probe's picture is averaged down again");
	gTime += RenderService::kRegistrationSeconds + 0.5;
	svc.touch(&probe, kBus, kSmallW, kSmallH, true);
	before = svc.renderTotal();
	tick.run();
	check(svc.renderTotal() - before == 1, "when the Display has gone the bus is rendered once, at the probe's size");
	check(taken(svc, &probe, kBus, kSmallW, kSmallH, true) == nativeSmall, "and the probe's picture is the graph drawn small again");

	// The bus goes: the large picture has no signal, so neither has the small one.
	svc.touch(&display, kBus, 640, 480);
	svc.touch(&probe, kBus, kSmallW, kSmallH, true);
	tick.run();
	videoBusClear(kBus);
	svc.touch(&display, kBus, 640, 480);
	svc.touch(&probe, kBus, kSmallW, kSmallH, true);
	tick.run();
	const RenderService::Result gone = svc.frame(&probe, kBus, kSmallW, kSmallH, true);
	check(!gone.signal && !gone.pending, "a bus with no picture gives the probe no picture either");
	videoBusClear(kOtherBus);
}

void testUiMode() {
	std::printf("UI-driven mode ignores the flag\n");
	videoBusPublishRamps(kBus, rampsA());
	const Bytes nativeSmall = nativeRender(kBus, kSmallW, kSmallH);
	RenderService svc;
	int display = 0, probe = 0;
	const RenderService::Result large = svc.frame(&display, kBus, 640, 480);
	const RenderService::Result small = svc.frame(&probe, kBus, kSmallW, kSmallH, true);
	check(large.signal && small.signal, "both pictures rendered");
	check(Bytes(small.pixels->begin(), small.pixels->end()) == nativeSmall, "the probe's picture is the graph drawn at its own size");
	check(svc.derivedTotal() == 0, "nothing was averaged down");
	videoBusClear(kBus);
}

}  // namespace

int main() {
	testBox();
	testThreadMode();
	testUiMode();
	std::printf("render_derive_test: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
