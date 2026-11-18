#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// The last Vector Osc wave shape: one closed loop that makes the beam write the letters V R U, left to
// right, as a vector display does.
//
// Method. Each letter is one stroke of straight segments and arcs. The beam moves along strokes at a
// constant speed, so the trace is evenly bright (Scope Trace's DWELL at 0 gives energy in proportion to
// length), and it moves kJumpSpeed times faster between strokes, so those lines are drawn dim whenever
// Scope Trace's DWELL is up (brightness goes as speed to the minus DWELL). The R retraces part of its bar once,
// so no stroke has to be left and rejoined. Each stroke is smoothed a little (kCornerRadius), which rounds
// every corner, and the mean is taken out so X and Y have no DC. The picture is drawn for a 4:3 Display, as Display makes it, so the letters are not stretched.
namespace vector_osc {

constexpr int kLetterTable = 4096;
constexpr float kJumpSpeed = 200.f;     // between-stroke speed over drawing speed
constexpr double kCornerRadius = .03;  // Gaussian sigma of the corner rounding along a stroke (letter height is 1)
constexpr float kLetterHeight = .9f;   // letters are +/-0.45 tall of the +/-1 screen
constexpr float kScreenAspect = 4.f / 3.f;

struct LetterTable {
	std::array<float, kLetterTable + 1> x{}, y{};   // the last entry repeats the first, for interpolation
};

namespace letters_detail {

using Stroke = std::vector<std::pair<double, double>>;

// Appends one stroke given as a list of points (local letter units: height 1, y from -.5 to .5).
inline void addStroke(std::vector<Stroke>& out, double x0, const Stroke& pts) {
	Stroke s;
	for (const auto& p : pts) s.push_back({x0 + p.first, p.second});
	out.push_back(std::move(s));
}

inline void arc(Stroke& v, double cx, double cy, double r, double a0, double a1, int steps) {
	for (int i = 1; i <= steps; ++i) {
		const double a = a0 + (a1 - a0) * i / steps;
		v.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
	}
}

// Rounds the corners of one stroke: it is resampled evenly along its length and smoothed with a Gaussian of
// sigma kCornerRadius, with the ends mirrored about themselves so the two ends stay where they are.
inline Stroke roundStroke(const Stroke& in) {
	const double ds = .004;
	std::vector<double> cum(in.size(), 0.0);
	for (size_t i = 1; i < in.size(); ++i) cum[i] = cum[i - 1] + std::hypot(in[i].first - in[i - 1].first, in[i].second - in[i - 1].second);
	const double len = cum.back();
	const int n = std::max(2, static_cast<int>(std::ceil(len / ds)) + 1);
	std::vector<double> rx(n), ry(n);
	size_t seg = 0;
	for (int j = 0; j < n; ++j) {
		const double t = len * j / (n - 1);
		while (seg + 2 < in.size() && cum[seg + 1] < t) ++seg;
		const double f = cum[seg + 1] > cum[seg] ? (t - cum[seg]) / (cum[seg + 1] - cum[seg]) : 0.0;
		rx[j] = in[seg].first + (in[seg + 1].first - in[seg].first) * f;
		ry[j] = in[seg].second + (in[seg + 1].second - in[seg].second) * f;
	}
	const int rad = static_cast<int>(std::ceil(3.0 * kCornerRadius / ds));
	std::vector<double> kern(2 * rad + 1);
	double ksum = 0.0;
	for (int k = -rad; k <= rad; ++k) {
		const double d = k * ds;
		kern[k + rad] = std::exp(-.5 * d * d / (kCornerRadius * kCornerRadius));
		ksum += kern[k + rad];
	}
	auto at = [&](const std::vector<double>& v, int i) {
		if (i < 0) return 2.0 * v[0] - v[std::min(-i, n - 1)];
		if (i > n - 1) return 2.0 * v[n - 1] - v[std::max(2 * (n - 1) - i, 0)];
		return v[i];
	};
	Stroke out(n);
	for (int j = 0; j < n; ++j) {
		double ax = 0.0, ay = 0.0;
		for (int k = -rad; k <= rad; ++k) {
			ax += kern[k + rad] * at(rx, j + k);
			ay += kern[k + rad] * at(ry, j + k);
		}
		out[j] = {ax / ksum, ay / ksum};
	}
	return out;
}

struct Pt { double x, y; bool jumpNext; };

} // namespace letters_detail

inline LetterTable buildLetterTable() {
	using namespace letters_detail;
	const double pi = 3.14159265358979323846;
	std::vector<Stroke> strokes;
	// V
	addStroke(strokes, 0.0, {{0, .5}, {.36, -.5}, {.72, .5}});
	// R, in two strokes so every jump runs along the top or the bottom of the letters: the bowl from the top
	// of the stem, round to its bar, along the bar to the stem and back out a little (a short retrace),
	// then down the leg; and, after a jump along the baseline, the stem up to where the bowl began.
	{
		Stroke r = {{0, .5}, {.47, .5}};
		arc(r, .47, .25, .25, pi / 2, -pi / 2, 20);
		r.push_back({0, 0});
		r.push_back({.3, 0});
		r.push_back({.72, -.5});
		addStroke(strokes, .94, r);
		addStroke(strokes, .94, {{0, -.5}, {0, .5}});
	}
	// U: down the left side, round the bottom, up the right side.
	{
		Stroke u = {{0, .5}, {0, -.14}};
		arc(u, .36, -.14, .36, pi, 2 * pi, 28);
		u.push_back({.72, .5});
		addStroke(strokes, 1.88, u);
	}
	std::vector<Pt> pts;
	for (const Stroke& st : strokes) {
		const Stroke r = roundStroke(st);
		for (const auto& p : r) pts.push_back({p.first, p.second, false});
		pts.back().jumpNext = true;   // after a stroke the beam jumps to the next one
	}
	// Time along the loop: drawing speed 1, jumps kJumpSpeed.
	const size_t n = pts.size();
	std::vector<double> cum(n + 1, 0.0);
	for (size_t i = 0; i < n; ++i) {
		const Pt& a = pts[i];
		const Pt& b = pts[(i + 1) % n];
		const double len = std::hypot(b.x - a.x, b.y - a.y);
		cum[i + 1] = cum[i] + (a.jumpNext ? len / kJumpSpeed : len);
	}
	const double total = cum[n];
	const int N = kLetterTable;
	std::vector<double> sx(N), sy(N);
	size_t seg = 0;
	for (int j = 0; j < N; ++j) {
		const double t = total * j / N;
		while (seg + 1 < n && cum[seg + 1] <= t) ++seg;
		const Pt& a = pts[seg];
		const Pt& b = pts[(seg + 1) % n];
		const double f = cum[seg + 1] > cum[seg] ? (t - cum[seg]) / (cum[seg + 1] - cum[seg]) : 0.0;
		sx[j] = a.x + (b.x - a.x) * f;
		sy[j] = a.y + (b.y - a.y) * f;
	}
	// Centre on the loop's mean (so there is no DC), then scale into the unit box: x is divided by the
	// screen aspect so the letters keep their shape on a 4:3 picture.
	double mx = 0.0, my = 0.0;
	for (int j = 0; j < N; ++j) { mx += sx[j]; my += sy[j]; }
	mx /= N;
	my /= N;
	LetterTable t;
	for (int j = 0; j < N; ++j) {
		t.x[j] = static_cast<float>((sx[j] - mx) * kLetterHeight / kScreenAspect);
		t.y[j] = static_cast<float>((sy[j] - my) * kLetterHeight);
	}
	t.x[N] = t.x[0];
	t.y[N] = t.y[0];
	return t;
}

inline const LetterTable& letterTable() {
	static const LetterTable t = buildLetterTable();
	return t;
}

// The beam position (+/-1) at phase p of the loop.
inline void letterPoint(float p, float& x, float& y) {
	const LetterTable& t = letterTable();
	p -= std::floor(p);
	const float pos = p * kLetterTable;
	int i = static_cast<int>(pos);
	if (i >= kLetterTable) i = kLetterTable - 1;
	const float f = pos - static_cast<float>(i);
	x = t.x[i] + (t.x[i + 1] - t.x[i]) * f;
	y = t.y[i] + (t.y[i + 1] - t.y[i]) * f;
}

} // namespace vector_osc
