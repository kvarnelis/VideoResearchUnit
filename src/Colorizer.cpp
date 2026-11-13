#include "plugin.hpp"
#include "panel_style.hpp"
#include "video_bus.hpp"

#include <cmath>

struct Colorizer : Module {
	enum ParamId { HUE1_PARAM, HUE2_PARAM, HUE3_PARAM, HUE4_PARAM,
		LEVEL1_PARAM, LEVEL2_PARAM, LEVEL3_PARAM, LEVEL4_PARAM,
		SAT_PARAM, BANDS_PARAM, OFFSET_PARAM, PARAMS_LEN };
	enum InputId { VIDEO_BUS_INPUT, HUE1_INPUT, HUE2_INPUT, HUE3_INPUT, HUE4_INPUT,
		SAT_INPUT, BANDS_INPUT, OFFSET_INPUT, INPUTS_LEN };
	enum OutputId { VIDEO_BUS_OUTPUT, OUTPUTS_LEN };
	enum LightId { SIGNAL_LIGHT, LIGHTS_LEN };
	VideoBusOwner videoBus;

	Colorizer() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		const ColorizerParams defaults;
		for (int i = 0; i < 4; ++i) {
			const std::string number = std::to_string(i + 1);
			configParam(HUE1_PARAM + i, 0.f, 1.f, defaults.hue[i], "Hue " + number, "°", 0.f, 360.f);
			configParam(LEVEL1_PARAM + i, 0.f, 1.f, defaults.level[i], "Level " + number);
			configInput(HUE1_INPUT + i, "Hue " + number + " CV");
		}
		configParam(SAT_PARAM, 0.f, 1.f, 0.9f, "Saturation");
		configSwitch(BANDS_PARAM, 0.f, 7.f, 0.f, "Bands", {"Smooth", "2", "3", "4", "5", "6", "7", "8"});
		configParam(OFFSET_PARAM, 0.f, 1.f, 0.f, "Offset");
		configInput(VIDEO_BUS_INPUT, "Video bus token");
		configInput(SAT_INPUT, "Saturation CV");
		configInput(BANDS_INPUT, "Bands CV");
		configInput(OFFSET_INPUT, "Offset CV");
		configOutput(VIDEO_BUS_OUTPUT, "Video bus token");
	}

	void process(const ProcessArgs& args) override {
		const int inBus = inputs[VIDEO_BUS_INPUT].isConnected() ? videoBusIdFromVoltage(inputs[VIDEO_BUS_INPUT].getVoltage()) : 1;
		const int outBus = videoBus.id();
		const bool active = videoBusIsActive(inBus) && inBus != outBus;
		ColorizerParams p;
		for (int i = 0; i < 4; ++i) {
			const float hue = params[HUE1_PARAM + i].getValue() + inputs[HUE1_INPUT + i].getVoltage() * 0.1f;
			p.hue[i] = hue - std::floor(hue);
			p.level[i] = clamp(params[LEVEL1_PARAM + i].getValue(), 0.f, 1.f);
		}
		p.saturation = clamp(params[SAT_PARAM].getValue() + inputs[SAT_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		const int bands = static_cast<int>(clamp(std::round(params[BANDS_PARAM].getValue() + inputs[BANDS_INPUT].getVoltage() * 0.7f), 0.f, 7.f));
		p.bands = bands == 0 ? 0 : bands + 1;
		const float offset = params[OFFSET_PARAM].getValue() + inputs[OFFSET_INPUT].getVoltage() * 0.1f;
		p.offset = offset - std::floor(offset);
		if (active) videoBusPublishColorizer(outBus, inBus, p); else videoBusClear(outBus);
		outputs[VIDEO_BUS_OUTPUT].setVoltage(videoBus.voltage());
		lights[SIGNAL_LIGHT].setBrightnessSmooth(active ? 1.f : 0.f, args.sampleTime);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		saveVideoBusId(rootJ, videoBus);
		return rootJ;
	}
	void dataFromJson(json_t* rootJ) override { loadVideoBusId(rootJ, videoBus, "outputBusVoltage"); }
};

struct ColorizerWidget : ModuleWidget {
	ColorizerWidget(Colorizer* module) {
		using namespace rasterlab_panel;
		const float W = 60.96f;
		const float P = 13.f;
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Colorizer.svg")));
		addPanelHeader(this, W, "10", "COLORIZER");
		for (int i = 0; i < 4; ++i) {
			const float x = rowX(W, 4, i, P);
			const std::string number = std::to_string(i + 1);
			addKnobLabel(this, Vec(x, 30), "HUE " + number);
			addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(x, 30)), module, Colorizer::HUE1_PARAM + i));
			addKnobLabel(this, Vec(x, 50), "LVL " + number, R_SMALL);
			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(x, 50)), module, Colorizer::LEVEL1_PARAM + i));
			addJackLabel(this, Vec(x, 68), "CV " + number);
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, 68)), module, Colorizer::HUE1_INPUT + i));
		}
		const char* labels[] = {"SAT", "BANDS", "OFFSET"};
		for (int i = 0; i < 3; ++i) {
			const float x = rowX(W, 3, i, 19.f);
			addKnobLabel(this, Vec(x, 86), labels[i]);
			if (i == 1) addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(x, 86)), module, Colorizer::BANDS_PARAM));
			else addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(x, 86)), module, Colorizer::SAT_PARAM + i));
			addJackLabel(this, Vec(x, 102), labels[i]);
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, 102)), module, Colorizer::SAT_INPUT + i));
		}
		addJackLabel(this, Vec(rowX(W, 3, 0, 19.f), 116), "BUS IN");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 0, 19.f), 116)), module, Colorizer::VIDEO_BUS_INPUT));
		addJackLabel(this, Vec(W * 0.5f, 116), "SIG");
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(W * 0.5f, 116)), module, Colorizer::SIGNAL_LIGHT));
		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 3, 2, 19.f), 116), "BUS", module, Colorizer::VIDEO_BUS_OUTPUT);
	}
};

Model* modelColorizer = createModel<Colorizer, ColorizerWidget>("Colorizer");
