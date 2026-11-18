// Offline checks for the raster rule (src/raster_scan.hpp): Video Probe's SCAN mode (picture -> signal) and Raster
// (VRU-29, signal -> picture). Needs no Rack SDK.
//
//   - Probe with X and Y at fixed voltages returns the known pixel; with SCAN off nothing changed.
//   - SCAN over vertical bars gives the right number of cycles a line (at 192 kHz, where a line has 12 samples).
//   - Round trip: test picture -> Video Probe scan -> Raster at the same LINES reproduces the picture averaged over each
//     sample's cell, at 44.1, 48 and 96 kHz and at three line counts, in every field up to field 600 (no drift).
//   - A processing step between them (invert) gives the inverted picture.
//   - Raster with X and Y placed: dots, joins, and one axis placed with the other on the raster.
//   - `raster_scan_test thread` (make tsan): the ring and the picture hand-off across threads.
#include "raster_scan.hpp"
#include "scan_sync_math.hpp"
#include "video_probe_math.hpp"
#include "video_bus.hpp"
#include "video_graph_renderer.hpp"
#include "signal_ring.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace vp = video_probe;
static int checks = 0, failures = 0;
static void check(bool ok, const std::string& msg) {
	++checks;
	if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", msg.c_str()); }
}

constexpr int W = vp::kW, H = vp::kH;
using Pic = std::vector<unsigned char>;

template <class F>
static Pic make(F pixel) {
	Pic p(static_cast<size_t>(W) * H * 4);
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			int rgb[3];
			pixel(x, y, rgb);
			unsigned char* q = &p[(static_cast<size_t>(y) * W + x) * 4];
			q[0] = static_cast<unsigned char>(rgb[0]); q[1] = static_cast<unsigned char>(rgb[1]);
			q[2] = static_cast<unsigned char>(rgb[2]); q[3] = 255;
		}
	return p;
}

// What Video Probe's module owns: three frames and a hand-off.
struct ProbeRig {
	std::unique_ptr<vp::Frame[]> frames{new vp::Frame[3]()};
	vp::TripleBuffer tb;
	vp::Analyzer an;
	vp::Reader reader;
	void show(const Pic& pic) {
		an.analyze(pic.data(), frames[tb.writeIndex()]);
		tb.publish();
	}
};

// A picture with something different in every channel and place: red across, green down, blue a checker.
static Pic testPicture() {
	return make([](int x, int y, int* c) {
		c[0] = static_cast<int>(std::lround(x * 255.0 / (W - 1)));
		c[1] = static_cast<int>(std::lround(y * 255.0 / (H - 1)));
		c[2] = (((x / 20) + (y / 12)) & 1) ? 230 : 25;
	});
}

// ---- the reference: the picture averaged over each sample's cell, worked out straight from the picture ----
struct Ref {
	std::vector<double> rgb;   // W x H x 3, 0..255
};

static double boxMean(const Pic& pic, int ch, double x0, double x1, double y0, double y1) {
	double sum = 0.0, area = 0.0;
	for (int y = static_cast<int>(std::floor(y0)); y < static_cast<int>(std::ceil(y1)) && y < H; ++y) {
		const double wy = std::min<double>(y + 1, y1) - std::max<double>(y, y0);
		if (wy <= 0) continue;
		for (int x = static_cast<int>(std::floor(x0)); x < static_cast<int>(std::ceil(x1)) && x < W; ++x) {
			const double wx = std::min<double>(x + 1, x1) - std::max<double>(x, x0);
			if (wx <= 0) continue;
			sum += wx * wy * pic[(static_cast<size_t>(y) * W + x) * 4 + ch];
			area += wx * wy;
		}
	}
	return area > 0 ? sum / area : 0.0;
}

