#include "plugin.hpp"
#include "panel_style.hpp"
#include "video_bus.hpp"

#include <atomic>
#include <cmath>

struct ColorLab : Module {
	enum ParamId { HUE_PARAM, SAT_PARAM, BRIGHT_PARAM, CONTRAST_PARAM, POSTER_PARAM, SOLAR_PARAM, CHANNEL_PARAM, INVERT_PARAM, MIX_PARAM, PARAMS_LEN };
	enum InputId { VIDEO_BUS_INPUT, HUE_INPUT, SAT_INPUT, BRIGHT_INPUT, MIX_INPUT, INPUTS_LEN };
	enum OutputId { VIDEO_BUS_OUTPUT, OUTPUTS_LEN };
	enum LightId { SIGNAL_LIGHT, LIGHTS_LEN };
	VideoBusOwner videoBus;
	ColorLab() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(HUE_PARAM, -1.f, 1.f, 0.f, "Hue shift");
		configParam(SAT_PARAM, 0.f, 2.f, 1.f, "Saturation");
		configParam(BRIGHT_PARAM, 0.f, 2.f, 1.f, "Brightness");
		configParam(CONTRAST_PARAM, 0.f, 2.f, 1.f, "Contrast");
		configParam(POSTER_PARAM, 0.f, 1.f, 0.f, "Posterize");
		configParam(SOLAR_PARAM, 0.f, 1.f, 0.f, "Solarize");
		configSwitch(CHANNEL_PARAM, 0.f, 3.f, 0.f, "Channel swap", {"RGB", "GRB", "GBR", "BRG"});
		configSwitch(INVERT_PARAM, 0.f, 1.f, 0.f, "Invert", {"Off", "On"});
		configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Mix");
		configInput(VIDEO_BUS_INPUT, "Video bus token"); configInput(HUE_INPUT, "Hue CV"); configInput(SAT_INPUT, "Saturation CV"); configInput(BRIGHT_INPUT, "Brightness CV"); configInput(MIX_INPUT, "Mix CV");
		configOutput(VIDEO_BUS_OUTPUT, "Video bus token");
	}
	void process(const ProcessArgs& args) override {
		const int inBus = inputs[VIDEO_BUS_INPUT].isConnected() ? videoBusIdFromVoltage(inputs[VIDEO_BUS_INPUT].getVoltage()) : 1;
		const int outBus = videoBus.id();
		const bool active = videoBusIsActive(inBus) && inBus != outBus;
		ColorLabParams p;
		p.hue = clamp(params[HUE_PARAM].getValue() + inputs[HUE_INPUT].getVoltage() * 0.1f, -1.f, 1.f);
		p.saturation = clamp(params[SAT_PARAM].getValue() + inputs[SAT_INPUT].getVoltage() * 0.1f, 0.f, 2.f);
		p.brightness = clamp(params[BRIGHT_PARAM].getValue() + inputs[BRIGHT_INPUT].getVoltage() * 0.1f, 0.f, 2.f);
		p.contrast = clamp(params[CONTRAST_PARAM].getValue(), 0.f, 2.f);
		p.posterize = clamp(params[POSTER_PARAM].getValue(), 0.f, 1.f);
		p.solarize = clamp(params[SOLAR_PARAM].getValue(), 0.f, 1.f);
		p.channelMode = static_cast<int>(std::round(params[CHANNEL_PARAM].getValue()));
		p.invert = params[INVERT_PARAM].getValue() > 0.5f;
		p.mix = clamp(params[MIX_PARAM].getValue() + inputs[MIX_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		if (active) videoBusPublishColorLab(outBus, inBus, p); else videoBusClear(outBus);
		outputs[VIDEO_BUS_OUTPUT].setVoltage(videoBus.voltage()); lights[SIGNAL_LIGHT].setBrightnessSmooth(active ? 1.f : 0.f, args.sampleTime);
	}
	json_t* dataToJson() override { json_t* r=json_object(); saveVideoBusId(r, videoBus); return r; }
	void dataFromJson(json_t* r) override { loadVideoBusId(r, videoBus, "outputBusVoltage"); }
};
struct ColorLabWidget : ModuleWidget {
	ColorLabWidget(ColorLab* module) {
		using namespace rasterlab_panel;
		const float W = 50.8f;	// 10 HP
		const float P = 12.2f;	// column pitch, so four 3.8 mm labels (BRIGHT, CONTR) fit side by side
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/ColorLab.svg")));
		addPanelHeader(this, W, "09", "COLOR LAB");

		addKnobLabel(this, Vec(rowX(W, 4, 0, P), 32), "INV");
		addParam(createParamCentered<CKSS>(mm2px(Vec(rowX(W, 4, 0, P), 32)), module, ColorLab::INVERT_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 1, P), 32), "HUE");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(rowX(W, 4, 1, P), 32)), module, ColorLab::HUE_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 2, P), 32), "SAT");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(rowX(W, 4, 2, P), 32)), module, ColorLab::SAT_PARAM));

		addKnobLabel(this, Vec(rowX(W, 4, 0, P), 54), "BRIGHT", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 0, P), 54)), module, ColorLab::BRIGHT_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 1, P), 54), "CONTR", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 1, P), 54)), module, ColorLab::CONTRAST_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 2, P), 54), "POST", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 2, P), 54)), module, ColorLab::POSTER_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 3, P), 54), "SOLAR", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 3, P), 54)), module, ColorLab::SOLAR_PARAM));

		addKnobLabel(this, Vec(rowX(W, 4, 0, P), 72), "CHAN", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 0, P), 72)), module, ColorLab::CHANNEL_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 1, P), 72), "MIX", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 1, P), 72)), module, ColorLab::MIX_PARAM));

		addJackLabel(this, Vec(rowX(W, 4, 0, P), 88), "BUS IN");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 0, P), 88)), module, ColorLab::VIDEO_BUS_INPUT));
		addJackLabel(this, Vec(rowX(W, 4, 1, P), 88), "HUE");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 1, P), 88)), module, ColorLab::HUE_INPUT));
		addJackLabel(this, Vec(rowX(W, 4, 2, P), 88), "SAT");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 2, P), 88)), module, ColorLab::SAT_INPUT));
		addJackLabel(this, Vec(rowX(W, 4, 3, P), 88), "BRIGHT");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 3, P), 88)), module, ColorLab::BRIGHT_INPUT));

		addJackLabel(this, Vec(rowX(W, 4, 0, P), 102), "MIX");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 0, P), 102)), module, ColorLab::MIX_INPUT));

		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 4, 1, P), 114), "BUS", module, ColorLab::VIDEO_BUS_OUTPUT);
		addJackLabel(this, Vec(rowX(W, 4, 2, P), 114), "SIG");
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(rowX(W, 4, 2, P), 114)), module, ColorLab::SIGNAL_LIGHT));
	}
};

Model* modelColorLab = createModel<ColorLab, ColorLabWidget>("ColorLab");
