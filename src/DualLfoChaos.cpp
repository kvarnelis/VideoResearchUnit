#include "plugin.hpp"
#include "panel_style.hpp"
#include "hz_param.hpp"
#include "lorenz.hpp"
#include "video_bus.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace {
constexpr float kLfoMinHz = 0.01f;
constexpr float kLfoMaxHz = 20.f;
// Lorenz time units per second at CHAOS = 0 and CHAOS = 1. CHAOS sets how fast the attractor moves.
constexpr double kSpeedMin = 0.5;
constexpr double kSpeedMax = 32.0;
// Typical extent of the attractor, used to scale the outputs to +/-1 before the 5 V range.
constexpr double kScaleX = 22.0;
constexpr double kScaleY = 28.0;
}

struct DualLfoChaos : Module {
	enum ParamId { RATE_A_PARAM, DEPTH_A_PARAM, RATE_B_PARAM, DEPTH_B_PARAM, CHAOS_PARAM, SLEW_PARAM, BIPOLAR_PARAM, PARAMS_LEN };
	enum InputId { RATE_A_INPUT, RATE_B_INPUT, RESET_INPUT, CHAOS_INPUT, INPUTS_LEN };
	enum OutputId { A_OUTPUT, B_OUTPUT, X_OUTPUT, Y_OUTPUT, SH_OUTPUT, CLOCK_OUTPUT, OUTPUTS_LEN };
	enum LightId { CLOCK_LIGHT, LIGHTS_LEN };

	float phaseA = 0.f;
	float phaseB = 0.25f;
	float sh = 0.f;
	float outX = 0.f;  // smoothed chaos outputs, -1..1
	float outY = 0.f;
	LorenzState chaos;
	dsp::SchmittTrigger reset;
	dsp::PulseGenerator clockPulse;

