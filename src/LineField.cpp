#include "plugin.hpp"
#include "panel_style.hpp"
#include "video_bus.hpp"

#include <atomic>
#include <cmath>

struct LineField : Module {
	enum ParamId {
		DEPTH_PARAM,
		X_TILT_PARAM,
		Y_TILT_PARAM,
		Z_SPIN_PARAM,
		PERSPECTIVE_PARAM,
		LINES_PARAM,
		GAIN_PARAM,
		MIX_PARAM,
		INVERT_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		VIDEO_BUS_INPUT,
		DEPTH_INPUT,
		X_TILT_INPUT,
		Y_TILT_INPUT,
		Z_SPIN_INPUT,
		LINES_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		VIDEO_BUS_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		SIGNAL_LIGHT,
		LIGHTS_LEN
	};

	std::atomic<int> sourceBus {1};
	std::atomic<bool> hasSignal {false};
	VideoBusOwner videoBus;

	LineField() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(DEPTH_PARAM, 0.f, 1.f, 0.55f, "Depth");
		configParam(X_TILT_PARAM, -1.f, 1.f, 0.28f, "X tilt");
		configParam(Y_TILT_PARAM, -1.f, 1.f, 0.f, "Y tilt");
		configParam(Z_SPIN_PARAM, -1.f, 1.f, 0.f, "Z spin");
		configParam(PERSPECTIVE_PARAM, 0.f, 1.f, 0.35f, "Perspective");
		configParam(LINES_PARAM, 0.f, 1.f, 0.55f, "Line density");
		configParam(GAIN_PARAM, 0.f, 2.f, 1.15f, "Line gain");
		configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Mix");
		configSwitch(INVERT_PARAM, 0.f, 1.f, 0.f, "Invert height", {"Off", "On"});
		configInput(VIDEO_BUS_INPUT, "Video bus token");
		configInput(DEPTH_INPUT, "Depth CV");
		configInput(X_TILT_INPUT, "X tilt CV");
		configInput(Y_TILT_INPUT, "Y tilt CV");
		configInput(Z_SPIN_INPUT, "Z spin CV");
		configInput(LINES_INPUT, "Line density CV");
		configOutput(VIDEO_BUS_OUTPUT, "Video bus token");
	}

	void process(const ProcessArgs& args) override {
		const int inputBusId = inputs[VIDEO_BUS_INPUT].isConnected() ? videoBusIdFromVoltage(inputs[VIDEO_BUS_INPUT].getVoltage()) : 1;
		const bool active = videoBusIsActive(inputBusId);

		LineFieldParams lineField;
		lineField.depth = clamp(params[DEPTH_PARAM].getValue() + inputs[DEPTH_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		lineField.xTilt = clamp(params[X_TILT_PARAM].getValue() + inputs[X_TILT_INPUT].getVoltage() * 0.1f, -1.f, 1.f);
		lineField.yTilt = clamp(params[Y_TILT_PARAM].getValue() + inputs[Y_TILT_INPUT].getVoltage() * 0.1f, -1.f, 1.f);
		lineField.zSpin = clamp(params[Z_SPIN_PARAM].getValue() + inputs[Z_SPIN_INPUT].getVoltage() * 0.1f, -1.f, 1.f);
		lineField.perspective = clamp(params[PERSPECTIVE_PARAM].getValue(), 0.f, 1.f);
		lineField.lineDensity = clamp(params[LINES_PARAM].getValue() + inputs[LINES_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		lineField.gain = clamp(params[GAIN_PARAM].getValue(), 0.f, 2.f);
		lineField.mix = clamp(params[MIX_PARAM].getValue(), 0.f, 1.f);
		lineField.invert = params[INVERT_PARAM].getValue() > 0.5f;

		sourceBus.store(inputBusId, std::memory_order_relaxed);
		hasSignal.store(active, std::memory_order_relaxed);

		const int outputBusId = videoBus.id();
		if (active && inputBusId != outputBusId) {
			videoBusPublishLineField(outputBusId, inputBusId, lineField);
		}
		else {
			videoBusClear(outputBusId);
		}

		outputs[VIDEO_BUS_OUTPUT].setVoltage(videoBus.voltage());
		lights[SIGNAL_LIGHT].setBrightnessSmooth(active ? 1.f : 0.f, args.sampleTime);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		saveVideoBusId(rootJ, videoBus);
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		loadVideoBusId(rootJ, videoBus, "outputBusVoltage");
	}
};

struct LineFieldWidget : ModuleWidget {
	LineFieldWidget(LineField* module) {
		using namespace rasterlab_panel;
		const float W = 60.96f;	// 12 HP
		const float P = 15.24f;	// column pitch (3 HP): four columns of 3.8 mm labels, full words on the jacks
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/LineField.svg")));
		addPanelHeader(this, W, "04", "LINE FIELD");

		addKnobLabel(this, Vec(rowX(W, 2, 0, P), 30), "DEPTH");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(rowX(W, 2, 0, P), 30)), module, LineField::DEPTH_PARAM));
		addKnobLabel(this, Vec(rowX(W, 2, 1, P), 30), "X TILT");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(rowX(W, 2, 1, P), 30)), module, LineField::X_TILT_PARAM));

		addKnobLabel(this, Vec(rowX(W, 4, 0, P), 50), "Y TILT", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 0, P), 50)), module, LineField::Y_TILT_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 1, P), 50), "Z SPIN", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 1, P), 50)), module, LineField::Z_SPIN_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 2, P), 50), "PERSP", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 2, P), 50)), module, LineField::PERSPECTIVE_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 3, P), 50), "LINES", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 3, P), 50)), module, LineField::LINES_PARAM));

		addKnobLabel(this, Vec(rowX(W, 4, 0, P), 70), "GAIN", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 0, P), 70)), module, LineField::GAIN_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 1, P), 70), "MIX", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 1, P), 70)), module, LineField::MIX_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 2, P), 70), "INV Z", R_SMALL);
		addParam(createParamCentered<CKSS>(mm2px(Vec(rowX(W, 4, 2, P), 70)), module, LineField::INVERT_PARAM));

		addJackLabel(this, Vec(rowX(W, 4, 0, P), 90), "BUS IN");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 0, P), 90)), module, LineField::VIDEO_BUS_INPUT));
		addJackLabel(this, Vec(rowX(W, 4, 1, P), 90), "DEPTH");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 1, P), 90)), module, LineField::DEPTH_INPUT));
		addJackLabel(this, Vec(rowX(W, 4, 2, P), 90), "X TILT");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 2, P), 90)), module, LineField::X_TILT_INPUT));
		addJackLabel(this, Vec(rowX(W, 4, 3, P), 90), "Y TILT");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 3, P), 90)), module, LineField::Y_TILT_INPUT));

		addJackLabel(this, Vec(rowX(W, 4, 0, P), 104), "Z SPIN");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 0, P), 104)), module, LineField::Z_SPIN_INPUT));
		addJackLabel(this, Vec(rowX(W, 4, 1, P), 104), "LINES");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 1, P), 104)), module, LineField::LINES_INPUT));

		addPlatedOutput<PJ301MPort>(this, Vec(W * 0.5f, 114), "BUS", module, LineField::VIDEO_BUS_OUTPUT);
		addJackLabel(this, Vec(W * 0.5f + GRID_MM, 114), "SIG");
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(W * 0.5f + GRID_MM, 114)), module, LineField::SIGNAL_LIGHT));
	}
};

Model* modelLineField = createModel<LineField, LineFieldWidget>("LineField");
