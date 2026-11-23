#include "plugin.hpp"
#include "panel_style.hpp"
#include "panel_items.hpp"
#include "rotator_math.hpp"
#include "video_bus.hpp"

#include <atomic>
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
	// Input ids are append-only: the jacks added after RESET_INPUT are the CV for the controls that had none.
	enum InputId { VIDEO_BUS_INPUT, ANGLE_INPUT, SPIN_INPUT, ZOOM_INPUT, X_INPUT, Y_INPUT, NOTCH_INPUT, RESET_INPUT,
		PIVOT_X_INPUT, PIVOT_Y_INPUT, EDGE_INPUT, FOLDS_INPUT, STEP_INPUT, GLIDE_INPUT, INPUTS_LEN };
	enum OutputId { VIDEO_BUS_OUTPUT, OUTPUTS_LEN };
	enum LightId { SIGNAL_LIGHT, LIGHTS_LEN };
	VideoBusOwner videoBus;
	RotatorState state;
	dsp::SchmittTrigger notchTrigger;
	dsp::SchmittTrigger resetTrigger;
	// What EDGE, FOLDS, STEP and GLIDE come to with their CV added, for the readouts under the knobs.
	std::atomic<int> shownEdge{0}, shownFolds{1}, shownStep{2};
	std::atomic<float> shownGlide{0.f};

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
		configInput(PIVOT_X_INPUT, "Pivot X CV (10 V moves a picture width)");
		configInput(PIVOT_Y_INPUT, "Pivot Y CV (10 V moves a picture height)");
		configInput(EDGE_INPUT, "Edge CV (10 V sweeps Black to Mirror)");
		configInput(FOLDS_INPUT, "Folds CV (10 V sweeps Off to 12)");
		configInput(STEP_INPUT, "Notch step CV (10 V sweeps 1/16 to 1/4 turn)");
		configInput(GLIDE_INPUT, "Glide CV (10 V is 2 s)");
		configOutput(VIDEO_BUS_OUTPUT, "Video bus token");
	}

	void onReset() override { state.reset(); }

	void process(const ProcessArgs& args) override {
		auto cv = [&](int param, int input, float lo, float hi) {
			return clamp(params[param].getValue() + inputs[input].getVoltage() * 0.1f, lo, hi);
		};
		const bool notch = notchTrigger.process(inputs[NOTCH_INPUT].getVoltage(), 0.1f, 2.f);
		const bool reset = resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 2.f);
		const int stepIndex = rotatorSteppedCv(params[STEP_PARAM].getValue(), inputs[STEP_INPUT].getVoltage(), 0.4f, 0, 4);
		const float glide = rotatorCv(params[GLIDE_PARAM].getValue(), inputs[GLIDE_INPUT].getVoltage(), 0.2f, 0.f, 2.f);
		// SPIN CV: 5 V is full scale, so a bipolar LFO swings it both ways.
		const float spinKnob = clamp(params[SPIN_PARAM].getValue() + inputs[SPIN_INPUT].getVoltage() * 0.2f, -1.f, 1.f);
		state.advance(args.sampleTime, spinKnob, rotatorStepTurns(stepIndex), glide, notch, reset);

		const int inBus = inputs[VIDEO_BUS_INPUT].isConnected() ? videoBusIdFromVoltage(inputs[VIDEO_BUS_INPUT].getVoltage()) : 1;
		const int outBus = videoBus.id();
		const bool active = videoBusIsActive(inBus) && inBus != outBus;
		RotatorParams p;
		p.angle = rotatorAngle(params[ANGLE_PARAM].getValue(), inputs[ANGLE_INPUT].getVoltage(), state.spin, state.notchPos);
		p.zoom = std::exp2(3.f * cv(ZOOM_PARAM, ZOOM_INPUT, -1.f, 1.f));
		p.offsetX = cv(X_PARAM, X_INPUT, -0.5f, 0.5f);
		p.offsetY = cv(Y_PARAM, Y_INPUT, -0.5f, 0.5f);
		p.pivotX = rotatorCv(params[PIVOT_X_PARAM].getValue(), inputs[PIVOT_X_INPUT].getVoltage(), 0.1f, -0.5f, 0.5f);
		p.pivotY = rotatorCv(params[PIVOT_Y_PARAM].getValue(), inputs[PIVOT_Y_INPUT].getVoltage(), 0.1f, -0.5f, 0.5f);
		p.edge = rotatorSteppedCv(params[EDGE_PARAM].getValue(), inputs[EDGE_INPUT].getVoltage(), 0.3f, 0, 3);
		p.folds = rotatorSteppedCv(params[FOLDS_PARAM].getValue(), inputs[FOLDS_INPUT].getVoltage(), 1.1f, 1, 12);
		shownEdge.store(p.edge, std::memory_order_relaxed);
		shownFolds.store(p.folds, std::memory_order_relaxed);
		shownStep.store(stepIndex, std::memory_order_relaxed);
		shownGlide.store(glide, std::memory_order_relaxed);
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
		// With a module, the value after its CV is added, so the word follows what a cable is doing.
		float v = paramId == Rotator::STEP_PARAM ? 2.f : paramId == Rotator::FOLDS_PARAM ? 1.f : 0.f;
		if (module) {
			switch (paramId) {
			case Rotator::EDGE_PARAM: v = static_cast<float>(module->shownEdge.load(std::memory_order_relaxed)); break;
			case Rotator::FOLDS_PARAM: v = static_cast<float>(module->shownFolds.load(std::memory_order_relaxed)); break;
			case Rotator::STEP_PARAM: v = static_cast<float>(module->shownStep.load(std::memory_order_relaxed)); break;
			default: v = module->shownGlide.load(std::memory_order_relaxed); break;
			}
		}
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

#include "rotator_layout.hpp"

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
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Rotator.svg")));
		addPanelHeader(this, kRotatorPanelWidth, "20", "ROTATOR");
		PANEL_PLACE_ALL(ROTATOR_LAYOUT)
		// The setting of each second-row knob, written under it.
		for (int id : {Rotator::EDGE_PARAM, Rotator::FOLDS_PARAM, Rotator::STEP_PARAM, Rotator::GLIDE_PARAM}) {
			if (ParamWidget* knob = getParam(id)) {
				const Vec centre = knob->box.getCenter();
				const float perMm = mm2px(1.f);
				addReadout(module, Vec(centre.x / perMm, centre.y / perMm + R_KNOB + 3.6f), id);
			}
		}
	}
};

Model* modelRotator = createModel<Rotator, RotatorWidget>("Rotator");
