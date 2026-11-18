#pragma once
// The raster rule shared by Video Probe's SCAN mode (picture -> signal) and Raster (VRU-29, signal -> picture).
// See docs/architecture.md, "Raster rule". No Rack types.
//
// One audio sample is one place in the picture, taken from the video clock (src/video_clock.hpp): its line, and how
// far along the line it is. The sample stands for a cell: the whole of the line's row band, and across the line the
// stretch from halfway back to the previous sample to halfway on to the next. The first sample of a line reaches back to
// the left edge and the last reaches on to the right edge, so the cells of a line tile it with no gap and no overlap.
// Video Probe reads the picture's average over that cell; Raster paints that cell with the sample's value.
//
// Resolution is what the line rate gives: a line lasts 63.55 us, so there are 3.05 samples to a line at 48 kHz (2.80 at
// 44.1 kHz, 6.10 at 96 kHz) whatever LINES is. LINES sets how many lines a picture has, so the number of samples a
// picture gets is about (LINES + 22.5) x 3.05 at 48 kHz, 22.5 of those lines being the flyback, which carries no picture.
#include "signal_ring.hpp"
#include "video_clock.hpp"
#include "video_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace raster_scan {

constexpr int kMinLines = 16;
constexpr int kMaxLines = 600;
constexpr int kDefaultLines = vclock::kDefaultLines;

inline int clampLines(float v) {
	if (!(v == v)) return kDefaultLines;
	return std::clamp(static_cast<int>(std::lround(v)), kMinLines, kMaxLines);
}

// Samples in one line at this sample rate (the line rate is fixed; LINES does not change it).
inline double samplesPerLine(double sampleRate) { return sampleRate / vclock::kLineRateHz; }
// Samples in one picture of `lines` visible lines, flyback included.
inline double samplesPerPicture(double sampleRate, int lines) { return vclock::Clock(sampleRate, lines).samplesPerField(); }

struct Cell {
	bool visible = false;   // false in the flyback, which has no picture
	int line = 0;           // 0 for the top line
	int64_t field = 0;      // the picture (of the slow clock) this sample is in
	double u = 0.0;         // the sample's own position along the line, 0 to 1
	double u0 = 0.0, u1 = 1.0;   // the cell's edges along the line
};

// `clock` is the plain clock, or the slow one (vclock::Clock(rate, lines, K)): then a cell is K times wider in time and
// a picture is K fields long, and nothing else changes.
inline Cell cellAt(const vclock::Clock& clock, int64_t frame) {
	const vclock::VideoTime t = clock.fromFrame(static_cast<double>(frame));
	const vclock::VideoTime prev = clock.fromFrame(static_cast<double>(frame - 1));
	const vclock::VideoTime next = clock.fromFrame(static_cast<double>(frame + 1));
	const double du = 1.0 / clock.samplesPerLine();
	Cell c;
	c.visible = t.visible;
	c.line = t.line;
	c.field = t.field;
	c.u = t.linePhase;
	const bool first = prev.field != t.field || prev.line != t.line;
	const bool last = next.field != t.field || next.line != t.line;
	c.u0 = first ? 0.0 : std::max(0.0, t.linePhase - du * 0.5);
	c.u1 = last ? 1.0 : std::min(1.0, t.linePhase + du * 0.5);
	return c;
}

// First pixel (of n) whose centre is at or past `edge` (0 to 1 across the picture). The pixels between two edges are
// firstPixel(a) to firstPixel(b) - 1; the same edge from both sides gives the same pixel, so cells leave no gap.
inline int firstPixel(double edge, int n) {
	return std::clamp(static_cast<int>(std::ceil(edge * n - 0.5 - 1e-9)), 0, n);
}

// A ring value (volts / 10) to a brightness 0..1: 0 V is black, 10 V is white, anything outside clips.
inline float norm(float v) {
	if (!(v == v)) return 0.f;
	return std::clamp(v, 0.f, 1.f);
}