static Ref reference(const Pic& pic, double rate, int lines, int64_t field, int slow = 1) {
	const vclock::Clock clock(rate, lines, slow);
	Ref ref;
	ref.rgb.assign(static_cast<size_t>(W) * H * 3, 0.0);
	// The cells of every line of this field.
	std::vector<std::vector<raster_scan::Cell>> cells(static_cast<size_t>(lines));
	const int64_t first = static_cast<int64_t>(std::floor(clock.fieldStartFrame(field))) - 1;
	const int64_t last = static_cast<int64_t>(std::ceil(clock.fieldStartFrame(field + 1))) + 1;
	for (int64_t f = first; f <= last; ++f) {
		if (clock.fromFrame(static_cast<double>(f)).field != field) continue;
		const raster_scan::Cell c = raster_scan::cellAt(clock, f);
		if (c.visible && c.line < lines) cells[static_cast<size_t>(c.line)].push_back(c);
	}
	for (int y = 0; y < H; ++y) {
		const int line = std::min(lines - 1, static_cast<int>(std::floor((y + 0.5) / H * lines)));
		const double y0 = static_cast<double>(line) * H / lines, y1 = static_cast<double>(line + 1) * H / lines;
		for (int x = 0; x < W; ++x) {
			const double uc = (x + 0.5) / W;
			for (const raster_scan::Cell& c : cells[static_cast<size_t>(line)]) {
				if (uc >= c.u0 && (uc < c.u1 || c.u1 >= 1.0)) {
					for (int ch = 0; ch < 3; ++ch) ref.rgb[(static_cast<size_t>(y) * W + x) * 3 + ch] = boxMean(pic, ch, c.u0 * W, c.u1 * W, y0, y1);
					break;
				}
			}
		}
	}
	return ref;
}

// ---- 1. Probe at fixed X and Y; SCAN off is the old behaviour ----
static void testFixedPoint() {
	ProbeRig rig;
	const Pic pic = testPicture();
	rig.show(pic);
	const vclock::Clock vc(48000.0, 240);
	int bad = 0, tried = 0;
	for (int px : {0, 1, 100, 160, 319}) {
		for (int py : {0, 7, 120, 239}) {
			const float xv = static_cast<float>((px + 0.5) / W * 10.0 - 5.0);
			const float yv = static_cast<float>(5.0 - (py + 0.5) / H * 10.0);
			const vp::Reader::Out o = rig.reader.processScan(rig.tb, rig.frames.get(), vc, 1000, false, false, xv, yv, 0.f, 0.f, .5f, 1.f / 48000.f);
			const unsigned char* q = &pic[(static_cast<size_t>(py) * W + px) * 4];
			++tried;
			if (std::fabs(o.r - q[0] / 255.f * 10.f) > .03f || std::fabs(o.g - q[1] / 255.f * 10.f) > .03f || std::fabs(o.b - q[2] / 255.f * 10.f) > .03f) ++bad;
		}
	}
	check(bad == 0, "Probe at fixed X and Y returns the known pixel's R, G and B (" + std::to_string(bad) + " of " + std::to_string(tried) + " off)");

	// SCAN off (both axes false) is bit for bit the old process().
	ProbeRig a, b;
	a.show(pic); b.show(pic);
	bool same = true;
	for (int i = 0; i < 2000; ++i) {
		const float xv = std::sin(i * .01f) * 5.f, yv = std::cos(i * .013f) * 5.f;
		const vp::Reader::Out p = a.reader.process(a.tb, a.frames.get(), xv, yv, .1f, .2f, .5f, 1.f / 48000.f);
		const vp::Reader::Out q = b.reader.processScan(b.tb, b.frames.get(), vc, i, false, false, xv, yv, .1f, .2f, .5f, 1.f / 48000.f);
		if (p.luma != q.luma || p.r != q.r || p.g != q.g || p.b != q.b || p.avg != q.avg || p.motion != q.motion || p.gate != q.gate || p.frame != q.frame) same = false;
	}
	check(same, "processScan with both scan flags off equals process() exactly");
}

