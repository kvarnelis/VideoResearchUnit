#pragma once

#include <algorithm>
#include <cmath>

// Exponential rate knobs. The knob value v in [0, 1] maps to minHz * (maxHz / minHz)^v, so each
// equal turn is the same musical step. Pure math, shared by the plugin and the offline tests.
inline float expHzFromNormalized(float v, float minHz, float maxHz) {
	v = std::clamp(v, 0.f, 1.f);
	return minHz * std::pow(maxHz / minHz, v);
}

inline float normalizedFromExpHz(float hz, float minHz, float maxHz) {
	hz = std::clamp(hz, minHz, maxHz);
	return std::log(hz / minHz) / std::log(maxHz / minHz);
}
