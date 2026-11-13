#pragma once

// DeflectionYoke math: the parts of the module that are plain functions of numbers. Header-only and free of
// Rack, so the offline tests check exactly what the module and the renderer run. See
// docs/architecture.md, "DeflectionYoke".

#include <algorithm>
#include <cmath>

namespace deflection_yoke {

constexpr float kPi = 3.14159265358979323846f;

// ---- S coil ----
// The S coil rotates the beam's total deflection (normal scan plus the H and V coils) about the screen
// centre by `signal x kSCoilRadians`. The module sends depth x amount x source, so a full knob at full
// DEPTH on a sine swings the angle by +/-kSCoilRadians. The raster is rotated by a different angle on
// every scanline, which bends a straight line into an S and compresses and stretches it along the way.
// (Inference: no source states how the real instrument's continuous-wind coil acts.)
//
// 0.5 rad (about 29 degrees) was chosen by eye from a strength sweep of 0.25 to 1.0. One sine cycle per
// field folds the corners (scanlines cross, the picture turns over on itself) once depth x amount x
// kSCoilRadians passes about 0.26 rad; the default DEPTH of 0.5 keeps a full S knob just under that,
// and driving DEPTH and the knob toward full is where the fold-over lives. At 1.0 rad even a modest
// setting made a vortex of the picture.
constexpr float kSCoilRadians = 0.5f;

// ---- Oscillator rates ----
// A RATE knob stores v and plays kRateMinHz * 80000^v. v = 1 is 8 kHz, where the knob stopped before
// the rework, so every saved patch keeps its exact Hz. v = kRateTop is 20 kHz, past the 15.7 kHz line
// rate. expHzFromNormalized() clamps v to 0..1 and is shared with other modules, so the DeflectionYoke
// has this helper instead.
constexpr float kRateMinHz = 0.1f;
constexpr float kRateKnobOneHz = 8000.f;
constexpr float kRateMaxHz = 20000.f;
constexpr float kRateTop = 1.0811610657f;  // log(kRateMaxHz / kRateMinHz) / log(kRateKnobOneHz / kRateMinHz)
constexpr float kOscFloorHz = 0.001f;
constexpr float kOscNyquistFraction = 0.45f;

// Same expression as expHzFromNormalized(v, 0.1, 8000), so v in 0..1 gives bit-identical Hz.
inline float rateHzFromKnob(float v) {
	v = std::clamp(v, 0.f, kRateTop);
	return kRateMinHz * std::pow(kRateKnobOneHz / kRateMinHz, v);
}

inline float rateKnobFromHz(float hz) {
	hz = std::clamp(hz, kRateMinHz, kRateMaxHz);
	return std::log(hz / kRateMinHz) / std::log(kRateKnobOneHz / kRateMinHz);
}

// The oscillator never runs below 1 mHz or above 0.45 of the sample rate.
inline float oscillatorHz(float hz, float sampleRate) {
	return std::clamp(hz, kOscFloorHz, kOscNyquistFraction * sampleRate);
}

// ---- Waveforms ----
enum Wave : int {
	kWaveSine = 0,
	kWaveTriangle = 1,
	kWaveSquare = 2,
	kWaveDc = 3,
};

// One value of a coil source, -1..1. `phase` is in cycles and may be any number. Sine and triangle both
// start at 0 and rise. The square is +1 for the first half cycle. DC is a constant +1, which shifts the
// raster (or turns the S coil a fixed angle) when the coil's amount is set.
inline float wave(int shape, double phase) {
	switch (shape) {
		case kWaveTriangle: {
			const float f = static_cast<float>(phase - std::floor(phase));
			if (f < 0.25f) {
				return 4.f * f;
			}
			if (f < 0.75f) {
				return 2.f - 4.f * f;
			}
			return 4.f * f - 4.f;
		}
		case kWaveSquare:
			return (phase - std::floor(phase)) < 0.5 ? 1.f : -1.f;
		case kWaveDc:
			return 1.f;
		default:
			return std::sin(2.f * kPi * static_cast<float>(phase));
	}
}

// ---- Control voltage scalings ----
// Each CV adds to its knob and the sum is clamped. The module reads a CV only when its cable is connected.

// 1 V/oct, +/-10 V.
inline float octaveRatio(float volts) {
	return std::pow(2.f, std::clamp(volts, -10.f, 10.f));
}

// FINE is +/-50 cents on the knob; +/-5 V on its CV is the full +/-1.
inline float fineRatio(float fineKnob, float volts) {
	return std::pow(2.f, std::clamp(fineKnob + volts * 0.2f, -1.f, 1.f) / 24.f);
}

// X, Y and S AMOUNT: +/-5 V is +/-1.
inline float amountWithCv(float knob, float volts) {
	return std::clamp(knob + volts * 0.2f, -1.f, 1.f);
}

// DEPTH: 0.1 per volt.
inline float depthWithCv(float knob, float volts) {
	return std::clamp(knob + volts * 0.1f, 0.f, 1.f);
}

// PHASE, Y PHASE, S PHASE: 5 V is one cycle.
inline float phaseWithCv(float knob, float volts) {
	return knob + volts * 0.2f;
}

// CURL and TWIST: +/-5 V is +/-1. The knob reaches +/-1, the total +/-3.
constexpr float kShapeLimit = 3.f;
inline float shapeWithCv(float knob, float volts) {
	return std::clamp(knob + volts * 0.2f, -kShapeLimit, kShapeLimit);
}

// H SIZE, V SIZE and PERSIST (0..1), SPOT (0.1..0.6), GLOW (0..2): +/-5 V sweeps the knob's whole range.
inline float rangeWithCv(float knob, float volts, float lo, float hi) {
	return std::clamp(knob + volts * 0.2f * (hi - lo), lo, hi);
}

// LINES: exponential, 5 V is one octave, rounded to a whole line count.
inline int linesWithCv(float knobLines, float volts, int minLines, int maxLines) {
	const float lines = knobLines * std::pow(2.f, std::clamp(volts, -10.f, 10.f) / 5.f);
	return std::clamp(static_cast<int>(std::lround(lines)), minLines, maxLines);
}

// COIL: 1 V/oct on the cutoff, up to 0.45 of the sample rate.
constexpr float kCoilFloorHz = 10.f;
inline float coilCutoffWithCv(float knobHz, float volts, float sampleRate) {
	return std::clamp(knobHz * octaveRatio(volts), kCoilFloorHz, kOscNyquistFraction * sampleRate);
}

// FLIP H and V gates: above 1 V inverts the switch.
inline bool flipWithGate(bool switchOn, float volts) {
	return switchOn != (volts > 1.f);
}

}  // namespace deflection_yoke
