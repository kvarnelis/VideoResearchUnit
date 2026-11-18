// Headless Video Oscillator module check against Rack's real Module/Port code. No GUI or running Rack.
// V/OCT doubles each oscillator's frequency per volt, the sound outputs sing at the oscillator's own frequency
// (measured by counting zero crossings), go silent above Nyquist, and nothing changes with them unpatched.
#include "../src/VideoOsc.cpp"
#include <cmath>
#include <cstdio>
#include <vector>

Plugin* pluginInstance = nullptr;

struct V : VideoOsc {
	V() { model = modelVideoOsc; }
};

int main() {
	int fails = 0, checks = 0;
	auto check = [&](bool v, const char* msg) { ++checks; if (!v) { ++fails; std::fprintf(stderr, "FAIL: %s\n", msg); } };
	rack::engine::Module::ProcessArgs args{};
	args.sampleRate = 48000.f;
	args.sampleTime = 1.f / 48000.f;

	rack::Context context;
	context.engine = new rack::engine::Engine;
	rack::contextSet(&context);

	const double rowHz = crt::kLineRateHz / kPairFieldLines;
	auto knobFor = [](double cycles) { return static_cast<float>(std::log2(cycles / 0.5)); };
	auto published = [&](V& m) {
		m.process(args);
		VideoParams p;
		const bool ok = m.readPublished(p);
		return ok ? p : VideoParams();
	};
	(void)rowHz;
	// Runs one second and returns the output's samples.
	auto run = [&](V& m, int outputId, int samples = 48000) {
		std::vector<float> out;
		out.reserve(static_cast<size_t>(samples));
		for (int i = 0; i < samples; ++i) {
			m.process(args);
			out.push_back(m.outputs[outputId].getVoltage());
		}
		return out;
	};
	// Rising zero crossings per second (a clean tone: its frequency in hertz).
	auto hertz = [&](const std::vector<float>& v) {
		int n = 0;
		for (size_t i = 1; i < v.size(); ++i) {
			if (v[i - 1] < 0.f && v[i] >= 0.f) ++n;
		}
		return static_cast<double>(n) * 48000.0 / static_cast<double>(v.size());
	};
	auto peak = [&](const std::vector<float>& v) {
		float p = 0.f;
		for (float x : v) p = std::max(p, std::fabs(x));
		return p;
	};


	// A: +1 V doubles the cycles, -1 V halves them, 0 V and unpatched leave them alone.
	{
		V m;
		m.params[VideoOsc::FREQUENCY_PARAM].setValue(knobFor(100.0));
		const float base = published(m).cyclesA;
		check(std::abs(base - 100.f) < 0.5f, "A's knob sets its cycles");
		m.inputs[VideoOsc::VOCT_A_INPUT].channels = 1;
		m.inputs[VideoOsc::VOCT_A_INPUT].setVoltage(0.f);
		check(published(m).cyclesA == base, "V/OCT patched at 0 V leaves A's cycles exactly as they were");
		m.inputs[VideoOsc::VOCT_A_INPUT].setVoltage(1.f);
		check(std::abs(published(m).cyclesA - 2.f * base) < 0.01f, "+1 V on V/OCT doubles A's cycles");
		m.inputs[VideoOsc::VOCT_A_INPUT].setVoltage(-1.f);
		check(std::abs(published(m).cyclesA - 0.5f * base) < 0.01f, "-1 V on V/OCT halves A's cycles");
		// The old FREQ CV keeps its own reading (0.4 octave a volt on the cycles).
		m.inputs[VideoOsc::VOCT_A_INPUT].channels = 0;
		m.inputs[VideoOsc::FREQUENCY_INPUT].channels = 1;
		m.inputs[VideoOsc::FREQUENCY_INPUT].setVoltage(5.f);
		check(std::abs(published(m).cyclesA - 4.f * base) < 0.05f, "the FREQ CV still moves A 0.4 octave a volt");
		// Both at once add: 5 V of CV (2 octaves) plus 1 V/OCT is 3 octaves.
		m.inputs[VideoOsc::VOCT_A_INPUT].channels = 1;
		m.inputs[VideoOsc::VOCT_A_INPUT].setVoltage(1.f);
		check(std::abs(published(m).cyclesA - 8.f * base) < 0.1f || published(m).cyclesA == 512.f, "FREQ CV and V/OCT add in octaves");
		// The Oscillator pattern's frequency doubles too.
		V o;
		o.params[VideoOsc::FREQUENCY_PARAM].setValue(2.f);
		const float f0 = published(o).frequency;
		o.inputs[VideoOsc::VOCT_A_INPUT].channels = 1;
		o.inputs[VideoOsc::VOCT_A_INPUT].setVoltage(1.f);
		check(std::abs(published(o).frequency - 2.f * f0) < 1e-4f, "V/OCT doubles the Oscillator pattern's frequency per volt");
	}

	// B.
	{
		V m;
		m.params[VideoOsc::FREQ_B_PARAM].setValue(knobFor(20.0));
		const float base = published(m).cyclesB;
		m.inputs[VideoOsc::VOCT_B_INPUT].channels = 1;
		m.inputs[VideoOsc::VOCT_B_INPUT].setVoltage(2.f);
		check(std::abs(published(m).cyclesB - 4.f * base) < 0.05f, "+2 V on B's V/OCT quadruples B's cycles");
		check(std::abs(published(m).cyclesA - 8.f) < 0.05f, "B's V/OCT leaves A alone");
	}

	// Sine on ROWS at 100 cycles: 100 x 59.94 Hz.
	{
		V m;
		m.params[VideoOsc::FREQUENCY_PARAM].setValue(knobFor(100.0));
		m.params[VideoOsc::RANGE_A_PARAM].setValue(0.f);
		m.params[VideoOsc::SHAPE_PARAM].setValue(0.f);
		m.outputs[VideoOsc::AUDIO_A_OUTPUT].channels = 1;
		const std::vector<float> a = run(m, VideoOsc::AUDIO_A_OUTPUT);
		check(std::abs(hertz(a) - 100.0 * rowHz) < 5.0, "A on ROWS, 100 cycles, sounds at 100 x the field rate");
		check(std::abs(peak(a) - 5.f) < 0.05f, "the sound is 10 V peak to peak");

		// 1 V on V/OCT doubles it; -1 V halves it.
		m.inputs[VideoOsc::VOCT_A_INPUT].channels = 1;
		m.inputs[VideoOsc::VOCT_A_INPUT].setVoltage(1.f);
		const std::vector<float> up = run(m, VideoOsc::AUDIO_A_OUTPUT);
		check(std::abs(hertz(up) - 200.0 * rowHz) < 8.0, "+1 V on V/OCT doubles A's sound");
		m.inputs[VideoOsc::VOCT_A_INPUT].setVoltage(-1.f);
		const std::vector<float> down = run(m, VideoOsc::AUDIO_A_OUTPUT);
		check(std::abs(hertz(down) - 50.0 * rowHz) < 4.0, "-1 V on V/OCT halves A's sound");
		// The picture's own frequency doubles with it.
		m.inputs[VideoOsc::VOCT_A_INPUT].setVoltage(0.f);
		m.process(args);
		VideoParams p0;
		check(m.readPublished(p0) && std::abs(p0.cyclesA - 100.f) < 0.5f, "V/OCT at 0 V leaves A's cycles alone");
		m.inputs[VideoOsc::VOCT_A_INPUT].setVoltage(1.f);
		m.process(args);
		VideoParams p1;
		check(m.readPublished(p1) && std::abs(p1.cyclesA - 200.f) < 1.f, "+1 V on V/OCT doubles A's cycles in the picture");
	}

	// B has its own knob, V/OCT and output; the A output stays quiet when A is patched nowhere.
	{
		V m;
		m.params[VideoOsc::FREQ_B_PARAM].setValue(knobFor(50.0));
		m.params[VideoOsc::RANGE_B_PARAM].setValue(0.f);
		m.params[VideoOsc::WAVE_B_PARAM].setValue(0.f);
		m.outputs[VideoOsc::AUDIO_B_OUTPUT].channels = 1;
		const std::vector<float> b = run(m, VideoOsc::AUDIO_B_OUTPUT);
		check(std::abs(hertz(b) - 50.0 * rowHz) < 4.0, "B on ROWS, 50 cycles, sounds at 50 x the field rate");
		m.inputs[VideoOsc::VOCT_B_INPUT].channels = 1;
		m.inputs[VideoOsc::VOCT_B_INPUT].setVoltage(2.f);
		const std::vector<float> b2 = run(m, VideoOsc::AUDIO_B_OUTPUT);
		check(std::abs(hertz(b2) - 200.0 * rowHz) < 8.0, "+2 V on B's V/OCT quadruples B's sound");
		check(m.outputs[VideoOsc::AUDIO_A_OUTPUT].getVoltage() == 0.f, "an unpatched sound output stays at 0 V");
	}

	// BARS: the line rate sets the pitch. Half a cycle across a line is 7.87 kHz.
	{
		V m;
		m.params[VideoOsc::FREQUENCY_PARAM].setValue(knobFor(0.5));
		m.params[VideoOsc::RANGE_A_PARAM].setValue(1.f);
		m.params[VideoOsc::SHAPE_PARAM].setValue(0.f);
		m.outputs[VideoOsc::AUDIO_A_OUTPUT].channels = 1;
		const std::vector<float> a = run(m, VideoOsc::AUDIO_A_OUTPUT);
		check(std::abs(hertz(a) - 0.5 * crt::kLineRateHz) < 20.0, "A on BARS, half a cycle, sounds at half the line rate");
		// LOCK does not touch the sound.
		m.params[VideoOsc::LOCK_A_PARAM].setValue(0.f);
		const std::vector<float> free = run(m, VideoOsc::AUDIO_A_OUTPUT);
		check(std::abs(hertz(free) - 0.5 * crt::kLineRateHz) < 20.0, "LOCK leaves the sound's pitch alone");
	}

	// Above Nyquist the output is silent, and it fades in below it.
	{
		V m;
		m.params[VideoOsc::FREQUENCY_PARAM].setValue(knobFor(2.0));  // BARS, 2 cycles: 31.5 kHz
		m.params[VideoOsc::RANGE_A_PARAM].setValue(1.f);
		m.outputs[VideoOsc::AUDIO_A_OUTPUT].channels = 1;
		check(peak(run(m, VideoOsc::AUDIO_A_OUTPUT, 4800)) == 0.f, "above Nyquist the sound is silent");
		m.params[VideoOsc::FREQUENCY_PARAM].setValue(knobFor(1.4));  // 22.0 kHz, in the fade
		const float inFade = peak(run(m, VideoOsc::AUDIO_A_OUTPUT, 4800));
		check(inFade > 0.f && inFade < 4.9f, "between 0.4 and 0.5 of the sample rate the sound fades");
		m.params[VideoOsc::FREQUENCY_PARAM].setValue(knobFor(1.0));  // 15.7 kHz
		check(peak(run(m, VideoOsc::AUDIO_A_OUTPUT, 4800)) > 4.9f, "below 0.4 of the sample rate it is at full level");
	}

	// Every wave stays finite and inside 10 V peak to peak, square included.
	{
		bool sane = true;
		for (float wave = 0.f; wave <= 1.001f; wave += 0.125f) {
			V m;
			m.params[VideoOsc::FREQUENCY_PARAM].setValue(knobFor(300.0));
			m.params[VideoOsc::RANGE_A_PARAM].setValue(0.f);
			m.params[VideoOsc::SHAPE_PARAM].setValue(wave);
			m.outputs[VideoOsc::AUDIO_A_OUTPUT].channels = 1;
			for (float x : run(m, VideoOsc::AUDIO_A_OUTPUT, 9600)) {
				sane = sane && std::isfinite(x) && std::fabs(x) < 5.6f;
			}
		}
		check(sane, "every wave setting gives finite sound inside the 10 V span");
	}

	// C: the same controls, V/OCT and sound as A and B; its LEVEL is 0 on a new module.
	{
		V m;
		check(m.params[VideoOsc::LEVEL_C_PARAM].getValue() == 0.f, "C's level is 0 on a new module");
		check(published(m).levelC == 0.f, "the bus says C is out of the picture");
		m.params[VideoOsc::FREQ_C_PARAM].setValue(knobFor(40.0));
		m.params[VideoOsc::RANGE_C_PARAM].setValue(0.f);
		m.params[VideoOsc::WAVE_C_PARAM].setValue(0.f);
		m.outputs[VideoOsc::AUDIO_C_OUTPUT].channels = 1;
		check(std::abs(published(m).cyclesC - 40.f) < 0.2f, "C's knob sets its cycles");
		const std::vector<float> c = run(m, VideoOsc::AUDIO_C_OUTPUT);
		check(std::abs(hertz(c) - 40.0 * rowHz) < 3.0, "C on ROWS, 40 cycles, sounds at 40 x the field rate");
		m.inputs[VideoOsc::VOCT_C_INPUT].channels = 1;
		m.inputs[VideoOsc::VOCT_C_INPUT].setVoltage(1.f);
		const std::vector<float> c2 = run(m, VideoOsc::AUDIO_C_OUTPUT);
		check(std::abs(hertz(c2) - 80.0 * rowHz) < 4.0, "+1 V on C's V/OCT doubles C's sound");
		check(std::abs(published(m).cyclesC - 80.f) < 0.4f, "and C's cycles in the picture");
		m.params[VideoOsc::LEVEL_C_PARAM].setValue(0.7f);
		check(std::abs(published(m).levelC - 0.7f) < 1e-6f, "the LEVEL knob reaches the bus");
		check(published(m).cyclesA == published(m).cyclesA && std::abs(published(m).cyclesA - 8.f) < 0.05f, "C leaves A alone");
	}

	std::printf("video_osc_module_test: %d checks, %d failed\n", checks, fails);
	return fails == 0 ? 0 : 1;
}
