#pragma once
// VRU-26 Video Probe: the maths that turns a picture into voltages. No Rack types.
//
// The picture is rendered off the audio thread (the module's widget, at kW x kH) and analysed there into a
// Frame: four summed-area tables (red, green, blue, luma) plus the whole-picture meters. process() only reads
// the latest finished Frame, once per audio sample, through sample(). Summed-area tables make a square region
// of any size cost four corner reads per channel, so SIZE costs nothing extra.
//
// Pixel i covers [i, i+1) along an axis. A table entry sat[c][y * kStride + x] is the sum of channel c over all
// pixels with ix < x and iy < y, so entry (0, 0) is 0 and entry (kW, kH) is the whole picture. Reading the
// table at fractional corners with bilinear weights gives the exact integral of the picture there (the table
// is bilinear inside a pixel), so a box average is exact for any position and size. A box one pixel wide is
// exactly the bilinear interpolation between pixel centres, so SIZE at its minimum is a plain point read.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace video_probe {

constexpr int kW = 320;                 // size the picture is rendered at for the probe
constexpr int kH = 240;
constexpr int kStride = kW + 1;
constexpr int kSatSize = (kW + 1) * (kH + 1);
enum Channel { kRed = 0, kGreen = 1, kBlue = 2, kLuma = 3, kChannels = 4 };

constexpr float kMaxHalfPx = 120.f;     // half the box side at SIZE 1: a 240 pixel square, the full picture height
constexpr float kMotionGain = 8.f;      // a mean change of 12.5% of full brightness reads 10 V
constexpr float kGateHysteresis = .03f; // GATE turns on above LEVEL + this and off below LEVEL - this (of 0..1)

// A voltage that is NaN or beyond a limit is made safe.
inline float sane(float v, float lo, float hi) {
	if (!(v == v)) return 0.f;
	return std::min(std::max(v, lo), hi);
}

struct Frame {
	uint32_t sat[kChannels][kSatSize];
	float avg = 0.f;      // mean brightness of the whole picture, 0..1
	float motion = 0.f;   // change from the previous picture, 0..1 (already multiplied by kMotionGain and clamped)
	bool valid = false;
};

inline int lumaOf(int r, int g, int b) { return (77 * r + 150 * g + 29 * b) >> 8; }   // 0..255, grey stays itself

// Builds Frames from RGBA pictures, on the UI side. Keeps the previous brightness picture for MOTION.
struct Analyzer {
	std::vector<uint8_t> prevLuma;
	bool havePrev = false;
	Analyzer() : prevLuma(static_cast<size_t>(kW) * kH, 0) {}
	void reset() { havePrev = false; }

	// rgba is kW x kH x 4, top row first.
	void analyze(const unsigned char* rgba, Frame& out) {
		for (int c = 0; c < kChannels; ++c)
			for (int x = 0; x <= kW; ++x) out.sat[c][x] = 0;
		uint64_t lumaSum = 0, diffSum = 0;
		for (int y = 0; y < kH; ++y) {
			uint32_t run[kChannels] = {0, 0, 0, 0};
			for (int c = 0; c < kChannels; ++c) out.sat[c][(y + 1) * kStride] = 0;
			for (int x = 0; x < kW; ++x) {
				const unsigned char* p = rgba + (static_cast<size_t>(y) * kW + x) * 4;
				const int l = lumaOf(p[0], p[1], p[2]);
				run[kRed] += p[0]; run[kGreen] += p[1]; run[kBlue] += p[2]; run[kLuma] += static_cast<uint32_t>(l);
				for (int c = 0; c < kChannels; ++c)
					out.sat[c][(y + 1) * kStride + x + 1] = out.sat[c][y * kStride + x + 1] + run[c];
				lumaSum += static_cast<uint64_t>(l);
				uint8_t& prev = prevLuma[static_cast<size_t>(y) * kW + x];
				if (havePrev) diffSum += static_cast<uint64_t>(std::abs(l - static_cast<int>(prev)));
				prev = static_cast<uint8_t>(l);
			}
		}
		const double n = static_cast<double>(kW) * kH * 255.0;
		out.avg = static_cast<float>(static_cast<double>(lumaSum) / n);
		out.motion = havePrev ? std::min(1.f, static_cast<float>(static_cast<double>(diffSum) / n) * kMotionGain) : 0.f;
		out.valid = true;
		havePrev = true;
	}
};

// Table value at fractional corner (fx, fy), both inside [0, kW] x [0, kH].
inline double satAt(const Frame& f, int c, double fx, double fy) {
	int ix = std::min(static_cast<int>(fx), kW - 1);
	int iy = std::min(static_cast<int>(fy), kH - 1);
	const double ax = fx - ix, ay = fy - iy;
	const uint32_t* t = f.sat[c] + iy * kStride + ix;
	const double top = t[0] * (1.0 - ax) + t[1] * ax;
	const double bottom = t[kStride] * (1.0 - ax) + t[kStride + 1] * ax;
	return top * (1.0 - ay) + bottom * ay;
}

struct Point { float r = 0.f, g = 0.f, b = 0.f, luma = 0.f; };   // each 0..1

// Half the box side, in picture pixels. 0.5 (a box one pixel wide) is the plain bilinear point read.
inline float halfWidthPx(float size) {
	const float s = sane(size, 0.f, 1.f);
	return .5f + s * s * (kMaxHalfPx - .5f);
}

// Volts to picture position. +/-5 V spans the picture; beyond clamps. +5 V on Y is the top edge.
inline double pixelX(float volts) { return (sane(volts, -5.f, 5.f) + 5.0) * (kW / 10.0); }
inline double pixelY(float volts) { return (5.0 - sane(volts, -5.f, 5.f)) * (kH / 10.0); }

