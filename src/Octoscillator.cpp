#include "plugin.hpp"
#include "panel_style.hpp"
#include "octoscillator_math.hpp"

struct Octoscillator : Module {
	enum ParamId { SPEED_PARAM, SPREAD_PARAM, RELATE_PARAM, LEVEL_PARAM, RANGE_PARAM,
		RATE_PARAMS, DEPTH_PARAMS = RATE_PARAMS + 8, OFFSET_PARAMS = DEPTH_PARAMS + 8,
		PARAMS_LEN = OFFSET_PARAMS + 8 };
	enum InputId { SPEED_INPUT, RELATE_INPUT, RESET_INPUT, INPUTS_LEN };
	enum OutputId { CV_OUTPUTS, OUTPUTS_LEN = CV_OUTPUTS + 24 };
	enum LightId { MOTION_LIGHTS, LIGHTS_LEN = MOTION_LIGHTS + 16 };
	octoscillator::Engine engine;
	dsp::SchmittTrigger resetTrigger;
	dsp::ClockDivider controlDivider;
	dsp::ClockDivider lightDivider;
	std::array<float, 8> rates{}, depths{}, offsets{};
	float commonRate = 1.f, relationship = 0.f;
	int range = 0;
	bool controlsReady = false;

	Octoscillator() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(SPEED_PARAM, -8.f, 7.f, 1.f, "Motion speed", "x", 2.f);
		configParam(SPREAD_PARAM, 0.f, 1.f, .5f, "Rate spread", "%", 0.f, 100.f);
		configParam(RELATE_PARAM, 0.f, 1.f, .25f, "Shared movement", "%", 0.f, 100.f);
		configParam(LEVEL_PARAM, 0.f, 1.f, 1.f, "Master depth", "%", 0.f, 100.f);
		configSwitch(RANGE_PARAM, 0.f, 2.f, 0.f, "Base output range", {"+/-5 V", "0-5 V", "+/-1 V"});
		configInput(SPEED_INPUT, "Speed 1V/oct");
		configInput(RELATE_INPUT, "Shared movement (10 V = full)");
		configInput(RESET_INPUT, "Restart all trajectories");
		for (int i = 0; i < 8; ++i) {
			const std::string name = "Channel " + std::to_string(i + 1);
			configParam(RATE_PARAMS + i, -2.f, 2.f, 0.f, name + " rate", "x", 2.f);
			configParam(DEPTH_PARAMS + i, 0.f, 1.f, .5f, name + " depth", "%", 0.f, 100.f);
			configParam(OFFSET_PARAMS + i, -5.f, 5.f, 0.f, name + " offset", " V");
			for (int c = 0; c < 3; ++c) configOutput(CV_OUTPUTS + i * 3 + c, name + " " + std::string(1, "XYZ"[c]) + " CV");
		}
		controlDivider.setDivision(32);
		lightDivider.setDivision(256);
	}
	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		engine.reset();
		controlsReady = false;
	}
	void process(const ProcessArgs& args) override {
		if (resetTrigger.process(inputs[RESET_INPUT].getVoltage(), .1f, 2.f)) engine.reset();
		if (controlDivider.process() || !controlsReady) {
			const float speed = octoscillator::sane(params[SPEED_PARAM].getValue() + octoscillator::sane(inputs[SPEED_INPUT].getVoltage(), -10.f, 10.f), -8.f, 7.f);
			commonRate = octoscillator::speed(speed, 0.f, 0, 0.f);
			relationship = octoscillator::sane(params[RELATE_PARAM].getValue() + inputs[RELATE_INPUT].getVoltage() * .1f, 0.f, 1.f);
			range = static_cast<int>(octoscillator::sane(params[RANGE_PARAM].getValue(), 0.f, 2.f));
			for (int i = 0; i < 8; ++i) {
				rates[i] = octoscillator::speed(speed, params[SPREAD_PARAM].getValue(), i, params[RATE_PARAMS + i].getValue());
				depths[i] = params[DEPTH_PARAMS + i].getValue() * params[LEVEL_PARAM].getValue();
				offsets[i] = params[OFFSET_PARAMS + i].getValue();
			}
			controlsReady = true;
		}
		engine.process(args.sampleTime, commonRate, rates, relationship);
		for (int i = 0; i < 8; ++i)
			for (int c = 0; c < 3; ++c)
				outputs[CV_OUTPUTS + i * 3 + c].setVoltage(octoscillator::voltage(engine.values[i][c], depths[i], offsets[i], range));
		if (lightDivider.process()) {
			for (int i = 0; i < 8; ++i) {
				const float x = engine.values[i][0];
				lights[MOTION_LIGHTS + i * 2].setBrightness(std::max(x, 0.f));
				lights[MOTION_LIGHTS + i * 2 + 1].setBrightness(std::max(-x, 0.f));
			}
		}
	}
	json_t* dataToJson() override {
		json_t* root = json_object();
		json_t* states = json_array();
		for (int i = 0; i < 9; ++i) {
			const auto& s = i == 8 ? engine.shared : engine.local[i];
			json_t* state = json_pack("[f,f,f]", s.x, s.y, s.z);
			json_array_append_new(states, state);
		}
		json_object_set_new(root, "trajectories", states);
		return root;
	}
	void dataFromJson(json_t* root) override {
		json_t* states = json_object_get(root, "trajectories");
		for (int i = 0; i < 9; ++i) {
			json_t* state = json_array_get(states, i);
			if (!json_is_array(state) || json_array_size(state) != 3) continue;
			bool numeric = true;
			for (int c = 0; c < 3; ++c) numeric &= json_is_number(json_array_get(state, c));
			if (!numeric) continue;
			LorenzState s{json_number_value(json_array_get(state, 0)), json_number_value(json_array_get(state, 1)), json_number_value(json_array_get(state, 2))};
			if (octoscillator::validState(s)) (i == 8 ? engine.shared : engine.local[i]) = s;
		}
		controlsReady = false;
	}
};