// ---- 2. bars: cycles a line ----
static void testBars() {
	for (const double rate : {48000.0, 192000.0}) {
		const int cycles = rate > 100000.0 ? 3 : 1;   // a line has 3 samples at 48 kHz and 12 at 192 kHz
		ProbeRig rig;
		rig.show(make([&](int x, int, int* c) { c[0] = c[1] = c[2] = ((x * 2 * cycles / W) & 1) ? 0 : 255; }));
		const vclock::Clock vc(rate, 240);
		const int64_t f0 = static_cast<int64_t>(std::ceil(vc.fieldStartFrame(5)));
		const int64_t f1 = static_cast<int64_t>(vc.toFrame(5, 200, 0.0));
		float prev = 0.f;
		int rising = 0;
		bool started = false;
		const int64_t startFrame = static_cast<int64_t>(vc.toFrame(5, 4, 0.0));
		for (int64_t f = f0; f < f1; ++f) {
			const vp::Reader::Out o = rig.reader.processScan(rig.tb, rig.frames.get(), vc, f, true, true, 0.f, 0.f, 0.f, 0.f, .5f, static_cast<float>(1.0 / rate));
			if (f >= startFrame) {
				if (started && prev < 5.f && o.luma >= 5.f) ++rising;
				started = true;
			}
			prev = o.luma;
		}
		const int linesCounted = 200 - 4;
		check(std::abs(rising - cycles * linesCounted) <= 2,
			"bars at " + std::to_string(static_cast<int>(rate)) + " Hz: " + std::to_string(rising) + " rising edges over " + std::to_string(linesCounted) +
			" lines, expected " + std::to_string(cycles * linesCounted));
	}
}

// ---- 3 and 4. the round trip, and a processing step in between ----
struct Rig {
	int slot = signalRingAllocate();
	~Rig() { signalRingRelease(slot); }
};

static int runRoundTrip(double rate, int lines, bool invert, const std::vector<int64_t>& checkFields, double& worstMean) {
	static VideoGraphRenderer renderer;
	Rig ring;
	signalRingSetSampleRate(ring.slot, static_cast<float>(rate));
	const Pic pic = testPicture();
	ProbeRig probe;
	probe.show(pic);
	const vclock::Clock vc(rate, lines);
	RasterParams rp;
	rp.slotId = ring.slot;
	rp.lines = lines;
	constexpr int kBus = 9;
	videoBusPublishRaster(kBus, rp);
	int64_t frame = 0;
	int problems = 0;
	for (const int64_t n : checkFields) {
		const int64_t until = static_cast<int64_t>(std::ceil(vc.fieldStartFrame(n + 1))) + 4;
		for (; frame <= until; ++frame) {
			const vp::Reader::Out o = probe.reader.processScan(probe.tb, probe.frames.get(), vc, frame, true, true, 0.f, 0.f, 0.f, 0.f, .5f, static_cast<float>(1.0 / rate));
			float v[3] = {o.r, o.g, o.b};
			if (invert) for (float& x : v) x = 10.f - x;
			float samples[kSignalRingChannels] = {v[0] * .1f, v[1] * .1f, v[2] * .1f, 0.f};
			signalRingPushAt(ring.slot, samples, frame);
		}
		SignalWindow win;
		if (!signalRingReadField(ring.slot, win, lines) || win.field != n) { ++problems; std::fprintf(stderr, "no field %lld\n", static_cast<long long>(n)); continue; }
		const VideoGraphRenderer::Frame& out = renderer.renderBus(kBus, W, H);
		if (out.size() != static_cast<size_t>(W) * H * 4) { ++problems; continue; }
		const Ref ref = reference(pic, rate, lines, n);
		double sum = 0.0;
		int over = 0;
		for (int i = 0; i < W * H; ++i) {
			for (int ch = 0; ch < 3; ++ch) {
				double want = ref.rgb[static_cast<size_t>(i) * 3 + ch];
				if (invert) want = 255.0 - want;
				const double d = std::fabs(out[static_cast<size_t>(i) * 4 + ch] - want);
				sum += d;
				if (d > 2.0) ++over;
			}
		}
		const double mean = sum / (3.0 * W * H);
		worstMean = std::max(worstMean, mean);
		if (over > 0) { ++problems; std::fprintf(stderr, "  field %lld: %d values more than 2 levels off\n", static_cast<long long>(n), over); }
	}
	return problems;
}

