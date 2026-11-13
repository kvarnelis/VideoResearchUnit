#pragma once

#include "lorenz.hpp"
#include <algorithm>
#include <array>
#include <cmath>

// Eight local Lorenz trajectories and one shared trajectory. RELATE is an output blend,
// not nonlinear coupling: at 1 every channel observes exactly the shared XYZ trajectory.
namespace octoscillator {
constexpr int kChannels = 8;
constexpr int kCoordinates = 3;
using Triple = std::array<float, kCoordinates>;
inline float sane(float v, float lo, float hi, float fallback = 0.f) {
	return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}
inline bool validState(const LorenzState& s) {
	return std::isfinite(s.x) && std::isfinite(s.y) && std::isfinite(s.z)
		&& std::abs(s.x) < 50. && std::abs(s.y) < 60. && s.z > 0. && s.z < 80.;
}
inline LorenzState initialState(int channel) {
	// Distinct points along a settled trajectory, generated with the same RK4 integrator.
	static constexpr LorenzState seeds[9] = {
		{11.82129659911268, 3.8111697376988927, 38.458065024893564},
		{10.578984254646882, 16.163589137218235, 21.434119338215794},
		{-2.7534545323799935, -4.68872118736427, 13.609075545860893},
		{-0.42239198290919955, -2.4718384419604593, 22.414050994797236},
		{-14.946421770720294, -22.952453282856666, 26.115661117746072},
		{12.995295690900845, 12.373715976071113, 33.567192380112949},
		{13.18911458653715, 18.087365867694061, 27.266769528513858},
		{-4.5600908682955561, -6.9769845840485569, 18.245632967844372},
		{-2.488892438099481, -3.6132961172200746, 16.102688840805794},
	};
	return seeds[std::clamp(channel, 0, 8)];
}
inline Triple coordinates(const LorenzState& s) {
	// Smooth bounded scaling; Z's resting level is removed before mapping to bipolar CV.
	return {static_cast<float>(std::tanh(s.x / 22.)),
		static_cast<float>(std::tanh(s.y / 28.)),
		static_cast<float>(std::tanh((s.z - 25.) / 22.))};
}
inline float speed(float octaves, float spread, int channel, float localOctaves) {
	const float position = (channel - 3.5f) / 3.5f;
	return std::clamp(std::exp2(sane(octaves, -8.f, 7.f)
		+ sane(spread, 0.f, 1.f) * position * 2.f
		+ sane(localOctaves, -2.f, 2.f)), 1.f / 1024.f, 128.f);
}
inline float voltage(float value, float depth, float offset, int range) {
	value = sane(value, -1.f, 1.f) * sane(depth, 0.f, 1.f);
	float v = range == 1 ? 2.5f + value * 2.5f : value * (range == 2 ? 1.f : 5.f);
	return std::clamp(v + sane(offset, -5.f, 5.f), -10.f, 10.f);
}
struct Engine {
	std::array<LorenzState, kChannels> local{};
	LorenzState shared;
	std::array<Triple, kChannels> values{};
	Engine() { reset(); }
	void reset() {
		for (int i = 0; i < kChannels; ++i) local[i] = initialState(i);
		shared = initialState(8);
		update(0.f);
	}
	static void advance(LorenzState& state, double elapsed, int seed) {
		if (!validState(state)) state = initialState(seed);
		// Bounded substeps preserve speed rather than silently clipping elapsed time.
		elapsed = std::isfinite(elapsed) ? std::clamp(elapsed, 0., .128) : 0.;
		const int count = std::max(1, static_cast<int>(std::ceil(elapsed / lorenz::kMaxStep)));
		for (int n = 0; n < count; ++n) lorenz::step(state, elapsed / count);
		if (!validState(state)) state = initialState(seed);
	}
	void update(float relationship) {
		const float r = sane(relationship, 0.f, 1.f);
		const Triple common = coordinates(shared);
		for (int i = 0; i < kChannels; ++i) {
			const Triple own = coordinates(local[i]);
			for (int c = 0; c < kCoordinates; ++c)
				values[i][c] = own[c] + r * (common[c] - own[c]);
		}
	}
	void process(float dt, float masterSpeed, const std::array<float, kChannels>& speeds, float relationship) {
		dt = sane(dt, 0.f, .001f);
		advance(shared, dt * sane(masterSpeed, 1.f / 1024.f, 128.f, 1.f), 8);
		for (int i = 0; i < kChannels; ++i) advance(local[i], dt * sane(speeds[i], 1.f / 1024.f, 128.f, 1.f), i);
		update(relationship);
	}
};
}
