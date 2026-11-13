#include "plugin.hpp"
#include "panel_style.hpp"
#include "rotator_math.hpp"
#include "video_bus.hpp"

#include <cmath>

// ZOOM is stored as octaves (-1..1) and shown as a factor: 1/8 (a postage stamp) to 8x (a close-up).
struct RotatorZoomQuantity : ParamQuantity {
	float getDisplayValue() override { return std::exp2(3.f * getValue()); }
	void setDisplayValue(float value) override { setValue(std::log2(clamp(value, 0.125f, 8.f)) / 3.f); }
};

struct RotatorSpinQuantity : ParamQuantity {
	float getDisplayValue() override { return static_cast<float>(rotatorSpinRate(getValue())); }
	void setDisplayValue(float value) override {
		const float v = std::sqrt(std::fabs(value) * 0.5f);
		setValue(value < 0.f ? -v : v);
	}
};

struct Rotator : Module {
	enum ParamId { ANGLE_PARAM, SPIN_PARAM, ZOOM_PARAM, X_PARAM, Y_PARAM, PIVOT_X_PARAM, PIVOT_Y_PARAM,
		EDGE_PARAM, FOLDS_PARAM, STEP_PARAM, GLIDE_PARAM, PARAMS_LEN };
	enum InputId { VIDEO_BUS_INPUT, ANGLE_INPUT, SPIN_INPUT, ZOOM_INPUT, X_INPUT, Y_INPUT, NOTCH_INPUT, RESET_INPUT, INPUTS_LEN };
	enum OutputId { VIDEO_BUS_OUTPUT, OUTPUTS_LEN };
	enum LightId { SIGNAL_LIGHT, LIGHTS_LEN };
	VideoBusOwner videoBus;
	RotatorState state;
	dsp::SchmittTrigger notchTrigger;
	dsp::SchmittTrigger resetTrigger;

