#pragma once

#include "video_engine.hpp"

#include <algorithm>
#include <cmath>

// Shapes (VRU-21) control mapping: knob positions and CV volts in, ShapesParams out. Pure maths with no Rack
// types, so the module and the tests share one definition of what a knob and a volt mean.
//
//   SIDES   knob 3..12 sides, CV 0.9 sides per volt (10 V sweeps the whole range)
//   ROUND, POINT, MELT   knob 0..1, CV 0.1 per volt
//   SIZE    knob 0..1 gives an inscribed radius of 2.5 k^2 half picture heights, CV 0.1 per volt
//   X, Y    knob -0.5..0.5 of the picture width and height, CV 0.1 per volt (10 V is a whole picture)
//   ASPECT  knob -1..1 gives a width-over-height ratio of 4^k, CV 0.2 per volt (10 V sweeps the range)
//   TURN    knob -0.5..0.5 turns, CV 0.1 turn per volt, wrapped, so a 0 to 10 V ramp spins the shape
//           round with no seam
//   SPREAD  knob 0..1 gives a grey ramp half-width of 0.004 x 500^k half picture heights, CV 0.1 per volt
//   RING    knob 0..1 gives a ring radius of 1.2 k half picture heights (no CV)
//   REPEAT, COPIES   whole numbers 1..8 (no CV)
namespace shapes_math {

constexpr float kSizeMax = 2.5f;
constexpr float kSpreadMin = 0.004f;
constexpr float kSpreadRatio = 500.f;
constexpr float kRingMax = 1.2f;

inline float clamp01(float v) {
	return std::clamp(v, 0.f, 1.f);
}

inline float sizeFromKnob(float knob) {
	const float k = clamp01(knob);
	return kSizeMax * k * k;
}
inline float knobFromSize(float size) {
	return std::sqrt(std::clamp(size / kSizeMax, 0.f, 1.f));
}
inline float spreadFromKnob(float knob) {
	return kSpreadMin * std::pow(kSpreadRatio, clamp01(knob));
}
inline float knobFromSpread(float spread) {
	return clamp01(std::log(std::max(spread, kSpreadMin) / kSpreadMin) / std::log(kSpreadRatio));
}
inline float aspectFromKnob(float knob) {
	return std::exp2(2.f * std::clamp(knob, -1.f, 1.f));
}
inline float knobFromAspect(float ratio) {
	return std::clamp(0.5f * std::log2(std::clamp(ratio, 0.25f, 4.f)), -1.f, 1.f);
}

// Everything the module reads each sample: the knobs as stored in the patch, and the CV voltages.
struct ShapesControls {
	float sides = 4.f;
	float round = 0.f;
	float point = 0.f;
	float size = 0.45f;
	float x = 0.f;
	float y = 0.f;
	float aspect = 0.f;
	float turn = 0.f;
	float repeat = 1.f;
	float copies = 1.f;
	float ring = 0.4f;
	float melt = 0.f;
	float spread = 0.55f;
	float matte = 0.f;
	float sidesCv = 0.f;
	float roundCv = 0.f;
	float pointCv = 0.f;
	float sizeCv = 0.f;
	float xCv = 0.f;
	float yCv = 0.f;
	float aspectCv = 0.f;
	float turnCv = 0.f;
	float meltCv = 0.f;
	float spreadCv = 0.f;
};

// A voltage that is not a number (a patch cable from a misbehaving module) counts as 0 V, so the knob rules.
inline float cleanVolts(float v) {
	return std::isfinite(v) ? v : 0.f;
}
inline float cleanKnob(float v, float fallback) {
	return std::isfinite(v) ? v : fallback;
}

inline ShapesParams shapesParams(const ShapesControls& c) {
	ShapesParams p;
	p.sides = std::clamp(cleanKnob(c.sides, 4.f) + cleanVolts(c.sidesCv) * 0.9f, 3.f, 12.f);
	p.round = clamp01(cleanKnob(c.round, 0.f) + cleanVolts(c.roundCv) * 0.1f);
	p.point = clamp01(cleanKnob(c.point, 0.f) + cleanVolts(c.pointCv) * 0.1f);
	p.size = sizeFromKnob(cleanKnob(c.size, 0.45f) + cleanVolts(c.sizeCv) * 0.1f);
	p.x = std::clamp(cleanKnob(c.x, 0.f) + cleanVolts(c.xCv) * 0.1f, -1.f, 1.f);
	p.y = std::clamp(cleanKnob(c.y, 0.f) + cleanVolts(c.yCv) * 0.1f, -1.f, 1.f);
	p.aspect = aspectFromKnob(cleanKnob(c.aspect, 0.f) + cleanVolts(c.aspectCv) * 0.2f);
	const float turn = cleanKnob(c.turn, 0.f) + cleanVolts(c.turnCv) * 0.1f;
	p.turn = std::isfinite(turn) ? turn - std::round(turn) : 0.f;
	p.repeat = std::clamp(static_cast<int>(std::lround(std::clamp(cleanKnob(c.repeat, 1.f), 1.f, 8.f))), 1, 8);
	p.copies = std::clamp(static_cast<int>(std::lround(std::clamp(cleanKnob(c.copies, 1.f), 1.f, 8.f))), 1, 8);
	p.ring = kRingMax * clamp01(cleanKnob(c.ring, 0.4f));
	p.melt = clamp01(cleanKnob(c.melt, 0.f) + cleanVolts(c.meltCv) * 0.1f);
	p.spread = spreadFromKnob(cleanKnob(c.spread, 0.55f) + cleanVolts(c.spreadCv) * 0.1f);
	p.matte = c.matte > 0.5f ? 1 : 0;
	return p;
}

}  // namespace shapes_math