// Average colour of the square of half-width halfWidthPx(size) centred on the volts position. Near an edge the
// square is cut at the edge and the average is over what is left.
inline Point sample(const Frame& f, float xVolts, float yVolts, float size) {
	Point p;
	if (!f.valid) return p;
	const double cx = pixelX(xVolts), cy = pixelY(yVolts), h = halfWidthPx(size);
	const double x0 = std::max(cx - h, 0.0), x1 = std::min(cx + h, static_cast<double>(kW));
	const double y0 = std::max(cy - h, 0.0), y1 = std::min(cy + h, static_cast<double>(kH));
	const double area = (x1 - x0) * (y1 - y0) * 255.0;
	if (!(area > 0.0)) return p;
	float* out[kChannels] = {&p.r, &p.g, &p.b, &p.luma};
	for (int c = 0; c < kChannels; ++c) {
		const double sum = satAt(f, c, x1, y1) - satAt(f, c, x0, y1) - satAt(f, c, x1, y0) + satAt(f, c, x0, y0);
		*out[c] = static_cast<float>(std::min(std::max(sum / area, 0.0), 1.0));
	}
	return p;
}

// Combined position: knob plus trimmer times jack, clamped to +/-5 V.
inline float position(float knobVolts, float trim, float jackVolts) {
	return sane(sane(knobVolts, -5.f, 5.f) + sane(trim, -1.f, 1.f) * sane(jackVolts, -20.f, 20.f), -5.f, 5.f);
}

// One-pole smoothing coefficient per sample. SMOOTH 0 is off (1); 1 is a time constant of 1 s; the time
// constant grows from 0.5 ms exponentially in between.
inline float smoothCoeff(float smooth, float sampleTime) {
	const float s = sane(smooth, 0.f, 1.f);
	if (s < .001f) return 1.f;
	const float tau = .0005f * std::pow(2000.f, s);
	return 1.f - std::exp(-sampleTime / tau);
}

// GATE: on above level + hysteresis, off below level - hysteresis, otherwise unchanged. level and luma are 0..1.
struct Gate {
	bool on = false;
	bool process(float luma, float level) {
		if (on) { if (luma < level - kGateHysteresis) on = false; }
		else if (luma > level + kGateHysteresis) on = true;
		return on;
	}
};

// Hands finished Frames from one writer thread (the widget) to one reader thread (process()) with no locks and
// no allocation. Three slots. The writer fills its own slot, then swaps it with the middle slot and marks the
// middle as new. The reader, when the middle is marked new, swaps it with the slot it holds. Neither ever
// touches a slot the other holds, and the reader always holds the latest complete Frame.
struct TripleBuffer {
	static constexpr int kNew = 4;
	std::atomic<int> middle{1};
	int back = 2;    // writer only
	int front = 0;   // reader only
	int writeIndex() const { return back; }
	void publish() { back = middle.exchange(back | kNew, std::memory_order_acq_rel) & 3; }
	// Returns the slot the reader should read. Call once per sample or block; cheap when nothing is new.
	int readIndex() {
		if (middle.load(std::memory_order_relaxed) & kNew) front = middle.exchange(front, std::memory_order_acq_rel) & 3;
		return front;
	}
	// The same, and says whether this call took a picture that had not been read before.
	int readIndex(bool& fresh) {
		fresh = (middle.load(std::memory_order_relaxed) & kNew) != 0;
		return readIndex();
	}
};

// FRAME output: a 10 V pulse, 1 ms long (the length Scope Trace's SYNC uses), each time a new picture arrives on the
// audio side. A picture that is published twice before process() looks counts once; a picture that is not
// replaced counts never.
struct FrameClock {
	int left = 0;   // samples of the pulse still to go
	float process(bool fresh, float sampleTime) {
		if (fresh) left = std::max(1, static_cast<int>(std::lround(1e-3 / std::max(static_cast<double>(sampleTime), 1e-9))));
		if (left > 0) { --left; return 10.f; }
		return 0.f;
	}
};

// What process() does with the hand-off, one audio sample at a time, without Rack types so a test can run it. The
// outputs are in volts. A new picture is taken at the same sample that FRAME goes high, so LUMA, R, G and B change
// at or after the pulse and never before it.
struct Reader {
	float luma = 0.f, r = 0.f, g = 0.f, b = 0.f;
	Gate gate;
	FrameClock clock;
	struct Out { bool valid = false; float luma = 0.f, r = 0.f, g = 0.f, b = 0.f, avg = 0.f, motion = 0.f, gate = 0.f, frame = 0.f; };
	void reset() { luma = r = g = b = 0.f; gate.on = false; }
	Out process(TripleBuffer& tb, const Frame* frames, float xv, float yv, float size, float smooth, float level, float sampleTime) {
		Out o;
		bool fresh = false;
		const Frame& f = frames[tb.readIndex(fresh)];
		if (f.valid) {
			const Point p = sample(f, xv, yv, size);
			const float a = smoothCoeff(smooth, sampleTime);
			luma += (p.luma - luma) * a;
			r += (p.r - r) * a;
			g += (p.g - g) * a;
			b += (p.b - b) * a;
			gate.process(p.luma, sane(level, 0.f, 1.f));
			o.valid = true;
			o.avg = f.avg * 10.f;
			o.motion = f.motion * 10.f;
		}
		o.luma = luma * 10.f; o.r = r * 10.f; o.g = g * 10.f; o.b = b * 10.f;
		o.gate = gate.on ? 10.f : 0.f;
		o.frame = clock.process(fresh && f.valid, sampleTime);
		return o;
	}
};

} // namespace video_probe
