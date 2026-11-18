// Video clock acceptance tests (docs/architecture.md, "Video clock"). No Rack SDK needed.
//   a) a tone at a whole multiple of the field rate lands on the same picture rows field after field, over 600
//      fields, at 44.1, 48 and 96 kHz, in the Deflection Yoke and the Scan Processor;
//   b) the same input draws the same picture at all three sample rates;
//   c) the clock round trip, sample -> video time -> sample, is exact over an hour of samples;
//   plus the field-agreement rule between the rings, the renderer and the Scan Sync outputs.
// The test that patches Scan Sync into a scan module is tests/scan_sync_module_test.cpp (it needs Rack).
#include "crt_beam.hpp"
#include "scan_sync_math.hpp"
#include "signal_ring.hpp"
#include "video_bus.hpp"
#include "video_clock.hpp"
#include "video_engine.hpp"
#include "video_external_frame.hpp"
#include "video_graph_renderer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

using Frame = std::vector<unsigned char>;

int failures = 0;
int checks = 0;
void check(bool ok, const char* what) {
	++checks;
	if (!ok) {
		++failures;
		std::printf("  FAIL: %s\n", what);
	}
}

constexpr double kPi = 3.14159265358979323846;
constexpr int kW = 192;
constexpr int kH = 240;  // one row per line at 240 lines
constexpr int kLines = 240;
const double kRates[3] = {44100.0, 48000.0, 96000.0};

Frame lineSource(int column) {
	Frame f(static_cast<size_t>(kW) * kH * 4u, 0);
	for (int y = 0; y < kH; ++y) {
		for (int x = 0; x < kW; ++x) {
			const size_t i = (static_cast<size_t>(y) * kW + x) * 4u;
			f[i] = f[i + 1] = f[i + 2] = x == column ? 255 : 0;
			f[i + 3] = 255;
		}
	}
	return f;
}

// The Deflection Yoke or the Scan Processor, drawn from a signal ring, the way the Display does it.
struct Rig {
	const bool yoke;
	int slot = 0;
	int frameId = 0;
	VideoGraphRenderer renderer;
	int64_t pushed = 0;  // frames written so far

	explicit Rig(bool useYoke) : yoke(useYoke) {
		slot = signalRingAllocate();
		frameId = videoExternalFrameAllocate();
		const Frame src = lineSource(kW / 2);
		videoExternalFramePublish(frameId, src.data(), kW, kH, 0.0);
		videoBusPublishExternalFrame(40, frameId);
	}
	~Rig() {
		videoBusClear(41);
		videoBusClear(40);
		videoExternalFrameRelease(frameId);
		signalRingRelease(slot);
	}
	// Writes frames up to (not including) `until`. gen(frame, out[4]).
	template <typename Gen>
	void pushUntil(double rate, int64_t until, Gen gen) {
		signalRingSetSampleRate(slot, static_cast<float>(rate));
		for (; pushed < until; ++pushed) {
			float v[kSignalRingChannels] = {0.f, 0.f, 0.f, 0.f};
			gen(pushed, v);
			signalRingPushAt(slot, v, pushed);
		}
	}
	Frame render() {
		if (yoke) {
			DeflectionYokeParams p;
			p.depth = 1.f;
			p.lines = kLines;
			p.signalSlotId = slot;
			videoBusPublishDeflectionYoke(41, 40, p);
		}
		else {
			ScanProcessorParams p;
			p.lines = kLines;
			p.signalSlotId = slot;
			p.lumaGain = 0.f;
			p.persist = 0.f;
			p.glow = 0.f;
			p.intensity = 1.f;
			videoBusPublishScanProcessor(41, 40, p);
		}
		return renderer.renderBus(41, kW, kH);
	}
};

// Luma-weighted centre column of each row, NaN where the row is dark.
std::vector<double> centroids(const Frame& f) {
	std::vector<double> c(static_cast<size_t>(kH), std::nan(""));
	double top = 0.0;
	std::vector<double> sums(static_cast<size_t>(kH), 0.0);
	for (int y = 0; y < kH; ++y) {
		double sum = 0.0;
		double weighted = 0.0;
		for (int x = 0; x < kW; ++x) {
			const size_t i = (static_cast<size_t>(y) * kW + x) * 4u;
			const double luma = (f[i] * 0.2126 + f[i + 1] * 0.7152 + f[i + 2] * 0.0722) / 255.0;
			sum += luma;
			weighted += luma * (x + 0.5);
		}
		sums[static_cast<size_t>(y)] = sum;
		top = std::max(top, sum);
		if (sum > 0.0) {
			c[static_cast<size_t>(y)] = weighted / sum;
		}
	}
	for (int y = 0; y < kH; ++y) {
		if (!(sums[static_cast<size_t>(y)] >= 0.5 * top)) {
			c[static_cast<size_t>(y)] = std::nan("");
		}
	}
	return c;
}

