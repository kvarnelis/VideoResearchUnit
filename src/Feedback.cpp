#include "plugin.hpp"
#include "panel_style.hpp"
#include "frame_ring.hpp"

#include <cmath>

struct FeedbackZoomQuantity : ParamQuantity {
	float getDisplayValue() override { return std::exp2(getValue()); }
	void setDisplayValue(float value) override { setValue(std::log2(std::max(value, 0.5f))); }
};

struct Feedback : Module {
	enum ParamId { ZOOM_PARAM, ROTATE_PARAM, X_PARAM, Y_PARAM, HUE_PARAM, GAIN_PARAM, KEY_PARAM, CLEAR_PARAM, EDGE_PARAM, MODE_PARAM, PARAMS_LEN };
	enum InputId { VIDEO_BUS_INPUT, ZOOM_INPUT, ROTATE_INPUT, X_INPUT, Y_INPUT, HUE_INPUT, GAIN_INPUT, KEY_INPUT, CLEAR_INPUT, INPUTS_LEN };
	enum OutputId { VIDEO_BUS_OUTPUT, OUTPUTS_LEN };
	enum LightId { SIGNAL_LIGHT, CLEAR_LIGHT, LIGHTS_LEN };
	VideoBusOwner videoBus;
	int ringSlotId = 0;
	int frameCounter = 0;
	int clearCounter = 0;
	double tickTime = 0.0;
	float clearLightTime = 0.f;
	dsp::SchmittTrigger clearTrigger;
	dsp::SchmittTrigger clearButton;

