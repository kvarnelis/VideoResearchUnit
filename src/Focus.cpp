#include "plugin.hpp"
#include "panel_style.hpp"
#include "cv_param.hpp"
#include "video_bus.hpp"

#include <cmath>

// VRU-24 Focus: softens, sharpens and misaligns the colours of a picture, the way an old picture tube did.
// The pixel work is in the renderer (renderFocusFrame, focus_filter.hpp); this file only publishes the knobs.
// Every knob has a CV jack, and a small CV AMOUNT trimmer scales (or flips) that CV; at its default +100% a
// patch with no cable reads exactly the knob.
struct Focus : Module {
	enum ParamId {
		FOCUS_PARAM, SMEAR_PARAM, SHARP_PARAM, CONVERGE_PARAM, MIX_PARAM,
		FOCUS_CV_PARAM, SMEAR_CV_PARAM, SHARP_CV_PARAM, CONVERGE_CV_PARAM, MIX_CV_PARAM,
		PARAMS_LEN
	};
	enum InputId { VIDEO_BUS_INPUT, FOCUS_INPUT, SMEAR_INPUT, SHARP_INPUT, CONVERGE_INPUT, MIX_INPUT, INPUTS_LEN };
	enum OutputId { VIDEO_BUS_OUTPUT, OUTPUTS_LEN };
	enum LightId { SIGNAL_LIGHT, LIGHTS_LEN };
	VideoBusOwner videoBus;

	void describe(int paramId, const char* text) { paramQuantities[paramId]->description = text; }