	DualLfoChaos() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		auto* rateA = configParam<ExpHzParamQuantity>(RATE_A_PARAM, 0.f, 1.f, normalizedFromExpHz(0.25f, kLfoMinHz, kLfoMaxHz), "LFO A rate");
		rateA->minHz = kLfoMinHz;
		rateA->maxHz = kLfoMaxHz;
		configParam(DEPTH_A_PARAM, 0.f, 1.f, 1.f, "LFO A depth");
		auto* rateB = configParam<ExpHzParamQuantity>(RATE_B_PARAM, 0.f, 1.f, normalizedFromExpHz(0.17f, kLfoMinHz, kLfoMaxHz), "LFO B rate");
		rateB->minHz = kLfoMinHz;
		rateB->maxHz = kLfoMaxHz;
		configParam(DEPTH_B_PARAM, 0.f, 1.f, 1.f, "LFO B depth");
		configParam(CHAOS_PARAM, 0.f, 1.f, 0.45f, "Chaos speed");
		configParam(SLEW_PARAM, 0.f, 1.f, 0.25f, "Chaos slew");
		configSwitch(BIPOLAR_PARAM, 0.f, 1.f, 1.f, "Output range", {"0-10V", "+/-5V"});
		configInput(RATE_A_INPUT, "Rate A 1V/oct");
		configInput(RATE_B_INPUT, "Rate B 1V/oct");
		configInput(RESET_INPUT, "Reset");
		configInput(CHAOS_INPUT, "Chaos speed CV");
		configOutput(A_OUTPUT, "LFO A CV");
		configOutput(B_OUTPUT, "LFO B CV");
		configOutput(X_OUTPUT, "Chaos X CV");
		configOutput(Y_OUTPUT, "Chaos Y CV");
		configOutput(SH_OUTPUT, "Sample and hold CV");
		configOutput(CLOCK_OUTPUT, "Clock gate");
	}

	void process(const ProcessArgs& args) override {
		if (inputs[RESET_INPUT].isConnected() && reset.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 2.f)) {
			phaseA = 0.f;
			phaseB = 0.25f;
		}

		// Rates: exponential knob, 1 V/oct CV.
		const float rateMax = 40.f;
		const float rateA = clamp(expHzFromNormalized(params[RATE_A_PARAM].getValue(), kLfoMinHz, kLfoMaxHz) * std::pow(2.f, clamp(inputs[RATE_A_INPUT].getVoltage(), -10.f, 10.f)), 0.f, rateMax);
		const float rateB = clamp(expHzFromNormalized(params[RATE_B_PARAM].getValue(), kLfoMinHz, kLfoMaxHz) * std::pow(2.f, clamp(inputs[RATE_B_INPUT].getVoltage(), -10.f, 10.f)), 0.f, rateMax);
		const float chaosAmount = clamp(params[CHAOS_PARAM].getValue() + inputs[CHAOS_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		const float slew = clamp(params[SLEW_PARAM].getValue(), 0.f, 1.f);

		const float oldA = phaseA;
		phaseA += rateA * args.sampleTime;
		phaseB += rateB * args.sampleTime;
		phaseA -= std::floor(phaseA);
		phaseB -= std::floor(phaseB);
		const float a = std::sin(2.f * M_PI * phaseA);
		const float b = std::sin(2.f * M_PI * phaseB + 1.7f * std::sin(2.f * M_PI * phaseA));

		// Lorenz attractor, one RK4 step per audio sample. CHAOS sets the speed on a log scale.
		const double speed = kSpeedMin * std::pow(kSpeedMax / kSpeedMin, static_cast<double>(chaosAmount));
		const double h = std::min(speed * static_cast<double>(args.sampleTime), lorenz::kMaxStep);
		lorenz::step(chaos, h);
		const float targetX = static_cast<float>(std::clamp(chaos.x / kScaleX, -1.0, 1.0));
		const float targetY = static_cast<float>(std::clamp(chaos.y / kScaleY, -1.0, 1.0));
		// Slew is a one-pole smoother on the outputs: 0 is unsmoothed, 1 is about 0.2 s.
		const float coef = slew < 1e-4f ? 1.f : 1.f - std::exp(-args.sampleTime / (slew * 0.2f));
		outX += (targetX - outX) * coef;
		outY += (targetY - outY) * coef;

		const bool clock = phaseA < oldA;
		if (clock) {
			sh = outX;
			clockPulse.trigger(1e-3f);
		}
		const bool clockHigh = clockPulse.process(args.sampleTime);

		const bool bipolar = params[BIPOLAR_PARAM].getValue() > 0.5f;
		auto out = [&](float v, float depth) {
			v = clamp(v * depth, -1.f, 1.f);
			return bipolar ? v * 5.f : (v * 0.5f + 0.5f) * 10.f;
		};
		outputs[A_OUTPUT].setVoltage(out(a, params[DEPTH_A_PARAM].getValue()));
		outputs[B_OUTPUT].setVoltage(out(b, params[DEPTH_B_PARAM].getValue()));
		outputs[X_OUTPUT].setVoltage(out(outX, 1.f));
		outputs[Y_OUTPUT].setVoltage(out(outY, 1.f));
		outputs[SH_OUTPUT].setVoltage(out(sh, 1.f));
		outputs[CLOCK_OUTPUT].setVoltage(clockHigh ? 10.f : 0.f);
		lights[CLOCK_LIGHT].setBrightnessSmooth(clockHigh ? 1.f : 0.f, args.sampleTime);
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_object_set_new(r, "phaseA", json_real(phaseA));
		json_object_set_new(r, "phaseB", json_real(phaseB));
		json_object_set_new(r, "x", json_real(chaos.x));
		json_object_set_new(r, "y", json_real(chaos.y));
		json_object_set_new(r, "z", json_real(chaos.z));
		json_object_set_new(r, "sh", json_real(sh));
		return r;
	}

	void dataFromJson(json_t* r) override {
		if (json_t* j = json_object_get(r, "phaseA")) phaseA = static_cast<float>(json_number_value(j));
		if (json_t* j = json_object_get(r, "phaseB")) phaseB = static_cast<float>(json_number_value(j));
		if (json_t* j = json_object_get(r, "sh")) sh = static_cast<float>(json_number_value(j));
		// Chaos state: a saved state is only restored when it is a usable Lorenz state. Older patches
		// stored a 2-D state in [0, 1], which is not on the attractor, so they start fresh.
		if (json_t* jx = json_object_get(r, "x")) {
			if (json_t* jy = json_object_get(r, "y")) {
				if (json_t* jz = json_object_get(r, "z")) {
					const double x = json_number_value(jx);
					const double y = json_number_value(jy);
					const double z = json_number_value(jz);
					if (std::abs(x) > 1.0 && std::abs(x) < 40.0 && std::abs(y) < 40.0 && z > 0.0 && z < 60.0) {
						chaos = {x, y, z};
					}
				}
			}
		}
	}
};

