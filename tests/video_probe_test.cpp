// Offline checks for Video Probe (VRU-26): reading a gradient, the plus and minus 5 V reach and clamp, SIZE across
// a boundary, AVG and MOTION on known pairs, GATE hysteresis, colour bars, smoothing, and the lock-free hand-off.
// Needs no Rack SDK.
#include "video_probe_math.hpp"
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <memory>
#include <thread>

namespace vp = video_probe;
static int checks = 0, failures = 0;
static void check(bool ok, const char* msg) {
	++checks;
	if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", msg); }
}
static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

using Pic = std::vector<unsigned char>;
template <class F>
static Pic make(F pixel) {
	Pic p(static_cast<size_t>(vp::kW) * vp::kH * 4);
	for (int y = 0; y < vp::kH; ++y)
		for (int x = 0; x < vp::kW; ++x) {
			unsigned char* q = &p[(static_cast<size_t>(y) * vp::kW + x) * 4];
			int rgb[3];
			pixel(x, y, rgb);
			q[0] = static_cast<unsigned char>(rgb[0]); q[1] = static_cast<unsigned char>(rgb[1]);
			q[2] = static_cast<unsigned char>(rgb[2]); q[3] = 255;
		}
	return p;
}

int main() {
	auto frame = std::make_unique<vp::Frame>();
	vp::Analyzer an;

	// A left to right grey gradient: column x is x * 255 / 319.
	auto grad = make([](int x, int, int* c) { c[0] = c[1] = c[2] = static_cast<int>(std::lround(x * 255.0 / (vp::kW - 1))); });
	an.analyze(grad.data(), *frame);
	check(near(vp::sample(*frame, 0.f, 0.f, 0.f).luma * 10.f, 5.f, .02f), "gradient reads mid grey at X 0 V");
	check(near(vp::sample(*frame, -5.f, 0.f, 0.f).luma * 10.f, 0.f, .02f), "gradient reads black at X -5 V");
	check(near(vp::sample(*frame, 5.f, 0.f, 0.f).luma * 10.f, 10.f, .02f), "gradient reads white at X +5 V");
	check(near(vp::sample(*frame, 2.5f, 0.f, 0.f).luma * 10.f, 7.5f, .03f), "gradient reads 7.5 V at X +2.5 V");
	check(near(vp::sample(*frame, 2.5f, 4.f, 0.f).luma, vp::sample(*frame, 2.5f, -4.f, 0.f).luma, 1e-4f), "a horizontal gradient does not change with Y");
	// Between pixel centres the read is a straight blend (bilinear), not a staircase.
	{
		float prev = -1.f; bool smooth = true;
		for (int i = 0; i <= 200; ++i) {
			const float v = -1.f + i * .01f;
			const float l = vp::sample(*frame, v, 0.f, 0.f).luma;
			if (prev >= 0.f && (l < prev - 1e-6f || l - prev > .0015f)) smooth = false;
			prev = l;
		}
		check(smooth, "sweeping X in small steps moves LUMA in small steps, never backwards");
	}
	// Reach and clamp.
	check(vp::sample(*frame, 9.f, 0.f, 0.f).luma == vp::sample(*frame, 5.f, 0.f, 0.f).luma, "X beyond +5 V clamps to the right edge");
	check(vp::sample(*frame, -40.f, 0.f, 0.f).luma == vp::sample(*frame, -5.f, 0.f, 0.f).luma, "X beyond -5 V clamps to the left edge");
	check(vp::sample(*frame, NAN, 0.f, 0.f).luma == vp::sample(*frame, 0.f, 0.f, 0.f).luma, "NaN reads as 0 V");
	check(vp::position(4.f, 1.f, 3.f) == 5.f && vp::position(-4.f, 1.f, -3.f) == -5.f, "knob plus jack clamps to +/-5 V");
	check(near(vp::position(1.f, .5f, 2.f), 2.f, 1e-6f) && near(vp::position(1.f, -1.f, 2.f), -1.f, 1e-6f), "the amount trimmer scales and flips the jack");

	// Y: +5 V is the top of the picture.
	{
		auto vert = make([](int, int y, int* c) { c[0] = c[1] = c[2] = y < vp::kH / 2 ? 255 : 0; });   // top half white
		an.analyze(vert.data(), *frame);
		check(vp::sample(*frame, 0.f, 4.f, 0.f).luma > .99f, "Y +4 V reads the top of the picture");
		check(vp::sample(*frame, 0.f, -4.f, 0.f).luma < .01f, "Y -4 V reads the bottom of the picture");
	}

	// SIZE across a boundary: left half black, right half white.
	{
		auto half = make([](int x, int, int* c) { c[0] = c[1] = c[2] = x < vp::kW / 2 ? 0 : 255; });
		an.analyze(half.data(), *frame);
		check(near(vp::sample(*frame, 0.f, 0.f, 1.f).luma, .5f, 1e-4f), "SIZE at maximum reads mid grey at the boundary");
		check(near(vp::sample(*frame, 0.f, 0.f, .5f).luma, .5f, 1e-4f), "SIZE at half reads mid grey at the boundary");
		check(vp::sample(*frame, 0.f, 0.f, 0.f).luma > .2f && vp::sample(*frame, 0.f, 0.f, 0.f).luma < .8f, "SIZE at minimum on the boundary blends only the two pixels beside it");
		check(vp::sample(*frame, -.1f, 0.f, 0.f).luma < .01f && vp::sample(*frame, .1f, 0.f, 0.f).luma > .99f, "SIZE at minimum reads one point either side of the boundary");
		check(vp::sample(*frame, 2.5f, 0.f, 0.f).luma > .99f, "SIZE at minimum well inside white reads white");
		const float wide = vp::sample(*frame, 2.5f, 0.f, 1.f).luma;
		check(wide > .5f && wide < .99f, "a wide SIZE reaches over the boundary at X +2.5 V");
		// A box that stays inside one half reads that half exactly.
		check(near(vp::sample(*frame, 4.f, 0.f, .2f).luma, 1.f, 1e-4f), "a small box inside the white half reads exactly white");
		// SIZE grows monotonically across the boundary region.
		float prev = vp::sample(*frame, 1.f, 0.f, 0.f).luma; bool mono = true;
		for (int i = 1; i <= 10; ++i) {
			const float l = vp::sample(*frame, 1.f, 0.f, i * .1f).luma;
			if (l > prev + 1e-6f) mono = false;
			prev = l;
		}
		check(mono, "growing SIZE next to black only ever darkens the read");
		// AVG on this picture.
		check(near(frame->avg, .5f, 1e-4f), "AVG of a half black half white picture is 0.5");
	}

	// Box average against a brute-force average, at awkward positions and sizes (edges included).
	{
		auto noise = make([](int x, int y, int* c) { c[0] = (x * 37 + y * 11) & 255; c[1] = (x * 5 + y * 91) & 255; c[2] = (x * 17 ^ y * 3) & 255; });
		an.analyze(noise.data(), *frame);
		double worst = 0.0;
		const float xs[] = {-5.f, -4.3f, 0.f, 1.234f, 4.9f, 5.f};
		const float ys[] = {-5.f, -2.1f, 0.f, 3.3f, 5.f};
		const float sizes[] = {0.f, .1f, .3f, .7f, 1.f};
		for (float xv : xs) for (float yv : ys) for (float sz : sizes) {
			const vp::Point p = vp::sample(*frame, xv, yv, sz);
			const double cx = vp::pixelX(xv), cy = vp::pixelY(yv), h = vp::halfWidthPx(sz);
			const double x0 = std::max(cx - h, 0.0), x1 = std::min(cx + h, (double)vp::kW);
			const double y0 = std::max(cy - h, 0.0), y1 = std::min(cy + h, (double)vp::kH);
			double acc = 0, area = 0;
			for (int y = 0; y < vp::kH; ++y) for (int x = 0; x < vp::kW; ++x) {
				const double wx = std::max(0.0, std::min(x + 1.0, x1) - std::max((double)x, x0));
				const double wy = std::max(0.0, std::min(y + 1.0, y1) - std::max((double)y, y0));
				if (wx * wy <= 0) continue;
				acc += wx * wy * noise[(static_cast<size_t>(y) * vp::kW + x) * 4 + 1] / 255.0; area += wx * wy;
			}
			worst = std::max(worst, std::fabs(acc / area - p.g));
		}
		check(worst < 1e-4, "box average matches a brute-force average at edges and odd sizes");
	}

	// Colour bars: white, yellow, cyan, green, magenta, red, blue, black, 40 pixels each.
	{
		static const int bars[8][3] = {{255,255,255},{255,255,0},{0,255,255},{0,255,0},{255,0,255},{255,0,0},{0,0,255},{0,0,0}};
		auto cb = make([](int x, int, int* c) { const int k = x / 40; c[0] = bars[k][0]; c[1] = bars[k][1]; c[2] = bars[k][2]; });
		an.analyze(cb.data(), *frame);
		bool ok = true;
		for (int k = 0; k < 8; ++k) {
			const float xv = (k * 40 + 20) / 32.f - 5.f;
			const vp::Point p = vp::sample(*frame, xv, 0.f, 0.f);
			if (!near(p.r, bars[k][0] / 255.f, .005f) || !near(p.g, bars[k][1] / 255.f, .005f) || !near(p.b, bars[k][2] / 255.f, .005f)) ok = false;
		}
		check(ok, "R, G and B read each colour bar");
		check(near(vp::sample(*frame, (3 * 40 + 20) / 32.f - 5.f, 0.f, 0.f).luma, (150 / 256.f), .005f), "green bar reads the green share of LUMA");
		check(vp::sample(*frame, (5 * 40 + 20) / 32.f - 5.f, 0.f, 0.f).g < .005f, "red bar has no green");
	}

	// AVG and MOTION on known pairs. The analyser needs a first picture before it can report a change.
	{
		vp::Analyzer a2;
		auto black = make([](int, int, int* c) { c[0] = c[1] = c[2] = 0; });
		auto white = make([](int, int, int* c) { c[0] = c[1] = c[2] = 255; });
		auto grey = make([](int, int, int* c) { c[0] = c[1] = c[2] = 16; });
		a2.analyze(black.data(), *frame);
		check(frame->avg == 0.f && frame->motion == 0.f, "a black first picture reads AVG 0 and MOTION 0");
		a2.analyze(white.data(), *frame);
		check(near(frame->avg, 1.f, 1e-6f) && near(frame->motion, 1.f, 1e-6f), "black to white reads AVG 10 V and MOTION 10 V");
		a2.analyze(white.data(), *frame);
		check(frame->motion == 0.f, "the same picture twice reads MOTION 0");
		a2.analyze(black.data(), *frame);
		a2.analyze(grey.data(), *frame);
		check(near(frame->avg, 16 / 255.f, 1e-5f), "AVG of flat 16 is 16/255");
		check(near(frame->motion, 16 / 255.f * vp::kMotionGain, 1e-4f), "a small change reads that change times the motion gain");
		a2.reset();
		a2.analyze(white.data(), *frame);
		check(frame->motion == 0.f, "after reset the next picture has nothing to compare with");
	}

	// GATE hysteresis around LEVEL 0.5.
	{
		vp::Gate g;
		check(!g.process(.52f, .5f), "just above LEVEL does not open the gate yet");
		check(g.process(.54f, .5f), "above LEVEL plus the margin opens the gate");
		check(g.process(.48f, .5f), "just below LEVEL keeps it open");
		check(!g.process(.46f, .5f), "below LEVEL minus the margin closes it");
		check(!g.process(.49f, .5f), "and it stays closed inside the margin");
	}

	// Smoothing: off is a straight copy; on moves a step gradually, and a longer SMOOTH is slower.
	{
		const float dt = 1.f / 48000.f;
		check(vp::smoothCoeff(0.f, dt) == 1.f, "SMOOTH 0 is no slew");
		auto settle = [&](float s) { float y = 0.f; const float a = vp::smoothCoeff(s, dt); int n = 0; while (y < .63f && n < 10000000) { y += (1.f - y) * a; ++n; } return n * dt; };
		const float t1 = settle(.3f), t2 = settle(.7f);
		check(t1 > .0005f && t2 > t1 * 5.f, "a larger SMOOTH is slower");
		check(near(settle(1.f), 1.f, .05f), "SMOOTH at maximum has a time constant of about a second");
	}

	// Hand-off: one thread publishes pictures filled with a running number, the other reads. Every read must be
	// one whole picture (all slots the same number), numbers never go backwards, and the last one arrives.
	{
		std::unique_ptr<vp::Frame[]> slots(new vp::Frame[3]());
		vp::TripleBuffer tb;
		const int total = 3000;
		std::atomic<bool> torn{false}, backwards{false};
		std::thread writer([&] {
			for (int n = 1; n <= total; ++n) {
				vp::Frame& f = slots[tb.writeIndex()];
				f.avg = static_cast<float>(n);
				for (int i = 0; i < vp::kSatSize; i += 97) f.sat[0][i] = static_cast<uint32_t>(n);
				f.sat[0][vp::kSatSize - 1] = static_cast<uint32_t>(n);
				f.valid = true;
				tb.publish();
			}
		});
		float last = 0.f;
		int seen = 0;
		while (last < total && seen < 200000000) {
			const vp::Frame& f = slots[tb.readIndex()];
			const float n = f.avg;
			if (n > 0.f) {
				for (int i = 0; i < vp::kSatSize; i += 97 * 53) if (f.sat[0][i] != static_cast<uint32_t>(n)) torn = true;
				if (f.sat[0][vp::kSatSize - 1] != static_cast<uint32_t>(n)) torn = true;
				if (n < last) backwards = true;
				last = n;
			}
			++seen;
		}
		writer.join();
		check(!torn, "the reader never sees a half-written picture");
		check(!backwards, "the reader never sees an older picture after a newer one");
		check(slots[tb.readIndex()].avg == static_cast<float>(total), "the reader ends on the last picture published");
	}

	// FRAME: one 10 V, 1 ms pulse per delivered picture; none when no new picture arrives; LUMA, R, G and B change
	// only at or after a pulse. This runs the same Reader that process() runs, with the real hand-off.
	{
		std::unique_ptr<vp::Frame[]> slots(new vp::Frame[3]());
		vp::TripleBuffer tb;
		vp::Reader rd;
		vp::Analyzer a3;
		const float dt = 1.f / 48000.f;
		auto grey = [](int v) { return make([v](int, int, int* c) { c[0] = c[1] = c[2] = v; }); };
		auto deliver = [&](const Pic& pic) { a3.analyze(pic.data(), slots[tb.writeIndex()]); tb.publish(); };
		std::vector<float> frameV, lumaV, rV;
		auto run = [&](int n) {
			for (int i = 0; i < n; ++i) {
				const vp::Reader::Out o = rd.process(tb, slots.get(), 0.f, 0.f, 0.f, 0.f, .5f, dt);
				frameV.push_back(o.frame); lumaV.push_back(o.luma); rV.push_back(o.r);
			}
		};
		run(500);
		check(std::all_of(frameV.begin(), frameV.end(), [](float v) { return v == 0.f; }), "FRAME stays at 0 V until a picture arrives");
		const int greys[] = {40, 90, 140, 190, 240, 40};
		std::vector<size_t> deliveredAt;
		for (int k = 0; k < 6; ++k) {
			deliveredAt.push_back(frameV.size());
			deliver(grey(greys[k]));
			run(800);   // 60 pictures a second
		}
		// Two pictures delivered before process() looks count once, and nothing is delivered for a long while.
		deliver(grey(10)); deliver(grey(20));
		const size_t twoAt = frameV.size();
		run(4000);

		std::vector<size_t> rises, falls;
		for (size_t i = 0; i < frameV.size(); ++i) {
			const float prev = i ? frameV[i - 1] : 0.f;
			if (frameV[i] > 0.f && prev == 0.f) rises.push_back(i);
			if (frameV[i] == 0.f && prev > 0.f) falls.push_back(i);
		}
		check(rises.size() == 7, "FRAME gives one pulse per delivered picture (six, then two read as one) and none in between");
		bool where = rises.size() == 7;
		for (int k = 0; where && k < 6; ++k) where = rises[k] == deliveredAt[k];
		check(where && rises[6] == twoAt, "each pulse rises on the sample that takes the new picture");
		bool lens = falls.size() == rises.size();
		for (size_t k = 0; lens && k < rises.size(); ++k) lens = falls[k] - rises[k] == 48;
		check(lens, "each FRAME pulse is 1 ms (48 samples at 48 kHz)");
		check(std::all_of(frameV.begin(), frameV.end(), [](float v) { return v == 0.f || v == 10.f; }), "FRAME is 0 V or 10 V and nothing else");
		// LUMA and R never change before a pulse rises: every change sits on a rising edge, or on the sample after it
		// (with SMOOTH off the one-pole copy can land one float step off its target and settle on the next sample).
		bool onlyAtPulse = true; int atEdge = 0;
		for (size_t i = 1; i < lumaV.size(); ++i) {
			if (lumaV[i] != lumaV[i - 1] || rV[i] != rV[i - 1]) {
				const bool edge = frameV[i] == 10.f && frameV[i - 1] == 0.f;
				const bool settle = i >= 2 && frameV[i - 1] == 10.f && frameV[i - 2] == 0.f;
				if (!edge && !settle) onlyAtPulse = false;
				if (edge) ++atEdge;
			}
		}
		check(onlyAtPulse && atEdge == 7, "LUMA and R change only at or just after a FRAME pulse rises (7 pictures, 7 changes on the edge)");
		check(near(lumaV[deliveredAt[2] + 100], 140 / 255.f * 10.f, .01f), "LUMA holds the delivered picture's value until the next one");
		// Running the audio side with no new picture never pulses (already covered by the 4000 samples above), and
		// the pulse follows the sample rate.
		vp::FrameClock c96;
		int on96 = 0;
		for (int i = 0; i < 400; ++i) on96 += c96.process(i == 10, 1.f / 96000.f) > 0.f;
		check(on96 == 96, "a 1 ms pulse is 96 samples at 96 kHz");
	}

	// The video-to-audio rule. The picture holds still. LUMA is its brightness along the path X and Y trace, so a
	// sawtooth on X across a left to right ramp is a sawtooth in LUMA at the same frequency, and a sawtooth on X across
	// N vertical bars is a tone at N times that frequency.
	{
		const double sr = 48000.0;
		const float dt = static_cast<float>(1.0 / sr);
		auto sweep = [&](const Pic& pic, double f, int cycles, std::vector<float>& xs, std::vector<float>& out) {
			std::unique_ptr<vp::Frame[]> slots(new vp::Frame[3]());
			vp::TripleBuffer tb;
			vp::Reader rd;
			vp::Analyzer a4;
			a4.analyze(pic.data(), slots[tb.writeIndex()]);
			tb.publish();
			const int n = static_cast<int>(std::lround(sr / f)) * cycles;
			for (int i = 0; i < n; ++i) {
				const double phase = std::fmod(f * i / sr, 1.0);
				const float x = static_cast<float>(-5.0 + 10.0 * phase);   // sawtooth, -5 V to +5 V, Y fixed at 0 V
				const vp::Reader::Out o = rd.process(tb, slots.get(), x, 0.f, 0.f, 0.f, .5f, dt);
				xs.push_back(x); out.push_back(o.luma);
			}
		};

		// A static left to right ramp, black to white: column x is x * 255 / 319.
		auto ramp = make([](int x, int, int* c) { c[0] = c[1] = c[2] = static_cast<int>(std::lround(x * 255.0 / (vp::kW - 1))); });
		const double f = 100.0;
		const int cycles = 10;
		std::vector<float> xs, luma;
		sweep(ramp, f, cycles, xs, luma);
		// Predicted from the ramp: X volts spans -5 to +5 across the picture, LUMA is 0 to 10 V, so LUMA = X + 5 V
		// (amplitude 10 V peak to peak, offset 5 V). The read is bilinear between pixel centres and the ramp has
		// 8-bit steps, so the allowed error is 0.05 V.
		double worst = 0.0, lo = 1e9, hi = -1e9;
		for (size_t i = 0; i < luma.size(); ++i) {
			worst = std::max(worst, static_cast<double>(std::fabs(luma[i] - (xs[i] + 5.f))));
			lo = std::min(lo, static_cast<double>(luma[i])); hi = std::max(hi, static_cast<double>(luma[i]));
		}
		std::printf("ramp sweep: worst error %.4f V, range %.4f to %.4f V\n", worst, lo, hi);
		check(worst < .05, "a sawtooth on X across a ramp gives LUMA = X + 5 V to within 0.05 V");
		check(near(static_cast<float>(hi - lo), 10.f, .05f) && near(static_cast<float>((hi + lo) * .5), 5.f, .05f), "the LUMA sawtooth is 10 V peak to peak with an offset of 5 V");
		int wraps = 0, rising = 0;
		for (size_t i = 1; i < luma.size(); ++i) {
			if (luma[i] < luma[i - 1] - 5.f) ++wraps;
			if (luma[i - 1] < 5.f && luma[i] >= 5.f) ++rising;
		}
		check(wraps == cycles - 1 && rising == cycles, "LUMA rises through its mid level once per X cycle and falls back once per X cycle: a sawtooth at f");

		// N white bars on black, N = 5, each bar and gap 32 pixels (a pattern 64 pixels long).
		const int N = 5;
		auto bars = make([](int x, int, int* c) { c[0] = c[1] = c[2] = (x % (vp::kW / 5)) >= (vp::kW / 5) / 2 ? 255 : 0; });
		const double fb = 40.0;
		std::vector<float> xb, lb;
		sweep(bars, fb, cycles, xb, lb);
		int up = 0, down = 0;
		for (size_t i = 1; i < lb.size(); ++i) {
			if (lb[i - 1] < 5.f && lb[i] >= 5.f) ++up;
			if (lb[i - 1] >= 5.f && lb[i] < 5.f) ++down;
		}
		std::printf("bars sweep: %d rising and %d falling mid-level crossings in %d cycles\n", up, down, cycles);
		// The last sweep ends on a white bar, and the fall back to black belongs to the cycle after the window.
		check(up == N * cycles && down == N * cycles - 1, "N bars swept at f cross the mid level N times per sweep: zero crossings count N times f");
		// The strongest harmonic of f in the output is the Nth.
		double best = 0.0; int bestK = 0;
		for (int k = 1; k <= 12; ++k) {
			double re = 0.0, im = 0.0;
			for (size_t i = 0; i < lb.size(); ++i) {
				const double ph = 2.0 * M_PI * k * fb * i / sr;
				re += (lb[i] - 5.0) * std::cos(ph); im += (lb[i] - 5.0) * std::sin(ph);
			}
			const double m = std::sqrt(re * re + im * im) / lb.size();
			if (m > best) { best = m; bestK = k; }
		}
		check(bestK == N, "LUMA's fundamental is N times the sweep frequency");
	}

	std::printf("video_probe_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