static void testRoundTrip() {
	const std::vector<int64_t> fields = {1, 2, 3, 7, 30, 100, 300, 600};
	for (const double rate : {44100.0, 48000.0, 96000.0}) {
		for (const int lines : {240, 120, 60}) {
			double worst = 0.0;
			const int bad = runRoundTrip(rate, lines, false, fields, worst);
			check(bad == 0, "round trip at " + std::to_string(static_cast<int>(rate)) + " Hz, " + std::to_string(lines) + " lines matches the cell averages in fields 1 to 600 (worst mean error " + std::to_string(worst) + " levels)");
		}
	}
	for (const auto& rl : {std::pair<double, int>{48000.0, 240}, {44100.0, 60}, {96000.0, 120}}) {
		double worst = 0.0;
		const int bad = runRoundTrip(rl.first, rl.second, true, {1, 5, 40}, worst);
		check(bad == 0, "inverting the signal between Probe and Raster gives the inverted picture at " + std::to_string(static_cast<int>(rl.first)) + " Hz, " + std::to_string(rl.second) + " lines");
	}

	// No drift: the green channel only depends on the row, so a row's mean must not move from field to field.
	{
		Rig ring;
		signalRingSetSampleRate(ring.slot, 48000.f);
		ProbeRig probe;
		const Pic pic = testPicture();
		probe.show(pic);
		const vclock::Clock vc(48000.0, 240);
		RasterParams rp; rp.slotId = ring.slot; rp.lines = 240;
		videoBusPublishRaster(10, rp);
		VideoGraphRenderer renderer;
		std::vector<std::vector<unsigned char>> rows;
		int64_t frame = 0;
		for (const int64_t n : {2, 600}) {
			const int64_t until = static_cast<int64_t>(std::ceil(vc.fieldStartFrame(n + 1))) + 4;
			for (; frame <= until; ++frame) {
				const vp::Reader::Out o = probe.reader.processScan(probe.tb, probe.frames.get(), vc, frame, true, true, 0.f, 0.f, 0.f, 0.f, .5f, 1.f / 48000.f);
				float samples[kSignalRingChannels] = {o.r * .1f, o.g * .1f, o.b * .1f, 0.f};
				signalRingPushAt(ring.slot, samples, frame);
			}
			const VideoGraphRenderer::Frame& out = renderer.renderBus(10, W, H);
			std::vector<unsigned char> g(H);
			for (int y = 0; y < H; ++y) g[static_cast<size_t>(y)] = out[(static_cast<size_t>(y) * W + 7) * 4 + 1];
			rows.push_back(g);
		}
		int moved = 0;
		for (int y = 0; y < H; ++y) if (std::abs(static_cast<int>(rows[0][static_cast<size_t>(y)]) - static_cast<int>(rows[1][static_cast<size_t>(y)])) > 1) ++moved;
		check(moved == 0, "rows stay put from field 2 to field 600 (" + std::to_string(moved) + " rows moved)");
	}
}

