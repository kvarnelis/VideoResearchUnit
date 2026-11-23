// Offline checks for Vector Osc (VRU-25): the circle, one voice against eight at no detune, ranges at
// every setting, repeatability from reset, and the Z brightness. Needs no Rack SDK.
#include "vector_osc_math.hpp"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>

static const unsigned long long GOLDEN = 8081521012212322312ull;

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

	// Golden: every WAVE value the knob had before the V R U shape (0 to 1) gives exactly the samples it gave
	// before. The hash was taken from the engine at main dc61b13, before the letters were added.
	{
		uint64_t h = 1469598103934665603ull;
		auto mix = [&](float f) {
			uint32_t b;
			std::memcpy(&b, &f, 4);
			for (int i = 0; i < 4; ++i) { h ^= (b >> (8 * i)) & 0xffu; h *= 1099511628211ull; }
		};
		const float waves[] = {0.f, .055f, .1f, .33f, .5f, .67f, .9f, 1.f};
		const float freqs[] = {147.f, 3000.f};
		for (float w : waves) for (float d : {0.f, .3f, 1.f}) for (int n : {1, 3, 8}) for (float r : {1.f, 4.f / 3.f, 2.5f})
			for (float off : {0.f, .4f, -.3f, .9f}) for (float f : freqs) {
				v::Engine e;
				v::Settings s;
				s.freq = f; s.wave = w; s.detune = d; s.voices = n; s.ratio = r; s.waveOffsetY = off; s.phase = .12f; s.level = .87f;
				for (int i = 0; i < 300; ++i) { const v::Output o = e.process(s, dt); mix(o.x); mix(o.y); mix(o.z); }
			}
		std::printf("golden hash %llu\n", static_cast<unsigned long long>(h));
		check(h == GOLDEN, "WAVE values 0 to 1 are bit-identical to the engine before the V R U shape");
	}

	// The letters V R U (WAVE past 1): one closed loop, no DC, in range, repeatable, and different from the square.
	{
		const v::LetterTable& t = v::letterTable();
		check(t.x[v::kLetterTable] == t.x[0] && t.y[v::kLetterTable] == t.y[0], "the letters are one closed loop");
		double mx = 0, my = 0, px = 0, py = 0;
		for (int i = 0; i < v::kLetterTable; ++i) {
			mx += t.x[i]; my += t.y[i];
			px = std::max(px, (double)std::abs(t.x[i])); py = std::max(py, (double)std::abs(t.y[i]));
		}
		mx /= v::kLetterTable; my /= v::kLetterTable;
		check(std::abs(mx) < 1e-4 && std::abs(my) < 1e-4, "the letters have no DC in X or Y");
		check(px <= 1.0 && py <= 1.0 && px > .5 && py > .3, "the letters peak inside the unit box and are not tiny");

		v::Settings s;
		s.freq = 147.f; s.voices = 1; s.wave = v::kWaveMax; s.level = 1.f; s.phase = .12f;
		v::Engine a, b;
		bool same = true, inRange = true;
		double sumX = 0, sumY = 0, peak = 0;
		const int n = 48000;
		for (int i = 0; i < n; ++i) {
			const v::Output oa = a.process(s, dt), ob = b.process(s, dt);
			same &= oa.x == ob.x && oa.y == ob.y;
			inRange &= std::abs(oa.x) <= 5.f + 1e-4f && std::abs(oa.y) <= 5.f + 1e-4f;
			sumX += oa.x; sumY += oa.y;
			peak = std::max(peak, (double)std::max(std::abs(oa.x), std::abs(oa.y)));
		}
		check(same, "the letters are deterministic");
		check(inRange && peak > 2.f, "the letters stay within +/-5 V at a useful level");
		check(std::abs(sumX / n) < .05 && std::abs(sumY / n) < .05, "the letters output has no DC");
		v::Engine c, d;
		v::Settings sq = s; sq.wave = 1.f;
		bool differs = false;
		for (int i = 0; i < 2000; ++i) differs |= std::abs(c.process(s, dt).x - d.process(sq, dt).x) > .1f;
		check(differs, "WAVE past 1 is not the square any more");
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

	// CV jacks and their attenuverters. At amount 1 each one is the sum the module used before the attenuverters
	// existed, value for value; at 0 the jack does nothing; at -1 it works the other way.
	{
		bool same = true, off = true, mirror = true, limits = true;
		for (int i = -24; i <= 24; ++i) {
			const float cv = i * .93f;
			for (float k = 0.f; k <= 1.f; k += .125f) {
				same &= v::phaseWithCv(k, cv, 1.f) == k + v::sane(cv, -20.f, 20.f) * .1f;
				same &= v::detuneWithCv(k, cv, 1.f) == v::sane(k + v::sane(cv, -20.f, 20.f) * .1f, 0.f, 1.f);
				same &= v::waveWithCv(k, cv, 1.f) == v::sane(k + v::sane(cv, -20.f, 20.f) * .1f, 0.f, 1.f);
				same &= v::ratioStepWithCv(k * 18.f, cv, 1.f) == v::ratioIndex(k * 18.f + std::round(v::sane(cv, -20.f, 20.f)));
				off &= v::phaseWithCv(k, cv, 0.f) == k && v::detuneWithCv(k, cv, 0.f) == k && v::waveWithCv(k, cv, 0.f) == k;
				off &= v::levelWithCv(k, cv, 0.f) == k && v::fineWithCv(k, cv, 0.f) == k && v::waveYWithCv(k, cv, 0.f) == k;
				off &= v::ratioStepWithCv(7.f, cv, 0.f) == 7 && v::voicesWithCv(5.f, cv, 0.f) == 5;
				mirror &= v::phaseWithCv(k, cv, -1.f) == v::phaseWithCv(k, -cv, 1.f) && v::levelWithCv(k, cv, -1.f) == v::levelWithCv(k, -cv, 1.f);
				const int n = v::voicesWithCv(1.f + k * 7.f, cv, 1.f);
				limits &= n >= 1 && n <= v::kMaxVoices;
				limits &= v::levelWithCv(k, cv, 1.f) >= 0.f && v::levelWithCv(k, cv, 1.f) <= 1.f;
				limits &= std::abs(v::fineWithCv(k, cv, 1.f)) <= 1.f && std::abs(v::waveYWithCv(k, cv, 1.f)) <= 1.f;
			}
		}
		check(same, "at amount 1 the PHASE, DETUNE, WAVE and RATIO jacks do exactly what they did before");
		check(off, "at amount 0 a jack leaves its knob alone");
		check(mirror, "at amount -1 a jack works the other way");
		check(limits, "VOICES, LEVEL, FINE and Y WAVE stay in range under any CV");
		// An open jack (0 V) leaves every knob value exactly as it is, at any amount.
		bool open = true;
		for (float a = -1.f; a <= 1.f; a += .25f)
			open &= v::fineWithCv(-.37f, 0.f, a) == -.37f && v::levelWithCv(.81f, 0.f, a) == .81f && v::waveYWithCv(.2f, 0.f, a) == .2f
				&& v::voicesWithCv(6.f, 0.f, a) == 6 && v::waveWithCv(1.2f, 0.f, a) == 1.2f;
		check(open, "an open jack changes nothing");
		check(v::voicesWithCv(1.f, 10.f, 1.f) == 8 && v::voicesWithCv(1.f, 5.f, 1.f) == 5 && v::voicesWithCv(8.f, -10.f, 1.f) == 1, "10 V sweeps VOICES from 1 to 8");
		check(v::levelWithCv(0.f, 10.f, 1.f) == 1.f && v::levelWithCv(0.f, 5.f, .5f) == .25f, "LEVEL: 10 V is full, and the amount scales it");
		check(v::fineWithCv(0.f, 5.f, 1.f) == 1.f && v::waveYWithCv(0.f, -5.f, 1.f) == -1.f, "FINE and Y WAVE: 5 V is the whole way");
		check(v::waveWithCv(.5f, 10.f, 1.f) == 1.f && v::waveWithCv(1.1f, 10.f, 1.f) == v::kWaveMax, "WAVE CV reaches the letters only when the knob is past 1");
		check(v::ratioStepWithCv(5.f, 2.f, .5f) == 6 && v::ratioStepWithCv(5.f, 2.f, -1.f) == 3, "RATIO steps scale with the amount");
		const float nan = std::nanf("");
		check(v::levelWithCv(.5f, nan, 1.f) == .5f && v::voicesWithCv(3.f, nan, nan) == 3 && v::ratioStepWithCv(5.f, nan, 1.f) == 5, "a broken CV is read as 0 V");
	}

	std::printf("vector_osc_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