	Feedback() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		ringSlotId = frameRingAllocateSlot();
		frameRingSetCapacity(ringSlotId, 1);
		configParam<FeedbackZoomQuantity>(ZOOM_PARAM, -1.f, 1.f, 0.f, "Zoom", "x");
		configParam(ROTATE_PARAM, -0.5f, 0.5f, 0.f, "Rotate", "°", 0.f, 360.f);
		configParam(X_PARAM, -0.5f, 0.5f, 0.f, "X offset (frame fraction)");
		configParam(Y_PARAM, -0.5f, 0.5f, 0.f, "Y offset (frame fraction)");
		configParam(HUE_PARAM, -0.5f, 0.5f, 0.f, "Hue per pass", "°", 0.f, 360.f);
		configParam(GAIN_PARAM, 0.f, 1.2f, 0.9f, "Gain");
		configParam(KEY_PARAM, 0.f, 1.f, 0.f, "Key");
		configButton(CLEAR_PARAM, "Clear feedback");
		configSwitch(EDGE_PARAM, 0.f, 2.f, 0.f, "Edge", {"Clamp", "Wrap", "Black"});
		configSwitch(MODE_PARAM, 0.f, 1.f, 0.f, "Mode", {"Add", "Blend"});
		configInput(VIDEO_BUS_INPUT, "Video bus token");
		const char* names[] = {"Zoom CV", "Rotate CV", "X CV", "Y CV", "Hue CV", "Gain CV", "Key CV", "Clear trigger"};
		for (int i = ZOOM_INPUT; i <= CLEAR_INPUT; ++i) configInput(i, names[i - ZOOM_INPUT]);
		configOutput(VIDEO_BUS_OUTPUT, "Video bus token");
	}
	~Feedback() { frameRingReleaseSlot(ringSlotId); }

	void process(const ProcessArgs& args) override {
		tickTime += args.sampleTime;
		while (tickTime >= 1.0 / 60.0) {
			tickTime -= 1.0 / 60.0;
			frameCounter = frameCounter >= (1 << 30) ? 1 : frameCounter + 1;
		}
		const bool trigger = clearTrigger.process(inputs[CLEAR_INPUT].getVoltage(), 0.1f, 2.f);
		const bool button = clearButton.process(params[CLEAR_PARAM].getValue(), 0.1f, 0.5f);
		if (trigger || button) {
			clearCounter = clearCounter >= (1 << 30) ? 1 : clearCounter + 1;
			clearLightTime = 0.2f;
		}
		clearLightTime = std::max(0.f, clearLightTime - args.sampleTime);
		const int inBus = inputs[VIDEO_BUS_INPUT].isConnected() ? videoBusIdFromVoltage(inputs[VIDEO_BUS_INPUT].getVoltage()) : 1;
		const int outBus = videoBus.id();
		const bool active = videoBusIsActive(inBus) && inBus != outBus;
		auto cv = [&](int param, int input, float lo, float hi) {
			return clamp(params[param].getValue() + inputs[input].getVoltage() * 0.1f, lo, hi);
		};
		FeedbackParams p;
		p.ringSlotId = ringSlotId;
		p.frameCounter = frameCounter;
		p.clearCounter = clearCounter;
		p.zoom = std::exp2(cv(ZOOM_PARAM, ZOOM_INPUT, -1.f, 1.f));
		p.rotate = cv(ROTATE_PARAM, ROTATE_INPUT, -0.5f, 0.5f);
		p.offsetX = cv(X_PARAM, X_INPUT, -0.5f, 0.5f);
		p.offsetY = cv(Y_PARAM, Y_INPUT, -0.5f, 0.5f);
		p.hue = cv(HUE_PARAM, HUE_INPUT, -0.5f, 0.5f);
		p.gain = cv(GAIN_PARAM, GAIN_INPUT, 0.f, 1.2f);
		p.key = cv(KEY_PARAM, KEY_INPUT, 0.f, 1.f);
		p.edge = static_cast<int>(std::round(params[EDGE_PARAM].getValue()));
		p.mode = static_cast<int>(std::round(params[MODE_PARAM].getValue()));
		if (active) videoBusPublishFeedback(outBus, inBus, p);
		else videoBusClear(outBus);
		outputs[VIDEO_BUS_OUTPUT].setVoltage(videoBus.voltage());
		lights[SIGNAL_LIGHT].setBrightnessSmooth(active ? 1.f : 0.f, args.sampleTime);
		lights[CLEAR_LIGHT].setBrightness(clearLightTime > 0.f ? 1.f : 0.f);
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

struct FeedbackWidget : ModuleWidget {
	FeedbackWidget(Feedback* module) {
		using namespace rasterlab_panel;
		const float W = 60.96f;
		const float P = 14.f;
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Feedback.svg")));
		addPanelHeader(this, W, "14", "FEEDBACK");
		const char* knobs[] = {"ZOOM", "ROTATE", "X", "Y", "HUE", "GAIN", "KEY"};
		for (int i = 0; i < 7; ++i) {
			const Vec pos(rowX(W, 4, i % 4, P), i < 4 ? 30.f : 50.f);
			addKnobLabel(this, pos, knobs[i]);
			addParam(createParamCentered<RoundBlackKnob>(mm2px(pos), module, Feedback::ZOOM_PARAM + i));
		}
		const Vec clearPos(rowX(W, 4, 3, P), 50.f);
		// R_KNOB keeps CLEAR on the same label line as HUE, GAIN and KEY.
		addKnobLabel(this, clearPos, "CLEAR", R_KNOB);
		addParam(createParamCentered<LEDButton>(mm2px(clearPos), module, Feedback::CLEAR_PARAM));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(clearPos), module, Feedback::CLEAR_LIGHT));
		const char* jacks[] = {"ZOOM", "ROT", "X", "Y", "HUE", "GAIN", "KEY", "CLEAR"};
		for (int i = 0; i < 8; ++i) {
			const Vec pos(rowX(W, 4, i % 4, P), i < 4 ? 68.f : 84.f);
			addJackLabel(this, pos, jacks[i]);
			addInput(createInputCentered<PJ301MPort>(mm2px(pos), module, Feedback::ZOOM_INPUT + i));
		}
		// A three-position switch (CKSSThree) is taller than a CKSS, and a heading placed for a CKSS touched its
		// top. Both headings in this row are raised by the same 1.4 mm (the usual LABEL_GAP_MM), so EDGE clears
		// its switch by about that much and MODE stays level with it. Only the headings move; the lever words stay.
		const float toggleHeadingRadius = R_SWITCH + 1.4f;
		// The three-position lever is up at 2, centre at 1 and down at 0.
		const Vec edgePos(rowX(W, 4, 0, P), 102.f);
		addToggleLabels(this, edgePos, "EDGE", "BLACK", "CLAMP", 1, toggleHeadingRadius);
		addChild(createPanelTextAt(Vec(edgePos.x + TOGGLE_WORD_DX_MM, edgePos.y), "WRAP", NOTE_MM, NVG_ALIGN_LEFT));
		addParam(createParamCentered<CKSSThree>(mm2px(edgePos), module, Feedback::EDGE_PARAM));
		const Vec modePos(rowX(W, 4, 2, P), 102.f);
		addToggleLabels(this, modePos, "MODE", "BLEND", "ADD", 1, toggleHeadingRadius);
		addParam(createParamCentered<CKSS>(mm2px(modePos), module, Feedback::MODE_PARAM));
		const Vec busIn(rowX(W, 4, 0, P), 116.f);
		addJackLabel(this, busIn, "BUS IN");
		addInput(createInputCentered<PJ301MPort>(mm2px(busIn), module, Feedback::VIDEO_BUS_INPUT));
		const Vec sig(rowX(W, 4, 2, P), 116.f);
		addJackLabel(this, sig, "SIG");
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(sig), module, Feedback::SIGNAL_LIGHT));
		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 4, 3, P), 116.f), "BUS", module, Feedback::VIDEO_BUS_OUTPUT);
	}
};

Model* modelFeedback = createModel<Feedback, FeedbackWidget>("Feedback");