// ---- 5. Raster with X and Y placed ----
static void testPlaced() {
	VideoGraphRenderer renderer;
	auto drive = [&](const RasterParams& base, auto sampleOf, int64_t n) {
		Rig ring;
		signalRingSetSampleRate(ring.slot, 48000.f);
		RasterParams rp = base;
		rp.slotId = ring.slot;
		const vclock::Clock vc(48000.0, rp.lines);
		const int64_t until = static_cast<int64_t>(std::ceil(vc.fieldStartFrame(n + 1))) + 4;
		for (int64_t f = 0; f <= until; ++f) {
			float s[kSignalRingChannels] = {};
			sampleOf(f, s);
			signalRingPushAt(ring.slot, s, f);
		}
		videoBusPublishRaster(11, rp);
		return renderer.renderBus(11, W, H);
	};
	auto lum = [&](const VideoGraphRenderer::Frame& fr, int x, int y) { return static_cast<int>(fr[(static_cast<size_t>(y) * W + x) * 4]); };

	// A steady 10 V grey at x = 0 V, y = 0 V: a dot in the middle of the picture and nothing else.
	RasterParams p;
	p.lines = 240; p.xPlaced = 1; p.yPlaced = 1;
	auto dot = drive(p, [](int64_t, float* s) { s[0] = 0.f; s[1] = 0.f; s[2] = 1.f; }, 4);
	check(lum(dot, W / 2, H / 2) == 255 && lum(dot, 10, 10) == 0 && lum(dot, W - 10, H - 10) == 0, "X and Y placed at 0 V: a white dot in the middle, black elsewhere");

	// A moving point with JOIN draws a line; with HOLD only dots. x runs -4 to +4 V across 100 samples, y = 2 V.
	auto sweep = [](int64_t f, float* s) { s[0] = static_cast<float>(((f % 100) / 99.0 * 8.0 - 4.0) * .1); s[1] = .2f; s[2] = 1.f; };
	p.join = 1;
	auto joined = drive(p, sweep, 4);
	p.join = 0;
	auto held = drive(p, sweep, 4);
	int lit = 0, litHeld = 0;
	const int row = static_cast<int>((5.0 - 2.0) / 10.0 * H);
	for (int x = 0; x < W; ++x) { if (lum(joined, x, row) > 128) ++lit; if (lum(held, x, row) > 128) ++litHeld; }
	check(lit > litHeld && lit > 150, "JOIN draws a line between points where HOLD leaves dots (" + std::to_string(lit) + " against " + std::to_string(litHeld) + " lit pixels)");

	// X placed, Y on the raster: x = -5 V means the left edge of every line, so the left of every visible row is lit.
	RasterParams q;
	q.lines = 240; q.xPlaced = 1; q.yPlaced = 0;
	auto edge = drive(q, [](int64_t, float* s) { s[0] = -.5f; s[1] = 0.f; s[2] = 1.f; }, 4);
	int rowsLit = 0;
	for (int y = 0; y < H; ++y) if (lum(edge, 0, y) == 255) ++rowsLit;
	check(rowsLit > H * 0.9 && lum(edge, W / 2, H / 2) == 0, "X placed with Y on the raster: the left edge is lit in each row, the middle is black (" + std::to_string(rowsLit) + " rows lit)");
}


// ---- 7. slow scan ----
static void testSlowClock() {
	// Scan Sync's TOP fires once per slow picture: the gap between rising edges is K fields.
	for (const int k : {1, 4, 32}) {
		const vclock::Clock c(48000.0, 240, k);
		const vclock::Clock plain(48000.0, 240);
		check(std::fabs(c.samplesPerField() - k * plain.samplesPerField()) < 1e-9 && std::fabs(c.samplesPerLine() - k * plain.samplesPerLine()) < 1e-9,
			"slow clock x" + std::to_string(k) + ": a picture is K fields and a line is K lines long");
		float prev = 0.f;
		std::vector<int64_t> edges, lineEdges;
		const int64_t end = static_cast<int64_t>(c.samplesPerField() * 6.5);
		for (int64_t f = 0; f < end; ++f) {
			const scan_sync::Outputs o = scan_sync::at(c, f);
			if (prev < 5.f && o.top >= 5.f) edges.push_back(f);
			prev = o.top;
		}
		bool period = edges.size() == 7 || edges.size() == 6;
		for (size_t i = 1; i < edges.size(); ++i) period = period && std::fabs(static_cast<double>(edges[i] - edges[i - 1]) - c.samplesPerField()) <= 1.0;
		check(period, "Scan Sync TOP at SLOW x" + std::to_string(k) + " repeats every " + std::to_string(k) + " fields (" + std::to_string(c.samplesPerField()) + " samples), edges " + std::to_string(edges.size()));
		// DOWN ramps over the whole slow picture and ACROSS over the slow line.
		const scan_sync::Outputs mid = scan_sync::at(c, static_cast<int64_t>(c.samplesPerField() * 0.5 * c.visibleFraction()));
		check(std::fabs(mid.down - 5.f) < .1f, "DOWN is 5 V half way down a slow picture at x" + std::to_string(k));
		const scan_sync::Outputs line = scan_sync::at(c, static_cast<int64_t>(c.samplesPerLine() * 10.5));
		check(line.across > 1.f && line.across < 9.f, "ACROSS is about 5 V half way along a slow line at x" + std::to_string(k));
	}
}

