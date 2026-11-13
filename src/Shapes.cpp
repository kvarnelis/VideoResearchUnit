#include "plugin.hpp"
#include "panel_style.hpp"
#include "shapes_math.hpp"
#include "video_bus.hpp"

#include <cmath>

// SIZE is stored as a knob position and shown as the shape's inscribed radius, in percent of the picture height.
struct ShapesSizeQuantity : ParamQuantity {
	float getDisplayValue() override { return 50.f * shapes_math::sizeFromKnob(getValue()); }
	void setDisplayValue(float value) override { setValue(shapes_math::knobFromSize(value / 50.f)); }
};

// SPREAD is stored as a knob position and shown as the half-width of the grey ramp, in percent of the picture height.
struct ShapesSpreadQuantity : ParamQuantity {
	float getDisplayValue() override { return 50.f * shapes_math::spreadFromKnob(getValue()); }
	void setDisplayValue(float value) override { setValue(shapes_math::knobFromSpread(value / 50.f)); }
};

// ASPECT is stored as octaves of two (-1..1) and shown as the width-over-height ratio, 1/4 to 4.
struct ShapesAspectQuantity : ParamQuantity {
	float getDisplayValue() override { return shapes_math::aspectFromKnob(getValue()); }
	void setDisplayValue(float value) override { setValue(shapes_math::knobFromAspect(value)); }
};

// Shapes draws one closed shape on its own video bus: a grey picture whose brightness is the distance from the
// shape's edge. It has no video input, so, like Ramps, it publishes all the time.
struct Shapes : Module {
	enum ParamId { SIDES_PARAM, ROUND_PARAM, POINT_PARAM, SIZE_PARAM, X_PARAM, Y_PARAM, ASPECT_PARAM, TURN_PARAM,
		REPEAT_PARAM, COPIES_PARAM, RING_PARAM, MELT_PARAM, SPREAD_PARAM, MATTE_PARAM, PARAMS_LEN };
	enum InputId { SIDES_INPUT, ROUND_INPUT, POINT_INPUT, SIZE_INPUT, X_INPUT, Y_INPUT, ASPECT_INPUT, TURN_INPUT,
		MELT_INPUT, SPREAD_INPUT, INPUTS_LEN };
	enum OutputId { VIDEO_BUS_OUTPUT, OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };
	VideoBusOwner videoBus;