// Largest difference between two pictures' row centres, in pixels, over rows both show.
double worstCentreDiff(const Frame& a, const Frame& b, int* rowsCompared = nullptr) {
	const std::vector<double> ca = centroids(a);
	const std::vector<double> cb = centroids(b);
	double worst = 0.0;
	int n = 0;
	for (int y = 0; y < kH; ++y) {
		const size_t i = static_cast<size_t>(y);
		if (std::isnan(ca[i]) || std::isnan(cb[i])) {
			continue;
		}
		worst = std::max(worst, std::fabs(ca[i] - cb[i]));
		++n;
	}
	if (rowsCompared) {
		*rowsCompared = n;
	}
	return n > 0 ? worst : 1e9;
}

int maxPixelDiff(const Frame& a, const Frame& b) {
	int worst = 0;
	for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
		if ((i & 3u) != 3u) {
			worst = std::max(worst, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
		}
	}
	return worst;
}

// ---- c) the clock itself ----
void testClockBasics() {
	std::printf("video clock: the field is exact and fractional\n");
	const vclock::Clock c48(48000.0, 240);
	check(std::fabs(c48.fieldRateHz() - 60000.0 / 1001.0) < 1e-9, "240 lines is exactly 60000/1001 Hz");
	check(std::fabs(c48.linesPerField() - 262.5) < 1e-12, "a field is 262.5 lines");
	check(std::fabs(c48.samplesPerField() - 800.8) < 1e-9, "a field is 800.8 samples at 48 kHz, not 801");
	check(std::fabs(vclock::Clock(44100.0, 240).samplesPerField() - 735.735) < 1e-9, "735.735 samples at 44.1 kHz");
	check(std::fabs(vclock::Clock(96000.0, 240).samplesPerField() - 1601.6) < 1e-9, "1601.6 samples at 96 kHz");
	check(std::fabs(c48.samplesPerLine() - 48000.0 / vclock::kLineRateHz) < 1e-12, "a line is 3.05 samples at 48 kHz");
	check(std::fabs(vclock::Clock(48000.0, 120).fieldRateHz() * (120.0 + 22.5) - vclock::kLineRateHz) < 1e-6, "the field rate follows the line count");

	const vclock::VideoTime t0 = c48.fromFrame(0.0);
	check(t0.field == 0 && t0.line == 0 && t0.fieldPhase == 0.0 && t0.visible, "frame 0 is the top of field 0");
	const vclock::VideoTime t1 = c48.fromFrame(c48.samplesPerField() * 3.0 + c48.samplesPerLine() * 100.5);
	check(t1.field == 3 && t1.line == 100 && std::fabs(t1.linePhase - 0.5) < 1e-9 && t1.visible, "field, line and phase down the line");
	const vclock::VideoTime t2 = c48.fromFrame(c48.samplesPerLine() * 250.25);
	check(t2.field == 0 && t2.line == 250 && !t2.visible, "line 250 of 262.5 is in the flyback");
	const vclock::VideoTime t3 = c48.fromFrame(-0.5);
	check(t3.field == -1 && t3.fieldPhase > 0.99, "a frame before 0 is the end of field -1");
}