// Slow round trip: the Probe scans, Raster is asked for a picture after every field's worth of samples (the way the screen
// asks), and at the end of each checked picture its held picture is compared with the cell averages of that picture.
static int runSlowRoundTrip(double rate, int lines, int slow, bool invert, const std::vector<int64_t>& pictures, double& worstMean) {
	VideoGraphRenderer renderer;
	Rig ring;
	signalRingSetSampleRate(ring.slot, static_cast<float>(rate));
	const Pic pic = testPicture();
	ProbeRig probe;
	probe.show(pic);
	const vclock::Clock vc(rate, lines, slow);
	const vclock::Clock plain(rate, lines);
	RasterParams rp;
	rp.slotId = ring.slot;
	rp.lines = lines;
	rp.slow = slow;
	constexpr int kBus = 14;
	videoBusPublishRaster(kBus, rp);
	int64_t frame = 0;
	int problems = 0;
	int64_t nextRender = static_cast<int64_t>(plain.samplesPerField());
	for (const int64_t m : pictures) {
		const int64_t lastOfPicture = static_cast<int64_t>(std::ceil(vc.fieldStartFrame(m + 1) - 1e-7)) - 1;
		for (; frame <= lastOfPicture; ++frame) {
			const vp::Reader::Out o = probe.reader.processScan(probe.tb, probe.frames.get(), vc, frame, true, true, 0.f, 0.f, 0.f, 0.f, .5f, static_cast<float>(1.0 / rate));
			float v[3] = {o.r, o.g, o.b};
			if (invert) for (float& x : v) x = 10.f - x;
			float samples[kSignalRingChannels] = {v[0] * .1f, v[1] * .1f, v[2] * .1f, 0.f};
			signalRingPushAt(ring.slot, samples, frame);
			if (frame >= nextRender && frame < lastOfPicture) { renderer.renderBus(kBus, W, H); nextRender += static_cast<int64_t>(plain.samplesPerField()); }
		}
		const VideoGraphRenderer::Frame& out = renderer.renderBus(kBus, W, H);
		if (out.size() != static_cast<size_t>(W) * H * 4) { ++problems; continue; }
		const Ref ref = reference(pic, rate, lines, m, slow);
		double sum = 0.0;
		int over = 0;
		for (int i = 0; i < W * H; ++i) {
			for (int ch = 0; ch < 3; ++ch) {
				double want = ref.rgb[static_cast<size_t>(i) * 3 + ch];
				if (invert) want = 255.0 - want;
				const double d = std::fabs(out[static_cast<size_t>(i) * 4 + ch] - want);
				sum += d;
				if (d > 2.0) ++over;
			}
		}
		worstMean = std::max(worstMean, sum / (3.0 * W * H));
		if (over > 0) { ++problems; std::fprintf(stderr, "  picture %lld: %d values more than 2 levels off\n", static_cast<long long>(m), over); }
	}
	return problems;
}

static void testSlowRoundTrip() {
	for (const double rate : {44100.0, 48000.0, 96000.0}) {
		for (const int slow : {16, rate > 90000.0 ? 32 : 64}) {   // the picture Probe reads is 320 wide: more than 320 columns would be finer than it
			double worst = 0.0;
			const int bad = runSlowRoundTrip(rate, 240, slow, false, slow == 16 ? std::vector<int64_t>{1, 2, 3, 9} : std::vector<int64_t>{1, 2, 4}, worst);
			const vclock::Clock c(rate, 240, slow);
			check(bad == 0, "slow round trip at " + std::to_string(static_cast<int>(rate)) + " Hz, x" + std::to_string(slow) + " (" + std::to_string(c.samplesPerLine()).substr(0, 5) +
				" samples a line), cell averages within 2 levels over several pictures (worst mean error " + std::to_string(worst) + " levels)");
		}
	}
	double worst = 0.0;
	check(runSlowRoundTrip(48000.0, 120, 16, false, {1, 2, 5}, worst) == 0, "slow round trip at 120 lines, x16");
	check(runSlowRoundTrip(48000.0, 240, 16, true, {1, 3}, worst) == 0, "slow round trip, inverted signal, x16");
	// The slow picture is much sharper than the plain one: count the distinct colours across one row (the test picture's
	// red ramps across, so distinct red values are distinct columns).
	{
		VideoGraphRenderer renderer;
		Rig ring;
		signalRingSetSampleRate(ring.slot, 48000.f);
		ProbeRig probe;
		probe.show(testPicture());
		const vclock::Clock vc(48000.0, 240, 32);
		RasterParams rp; rp.slotId = ring.slot; rp.lines = 240; rp.slow = 32;
		videoBusPublishRaster(15, rp);
		const int64_t last = static_cast<int64_t>(std::ceil(vc.fieldStartFrame(2) - 1e-7)) - 1;
		for (int64_t f = 0; f <= last; ++f) {
			const vp::Reader::Out o = probe.reader.processScan(probe.tb, probe.frames.get(), vc, f, true, true, 0.f, 0.f, 0.f, 0.f, .5f, 1.f / 48000.f);
			float s[kSignalRingChannels] = {o.r * .1f, o.g * .1f, o.b * .1f, 0.f};
			signalRingPushAt(ring.slot, s, f);
			if (f % 800 == 799) renderer.renderBus(15, W, H);
		}
		const VideoGraphRenderer::Frame& out = renderer.renderBus(15, W, H);
		int distinct = 0, prev = -1;
		for (int x = 0; x < W; ++x) { const int r = out[(static_cast<size_t>(120) * W + x) * 4]; if (r != prev) ++distinct; prev = r; }
		check(distinct >= 80, "at SLOW x32 a row has about 98 columns (" + std::to_string(distinct) + " distinct steps across 320 pixels)");
	}
}

