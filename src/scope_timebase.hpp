#pragma once

#include <algorithm>
#include <cmath>

// Scope Trace timebase. TIME sets how many seconds of audio the trace shows, independent of the
// sample rate: 1 ms at the bottom of the knob, 100 ms at the top (10^(-3 + 2 * TIME) s).
// The trace is captured at full sample rate, so nothing above the display is decimated away.
inline float scopeWindowSeconds(float timebase) {
	const float tb = std::clamp(timebase, 0.f, 1.f);
	return std::pow(10.f, -3.f + 2.f * tb);
}

// Internal fallback tone, used for an axis with no cable. It runs at two cycles per window, so
// the trace always shows two full cycles whatever TIME is. This is the frequency in Hz that the
// oscillator actually advances by.
inline float scopeFallbackHz(float timebase) {
	return 2.f / scopeWindowSeconds(timebase);
}

// One sample of the fallback phase (0..1 cycles). Used by the module once per audio sample.
inline double advanceScopeFallbackPhase(double phase, float timebase, float sampleTime) {
	phase += static_cast<double>(scopeFallbackHz(timebase)) * static_cast<double>(sampleTime);
	return phase - std::floor(phase);
}

// Trigger hysteresis, in channel units (+/-1 is full scale). A Schmitt trigger centred on the
// trigger level: it arms when the signal is at or below level - h and fires when it rises to
// level + h. Noise inside the band cannot fire it, and a sample that sits on the level (where
// float rounding decides its side) cannot move the start.
constexpr float kScopeTriggerHysteresis = 0.05f;

// Start index of the window for the time-based modes (Oscilloscope, Dual): the latest rising
// edge of x through `level` (Schmitt trigger, see kScopeTriggerHysteresis; level is in channel
// units, the TRIG LVL knob in volts / 5) whose window still fits, i.e. start <= length -
// windowSamples. Returns -1 when there is none, and the caller then free-runs (auto mode). Only x
// before the chosen start is used, so the start depends on the signal alone, which makes
// successive frames of a periodic input identical. With level = 0 (the default) this is the
// original zero-crossing trigger, bit for bit.
inline int scopeFindTrigger(const float* x, int length, int windowSamples, float hysteresis, float level = 0.f) {
	const int lastStart = length - windowSamples;
	if (x == nullptr || windowSamples < 2 || lastStart < 0) {
		return -1;
	}
	const float armBelow = level - hysteresis;
	const float fireAbove = level + hysteresis;
	int trigger = -1;
	bool armed = false;
	for (int i = 0; i <= lastStart; ++i) {
		if (x[i] <= armBelow) {
			armed = true;
		}
		else if (armed && x[i] >= fireAbove) {
			trigger = i;
			armed = false;
		}
	}
	return trigger;
}

// Internal fallback for an axis with no cable, per mode. phase is in cycles (0..1). Each mode
// gets a signal that suits what it draws:
// - XY and Polar: the Lissajous pair (X sine, Y sine at 1.51x with a 90 degree offset).
// - Oscilloscope: one stable sine. Only X is drawn, so Y repeats it.
// - Dual: X sine and Y triangle at the same rate, so both traces are stable and distinct.
inline void scopeFallbackSignal(int mode, double phase, float& x, float& y) {
	constexpr double kTwoPi = 6.28318530717958647692;
	constexpr int kOscilloscope = 1;
	constexpr int kDual = 2;
	const double angle = phase * kTwoPi;
	const float sine = static_cast<float>(std::sin(angle));
	x = sine;
	if (mode == kOscilloscope) {
		y = sine;
	}
	else if (mode == kDual) {
		y = static_cast<float>(1.0 - 4.0 * std::abs(phase - 0.5));
	}
	else {
		y = static_cast<float>(std::sin(angle * 1.51 + kTwoPi * 0.25));
	}
}