void testRoundTrip() {
	std::printf("video clock: sample -> video time -> sample over an hour\n");
	double worst = 0.0;
	double worstLines = 0.0;
	long checked = 0;
	for (double rate : kRates) {
		for (int lines : {120, 240, 600}) {
			const vclock::Clock clock(rate, lines);
			const int64_t hour = static_cast<int64_t>(3600.0 * rate);
			// Every frame of the first second and the last second, and every 9973rd frame between, with and without a fraction.
			auto visit = [&](int64_t frame, double fraction) {
				const double f = static_cast<double>(frame) + fraction;
				const vclock::VideoTime t = clock.fromFrame(f);
				worst = std::max(worst, std::fabs(clock.toFrame(t) - f));
				worstLines = std::max(worstLines, std::fabs(clock.toFrame(t.field, t.line, t.linePhase) - f));
				if (t.field != clock.fieldOfFrame(frame) && fraction == 0.0) {
					check(false, "fromFrame and fieldOfFrame agree");
				}
				if (!(t.fieldPhase >= 0.0 && t.fieldPhase < 1.0 && t.linePhase >= 0.0 && t.linePhase <= 1.0 && t.line >= 0)) {
					check(false, "phases stay in range");
				}
				++checked;
			};
			for (int64_t f = 0; f < static_cast<int64_t>(rate); ++f) {
				visit(f, 0.0);
			}
			for (int64_t f = static_cast<int64_t>(rate); f < hour - static_cast<int64_t>(rate); f += 9973) {
				visit(f, 0.0);
				visit(f, 0.37);
			}
			for (int64_t f = hour - static_cast<int64_t>(rate); f <= hour; ++f) {
				visit(f, 0.0);
			}
		}
	}
	std::printf("  %ld points, worst error %.3g samples (field phase) and %.3g (line phase)\n", checked, worst, worstLines);
	check(worst < 1e-6, "the round trip is exact to 1e-6 of a sample over an hour");
	check(worstLines < 1e-6, "the line and phase form of the round trip is exact to 1e-6 of a sample");

	// Every field starts where the clock says: field n at frame n x samplesPerField, and the field after it one field later.
	const vclock::Clock clock(48000.0, 240);
	bool contiguous = true;
	for (int64_t n = 0; n < 200000; n += 997) {
		contiguous = contiguous && clock.fromFrame(clock.fieldStartFrame(n)).field == n && clock.fromFrame(clock.fieldStartFrame(n) + 0.001).field == n &&
			clock.fromFrame(clock.fieldStartFrame(n + 1) - 0.001).field == n;
	}
	check(contiguous, "fields tile the time line with no gap and no overlap");
}

// ---- a) a locked tone does not drift ----
constexpr int kFields = 610;

void lockedToneCase(bool yoke, double rate, int multiple, double* worstDrift, double hzOverride = 0.0) {
	const vclock::Clock clock(rate, kLines);
	const double hz = hzOverride > 0.0 ? hzOverride : clock.fieldRateHz() * multiple;
	Rig rig(yoke);
	auto gen = [&](int64_t n, float* v) {
		const float s = static_cast<float>(0.25 * std::sin(2.0 * kPi * hz * static_cast<double>(n) / rate));
		if (yoke) {
			v[kDeflectionYokeDeflectX] = s;
		}
		else {
			v[kScanProcessorDeflectX] = s;
		}
	};
	// Render after field k completes, for the first fields, then every 17th, then the last few.
	Frame first;
	double worst = 0.0;
	int renders = 0;
	for (int k = 3; k < kFields; ++k) {
		const bool wanted = k < 8 || k % 17 == 0 || k >= kFields - 6;
		if (!wanted) {
			continue;
		}
		// Field k is complete once its last sample and the interpolation margin are in.
		rig.pushUntil(rate, static_cast<int64_t>(std::floor((k + 1) * clock.samplesPerField())) + 5, gen);
		SignalWindow w;
		if (!signalRingReadField(rig.slot, w, kLines) || w.field != k) {
			check(false, "the window is the field that was just completed");
			return;
		}
		const Frame out = rig.render();
		if (first.empty()) {
			first = out;
			continue;
		}
		int rows = 0;
		worst = std::max(worst, worstCentreDiff(first, out, &rows));
		check(rows > kH / 2, "most rows are lit and compared");
		++renders;
	}
	*worstDrift = worst;
	std::printf("  %s %6.0f Hz, tone %2d x field rate (%.3f Hz): worst row shift over %d fields %.4f px (%d pictures)\n", yoke ? "yoke" : "scan", rate, multiple, hz, kFields, worst, renders + 1);
}

void testLockedTone() {
	std::printf("video clock: a tone at a whole multiple of the field rate stands still\n");
	for (bool yoke : {true, false}) {
		for (double rate : kRates) {
			for (int multiple : {1, 6}) {
				double worst = 0.0;
				lockedToneCase(yoke, rate, multiple, &worst);
				// The picture is 192 px wide and the tone is 0.25 of it: 48 px of swing. A drift of even one line in 240
				// would move the rows by several pixels. Interpolation noise is far below a tenth of a pixel.
				check(worst < 0.1, "a locked tone draws the same rows after 600 fields");
			}
		}
	}
	// Control: 60.000 Hz is not the field rate (59.94 Hz), so the rows creep, and the test would see it.
	double creep = 0.0;
	lockedToneCase(true, 48000.0, 1, &creep, 60.0);
	check(creep > 2.0, "control: a 60 Hz tone is not locked and its rows move");
}