struct DualLfoChaosWidget : ModuleWidget {
	DualLfoChaosWidget(DualLfoChaos* module) {
		using namespace rasterlab_panel;
		const float W = 40.64f;	// 8 HP
		const float P = 13.0f;	// column pitch: RATE A / CHAOS / RANGE need more than the 10.16 mm grid at the label size
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/DualLfoChaos.svg")));
		// The module name is the longest title; 4.5 mm keeps it inside the hairline rule.
		addPanelHeader(this, W, "18", "DUAL LFO CHAOS", 4.5f);

		// Row 1: main rates (large knobs) with the clock LED between them.
		addKnobLabel(this, Vec(rowX(W, 3, 0, P), 29.f), "RATE A");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(rowX(W, 3, 0, P), 29.f)), module, DualLfoChaos::RATE_A_PARAM));
		addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(rowX(W, 3, 1, P), 29.f)), module, DualLfoChaos::CLOCK_LIGHT));
		addKnobLabel(this, Vec(rowX(W, 3, 2, P), 29.f), "RATE B");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(rowX(W, 3, 2, P), 29.f)), module, DualLfoChaos::RATE_B_PARAM));

		// Row 2: depths and chaos speed (small knobs).
		addKnobLabel(this, Vec(rowX(W, 3, 0, P), 46.5f), "DEP A", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 3, 0, P), 46.5f)), module, DualLfoChaos::DEPTH_A_PARAM));
		addKnobLabel(this, Vec(rowX(W, 3, 1, P), 46.5f), "CHAOS", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 3, 1, P), 46.5f)), module, DualLfoChaos::CHAOS_PARAM));
		addKnobLabel(this, Vec(rowX(W, 3, 2, P), 46.5f), "DEP B", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 3, 2, P), 46.5f)), module, DualLfoChaos::DEPTH_B_PARAM));

		// Row 3: chaos speed CV jack, chaos slew and the output range switch. A jack is the tallest
		// control here, so all three labels use its offset and share a baseline.
		addKnobLabel(this, Vec(rowX(W, 3, 0, P), 64.f), "CHAOS", R_JACK);
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 0, P), 64.f)), module, DualLfoChaos::CHAOS_INPUT));
		addKnobLabel(this, Vec(rowX(W, 3, 1, P), 64.f), "SLEW", R_JACK);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 3, 1, P), 64.f)), module, DualLfoChaos::SLEW_PARAM));
		addKnobLabel(this, Vec(rowX(W, 3, 2, P), 64.f), "RANGE", R_JACK);
		addParam(createParamCentered<CKSS>(mm2px(Vec(rowX(W, 3, 2, P), 64.f)), module, DualLfoChaos::BIPOLAR_PARAM));

		// Inputs: plain jacks, label above.
		addJackLabel(this, Vec(rowX(W, 3, 0, P), 81.5f), "RATE A");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 0, P), 81.5f)), module, DualLfoChaos::RATE_A_INPUT));
		addJackLabel(this, Vec(rowX(W, 3, 1, P), 81.5f), "RATE B");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 1, P), 81.5f)), module, DualLfoChaos::RATE_B_INPUT));
		addJackLabel(this, Vec(rowX(W, 3, 2, P), 81.5f), "RESET");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 2, P), 81.5f)), module, DualLfoChaos::RESET_INPUT));

		// Outputs: jack on a black plate with reversed label.
		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 3, 0, P), 101.f), "A", module, DualLfoChaos::A_OUTPUT);
		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 3, 1, P), 101.f), "B", module, DualLfoChaos::B_OUTPUT);
		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 3, 2, P), 101.f), "X", module, DualLfoChaos::X_OUTPUT);
		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 3, 0, P), 117.f), "Y", module, DualLfoChaos::Y_OUTPUT);
		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 3, 1, P), 117.f), "S&H", module, DualLfoChaos::SH_OUTPUT);
		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 3, 2, P), 117.f), "CLK", module, DualLfoChaos::CLOCK_OUTPUT);
	}
};

Model* modelDualLfoChaos = createModel<DualLfoChaos, DualLfoChaosWidget>("DualLfoChaos");