	Shapes() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(SIDES_PARAM, 3.f, 12.f, 4.f, "Sides", " sides");
		configParam(ROUND_PARAM, 0.f, 1.f, 0.f, "Round (sharp corners to circle)", "%", 0.f, 100.f);
		configParam(POINT_PARAM, 0.f, 1.f, 0.f, "Point (polygon to star)", "%", 0.f, 100.f);
		configParam<ShapesSizeQuantity>(SIZE_PARAM, 0.f, 1.f, 0.45f, "Size (inscribed radius)", "% of height");
		configParam(X_PARAM, -0.5f, 0.5f, 0.f, "X (frame fraction)");
		configParam(Y_PARAM, -0.5f, 0.5f, 0.f, "Y (frame fraction)");
		configParam<ShapesAspectQuantity>(ASPECT_PARAM, -1.f, 1.f, 0.f, "Aspect (width over height)", "x");
		configParam(TURN_PARAM, -0.5f, 0.5f, 0.f, "Turn", "°", 0.f, 360.f);
		configSwitch(REPEAT_PARAM, 1.f, 8.f, 1.f, "Repeat (tiles per picture height)", {"Off", "2", "3", "4", "5", "6", "7", "8"});
		configSwitch(COPIES_PARAM, 1.f, 8.f, 1.f, "Copies (in a ring)", {"Off", "2", "3", "4", "5", "6", "7", "8"});
		configParam(RING_PARAM, 0.f, 1.f, 0.4f, "Ring radius", "% of height", 0.f, 100.f * shapes_math::kRingMax * 0.5f);
		configParam(MELT_PARAM, 0.f, 1.f, 0.f, "Melt (neighbours fuse)", "%", 0.f, 100.f);
		configParam<ShapesSpreadQuantity>(SPREAD_PARAM, 0.f, 1.f, 0.55f, "Spread (grey ramp half-width)", "% of height");
		configSwitch(MATTE_PARAM, 0.f, 1.f, 0.f, "Matte", {"Off (grey picture)", "On (white on black)"});
		configInput(SIDES_INPUT, "Sides CV (10 V sweeps 3 to 12)");
		configInput(ROUND_INPUT, "Round CV (10 V is the whole range)");
		configInput(POINT_INPUT, "Point CV (10 V is the whole range)");
		configInput(SIZE_INPUT, "Size CV (10 V is the whole range)");
		configInput(X_INPUT, "X CV (10 V moves a picture width)");
		configInput(Y_INPUT, "Y CV (10 V moves a picture height)");
		configInput(ASPECT_INPUT, "Aspect CV (5 V is full scale)");
		configInput(TURN_INPUT, "Turn CV (10 V is one full turn)");
		configInput(MELT_INPUT, "Melt CV (10 V is the whole range)");
		configInput(SPREAD_INPUT, "Spread CV (10 V is the whole range)");
		configOutput(VIDEO_BUS_OUTPUT, "Video bus token");
	}

	void process(const ProcessArgs& args) override {
		(void) args;
		shapes_math::ShapesControls c;
		c.sides = params[SIDES_PARAM].getValue();
		c.round = params[ROUND_PARAM].getValue();
		c.point = params[POINT_PARAM].getValue();
		c.size = params[SIZE_PARAM].getValue();
		c.x = params[X_PARAM].getValue();
		c.y = params[Y_PARAM].getValue();
		c.aspect = params[ASPECT_PARAM].getValue();
		c.turn = params[TURN_PARAM].getValue();
		c.repeat = params[REPEAT_PARAM].getValue();
		c.copies = params[COPIES_PARAM].getValue();
		c.ring = params[RING_PARAM].getValue();
		c.melt = params[MELT_PARAM].getValue();
		c.spread = params[SPREAD_PARAM].getValue();
		c.matte = params[MATTE_PARAM].getValue();
		c.sidesCv = inputs[SIDES_INPUT].getVoltage();
		c.roundCv = inputs[ROUND_INPUT].getVoltage();
		c.pointCv = inputs[POINT_INPUT].getVoltage();
		c.sizeCv = inputs[SIZE_INPUT].getVoltage();
		c.xCv = inputs[X_INPUT].getVoltage();
		c.yCv = inputs[Y_INPUT].getVoltage();
		c.aspectCv = inputs[ASPECT_INPUT].getVoltage();
		c.turnCv = inputs[TURN_INPUT].getVoltage();
		c.meltCv = inputs[MELT_INPUT].getVoltage();
		c.spreadCv = inputs[SPREAD_INPUT].getVoltage();
		videoBusPublishShapes(videoBus.id(), shapes_math::shapesParams(c));
		outputs[VIDEO_BUS_OUTPUT].setVoltage(videoBus.voltage());
	}
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		saveVideoBusId(rootJ, videoBus);
		return rootJ;
	}
	void dataFromJson(json_t* rootJ) override { loadVideoBusId(rootJ, videoBus, "outputBusVoltage"); }
};

// The setting of a stepped knob, written under it so the panel needs no printed scale: OFF at 1, else the count.
struct ShapesReadout : TransparentWidget {
	Shapes* module = nullptr;
	int paramId = 0;

	std::string text() const {
		const float v = module ? module->params[paramId].getValue() : 1.f;
		const int i = static_cast<int>(std::round(v));
		return i <= 1 ? "OFF" : std::to_string(clamp(i, 2, 8));
	}
	void draw(const DrawArgs& args) override {
		rasterlab_panel::useLabelFont(args.vg);
		nvgFontSize(args.vg, mm2px(rasterlab_panel::LABEL_MM));
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, nvgRGB(27, 27, 27));
		nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.5f, text().c_str(), nullptr);
	}
};

struct ShapesWidget : ModuleWidget {
	void addReadout(Shapes* module, Vec centreMm, int paramId) {
		auto* r = new ShapesReadout;
		r->module = module;
		r->paramId = paramId;
		const Vec size = mm2px(Vec(12.f, 4.f));
		r->box.size = size;
		r->box.pos = mm2px(centreMm).minus(size.mult(0.5f));
		addChild(r);
	}

