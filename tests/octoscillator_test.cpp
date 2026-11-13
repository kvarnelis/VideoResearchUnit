#include "octoscillator_math.hpp"
#include <cstdio>
#include <limits>

namespace o = octoscillator;
int checks = 0, failures = 0;
void check(bool ok, const char* msg) {
	++checks;
	if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", msg); }
}
bool near(float a, float b, float eps = 1e-6f) { return std::abs(a - b) < eps; }
int main() {
	// Numerical accuracy against a finer integration, at the fastest supported rate.
	LorenzState coarse = o::initialState(0), fine = coarse;
	for (int i = 0; i < 1000; ++i) {
		o::Engine::advance(coarse, 128. / 8000., 0);
		for (int j = 0; j < 16; ++j) lorenz::step(fine, 128. / 8000. / 16.);
		if (i < 30) check(std::abs(coarse.x - fine.x) < .005, "bounded substeps match fine reference");
	}
	check(o::validState(coarse), "fast integration remains valid");
	// Each local trajectory starts on the attractor and has distinct state.
	o::Engine e;
	for (int i = 0; i < 8; ++i) {
		check(o::validState(e.local[i]), "settled seed is valid");
		for (int j = 0; j < i; ++j) check(std::abs(e.local[i].x - e.local[j].x) > .001, "distinct channel seeds");
	}
	std::array<float, 8> rates;
	for (int i = 0; i < 8; ++i) rates[i] = o::speed(1.f, .5f, i, 0.f);
	check(rates.front() < rates.back(), "spread orders rates");
	check(near(o::speed(1,0,0,0), o::speed(1,0,7,0)), "zero spread preserves equal rates");
	check(near(o::speed(0,0,0,1), 2), "one octave doubles speed");
	check(near(o::speed(-8,0,0,0), 1.f/256), "very slow range retained");
	check(near(o::speed(7,1,7,2), 128), "upper rate is bounded");
	// Relationship control is an exact convex blend; at full it ignores local rates.
	e.update(0);
	const auto independent = e.values;
	e.update(1);
	const auto together = e.values;
	for (int i = 1; i < 8; ++i) for (int c = 0; c < 3; ++c)
		check(near(together[i][c], together[0][c]), "full relationship gives shared coordinates");
	e.update(.5f);
	for (int i = 0; i < 8; ++i) for (int c = 0; c < 3; ++c)
		check(near(e.values[i][c], (independent[i][c] + together[i][c]) / 2), "intermediate relationship is halfway");
	// Every coordinate moves at every supported sample rate; no infinities or range escapes.
	for (float sampleRate : {8000.f, 44100.f, 48000.f, 96000.f}) {
		e.reset();
		for (auto& rate : rates) rate = 128.f;
		const auto start = e.values;
		for (int n = 0; n < 60000; ++n) {
			e.process(1.f/sampleRate, 128, rates, .3f);
			if (n % 100 == 0) for (int i = 0; i < 8; ++i) for (float v : e.values[i])
				check(std::isfinite(v) && std::abs(v) < 1.f, "bounded finite continuous output");
		}
		for (int i = 0; i < 8; ++i) for (int c = 0; c < 3; ++c)
			check(std::abs(e.values[i][c] - start[i][c]) > 1e-5, "all coordinates evolve");
	}
	check(near(o::coordinates({0,0,25})[2], 0), "Z is centered");
	// Local depth, global scaling and offsets are voltage math, independent of trajectories.
	check(near(o::voltage(1,1,0,0),5), "bipolar upper range");
	check(near(o::voltage(-1,1,0,0),-5), "bipolar lower range");
	check(near(o::voltage(-1,1,0,1),0), "unipolar lower range");
	check(near(o::voltage(1,1,0,1),5), "unipolar upper range");
	check(near(o::voltage(.5f,.5f,1,2),1.25f), "small CV plus offset");
	check(near(o::voltage(-.8f,0,3,0),3), "zero depth leaves offset");
	check(near(o::voltage(.9f,0,0,1),2.5f), "unipolar zero depth leaves midpoint");
	// Reset and restoring fixed-size state reproduce the motion without allocating.
	e.reset();
	o::Engine saved = e;
	for (int n = 0; n < 100; ++n) e.process(1.f/48000,2,rates,.4f);
	const auto target = e.values;
	e = saved;
	for (int n = 0; n < 100; ++n) e.process(1.f/48000,2,rates,.4f);
	check(e.values == target, "state restoration is deterministic");
	e.reset();
	check(e.local[0].x == saved.local[0].x, "reset restores seed");
	e.local[0].x = std::numeric_limits<double>::quiet_NaN();
	e.shared.z = std::numeric_limits<double>::infinity();
	rates[2] = std::numeric_limits<float>::quiet_NaN();
	e.process(1.f/48000, 2, rates, std::numeric_limits<float>::quiet_NaN());
	check(o::validState(e.local[0]) && o::validState(e.shared), "invalid state recovers");
	for (const auto& channel : e.values) for (float v : channel) check(std::isfinite(v), "malformed controls produce finite output");
	check(!o::validState({0,0,0}), "unusable saved state rejected");
	std::printf("Octoscillator: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