// ---- 6. across threads (make tsan runs this part alone) ----
static void testThreads(int slow) {
	Rig ring;
	signalRingSetSampleRate(ring.slot, 48000.f);
	RasterParams rp; rp.slotId = ring.slot; rp.lines = 120; rp.slow = slow;
	videoBusPublishRaster(12, rp);
	ProbeRig probe;
	const Pic a = testPicture();
	const Pic b = make([](int x, int y, int* c) { c[0] = c[1] = c[2] = (x + y) & 255; });
	probe.show(a);
	std::atomic<bool> stop{false};
	std::atomic<int64_t> written{0};
	// "audio": reads the probe's picture through the hand-off and writes the ring, one sample a step.
	std::thread audio([&] {
		const vclock::Clock vc(48000.0, 120, slow);
		for (int64_t f = 0; f < 200000 && !stop.load(); ++f) {
			const vp::Reader::Out o = probe.reader.processScan(probe.tb, probe.frames.get(), vc, f, true, true, 0.f, 0.f, 0.f, 0.f, .5f, 1.f / 48000.f);
			float s[kSignalRingChannels] = {o.r * .1f, o.g * .1f, o.b * .1f, 0.f};
			signalRingPushAt(ring.slot, s, f);
			written.store(f, std::memory_order_relaxed);
		}
	});
	// "widget": publishes a new analysed picture now and then (the one writer of the hand-off).
	std::thread widget([&] {
		bool flip = false;
		while (!stop.load()) {
			probe.show(flip ? a : b);
			flip = !flip;
			std::this_thread::sleep_for(std::chrono::microseconds(300));
		}
	});
	// "render": reads the newest field and draws it.
	VideoGraphRenderer renderer;
	int drawn = 0;
	while (written.load(std::memory_order_relaxed) < 199000) {
		const VideoGraphRenderer::Frame& out = renderer.renderBus(12, 160, 120);
		if (out.size() == 160u * 120u * 4u) ++drawn;
	}
	stop.store(true);
	audio.join();
	widget.join();
	check(drawn > 10, "the render side kept drawing while the audio thread wrote, SLOW x" + std::to_string(slow) + " (" + std::to_string(drawn) + " pictures)");
}

int main(int argc, char** argv) {
	if (argc > 1 && std::string(argv[1]) == "thread") {
		testThreads(1);
		testThreads(4);
		std::printf("raster_scan_test (thread): %d checks, %d failed\n", checks, failures);
		return failures == 0 ? 0 : 1;
	}
	testFixedPoint();
	testBars();
	testRoundTrip();
	testPlaced();
	testSlowClock();
	testSlowRoundTrip();
	testThreads(1);
	testThreads(4);
	std::printf("raster_scan_test: %d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
