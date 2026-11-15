#pragma once

#include <algorithm>
#include <array>
#include <cmath>

// Vector Osc (VRU-25): a pair of oscillators, X and Y, each made of up to eight slightly detuned
// voices that are averaged, with a wave shape that runs continuously from sine to triangle to saw to
// square. It feeds the X IN and Y IN jacks of Scope Trace. Everything here is plain and allocation free.
//
// Y runs at X's frequency times RATIO. PHASE turns Y against X. With one voice, no detune, a sine
// wave, RATIO 1 and PHASE a quarter turn, X is sin and Y is cos, so the pair draws a circle.
// DETUNE spreads the voice frequencies evenly above and below the base (and nudges each voice's
// starting phase), so the figure thickens into a bundle of traces that slowly drifts and breathes.
// At DETUNE 0 every voice is the same, so the figure is one clean line.
namespace vector_osc {

constexpr int kMaxVoices = 8;
constexpr float kTwoPi = 6.2831853f;

inline float sane(float v, float lo, float hi, float fallback = 0.f) {
	return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

// Whole and simple ratios for the RATIO knob. Y frequency = X frequency x table value.
constexpr int kRatioCount = 19;
constexpr float kRatios[kRatioCount] = {
	0.25f, 1.f / 3.f, 0.5f, 2.f / 3.f, 0.75f, 1.f, 5.f / 4.f, 4.f / 3.f, 3.f / 2.f, 5.f / 3.f,
	2.f, 5.f / 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f};
constexpr const char* kRatioNames[kRatioCount] = {
	"1:4", "1:3", "1:2", "2:3", "3:4", "1:1", "5:4", "4:3", "3:2", "5:3",
	"2:1", "5:2", "3:1", "4:1", "5:1", "6:1", "7:1", "8:1", "9:1"};
constexpr int kRatioUnity = 5;

inline int ratioIndex(float v) {
	return std::clamp(static_cast<int>(std::lround(sane(v, 0.f, kRatioCount - 1.f, kRatioUnity))), 0, kRatioCount - 1);
}

// FINE moves Y off the chosen ratio: up to one semitone either way, finer near the middle.
inline float fineFactor(float fine) {
	const float f = sane(fine, -1.f, 1.f);
	return std::exp2(f * std::abs(f) / 12.f);
}

// Voice spread in -1 to +1, evenly spaced; a single voice sits at 0.
inline float voiceSpread(int k, int n) {
	return n <= 1 ? 0.f : (2.f * k - (n - 1.f)) / (n - 1.f);
}

// The outer voices sit this many octaves above and below the base. The knob is squared, so the
// first part of its travel is slow, gentle beating and DETUNE 1 is a full octave each way.
inline float detuneOctaves(float detune) {
	const float d = sane(detune, 0.f, 1.f);
	return d * d;
}

// Fixed per-voice starting-phase offsets (in turns, -1 to 1), scaled by DETUNE, so a bundle opens
// out as DETUNE rises. They are constants, so a saved patch redraws the same figure.
constexpr float kSkewX[kMaxVoices] = {0.f, .31f, -.47f, .83f, -.19f, .59f, -.71f, .13f};
constexpr float kSkewY[kMaxVoices] = {0.f, -.53f, .29f, -.17f, .67f, -.89f, .41f, -.37f};

// Polynomial step correction for a jump at phase 0, with dt the phase step per sample.
inline float blep(float t, float dt) {
	if (t < dt) { t /= dt; return t + t - t * t - 1.f; }
	if (t > 1.f - dt) { t = (t - 1.f) / dt; return t * t + t + t + 1.f; }
	return 0.f;
}

inline float triangle(float p) {
	return p < .25f ? 4.f * p : (p < .75f ? 2.f - 4.f * p : 4.f * p - 4.f);
}

// One cycle of the wave at phase p. wave runs 0 to 1 through sine, triangle, saw and square, with a
// smooth cross-fade between neighbours. All four start at zero and rise (the square at +1), so at
// any wave setting X and Y keep their quarter-turn relation. dt is the phase step per sample, used
// to round the jumps of the saw and square, which keeps audio-rate figures clean.
inline float waveform(float p, float dt, float wave) {
	p -= std::floor(p);
	dt = std::clamp(dt, 1e-7f, .45f);
	const float pos = sane(wave, 0.f, 1.f) * 3.f;
	const int seg = std::min(static_cast<int>(pos), 2);
	const float f = pos - static_cast<float>(seg);
	const float w = f * f * (3.f - 2.f * f);
	float q = p + .5f;
	q -= std::floor(q);
	float a, b;
	if (seg == 0) {
		a = std::sin(kTwoPi * p);
		b = triangle(p);
	} else if (seg == 1) {
		a = triangle(p);
		b = 2.f * q - 1.f - blep(q, dt);
	} else {
		a = 2.f * q - 1.f - blep(q, dt);
		b = (p < .5f ? 1.f : -1.f) + blep(p, dt) - blep(q, dt);
	}
	return std::clamp(a + (b - a) * w, -1.f, 1.f);
}

struct Settings {
	float freq = 261.6256f;   // X base frequency in Hz
	int voices = 1;           // 1 to 8
	float detune = 0.f;       // 0 to 1
	float wave = 0.f;         // 0 to 1, X
	float waveOffsetY = 0.f;  // -1 to 1, added to wave for Y
	float ratio = 1.f;        // Y frequency over X frequency (already includes FINE)
	float phase = 0.f;        // Y phase against X, in turns
	float level = 1.f;        // 0 to 1
};

struct Output {
	float x = 0.f, y = 0.f, z = 0.f;   // volts: X and Y are +/-5 x level, Z is 0 to 10
};

struct Engine {
	std::array<double, kMaxVoices> phaseX{}, phaseY{};
	float lastX = 0.f, lastY = 0.f, zState = 10.f;
	bool primed = false;

	void reset() {
		phaseX.fill(0.0);
		phaseY.fill(0.0);
		primed = false;
	}

	// Z is brightness that follows beam speed: slow parts of the figure are bright, fast parts dim.
	// Speed is measured against a circle drawn at X's frequency, so a plain circle gives 6.7 V, a
	// resting point 10 V and a fast sweep close to 0 V. It is smoothed over about 2 ms so the jumps
	// of the saw and square shapes do not make Z flicker.
	Output process(const Settings& s, float sampleTime) {
		const float fx = sane(s.freq, 1e-3f, 20000.f, 261.6256f);
		const int n = std::clamp(s.voices, 1, kMaxVoices);
		const float ratio = sane(s.ratio, .01f, 64.f, 1.f);
		const float octaves = detuneOctaves(s.detune);
		const float skew = sane(s.detune, 0.f, 1.f);
		const float waveX = sane(s.wave, 0.f, 1.f);
		const float waveY = std::clamp(waveX + sane(s.waveOffsetY, -1.f, 1.f), 0.f, 1.f);
		const float turn = sane(s.phase, -4.f, 4.f);
		float sumX = 0.f, sumY = 0.f;
		for (int k = 0; k < n; ++k) {
			const float mult = std::exp2(octaves * voiceSpread(k, n));
			const float stepX = std::min(fx * mult * sampleTime, .45f);
			const float stepY = std::min(fx * mult * ratio * sampleTime, .45f);
			const float px = static_cast<float>(phaseX[k]) + skew * kSkewX[k];
			const float py = static_cast<float>(phaseY[k]) + turn + skew * kSkewY[k];
			sumX += waveform(px, stepX, waveX);
			sumY += waveform(py, stepY, waveY);
			phaseX[k] += stepX;
			phaseY[k] += stepY;
			phaseX[k] -= std::floor(phaseX[k]);
			phaseY[k] -= std::floor(phaseY[k]);
		}
		const float inv = 1.f / static_cast<float>(n);
		const float ux = std::clamp(sumX * inv, -1.f, 1.f);
		const float uy = std::clamp(sumY * inv, -1.f, 1.f);
		if (!primed) { lastX = ux; lastY = uy; primed = true; zState = 10.f; }
		const float step = std::hypot(ux - lastX, uy - lastY);
		const float circle = kTwoPi * fx * sampleTime;   // one step round a unit circle
		const float speed = circle > 0.f ? step / circle : 0.f;
		const float target = 10.f / (1.f + .5f * speed * speed);
		const float a = 1.f - std::exp(-sampleTime / .002f);
		zState += (target - zState) * a;
		lastX = ux;
		lastY = uy;
		const float lvl = sane(s.level, 0.f, 1.f);
		Output o;
		o.x = 5.f * lvl * ux;
		o.y = 5.f * lvl * uy;
		o.z = std::clamp(zState, 0.f, 10.f);
		return o;
	}
};

} // namespace vector_osc