	Rotator() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(ANGLE_PARAM, -0.5f, 0.5f, 0.f, "Angle", "°", 0.f, 360.f);
		configParam<RotatorSpinQuantity>(SPIN_PARAM, -1.f, 1.f, 0.f, "Spin", " turns/s");
		configParam<RotatorZoomQuantity>(ZOOM_PARAM, -1.f, 1.f, 0.f, "Zoom", "x");
		configParam(X_PARAM, -0.5f, 0.5f, 0.f, "X (frame fraction)");
		configParam(Y_PARAM, -0.5f, 0.5f, 0.f, "Y (frame fraction)");
		configParam(PIVOT_X_PARAM, -0.5f, 0.5f, 0.f, "Pivot X (frame fraction)");
		configParam(PIVOT_Y_PARAM, -0.5f, 0.5f, 0.f, "Pivot Y (frame fraction)");
		configSwitch(EDGE_PARAM, 0.f, 3.f, 0.f, "Edge", {"Black", "Hold", "Repeat", "Mirror"});
		configSwitch(FOLDS_PARAM, 1.f, 12.f, 1.f, "Folds", {"Off", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"});
		configSwitch(STEP_PARAM, 0.f, 4.f, 2.f, "Notch step", {"1/16 turn", "1/12 turn", "1/8 turn", "1/6 turn", "1/4 turn"});
		configParam(GLIDE_PARAM, 0.f, 2.f, 0.f, "Glide", " s");
		configInput(VIDEO_BUS_INPUT, "Video bus token");
		configInput(ANGLE_INPUT, "Angle CV (10 V is one full turn)");
		configInput(SPIN_INPUT, "Spin CV (5 V is full scale)");
		configInput(ZOOM_INPUT, "Zoom CV");
		configInput(X_INPUT, "X CV");
		configInput(Y_INPUT, "Y CV");
		configInput(NOTCH_INPUT, "Notch trigger");
		configInput(RESET_INPUT, "Reset trigger");
		configOutput(VIDEO_BUS_OUTPUT, "Video bus token");
	}

	void onReset() override { state.reset(); }

	void process(const ProcessArgs& args) override {
		auto cv = [&](int param, int input, float lo, float hi) {
			return clamp(params[param].getValue() + inputs[input].getVoltage() * 0.1f, lo, hi);
		};
		const bool notch = notchTrigger.process(inputs[NOTCH_INPUT].getVoltage(), 0.1f, 2.f);
		const bool reset = resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 2.f);
		const int stepIndex = static_cast<int>(std::round(params[STEP_PARAM].getValue()));
		// SPIN CV: 5 V is full scale, so a bipolar LFO swings it both ways.
		const float spinKnob = clamp(params[SPIN_PARAM].getValue() + inputs[SPIN_INPUT].getVoltage() * 0.2f, -1.f, 1.f);
		state.advance(args.sampleTime, spinKnob, rotatorStepTurns(stepIndex), params[GLIDE_PARAM].getValue(), notch, reset);

		const int inBus = inputs[VIDEO_BUS_INPUT].isConnected() ? videoBusIdFromVoltage(inputs[VIDEO_BUS_INPUT].getVoltage()) : 1;
		const int outBus = videoBus.id();
		const bool active = videoBusIsActive(inBus) && inBus != outBus;
		RotatorParams p;
		p.angle = rotatorAngle(params[ANGLE_PARAM].getValue(), inputs[ANGLE_INPUT].getVoltage(), state.spin, state.notchPos);
		p.zoom = std::exp2(3.f * cv(ZOOM_PARAM, ZOOM_INPUT, -1.f, 1.f));
		p.offsetX = cv(X_PARAM, X_INPUT, -0.5f, 0.5f);
		p.offsetY = cv(Y_PARAM, Y_INPUT, -0.5f, 0.5f);
		p.pivotX = params[PIVOT_X_PARAM].getValue();
		p.pivotY = params[PIVOT_Y_PARAM].getValue();
		p.edge = static_cast<int>(clamp(std::round(params[EDGE_PARAM].getValue()), 0.f, 3.f));
		p.folds = static_cast<int>(clamp(std::round(params[FOLDS_PARAM].getValue()), 1.f, 12.f));
		if (active) videoBusPublishRotator(outBus, inBus, p);
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

// The setting of a stepped knob, written under it so the panel needs no printed scale.
struct RotatorReadout : TransparentWidget {
	Rotator* module = nullptr;
	int paramId = 0;

	std::string text() const {
		const float v = module ? module->params[paramId].getValue() : (paramId == Rotator::STEP_PARAM ? 2.f : paramId == Rotator::FOLDS_PARAM ? 1.f : 0.f);
		const int i = static_cast<int>(std::round(v));
		switch (paramId) {
		case Rotator::EDGE_PARAM: {
			static const char* names[] = {"BLACK", "HOLD", "REPEAT", "MIRROR"};
			return names[clamp(i, 0, 3)];
		}
		case Rotator::FOLDS_PARAM: return i <= 1 ? "OFF" : std::to_string(i);
		case Rotator::STEP_PARAM: {
			static const char* names[] = {"1/16", "1/12", "1/8", "1/6", "1/4"};
			return names[clamp(i, 0, 4)];
		}
		default: {
			char buf[16];
			std::snprintf(buf, sizeof buf, "%.2f S", v);
			return buf;
		}
		}
	}
	void draw(const DrawArgs& args) override {
		rasterlab_panel::useLabelFont(args.vg);
		nvgFontSize(args.vg, mm2px(rasterlab_panel::LABEL_MM));
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, nvgRGB(27, 27, 27));
		nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.5f, text().c_str(), nullptr);
	}
};

struct RotatorWidget : ModuleWidget {
	void addReadout(Rotator* module, Vec centreMm, int paramId) {
		auto* r = new RotatorReadout;
		r->module = module;
		r->paramId = paramId;
		const Vec size = mm2px(Vec(12.f, 4.f));
		r->box.size = size;
		r->box.pos = mm2px(centreMm).minus(size.mult(0.5f));
		addChild(r);
	}

