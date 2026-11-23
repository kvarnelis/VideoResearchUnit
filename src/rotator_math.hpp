#pragma once

#include <cmath>

// Rotator (VRU-20) control maths, kept apart from the module so tests can drive it without Rack.
// All angles are in turns (1.0 is a full circle).

// The NOTCH step sizes, in turns: 1/16, 1/12, 1/8, 1/6, 1/4.
inline float rotatorStepTurns(int index) {
	static constexpr float kSteps[5] = {1.f / 16.f, 1.f / 12.f, 1.f / 8.f, 1.f / 6.f, 1.f / 4.f};
	return kSteps[index < 0 ? 0 : (index > 4 ? 4 : index)];
}

// SPIN knob (-1..1, plus CV already folded in) to turns per second. Quadratic, so the slow end is fine
// enough to set by hand; 1 is two turns a second.
inline double rotatorSpinRate(float spin) {
	const float v = spin < -1.f ? -1.f : (spin > 1.f ? 1.f : spin);
	return 2.0 * static_cast<double>(v) * std::fabs(static_cast<double>(v));
}

// Wraps a turn count into [-0.5, 0.5). Exactly 0 for whole turns, so a 0 to 10 V saw on the ANGLE input
// has no seam: 10 V is the same picture as 0 V.
inline float rotatorWrapTurns(double turns) {
	turns -= std::floor(turns + 0.5);
	return static_cast<float>(turns);
}

// The angle the renderer gets. angleKnob is in turns, angleVolts is the ANGLE input (10 V is one turn),
// spin and notch are the accumulated turns below.
inline float rotatorAngle(float angleKnob, float angleVolts, double spin, double notch) {
	return rotatorWrapTurns(static_cast<double>(angleKnob) + static_cast<double>(angleVolts) * 0.1 + spin + notch);
}

// State that has to be accumulated on the audio thread: the free-running spin and the notched turn.
struct RotatorState {
	double spin = 0.0;         // turns, wrapped to [0, 1)
	double notchTarget = 0.0;  // where the clicks have asked the picture to be
	double notchPos = 0.0;     // where the glide has got to
	double notchRate = 0.0;    // turns per second while gliding (set by the last click)

	void reset() { spin = notchTarget = notchPos = notchRate = 0.0; }

	// One tick of dt seconds. A trigger adds one step and sets the speed that reaches it in glide seconds
	// (instantly when glide is 0). The glide moves at that constant speed, so a fast clock just chases the target.
	void advance(double dt, float spinKnob, float stepTurns, float glideSeconds, bool notch, bool resetNow) {
		if (resetNow) {
			reset();
			return;
		}
		spin += rotatorSpinRate(spinKnob) * dt;
		spin -= std::floor(spin);
		if (notch) {
			notchTarget += stepTurns;
			notchRate = glideSeconds > 1e-4f ? static_cast<double>(stepTurns) / glideSeconds : 0.0;
		}
		if (notchRate <= 0.0) {
			notchPos = notchTarget;
		}
		else if (notchPos < notchTarget) {
			notchPos += notchRate * dt;
			if (notchPos > notchTarget) notchPos = notchTarget;
		}
		// Keep the numbers small on a long run: whole turns change nothing, but move both together.
		if (notchTarget > 64.0) {
			const double whole = std::floor(notchPos);
			notchTarget -= whole;
			notchPos -= whole;
		}
	}
};

// CV for the controls that got jacks later (PIVOT X, PIVOT Y, EDGE, FOLDS, STEP, GLIDE): the knob plus volts times a
// scale, kept inside the knob's own range. An open jack reads 0 V and leaves the knob exactly as it is. Scales: the
// pivots move a whole frame in 10 V (0.1 a volt, as X and Y do); 10 V sweeps EDGE (0.3), FOLDS (1.1), STEP (0.4) and
// GLIDE (0.2 s a volt) across their whole ranges.
inline float rotatorCv(float knob, float volts, float perVolt, float lo, float hi) {
	const float v = std::isfinite(volts) ? (volts < -20.f ? -20.f : (volts > 20.f ? 20.f : volts)) : 0.f;
	const float sum = knob + v * perVolt;
	return sum < lo ? lo : (sum > hi ? hi : sum);
}
// The same for a stepped knob: the sum rounded to a whole position.
inline int rotatorSteppedCv(float knob, float volts, float perVolt, int lo, int hi) {
	return static_cast<int>(std::lround(rotatorCv(knob, volts, perVolt, static_cast<float>(lo), static_cast<float>(hi))));
}
