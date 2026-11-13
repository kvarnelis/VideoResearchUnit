#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// Focus (VRU-24) picture maths: running-sum box passes on planar float pictures. No Rack types, no
// allocation inside the passes (the caller owns the buffers), so tests and the renderer share it.
//
// A picture is three planes (R, G, B) of width x height floats, 0..255. Every pass takes one plane in and
// writes another, never the same one. Cost per pixel does not grow with the radius: each output is the
// previous output plus one sample entering and one leaving the window.
//
// The window of a pass reaches `behind` pixels before the pixel and `ahead` pixels after it. Both are real
// numbers: the whole pixels count fully and the part of a pixel left over counts as a fraction at each end,
// so a knob moves the blur smoothly instead of in steps of a pixel. Edges repeat the border pixel.
namespace focus_filter {

struct Window {
	int behindWhole = 0;   // pixels at full weight before the centre
	int aheadWhole = 0;    // pixels at full weight after the centre
	float behindPart = 0;  // weight of the next pixel past the last whole one on the behind side, 0..1
	float aheadPart = 0;
	float norm = 1.f;      // 1 / total weight
	bool empty = true;     // window is just the pixel itself
};

inline Window makeWindow(float behind, float ahead) {
	Window w;
	behind = std::max(behind, 0.f);
	ahead = std::max(ahead, 0.f);
	w.behindWhole = static_cast<int>(std::floor(behind));
	w.aheadWhole = static_cast<int>(std::floor(ahead));
	w.behindPart = behind - static_cast<float>(w.behindWhole);
	w.aheadPart = ahead - static_cast<float>(w.aheadWhole);
	w.norm = 1.f / (static_cast<float>(w.behindWhole + w.aheadWhole + 1) + w.behindPart + w.aheadPart);
	w.empty = behind < 1e-4f && ahead < 1e-4f;
	return w;
}

// Largest reach of a window in whole pixels (plus the partial one), the padding a line needs on each side.
inline int reach(const Window& w) {
	return std::max(w.behindWhole, w.aheadWhole) + 1;
}

// One horizontal pass over one line of width pixels. line must hold at least width + 2 * reach(win) floats.
// in and out must not overlap.
inline void boxRow(const float* row, float* dst, int width, const Window& win, float* line) {
	const int pad = reach(win);
	const int a = win.behindWhole;
	const int b = win.aheadWhole;
	const float first = row[0];
	const float last = row[width - 1];
	for (int i = 0; i < pad; ++i) {
		line[i] = first;
		line[pad + width + i] = last;
	}
	std::copy(row, row + width, line + pad);
	const float* p = line + pad;  // p[x] is the pixel x, valid from -pad to width + pad - 1
	// Whole-pixel window sum for x = 0.
	float sum = 0.f;
	for (int k = -a; k <= b; ++k) {
		sum += p[k];
	}
	for (int x = 0; x < width; ++x) {
		dst[x] = (sum + win.behindPart * p[x - a - 1] + win.aheadPart * p[x + b + 1]) * win.norm;
		sum += p[x + b + 1] - p[x - a];
	}
}

// The same pass over the three channels of one line at once. The running sum of one channel waits on its own
// previous sum, so three independent sums in one loop run about three times faster than three separate passes.
// in[c], out[c] are the lines of channel c; line must hold 3 * (width + 2 * reach(win)) floats.
inline void boxRow3(const float* const in[3], float* const out[3], int width, const Window& win, float* line) {
	const int pad = reach(win);
	const int a = win.behindWhole;
	const int b = win.aheadWhole;
	const int stride = width + 2 * pad;
	const float* p[3];
	float sum[3];
	for (int c = 0; c < 3; ++c) {
		float* l = line + static_cast<size_t>(c) * stride;
		const float first = in[c][0];
		const float last = in[c][width - 1];
		for (int i = 0; i < pad; ++i) {
			l[i] = first;
			l[pad + width + i] = last;
		}
		std::copy(in[c], in[c] + width, l + pad);
		p[c] = l + pad;
		sum[c] = 0.f;
		for (int k = -a; k <= b; ++k) {
			sum[c] += p[c][k];
		}
	}
	const float* p0 = p[0];
	const float* p1 = p[1];
	const float* p2 = p[2];
	float s0 = sum[0], s1 = sum[1], s2 = sum[2];
	float* o0 = out[0];
	float* o1 = out[1];
	float* o2 = out[2];
	const float wb = win.behindPart, wa = win.aheadPart, norm = win.norm;
	for (int x = 0; x < width; ++x) {
		o0[x] = (s0 + wb * p0[x - a - 1] + wa * p0[x + b + 1]) * norm;
		o1[x] = (s1 + wb * p1[x - a - 1] + wa * p1[x + b + 1]) * norm;
		o2[x] = (s2 + wb * p2[x - a - 1] + wa * p2[x + b + 1]) * norm;
		s0 += p0[x + b + 1] - p0[x - a];
		s1 += p1[x + b + 1] - p1[x - a];
		s2 += p2[x + b + 1] - p2[x - a];
	}
}

// The same pass over a whole plane.
inline void boxRows(const float* in, float* out, int width, int height, const Window& win, float* line) {
	for (int y = 0; y < height; ++y) {
		boxRow(in + static_cast<size_t>(y) * width, out + static_cast<size_t>(y) * width, width, win, line);
	}
}

// One vertical pass over a plane, done row by row with a running sum per column, so memory is read in order.
// column must hold width floats.
inline void boxColumns(const float* in, float* out, int width, int height, const Window& win, float* column) {
	const int a = win.behindWhole;
	const int b = win.aheadWhole;
	auto rowAt = [&](int y) { return in + static_cast<size_t>(std::clamp(y, 0, height - 1)) * width; };
	for (int x = 0; x < width; ++x) {
		column[x] = 0.f;
	}
	for (int k = -a; k <= b; ++k) {
		const float* r = rowAt(k);
		for (int x = 0; x < width; ++x) {
			column[x] += r[x];
		}
	}
	for (int y = 0; y < height; ++y) {
		float* dst = out + static_cast<size_t>(y) * width;
		const float* rb = rowAt(y - a - 1);
		const float* ra = rowAt(y + b + 1);
		const float* rLeave = rowAt(y - a);
		const float wb = win.behindPart;
		const float wa = win.aheadPart;
		const float norm = win.norm;
		for (int x = 0; x < width; ++x) {
			dst[x] = (column[x] + wb * rb[x] + wa * ra[x]) * norm;
			column[x] += ra[x] - rLeave[x];
		}
	}
}

// Two passes of the same window in one direction: the kernel is a triangle (smoother than one box, no
// flat-topped steps in the result). Input in `a`, result back in `a`, `b` is scratch.
inline void twiceRows(float* a, float* b, int width, int height, const Window& win, float* line) {
	boxRows(a, b, width, height, win, line);
	boxRows(b, a, width, height, win, line);
}

inline void twiceColumns(float* a, float* b, int width, int height, const Window& win, float* column) {
	boxColumns(a, b, width, height, win, column);
	boxColumns(b, a, width, height, win, column);
}

// How far each control reaches, in pixels, for a picture width. These are the numbers the panel promises.
constexpr float kFocusMaxFraction = 0.06f;     // FOCUS at 1: blur radius as a fraction of the width
constexpr float kSmearMaxFraction = 0.15f;     // SMEAR at 1: tail length as a fraction of the width
constexpr float kConvergeMaxFraction = 0.03f;  // CONVERGE at +-1: shift of red and of blue
constexpr float kSharpRadiusFraction = 0.004f; // halo width of SHARP
constexpr float kSharpMaxAmount = 3.f;         // SHARP at 1: how much of the edge detail is added back
// Share of a smear pass's window that lies behind the pixel (to its left). The rest is ahead, so a bright
// edge is softened on both sides but trails mostly to the right, like a signal that lags.
constexpr float kSmearBehindShare = 0.8f;

inline float focusRadius(float focus, int width) {
	return std::clamp(focus, 0.f, 1.f) * kFocusMaxFraction * static_cast<float>(width);
}
inline float smearLength(float smear, int width) {
	return std::clamp(smear, 0.f, 1.f) * kSmearMaxFraction * static_cast<float>(width);
}
inline float convergeShift(float converge, int width) {
	return std::clamp(converge, -1.f, 1.f) * kConvergeMaxFraction * static_cast<float>(width);
}

}  // namespace focus_filter
