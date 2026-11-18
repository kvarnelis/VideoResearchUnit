#pragma once

// Video Oscillator sound outputs (A, B and C). Each oscillator of Bars and Rows is also an audio oscillator:
// its wave at its own frequency in hertz, drawn at Rack's sample rate. No Rack types here, so the tests can
// run it without Rack. Nothing allocates and nothing locks.
//
// The frequency is the one the picture uses. On BARS an oscillator counts cycles across one scan line, so its
// frequency is cycles x 15 734.26 Hz (the line rate in crt_beam.hpp); on ROWS it counts cycles down one field, so
// it is cycles x 59.94 Hz. BARS therefore sounds only for the lowest settings: a quarter of a cycle is 3.9 kHz,
// and one and a half cycles is already past what 48 kHz can carry.
//
// Above half the sample rate (Nyquist) a tone cannot be played, and folding it back would put a different pitch
// in the ear than the picture shows. The rule is silence: full level up to 0.4 of the sample rate, a smooth
// fade to nothing at 0.5, and no sound beyond. Where the oscillator is out of earshot at the low end, the output
// is simply a slow wave.
//
// LOCK, BEND, IN A/B/C and FM IN bend or hold the picture only. The sound is the free-running wave itself, so LOCK
// does not stop it and the pitch does not move with the scan.

#include "video_engine.hpp"

#include <algorithm>
#include <cmath>

namespace oscaudio {

constexpr double kFullBelow = 0.4;    // fraction of the sample rate at which the fade begins
constexpr double kSilentAt = 0.5;     // fraction of the sample rate at which it is silent
constexpr float kPeakVolts = 5.f;     // the wave runs -5 V to +5 V

// Frequency in hertz of a Bars and Rows oscillator at `cycles` (per line on BARS, per field on ROWS).
inline double hertz(double cycles, bool bars) {
	return cycles * (bars ? crt::kLineRateHz : crt::kLineRateHz / kPairFieldLines);
}

// Level, 0..1, for an oscillator whose phase advances `phaseStep` cycles each sample.
inline double fadeForStep(double phaseStep) {
	if (!(phaseStep > 0.0)) {
		return 1.0;
	}
	if (phaseStep >= kSilentAt) {
		return 0.0;
	}
	if (phaseStep <= kFullBelow) {
		return 1.0;
	}
	const double x = (kSilentAt - phaseStep) / (kSilentAt - kFullBelow);
	return x * x * (3.0 - 2.0 * x);
}

// The two places a pairWaveValue wave can jump, found once per WAVE setting: the end of the cycle, and the
// middle where a ramp steepened toward a square steps up.
struct Edges {
	float wave = -1.f;
	double wrap = 0.0;  // value after the end of the cycle minus the value before it
	double mid = 0.0;   // value just after phase 0.5 minus just before it

	void update(float w) {
		if (w == wave) {
			return;
		}
		wave = w;
		constexpr double e = 1e-7;
		wrap = pairWaveValue(e, w, 1e-9) - pairWaveValue(1.0 - e, w, 1e-9);
		mid = pairWaveValue(0.5 + e, w, 1e-9) - pairWaveValue(0.5 - e, w, 1e-9);
	}
};

// Two-sample polynomial correction for a unit step at phase 0, `t` cycles after it (wrapped to 0..1).
inline double polyBlep(double t, double dt) {
	if (t < dt) {
		const double x = t / dt;
		return x + x - x * x - 1.0;
	}
	if (t > 1.0 - dt) {
		const double x = (t - 1.0) / dt;
		return x * x + x + x + 1.0;
	}
	return 0.0;
}

// One sample, in volts, of the wave at `phase` (cycles, 0..1) advancing `phaseStep` cycles a sample. Edges are
// band-limited with a polynomial step correction, so a square or ramp does not fold back down the spectrum.
inline float sample(Edges& edges, double phase, double phaseStep, float wave) {
	const double fade = fadeForStep(phaseStep);
	if (fade <= 0.0) {
		return 0.f;
	}
	edges.update(wave);
	double p = phase - std::floor(phase);
	if (p >= 1.0) {
		p = 0.0;
	}
	double v = pairWaveValue(p, wave, 1e-9);
	if (edges.wrap != 0.0 || edges.mid != 0.0) {
		const double dt = std::min(phaseStep, 0.5);
		double mid = p - 0.5;
		mid -= std::floor(mid);
		v += 0.5 * edges.wrap * polyBlep(p, dt) + 0.5 * edges.mid * polyBlep(mid, dt);
	}
	return static_cast<float>((v - 0.5) * 2.0 * kPeakVolts * fade);
}

}  // namespace oscaudio
