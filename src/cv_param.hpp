#pragma once

#include <algorithm>
#include <cmath>

// CV maths shared by the modules' process() and the tests. Pure float maths, no Rack types.
//
// One convention for every CV-controlled knob: the knob sets the base value, the CV jack adds
// amount * volts * perVolt to it, and the attenuverter (amount, -1..+1, default +1) scales or
// inverts the CV. An unpatched jack reads 0 V, so it adds nothing.
namespace rasterlab_cv {

// Knob plus attenuated CV, clamped to [lo, hi]. perVolt is the knob units one volt moves, e.g.
// 1/5 for a bipolar +-5 V signal across a -1..+1 range, or 1/10 for a unipolar 0..10 V sweep of
// a 0..1 knob. With amount = +1 and cvVolts = 0 this is just clamp(knob, lo, hi).
inline float modulated(float knob, float cvVolts, float amount, float perVolt, float lo, float hi) {
	return std::clamp(knob + amount * cvVolts * perVolt, lo, hi);
}

// Same sum, wrapped into [0, 1) instead of clamped: for a circular control such as hue, where 10 V
// at 1/10 per volt is once around the wheel. Negative results wrap round from the top
// (-0.25 becomes 0.75). A sum already inside [0, 1) is returned unchanged.
inline float modulatedWrapped(float knob, float cvVolts, float amount, float perVolt) {
	const float sum = knob + amount * cvVolts * perVolt;
	float wrapped = sum - std::floor(sum);
	// A tiny negative sum rounds up to exactly 1.0 in float; keep the result below 1.
	if (wrapped >= 1.f) {
		wrapped = 0.f;
	}
	return wrapped;
}

// Wraps a turn count into [-1, 1): one full turn is 2 units, so +1 and -1 are the same angle (180 degrees)
// and the wrap is continuous in the picture. Floor-based, so negatives and large values work; NaN or
// infinity gives 0.
inline float wrapTurn(float value) {
	if (!std::isfinite(value)) {
		return 0.f;
	}
	float wrapped = value - 2.f * std::floor((value + 1.f) * 0.5f);
	// A tiny negative value just below -1 can round up to exactly 1.0 in float; keep the result below 1.
	if (wrapped >= 1.f) {
		wrapped = -1.f;
	}
	return wrapped;
}

// Knob plus attenuated CV, wrapped into [-1, 1) with wrapTurn: for a rotation, which has no end stop. A
// steadily rising CV turns the picture round and round, and the knob sets the start angle.
inline float modulatedTurn(float knob, float cvVolts, float amount, float perVolt) {
	return wrapTurn(knob + amount * cvVolts * perVolt);
}

// Beam brightness (Z) factor. Unpatched, the beam is always fully on (1). Patched with amount +1
// it is the classic Z input, clamp(volts / 5, 0, 2): 0 V blanks, 5 V is full, 10 V is double.
// A smaller positive amount lifts the floor (amount 0.5 at 0 V is half bright) and amount 0 is
// always full. A negative amount inverts it, Tektronix-style: a positive voltage dims the beam, fully
// blank at 5 V when the amount is -1 (at -0.5, 5 V leaves it half bright).
inline float zModulation(float cvVolts, float amount, bool patched) {
	if (!patched) {
		return 1.f;
	}
	return std::clamp(1.f - std::max(amount, 0.f) + amount * cvVolts / 5.f, 0.f, 2.f);
}

}  // namespace rasterlab_cv