// ---- b) the same input gives the same picture at every sample rate ----
void testSampleRateIndependence() {
	std::printf("video clock: the same tone draws the same picture at 44.1, 48 and 96 kHz\n");
	struct Case {
		const char* name;
		double hz;
		double tolerancePx;
	};
	// The tolerance comes from linear interpolation between samples: error about (pi f / rate)^2 / 2 of the amplitude
	// (48 px), at the slowest rate. 180 Hz is 3 fields' worth of cycles; 1000 Hz is not a multiple of the field rate.
	const Case cases[] = {{"179.82 Hz", 3.0 * 60000.0 / 1001.0, 0.1}, {"1000 Hz", 1000.0, 0.5}};
	for (bool yoke : {true, false}) {
		for (const Case& cs : cases) {
			Frame pictures[3];
			for (int r = 0; r < 3; ++r) {
				const double rate = kRates[r];
				const vclock::Clock clock(rate, kLines);
				Rig rig(yoke);
				auto gen = [&](int64_t n, float* v) {
					const float s = static_cast<float>(0.25 * std::sin(2.0 * kPi * cs.hz * static_cast<double>(n) / rate));
					v[0] = s;
				};
				const int k = 60;
				rig.pushUntil(rate, static_cast<int64_t>(std::floor((k + 1) * clock.samplesPerField())) + 5, gen);
				SignalWindow w;
				check(signalRingReadField(rig.slot, w, kLines) && w.field == k, "field 60 is the newest complete field at every rate");
				pictures[r] = rig.render();
			}
			double worst = 0.0;
			int worstPixels = 0;
			for (int a = 0; a < 3; ++a) {
				for (int b = a + 1; b < 3; ++b) {
					worst = std::max(worst, worstCentreDiff(pictures[a], pictures[b]));
					worstPixels = std::max(worstPixels, maxPixelDiff(pictures[a], pictures[b]));
				}
			}
			std::printf("  %s %s: worst row-centre difference between rates %.4f px, brightest pixel difference %d of 255\n", yoke ? "yoke" : "scan", cs.name, worst, worstPixels);
			check(worst < cs.tolerancePx, "the picture agrees across sample rates to within the stated interpolation tolerance");
		}
	}
}

// ---- the field rule: ring, renderer and Scan Sync agree ----
void testFieldAgreement() {
	std::printf("video clock: the picture's field, the ring window and the Scan Sync pulse agree\n");
	for (double rate : kRates) {
		const vclock::Clock clock(rate, kLines);
		// A ring that began late (frame 100003 is its first sample), as a module added to a running patch does.
		const int slot = signalRingAllocate();
		signalRingSetSampleRate(slot, static_cast<float>(rate));
		const int64_t begin = 100003;
		const int64_t end = begin + static_cast<int64_t>(rate * 2.0);
		int64_t lastField = -1;
		int64_t topFrame = -1;
		bool windowsRight = true;
		bool pulsesRight = true;
		int fields = 0;
		for (int64_t f = begin; f < end; ++f) {
			float v[kSignalRingChannels] = {static_cast<float>(f - begin), 0.f, 0.f, 0.f};
			signalRingPushAt(slot, v, f);
			const scan_sync::Outputs o = scan_sync::at(clock, f);
			const vclock::VideoTime t = clock.fromFrame(static_cast<double>(f));
			// The first sample of a field is the first frame at or after the fractional top of that field.
			if (t.field != lastField) {
				const bool partial = lastField < 0;  // the ring began part way into a field: not a top
				lastField = t.field;
				topFrame = f;
				pulsesRight = pulsesRight && (partial || o.top > 5.f && static_cast<double>(f) - clock.fieldStartFrame(t.field) < 1.0 && static_cast<double>(f) >= clock.fieldStartFrame(t.field) - 1e-6);
			}
			else if (f - topFrame > 1 && static_cast<double>(f - topFrame) > 0.001 * rate + 1.0) {
				pulsesRight = pulsesRight && o.top < 5.f;  // the pulse is over after 1 ms
			}
			// The newest complete field's window starts at that field's top: base sample + startFraction = n x spf.
			if ((f - begin) % 301 == 0) {
				SignalWindow w;
				if (signalRingReadField(slot, w, kLines)) {
					const double top = static_cast<double>(begin) + static_cast<double>(w.data[0]) + w.startFraction;
					windowsRight = windowsRight && std::fabs(top - clock.fieldStartFrame(w.field)) < 1e-9 && w.length == static_cast<int>(std::floor(clock.samplesPerField())) + 3 &&
						w.fieldWindow && w.startFraction >= 0.0 && w.startFraction < 1.0;
					++fields;
				}
			}
		}
		signalRingRelease(slot);
		check(windowsRight && fields > 10, "every window starts exactly at its field's top, wherever the ring began");
		check(pulsesRight, "the top pulse rises on the first frame at or after the top of the field, and lasts 1 ms");
	}

	// Scan Sync outputs
	const vclock::Clock clock(48000.0, 240);
	int topCount = 0;
	int lineCount = 0;
	bool rampsOk = true;
	float lastDown = 0.f;
	for (int64_t f = 0; f < 48000; ++f) {
		const scan_sync::Outputs o = scan_sync::at(clock, f);
		const scan_sync::Outputs p = scan_sync::at(clock, f > 0 ? f - 1 : 0);
		topCount += (o.top > 5.f && p.top < 5.f) || (f == 0 && o.top > 5.f);
		lineCount += (o.line > 5.f && p.line < 5.f) || (f == 0 && o.line > 5.f);
		rampsOk = rampsOk && o.down >= 0.f && o.down <= 10.f && o.across >= 0.f && o.across <= 10.f;
		if (o.down + 1e-4f < lastDown) {
			rampsOk = rampsOk && lastDown > 9.99f && o.down < 1.f;  // falls only by the jump back to the top
		}
		lastDown = o.down;
	}
	std::printf("  one second at 48 kHz: %d picture pulses, %d line pulses\n", topCount, lineCount);
	check(topCount == 60 && rampsOk, "59.94 picture pulses in a second (60 here) and both ramps stay within 0..10 V and only jump at the top");
	check(lineCount > 15700 && lineCount <= 15740, "about 15734 line pulses a second (the half line at the end of a field may fall between two samples)");
	const scan_sync::Outputs half = scan_sync::at(clock, static_cast<int64_t>(clock.samplesPerField() * 0.5 * clock.visibleFraction()));
	check(std::fabs(half.down - 5.f) < 0.05f, "the picture ramp is at 5 V half way down the visible lines");
	const scan_sync::Outputs flyback = scan_sync::at(clock, static_cast<int64_t>(clock.samplesPerField() * 0.99));
	check(flyback.down > 9.99f, "the picture ramp holds 10 V through the flyback");
}