	Focus() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(FOCUS_PARAM, 0.f, 1.f, 0.f, "Focus", "%", 0.f, 100.f);
		configParam(SMEAR_PARAM, 0.f, 1.f, 0.f, "Smear", "%", 0.f, 100.f);
		configParam(SHARP_PARAM, 0.f, 1.f, 0.f, "Sharp", "%", 0.f, 100.f);
		configParam(CONVERGE_PARAM, -1.f, 1.f, 0.f, "Converge", "%", 0.f, 100.f);
		configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Mix", "%", 0.f, 100.f);
		describe(FOCUS_PARAM, "Softens the whole picture in every direction, like a badly focused tube. Zero leaves it alone.");
		describe(SMEAR_PARAM, "Softens along each line only, so bright shapes trail to the right and lines stay separate. Zero leaves it alone.");
		describe(SHARP_PARAM, "Makes edges crisper with a thin bright and dark rim beside them, the opposite of blur. Zero leaves it alone.");
		describe(CONVERGE_PARAM, "Slides the red and blue pictures apart sideways in opposite directions, leaving colour fringes. Zero lines them up.");
		describe(MIX_PARAM, "How much of the changed picture you see. Full is all changed, zero is the original.");
		configParam(FOCUS_CV_PARAM, -1.f, 1.f, 1.f, "Focus CV amount", "%", 0.f, 100.f);
		configParam(SMEAR_CV_PARAM, -1.f, 1.f, 1.f, "Smear CV amount", "%", 0.f, 100.f);
		configParam(SHARP_CV_PARAM, -1.f, 1.f, 1.f, "Sharp CV amount", "%", 0.f, 100.f);
		configParam(CONVERGE_CV_PARAM, -1.f, 1.f, 1.f, "Converge CV amount", "%", 0.f, 100.f);
		configParam(MIX_CV_PARAM, -1.f, 1.f, 1.f, "Mix CV amount", "%", 0.f, 100.f);
		configInput(VIDEO_BUS_INPUT, "Video bus token");
		configInput(FOCUS_INPUT, "Focus CV (10 V is full)");
		configInput(SMEAR_INPUT, "Smear CV (10 V is full)");
		configInput(SHARP_INPUT, "Sharp CV (10 V is full)");
		configInput(CONVERGE_INPUT, "Converge CV (5 V is full)");
		configInput(MIX_INPUT, "Mix CV (10 V is full)");
		configOutput(VIDEO_BUS_OUTPUT, "Video bus token");
	}

	void process(const ProcessArgs& args) override {
		const int inBus = inputs[VIDEO_BUS_INPUT].isConnected() ? videoBusIdFromVoltage(inputs[VIDEO_BUS_INPUT].getVoltage()) : 1;
		const int outBus = videoBus.id();
		const bool active = videoBusIsActive(inBus) && inBus != outBus;
		auto cv = [&](int knob, int amount, int input, float perVolt, float lo, float hi) {
			return rasterlab_cv::modulated(params[knob].getValue(), inputs[input].getVoltage(), params[amount].getValue(), perVolt, lo, hi);
		};
		FocusParams p;
		p.focus = cv(FOCUS_PARAM, FOCUS_CV_PARAM, FOCUS_INPUT, 0.1f, 0.f, 1.f);
		p.smear = cv(SMEAR_PARAM, SMEAR_CV_PARAM, SMEAR_INPUT, 0.1f, 0.f, 1.f);
		p.sharp = cv(SHARP_PARAM, SHARP_CV_PARAM, SHARP_INPUT, 0.1f, 0.f, 1.f);
		p.converge = cv(CONVERGE_PARAM, CONVERGE_CV_PARAM, CONVERGE_INPUT, 0.2f, -1.f, 1.f);
		p.mix = cv(MIX_PARAM, MIX_CV_PARAM, MIX_INPUT, 0.1f, 0.f, 1.f);
		if (active) videoBusPublishFocus(outBus, inBus, p);
		else videoBusClear(outBus);
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

struct FocusWidget : ModuleWidget {
	FocusWidget(Focus* module) {
		using namespace rasterlab_panel;
		const float W = 71.12f;   // 14 HP
		const float P = 21.f;     // column pitch, so a long word like CONVERGE fits
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Focus.svg")));
		addPanelHeader(this, W, "24", "FOCUS");

		auto column = [&](float x, float yKnob, const char* label, int knob, int trim, int jack) {
			addKnobLabel(this, Vec(x, yKnob), label);
			addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(x, yKnob)), module, knob));
			addKnobLabel(this, Vec(x, yKnob + 14.f), "CV AMT", R_TRIM);
			addParam(createParamCentered<Trimpot>(mm2px(Vec(x, yKnob + 14.f)), module, trim));
			addJackLabel(this, Vec(x, yKnob + 27.f), "CV");
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, yKnob + 27.f)), module, jack));
		};
		column(rowX(W, 3, 0, P), 32.f, "FOCUS", Focus::FOCUS_PARAM, Focus::FOCUS_CV_PARAM, Focus::FOCUS_INPUT);
		column(rowX(W, 3, 1, P), 32.f, "SMEAR", Focus::SMEAR_PARAM, Focus::SMEAR_CV_PARAM, Focus::SMEAR_INPUT);
		column(rowX(W, 3, 2, P), 32.f, "SHARP", Focus::SHARP_PARAM, Focus::SHARP_CV_PARAM, Focus::SHARP_INPUT);
		column(rowX(W, 2, 0, P), 82.f, "CONVERGE", Focus::CONVERGE_PARAM, Focus::CONVERGE_CV_PARAM, Focus::CONVERGE_INPUT);
		column(rowX(W, 2, 1, P), 82.f, "MIX", Focus::MIX_PARAM, Focus::MIX_CV_PARAM, Focus::MIX_INPUT);

		const Vec busIn(rowX(W, 5, 0, 13.f), 118.f);
		addJackLabel(this, busIn, "BUS IN");
		addInput(createInputCentered<PJ301MPort>(mm2px(busIn), module, Focus::VIDEO_BUS_INPUT));
		const Vec sig(rowX(W, 5, 2, 13.f), 118.f);
		addJackLabel(this, sig, "SIG");
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(sig), module, Focus::SIGNAL_LIGHT));
		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 5, 4, 13.f), 118.f), "BUS", module, Focus::VIDEO_BUS_OUTPUT);
	}
};

Model* modelFocus = createModel<Focus, FocusWidget>("Focus");