	RotatorWidget(Rotator* module) {
		using namespace rasterlab_panel;
		const float W = 71.12f;
		const float P = 13.f;
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Rotator.svg")));
		addPanelHeader(this, W, "20", "ROTATOR");

		// Row 1: where the picture is. Each knob has its CV jack straight below it.
		const char* knobs[] = {"ANGLE", "SPIN", "ZOOM", "X", "Y"};
		const int knobIds[] = {Rotator::ANGLE_PARAM, Rotator::SPIN_PARAM, Rotator::ZOOM_PARAM, Rotator::X_PARAM, Rotator::Y_PARAM};
		const int jackIds[] = {Rotator::ANGLE_INPUT, Rotator::SPIN_INPUT, Rotator::ZOOM_INPUT, Rotator::X_INPUT, Rotator::Y_INPUT};
		for (int i = 0; i < 5; ++i) {
			const Vec knob(rowX(W, 5, i, P), 30.f);
			addKnobLabel(this, knob, knobs[i]);
			addParam(createParamCentered<RoundBlackKnob>(mm2px(knob), module, knobIds[i]));
			const Vec jack(rowX(W, 5, i, P), 46.f);
			addJackLabel(this, jack, "CV");
			addInput(createInputCentered<PJ301MPort>(mm2px(jack), module, jackIds[i]));
		}
		addChild(createPanelRule(4.f, W - 4.f, 55.f));

		// Row 2: the pivot, then what shows past the border and the wedges.
		const char* pivots[] = {"PIVOT X", "PIVOT Y"};
		for (int i = 0; i < 2; ++i) {
			const Vec pos(rowX(W, 4, i, 16.f), 69.f);
			addKnobLabel(this, pos, pivots[i]);
			addParam(createParamCentered<RoundBlackKnob>(mm2px(pos), module, Rotator::PIVOT_X_PARAM + i));
		}
		const Vec edge(rowX(W, 4, 2, 16.f), 69.f);
		addKnobLabel(this, edge, "EDGE");
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(edge), module, Rotator::EDGE_PARAM));
		addReadout(module, Vec(edge.x, edge.y + R_KNOB + 3.6f), Rotator::EDGE_PARAM);
		const Vec folds(rowX(W, 4, 3, 16.f), 69.f);
		addKnobLabel(this, folds, "FOLDS");
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(folds), module, Rotator::FOLDS_PARAM));
		addReadout(module, Vec(folds.x, folds.y + R_KNOB + 3.6f), Rotator::FOLDS_PARAM);
		addChild(createPanelRule(4.f, W - 4.f, 82.f));

		// Row 3: the indexing table. A clock into NOTCH clicks the picture round by STEP, taking GLIDE.
		const Vec step(rowX(W, 4, 0, 16.f), 97.f);
		addKnobLabel(this, step, "STEP");
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(step), module, Rotator::STEP_PARAM));
		addReadout(module, Vec(step.x, step.y + R_KNOB + 3.6f), Rotator::STEP_PARAM);
		const Vec glide(rowX(W, 4, 1, 16.f), 97.f);
		addKnobLabel(this, glide, "GLIDE");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(glide), module, Rotator::GLIDE_PARAM));
		addReadout(module, Vec(glide.x, glide.y + R_KNOB + 3.6f), Rotator::GLIDE_PARAM);
		const Vec notch(rowX(W, 4, 2, 16.f), 97.f);
		addJackLabel(this, notch, "NOTCH");
		addInput(createInputCentered<PJ301MPort>(mm2px(notch), module, Rotator::NOTCH_INPUT));
		const Vec reset(rowX(W, 4, 3, 16.f), 97.f);
		addJackLabel(this, reset, "RESET");
		addInput(createInputCentered<PJ301MPort>(mm2px(reset), module, Rotator::RESET_INPUT));

		const Vec busIn(rowX(W, 5, 0, P), 118.f);
		addJackLabel(this, busIn, "BUS IN");
		addInput(createInputCentered<PJ301MPort>(mm2px(busIn), module, Rotator::VIDEO_BUS_INPUT));
		const Vec sig(rowX(W, 5, 2, P), 118.f);
		addJackLabel(this, sig, "SIG");
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(sig), module, Rotator::SIGNAL_LIGHT));
		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 5, 4, P), 118.f), "BUS", module, Rotator::VIDEO_BUS_OUTPUT);
	}
};

Model* modelRotator = createModel<Rotator, RotatorWidget>("Rotator");