struct OctoscillatorWidget : ModuleWidget {
	OctoscillatorWidget(Octoscillator* module) {
		using namespace rasterlab_panel;
		constexpr float W = 182.88f; // 36 HP, eight clear channel strips.
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Octoscillator.svg")));
		addPanelHeader(this, W, "23", "OCTOSCILLATOR");
		const char* top[] = {"SPEED", "SPREAD", "RELATE", "LEVEL", "RANGE", "SPEED", "RELATE", "RESET"};
		for (int i = 0; i < 8; ++i) {
			const float x = rowX(W, 8, i, 20.32f);
			addKnobLabel(this, Vec(x, 27.f), top[i], i < 4 ? R_KNOB : R_JACK);
			if (i < 4) addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(x, 27.f)), module, i));
			else if (i == 4) addParam(createParamCentered<CKSSThree>(mm2px(Vec(x, 27.f)), module, Octoscillator::RANGE_PARAM));
			else addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, 27.f)), module, i - 5));
			addChild(createPanelText(Vec(x - 3.f, 37.f), std::to_string(i + 1), NUMBER_MM));
			addChild(createLightCentered<SmallLight<GreenRedLight>>(mm2px(Vec(x + 3.f, 37.f)), module, Octoscillator::MOTION_LIGHTS + i * 2));
			const char* labels[] = {"RATE", "DEPTH", "OFFSET"};
			const int ids[] = {Octoscillator::RATE_PARAMS, Octoscillator::DEPTH_PARAMS, Octoscillator::OFFSET_PARAMS};
			for (int row = 0; row < 3; ++row) {
				const Vec p(x, 49.f + row * 15.f);
				addKnobLabel(this, p, labels[row], R_SMALL);
				addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(p), module, ids[row] + i));
			}
			for (int c = 0; c < 3; ++c)
				addPlatedOutput<PJ301MPort>(this, Vec(x, 94.f + c * 13.f), std::string(1, "XYZ"[c]), module, Octoscillator::CV_OUTPUTS + i * 3 + c);
		}
	}
};
Model* modelOctoscillator = createModel<Octoscillator, OctoscillatorWidget>("Octoscillator");