// ---- the ring across threads (this is the part `make tsan` runs): one writer stamping frames, one reader taking fields ----
void testRingThreads() {
	std::printf("video clock: one writer, one reader, no torn windows\n");
	const double rate = 48000.0;
	const vclock::Clock clock(rate, kLines);
	const int slot = signalRingAllocate();
	signalRingSetSampleRate(slot, static_cast<float>(rate));
	std::atomic<bool> done {false};
	const int64_t total = 600000;
	std::thread writer([&] {
		for (int64_t f = 0; f < total; ++f) {
			// A gap in the frame numbers half way, as when a module is bypassed and brought back: the ring restarts.
			const int64_t frame = f < total / 2 ? f : f + 5000;
			float v[kSignalRingChannels] = {static_cast<float>(frame), 0.f, 0.f, 0.f};
			signalRingPushAt(slot, v, frame);
		}
		done.store(true);
	});
	long reads = 0;
	long bad = 0;
	SignalWindow w;
	while (!done.load()) {
		if (!signalRingReadField(slot, w, kLines)) {
			continue;
		}
		++reads;
		bool ok = w.fieldWindow && w.length == static_cast<int>(std::floor(clock.samplesPerField())) + 3;
		for (int i = 1; i < w.length && ok; ++i) {
			ok = w.data[static_cast<size_t>(i)] == w.data[0] + static_cast<float>(i);  // consecutive frames: not torn
		}
		const double top = static_cast<double>(w.data[0]) + w.startFraction;
		ok = ok && std::fabs(top - clock.fieldStartFrame(w.field)) < 1e-6;
		bad += ok ? 0 : 1;
	}
	writer.join();
	signalRingRelease(slot);
	std::printf("  %ld windows read while the writer ran, %ld bad\n", reads, bad);
	check(bad == 0, "every window read while the writer runs is whole and starts at its field's top");
}

}  // namespace

int main(int argc, char** argv) {
	if (argc > 1 && std::string(argv[1]) == "thread") {
		testRingThreads();
		std::printf("video_clock_test (thread): %d checks, %d failed\n", checks, failures);
		return failures == 0 ? 0 : 1;
	}
	testRingThreads();
	testClockBasics();
	testRoundTrip();
	testFieldAgreement();
	testLockedTone();
	testSampleRateIndependence();
	std::printf("video_clock_test: %d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
