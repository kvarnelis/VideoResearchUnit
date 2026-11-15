// Offline checks for Vector Osc (VRU-25): the circle, one voice against eight at no detune, ranges at
// every setting, repeatability from reset, and the Z brightness. Needs no Rack SDK.
#include "vector_osc_math.hpp"
#include <cstdio>
#include <cmath>

namespace v = vector_osc;
static int checks = 0, failures = 0;
static void check(bool ok, const char* msg) {
	++checks;
	if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", msg); }
}

int main() {
	const float dt = 1.f / 48000.f;

	// A circle: one voice, no detune, sine, ratio 1, a quarter turn.
	{
		v::Engine e;
		v::Settings s;
		s.freq = 100.f; s.voices = 1; s.detune = 0.f; s.wave = 0.f; s.ratio = 1.f; s.phase = .25f; s.level = 1.f;
		float worst = 0.f, lo = 9.f, hi = 0.f;
		for (int i = 0; i < 48000; ++i) {
			const v::Output o = e.process(s, dt);
			const float r = std::hypot(o.x, o.y);
			worst = std::max(worst, std::abs(r - 5.f));
			lo = std::min(lo, r); hi = std::max(hi, r);
		}
		check(worst < 2e-3f, "sine pair at ratio 1, quarter turn is a circle of radius 5 V");
		check(hi > 4.99f && lo > 4.99f, "circle radius stays at the stated amplitude");
		// Level scales the amplitude.
		v::Engine e2;
		s.level = .4f;
		float rmax = 0.f;
		for (int i = 0; i < 4800; ++i) { const v::Output o = e2.process(s, dt); rmax = std::max(rmax, std::hypot(o.x, o.y)); }
		check(std::abs(rmax - 2.f) < 2e-3f, "LEVEL .4 gives a circle of radius 2 V");
	}

	// Eight voices at no detune equal one voice, for every wave shape.
	for (float wave : {0.f, .2f, .5f, .8f, 1.f}) {
		v::Engine a, b;
		v::Settings s;
		s.freq = 330.f; s.wave = wave; s.ratio = 1.5f; s.phase = .1f; s.detune = 0.f;
		float worst = 0.f;
		for (int i = 0; i < 20000; ++i) {
			s.voices = 1; const v::Output oa = a.process(s, dt);
			s.voices = 8; const v::Output ob = b.process(s, dt);
			worst = std::max({worst, std::abs(oa.x - ob.x), std::abs(oa.y - ob.y)});
		}
		check(worst < 1e-4f, "DETUNE 0 with 8 voices equals 1 voice");
	}

	// Detune really opens the figure: with detune the figure leaves the single circle.
	{
		v::Engine e;
		v::Settings s;
		s.freq = 50.f; s.voices = 8; s.detune = .5f; s.ratio = 1.f; s.phase = .25f;
		float lo = 9.f;
		for (int i = 0; i < 96000; ++i) { const v::Output o = e.process(s, dt); lo = std::min(lo, std::hypot(o.x, o.y)); }
		check(lo < 4.5f, "detuned voices thicken the figure (radius is no longer constant)");
	}

	// Ranges at every setting, including out-of-range and non-finite inputs.
	{
		const float nan = std::nanf("");
		float worstXY = 0.f, zlo = 99.f, zhi = -99.f;
		bool finite = true;
		const float waves[] = {0.f, .1f, .33f, .5f, .67f, .9f, 1.f, 2.f, -1.f, nan};
		const float details[] = {0.f, .3f, 1.f, 5.f, nan};
		const float freqs[] = {0.01f, .5f, 20.f, 440.f, 5000.f, 18000.f, 1e9f, nan};
		const float ratios[] = {.25f, 1.f, 2.5f, 9.f, 1e6f, nan};
		for (float w : waves) for (float d : details) for (float f : freqs) for (float r : ratios) for (int n : {1, 3, 8, 99, -2}) {
			v::Engine e;
			v::Settings s;
			s.freq = f; s.wave = w; s.detune = d; s.ratio = r; s.voices = n; s.waveOffsetY = .7f; s.phase = .3f; s.level = 1.f;
			for (int i = 0; i < 400; ++i) {
				const v::Output o = e.process(s, dt);
				finite &= std::isfinite(o.x) && std::isfinite(o.y) && std::isfinite(o.z);
				worstXY = std::max({worstXY, std::abs(o.x), std::abs(o.y)});
				zlo = std::min(zlo, o.z); zhi = std::max(zhi, o.z);
			}
		}
		check(finite, "every output is finite at every setting");
		check(worstXY <= 5.f + 1e-4f, "X and Y stay within +/-5 V at every setting");
		check(zlo >= 0.f && zhi <= 10.f, "Z stays within 0 to 10 V at every setting");
	}

	// Level above 1 or below 0 is limited.
	{
		v::Engine e;
		v::Settings s; s.level = 3.f; s.phase = .25f; s.freq = 100.f;
		float m = 0.f;
		for (int i = 0; i < 2000; ++i) { const v::Output o = e.process(s, dt); m = std::max(m, std::abs(o.x)); }
		check(m <= 5.f + 1e-4f, "LEVEL is limited to 1");
	}

	// The same settings from reset give the same samples.
	{
		v::Settings s;
		s.freq = 77.f; s.voices = 6; s.detune = .4f; s.wave = .6f; s.waveOffsetY = -.2f; s.ratio = 3.f / 2.f; s.phase = .13f;
		v::Engine a, b;
		float first[2000][3];
		for (int i = 0; i < 2000; ++i) { const v::Output o = a.process(s, dt); first[i][0] = o.x; first[i][1] = o.y; first[i][2] = o.z; }
		// Run b elsewhere, then reset it and compare.
		for (int i = 0; i < 5000; ++i) b.process(s, dt);
		b.reset();
		bool same = true;
		for (int i = 0; i < 2000; ++i) {
			const v::Output o = b.process(s, dt);
			same &= o.x == first[i][0] && o.y == first[i][1] && o.z == first[i][2];
		}
		check(same, "the same settings from reset give the same samples");
	}

	// Wave shapes: ends are sine and square, and the shape changes continuously.
	{
		check(std::abs(v::waveform(.25f, .001f, 0.f) - 1.f) < 1e-5f, "wave 0 is a sine");
		check(std::abs(v::waveform(.125f, .001f, 1.f) - 1.f) < .01f && std::abs(v::waveform(.625f, .001f, 1.f) + 1.f) < .01f, "wave 1 is a square");
		check(std::abs(v::waveform(.125f, .001f, 1.f / 3.f) - .5f) < 1e-4f, "wave 1/3 is a triangle");
		float maxStep = 0.f, prev = v::waveform(.2f, .001f, 0.f);
		for (int i = 1; i <= 1000; ++i) {
			const float cur = v::waveform(.2f, .001f, i / 1000.f);
			maxStep = std::max(maxStep, std::abs(cur - prev)); prev = cur;
		}
		check(maxStep < .02f, "turning WAVE changes the shape continuously");
	}

	// Z: a resting point is brightest, a fast sweep is dim, and a circle sits between.
	{
		v::Engine e;
		v::Settings s; s.freq = 100.f; s.phase = .25f;
		v::Output o;
		for (int i = 0; i < 9600; ++i) o = e.process(s, dt);
		check(o.z > 6.f && o.z < 7.4f, "Z on a plain circle is about 6.7 V");
		v::Engine e2;
		s.ratio = 9.f; s.wave = 0.f;
		float zmin = 10.f;
		for (int i = 0; i < 9600; ++i) { o = e2.process(s, dt); zmin = std::min(zmin, o.z); }
		check(zmin < 6.f, "Z drops where the trace moves fast");
	}

	// Ratio table and fine control.
	{
		check(v::kRatios[v::kRatioUnity] == 1.f, "the table has 1:1 at the unity index");
		check(v::ratioIndex(5.4f) == 5 && v::ratioIndex(-3.f) == 0 && v::ratioIndex(99.f) == v::kRatioCount - 1, "ratio index rounds and limits");
		check(std::abs(v::fineFactor(0.f) - 1.f) < 1e-7f, "FINE at the middle leaves the ratio exact");
		check(v::fineFactor(1.f) > 1.05f && v::fineFactor(1.f) < 1.06f, "FINE at the top is one semitone up");
	}

	std::printf("vector_osc_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