// Paints the picture for one field. window: signalRingReadField() of the module's ring, whose channels hold volts / 10:
// red, green, blue; or, with an axis placed, x, y and a grey brightness. rgba is width x height x 4.
inline void render(std::vector<unsigned char>& rgba, int width, int height, const SignalWindow& window, const RasterParams& p) {
	rgba.assign(static_cast<size_t>(width) * height * 4, 0);
	for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
	if (!window.valid() || !window.fieldWindow || window.channels < 3 || width < 1 || height < 1) return;
	const int lines = std::max(p.lines, 1);
	const vclock::Clock clock(static_cast<double>(window.sampleRate), lines);
	const int64_t field = window.field;
	const int64_t base = static_cast<int64_t>(std::floor(clock.fieldStartFrame(field) + vclock::kSnap));
	const int len = window.length;
	const float* ch[3] = {window.data.data(), window.data.data() + len, window.data.data() + 2 * static_cast<size_t>(len)};
	const bool placed = p.xPlaced != 0 || p.yPlaced != 0;

	auto put = [&](int x, int y, float r, float g, float b) {
		if (x < 0 || y < 0 || x >= width || y >= height) return;
		unsigned char* px = &rgba[(static_cast<size_t>(y) * width + x) * 4];
		px[0] = static_cast<unsigned char>(std::lround(std::clamp(r, 0.f, 1.f) * 255.f));
		px[1] = static_cast<unsigned char>(std::lround(std::clamp(g, 0.f, 1.f) * 255.f));
		px[2] = static_cast<unsigned char>(std::lround(std::clamp(b, 0.f, 1.f) * 255.f));
	};

	if (!placed) {
		// Plain raster. Walk the samples of the field line by line.
		struct S { double u; float r, g, b; };
		std::vector<S> run;
		int runLine = -1;
		auto flush = [&]() {
			if (run.empty() || runLine < 0 || runLine >= lines) { run.clear(); return; }
			const int ya = firstPixel(static_cast<double>(runLine) / lines, height);
			const int yb = firstPixel(static_cast<double>(runLine + 1) / lines, height);
			const int n = static_cast<int>(run.size());
			// Cell edges: halfway between neighbours, the picture's edges at the ends.
			std::vector<double> edge(static_cast<size_t>(n) + 1);
			edge[0] = 0.0;
			edge[static_cast<size_t>(n)] = 1.0;
			for (int k = 1; k < n; ++k) edge[static_cast<size_t>(k)] = 0.5 * (run[static_cast<size_t>(k) - 1].u + run[static_cast<size_t>(k)].u);
			for (int k = 0; k < n; ++k) {
				const int xa = firstPixel(edge[static_cast<size_t>(k)], width);
				const int xb = firstPixel(edge[static_cast<size_t>(k) + 1], width);
				for (int x = xa; x < xb; ++x) {
					float r = run[static_cast<size_t>(k)].r, g = run[static_cast<size_t>(k)].g, b = run[static_cast<size_t>(k)].b;
					if (p.join) {
						// Straight join between the two sample centres either side of this pixel centre.
						const double uc = (x + 0.5) / width;
						const int a = uc < run[static_cast<size_t>(k)].u ? k - 1 : k;
						const int c = a + 1;
						if (a >= 0 && c < n) {
							const S& s0 = run[static_cast<size_t>(a)];
							const S& s1 = run[static_cast<size_t>(c)];
							const float f = s1.u > s0.u ? static_cast<float>((uc - s0.u) / (s1.u - s0.u)) : 0.f;
							r = s0.r + (s1.r - s0.r) * f;
							g = s0.g + (s1.g - s0.g) * f;
							b = s0.b + (s1.b - s0.b) * f;
						}
					}
					for (int y = ya; y < yb; ++y) put(x, y, r, g, b);
				}
			}
			run.clear();
		};
		for (int i = 0; i < len; ++i) {
			const vclock::VideoTime t = clock.fromFrame(static_cast<double>(base + i));
			if (t.field != field || !t.visible) { flush(); runLine = -1; continue; }
			if (t.line != runLine) { flush(); runLine = t.line; }
			run.push_back({t.linePhase, norm(ch[0][i]), norm(ch[1][i]), norm(ch[2][i])});
		}
		flush();
		return;
	}

	// Placed mode: x and y volts and a grey value. An axis with no cable follows the raster.
	const int dotW = std::max(1, static_cast<int>(std::lround(width / 160.0)));
	const int dotH = std::max(1, static_cast<int>(std::lround(height / 120.0)));
	// xa == xb (or ya == yb) means "a dot centred on cx (cy)"; otherwise the cell's own pixels.
	auto stamp = [&](double cx, double cy, int xa, int xb, int ya, int yb, float v) {
		if (xa == xb) { xa = static_cast<int>(std::floor(cx - dotW * 0.5 + 0.5)); xb = xa + dotW; }
		if (ya == yb) { ya = static_cast<int>(std::floor(cy - dotH * 0.5 + 0.5)); yb = ya + dotH; }
		for (int y = ya; y < yb; ++y)
			for (int x = xa; x < xb; ++x) put(x, y, v, v, v);
	};
	bool havePrev = false;
	double pcx = 0, pcy = 0;
	int pline = -1;
	for (int i = 0; i < len; ++i) {
		const int64_t frame = base + i;
		const vclock::VideoTime t = clock.fromFrame(static_cast<double>(frame));
		if (t.field != field) { havePrev = false; continue; }
		if (!p.yPlaced && !t.visible) { havePrev = false; continue; }
		const Cell c = cellAt(clock, frame);
		const float grey = norm(ch[2][i]);
		int xa = 0, xb = 0, ya = 0, yb = 0;
		double cx = 0, cy = 0;
		if (p.xPlaced) cx = (std::clamp(ch[0][i] * 10.f, -5.f, 5.f) + 5.0) / 10.0 * width;
		else { xa = firstPixel(c.u0, width); xb = firstPixel(c.u1, width); cx = c.u * width; }
		if (p.yPlaced) cy = (5.0 - std::clamp(ch[1][i] * 10.f, -5.f, 5.f)) / 10.0 * height;
		else { ya = firstPixel(static_cast<double>(c.line) / lines, height); yb = firstPixel(static_cast<double>(c.line + 1) / lines, height); cy = (c.line + 0.5) / lines * height; }
		const bool bothPlaced = p.xPlaced && p.yPlaced;
		if (p.join && havePrev && (bothPlaced || c.line == pline)) {
			const double dx = cx - pcx, dy = cy - pcy;
			const int steps = static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dy))));
			for (int k = 1; k < steps; ++k) {
				const double f = static_cast<double>(k) / steps;
				stamp(pcx + dx * f, pcy + dy * f, xa, xb, ya, yb, grey);
			}
		}
		stamp(cx, cy, xa, xb, ya, yb, grey);
		havePrev = true;
		pcx = cx; pcy = cy; pline = c.line;
	}
}