	ShapesWidget(Shapes* module) {
		using namespace rasterlab_panel;
		const float W = 71.12f;
		const float P = 13.f;
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Shapes.svg")));
		addPanelHeader(this, W, "21", "SHAPES");

		// Row 1: the outline, then how far the shapes and their edges melt and spread. Each knob has its CV jack
		// straight below it.
		const char* outline[] = {"SIDES", "ROUND", "POINT", "MELT", "SPREAD"};
		const int outlineKnobs[] = {Shapes::SIDES_PARAM, Shapes::ROUND_PARAM, Shapes::POINT_PARAM, Shapes::MELT_PARAM, Shapes::SPREAD_PARAM};
		const int outlineJacks[] = {Shapes::SIDES_INPUT, Shapes::ROUND_INPUT, Shapes::POINT_INPUT, Shapes::MELT_INPUT, Shapes::SPREAD_INPUT};
		for (int i = 0; i < 5; ++i) {
			const Vec knob(rowX(W, 5, i, P), 30.f);
			addKnobLabel(this, knob, outline[i]);
			addParam(createParamCentered<RoundBlackKnob>(mm2px(knob), module, outlineKnobs[i]));
			const Vec jack(rowX(W, 5, i, P), 46.f);
			addJackLabel(this, jack, "CV");
			addInput(createInputCentered<PJ301MPort>(mm2px(jack), module, outlineJacks[i]));
		}
		addChild(createPanelRule(4.f, W - 4.f, 55.f));

		// Row 2: where the shape sits.
		const char* place[] = {"SIZE", "X", "Y", "ASPECT", "TURN"};
		const int placeKnobs[] = {Shapes::SIZE_PARAM, Shapes::X_PARAM, Shapes::Y_PARAM, Shapes::ASPECT_PARAM, Shapes::TURN_PARAM};
		const int placeJacks[] = {Shapes::SIZE_INPUT, Shapes::X_INPUT, Shapes::Y_INPUT, Shapes::ASPECT_INPUT, Shapes::TURN_INPUT};
		for (int i = 0; i < 5; ++i) {
			const Vec knob(rowX(W, 5, i, P), 69.f);
			addKnobLabel(this, knob, place[i]);
			addParam(createParamCentered<RoundBlackKnob>(mm2px(knob), module, placeKnobs[i]));
			const Vec jack(rowX(W, 5, i, P), 85.f);
			addJackLabel(this, jack, "CV");
			addInput(createInputCentered<PJ301MPort>(mm2px(jack), module, placeJacks[i]));
		}
		addChild(createPanelRule(4.f, W - 4.f, 94.f));

		// Row 3: how many of it, and the matte switch. The two counts are whole numbers and say so under the knob.
		const float rowY = 107.f;
		const Vec repeat(rowX(W, 5, 0, P), rowY);
		addKnobLabel(this, repeat, "REPEAT");
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(repeat), module, Shapes::REPEAT_PARAM));
		addReadout(module, Vec(repeat.x, repeat.y + R_KNOB + 3.6f), Shapes::REPEAT_PARAM);
		const Vec copies(rowX(W, 5, 1, P), rowY);
		addKnobLabel(this, copies, "COPIES");
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(copies), module, Shapes::COPIES_PARAM));
		addReadout(module, Vec(copies.x, copies.y + R_KNOB + 3.6f), Shapes::COPIES_PARAM);
		const Vec ring(rowX(W, 5, 2, P), rowY);
		addKnobLabel(this, ring, "RING");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(ring), module, Shapes::RING_PARAM));
		// R_KNOB keeps the MATTE heading on the same label line as REPEAT, COPIES and RING.
		const Vec matte(rowX(W, 5, 3, P), rowY);
		addToggleLabels(this, matte, "MATTE", "WHITE", "GREY", 1, R_KNOB);
		addParam(createParamCentered<CKSS>(mm2px(matte), module, Shapes::MATTE_PARAM));

		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 5, 4, P), 118.f), "BUS", module, Shapes::VIDEO_BUS_OUTPUT);
	}
};

Model* modelShapes = createModel<Shapes, ShapesWidget>("Shapes");