// ---- slow scan: a picture takes K fields, painted a slice at a time ----

// The held picture of one Raster bus (renderer side). Each call to paintSince() paints the samples that arrived since the
// last call into `pic` and leaves the rest as it was, so a slow picture is refreshed top to bottom while the held one stays
// on screen. With X and Y both placed the picture is cleared when a new slow picture begins, since a placed sample can land
// anywhere.
struct Held {
	std::vector<unsigned char> pic;
	int width = 0, height = 0;
	int64_t lastFrame = INT64_MIN;   // the newest frame painted
	int64_t picture = -1;
};

inline void paintSince(Held& h, int width, int height, const SignalWindow& window, const RasterParams& p) {
	if (h.width != width || h.height != height || h.pic.size() != static_cast<size_t>(width) * height * 4) {
		h.width = width; h.height = height;
		h.pic.assign(static_cast<size_t>(width) * height * 4, 0);
		for (size_t i = 3; i < h.pic.size(); i += 4) h.pic[i] = 255;
		h.picture = -1;
	}
	if (!window.valid() || window.channels < 3) return;
	const int lines = std::max(p.lines, 1);
	const vclock::Clock clock(static_cast<double>(window.sampleRate), lines, std::max(p.slow, 1));
	const int len = window.length;
	const float* ch[3] = {window.data.data(), window.data.data() + len, window.data.data() + 2 * static_cast<size_t>(len)};
	const bool placed = p.xPlaced != 0 || p.yPlaced != 0;
	std::vector<unsigned char>& pic = h.pic;
	auto put = [&](int x, int y, float r, float g, float b) {
		if (x < 0 || y < 0 || x >= width || y >= height) return;
		unsigned char* px = &pic[(static_cast<size_t>(y) * width + x) * 4];
		px[0] = static_cast<unsigned char>(std::lround(std::clamp(r, 0.f, 1.f) * 255.f));
		px[1] = static_cast<unsigned char>(std::lround(std::clamp(g, 0.f, 1.f) * 255.f));
		px[2] = static_cast<unsigned char>(std::lround(std::clamp(b, 0.f, 1.f) * 255.f));
	};
	std::vector<Cell> cells(static_cast<size_t>(len));
	for (int i = 0; i < len; ++i) cells[static_cast<size_t>(i)] = cellAt(clock, window.firstFrame + i);
	const int dotW = std::max(1, static_cast<int>(std::lround(width / 160.0)));
	const int dotH = std::max(1, static_cast<int>(std::lround(height / 120.0)));
	auto stamp = [&](double cx, double cy, int xa, int xb, int ya, int yb, float v) {
		if (xa == xb) { xa = static_cast<int>(std::floor(cx - dotW * 0.5 + 0.5)); xb = xa + dotW; }
		if (ya == yb) { ya = static_cast<int>(std::floor(cy - dotH * 0.5 + 0.5)); yb = ya + dotH; }
		for (int y = ya; y < yb; ++y)
			for (int x = xa; x < xb; ++x) put(x, y, v, v, v);
	};
	bool havePrev = false;
	double pcx = 0, pcy = 0;
	int pline = -1;
	for (int i = 0; i < len; ++i) {
		const Cell& c = cells[static_cast<size_t>(i)];
		if (p.xPlaced && p.yPlaced && c.field != h.picture) {
			std::fill(pic.begin(), pic.end(), 0);
			for (size_t k = 3; k < pic.size(); k += 4) pic[k] = 255;
			havePrev = false;
		}
		h.picture = c.field;
		if (!placed) {
			if (!c.visible || c.line >= lines) continue;
			const int ya = firstPixel(static_cast<double>(c.line) / lines, height);
			const int yb = firstPixel(static_cast<double>(c.line + 1) / lines, height);
			const int xa = firstPixel(c.u0, width), xb = firstPixel(c.u1, width);
			for (int x = xa; x < xb; ++x) {
				float r = norm(ch[0][i]), g = norm(ch[1][i]), b = norm(ch[2][i]);
				if (p.join) {
					const double uc = (x + 0.5) / width;
					const int a = uc < c.u ? i - 1 : i;
					const int d = a + 1;
					if (a >= 0 && d < len) {
						const Cell& ca = cells[static_cast<size_t>(a)];
						const Cell& cd = cells[static_cast<size_t>(d)];
						if (ca.line == cd.line && ca.field == cd.field && cd.u > ca.u) {
							const float f = static_cast<float>((uc - ca.u) / (cd.u - ca.u));
							r = norm(ch[0][a]) + (norm(ch[0][d]) - norm(ch[0][a])) * f;
							g = norm(ch[1][a]) + (norm(ch[1][d]) - norm(ch[1][a])) * f;
							b = norm(ch[2][a]) + (norm(ch[2][d]) - norm(ch[2][a])) * f;
						}
					}
				}
				for (int y = ya; y < yb; ++y) put(x, y, r, g, b);
			}
			continue;
		}
		if (!p.yPlaced && !c.visible) { havePrev = false; continue; }
		const float grey = norm(ch[2][i]);
		int xa = 0, xb = 0, ya = 0, yb = 0;
		double cx = 0, cy = 0;
		if (p.xPlaced) cx = (std::clamp(ch[0][i] * 10.f, -5.f, 5.f) + 5.0) / 10.0 * width;
		else { xa = firstPixel(c.u0, width); xb = firstPixel(c.u1, width); cx = c.u * width; }
		if (p.yPlaced) cy = (5.0 - std::clamp(ch[1][i] * 10.f, -5.f, 5.f)) / 10.0 * height;
		else { ya = firstPixel(static_cast<double>(c.line) / lines, height); yb = firstPixel(static_cast<double>(c.line + 1) / lines, height); cy = (c.line + 0.5) / lines * height; }
		if (p.join && havePrev && ((p.xPlaced && p.yPlaced) || c.line == pline)) {
			const double dx = cx - pcx, dy = cy - pcy;
			const int steps = static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dy))));
			for (int k = 1; k < steps; ++k) {
				const double f = static_cast<double>(k) / steps;
				stamp(pcx + dx * f, pcy + dy * f, xa, xb, ya, yb, grey);
			}
		}
		stamp(cx, cy, xa, xb, ya, yb, grey);
		havePrev = true;
		pcx = cx; pcy = cy; pline = c.line;
	}
	h.lastFrame = window.firstFrame + len - 1;
}

}  // namespace raster_scan
