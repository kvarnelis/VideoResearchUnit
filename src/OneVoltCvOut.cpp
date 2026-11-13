#include "plugin.hpp"
#include "panel_style.hpp"

#include <algorithm>
#include <array>
#include <cmath>

struct OneVoltCvOut : Module {
	static constexpr int kLaneCount = 8;

	enum ParamId {
		RANGE_PARAM,
		CAL_PARAM,
		SAFE_PARAM,
		LANE_PARAMS,
		PARAMS_LEN = LANE_PARAMS + kLaneCount * 4
	};
	enum InputId {
		INPUTS_LEN = kLaneCount
	};
	enum OutputId {
		OUTPUTS_LEN = kLaneCount
	};
	enum LightId {
		OUTPUT_LIGHTS,
		CLIP_LIGHTS = OUTPUT_LIGHTS + kLaneCount,
		LIGHTS_LEN = CLIP_LIGHTS + kLaneCount
	};

	std::array<float, kLaneCount> smoothed {};

	OneVoltCvOut() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configSwitch(RANGE_PARAM, 0.f, 3.f, 0.f, "Output range", {"0-1 V", "+/-1 V", "0-5 V", "+/-5 V"});
		configSwitch(CAL_PARAM, 0.f, 3.f, 0.f, "Calibration output", {"Off", "0 V", "0.5 V", "1 V"});
		configSwitch(SAFE_PARAM, 0.f, 1.f, 0.f, "Safe mute", {"Run", "Mute"});

		for (int lane = 0; lane < kLaneCount; ++lane) {
			configParam(levelParam(lane), 0.f, 1.f, 0.1f, string::f("Lane %c input scale", laneName(lane)));
			configParam(offsetParam(lane), -1.f, 1.f, 0.f, string::f("Lane %c offset", laneName(lane)), " V");
			configParam(slewParam(lane), 0.f, 1.f, 0.f, string::f("Lane %c slew", laneName(lane)));
			configSwitch(invertParam(lane), 0.f, 1.f, 0.f, string::f("Lane %c invert", laneName(lane)), {"Off", "On"});
			configInput(lane, string::f("Lane %c CV", laneName(lane)));
			configOutput(lane, string::f("Lane %c scaled CV", laneName(lane)));
		}
	}

	void process(const ProcessArgs& args) override {
		const int range = static_cast<int>(clamp(std::round(params[RANGE_PARAM].getValue()), 0.f, 3.f));
		const int cal = static_cast<int>(clamp(std::round(params[CAL_PARAM].getValue()), 0.f, 3.f));
		const bool safeMute = params[SAFE_PARAM].getValue() > 0.5f;
		const RangeLimits limits = rangeLimits(range);

		for (int lane = 0; lane < kLaneCount; ++lane) {
			float target = 0.f;
			bool clipped = false;

			if (!safeMute && cal > 0) {
				target = cal == 1 ? 0.f : (cal == 2 ? 0.5f : 1.f);
				target = clamp(target, limits.min, limits.max);
			}
			else if (!safeMute) {
				float input = inputs[lane].isConnected() ? inputs[lane].getVoltage() : 0.f;
				if (params[invertParam(lane)].getValue() > 0.5f) {
					input = -input;
				}
				target = params[offsetParam(lane)].getValue() + input * params[levelParam(lane)].getValue();
				const float unclipped = target;
				target = clamp(target, limits.min, limits.max);
				clipped = std::abs(unclipped - target) > 0.0001f;
			}

			const float slew = params[slewParam(lane)].getValue();
			if (slew <= 0.0001f || safeMute || cal > 0) {
				smoothed[lane] = target;
			}
			else {
				const float cutoff = 4.f + (1.f - slew) * 180.f;
				const float amount = 1.f - std::exp(-cutoff * args.sampleTime);
				smoothed[lane] += (target - smoothed[lane]) * clamp(amount, 0.f, 1.f);
			}

			outputs[lane].setVoltage(smoothed[lane]);
			lights[OUTPUT_LIGHTS + lane].setBrightnessSmooth(lightLevel(smoothed[lane], limits), args.sampleTime);
			lights[CLIP_LIGHTS + lane].setBrightnessSmooth(clipped ? 1.f : 0.f, args.sampleTime);
		}
	}

private:
	struct RangeLimits {
		float min = 0.f;
		float max = 1.f;
	};

	static char laneName(int lane) {
		return static_cast<char>('A' + lane);
	}

	static int levelParam(int lane) {
		return LANE_PARAMS + lane * 4;
	}

	static int offsetParam(int lane) {
		return LANE_PARAMS + lane * 4 + 1;
	}

	static int slewParam(int lane) {
		return LANE_PARAMS + lane * 4 + 2;
	}

	static int invertParam(int lane) {
		return LANE_PARAMS + lane * 4 + 3;
	}

	RangeLimits rangeLimits(int range) const {
		switch (range) {
		case 1:
			return {-1.f, 1.f};
		case 2:
			return {0.f, 5.f};
		case 3:
			return {-5.f, 5.f};
		case 0:
		default:
			return {0.f, 1.f};
		}
	}

	float lightLevel(float voltage, const RangeLimits& limits) const {
		const float span = std::max(0.0001f, limits.max - limits.min);
		return clamp((voltage - limits.min) / span, 0.f, 1.f);
	}
};

struct OneVoltCvOutWidget : ModuleWidget {
	static std::string laneLetter(int lane) {
		return std::string(1, static_cast<char>('A' + lane));
	}

	OneVoltCvOutWidget(OneVoltCvOut* module) {
		using namespace rasterlab_panel;
		const float W = 96.52f;		// 19 HP
		const float LANE_X0 = 15.2f;	// centre of lane A; lanes step GRID_MM
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/OneVoltCvOut.svg")));
		addPanelHeader(this, W, "19", "1V CV OUT");

		// Global controls.
		const float gxRange = LANE_X0 + GRID_MM;
		const float gxCal = LANE_X0 + 3.f * GRID_MM;
		const float gxSafe = LANE_X0 + 5.f * GRID_MM;
		addKnobLabel(this, Vec(gxRange, 30.f), "RANGE");
		addKnobLabel(this, Vec(gxCal, 30.f), "CAL");
		addKnobLabel(this, Vec(gxSafe, 30.f), "SAFE");
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(gxRange, 30.f)), module, OneVoltCvOut::RANGE_PARAM));
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(gxCal, 30.f)), module, OneVoltCvOut::CAL_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(gxSafe, 30.f)), module, OneVoltCvOut::SAFE_PARAM));

		// Row names, left of the lanes, centred in the strip between the panel edge and lane A.
		const float ROW_NAME_X = 6.6f;
		addChild(createPanelText(Vec(ROW_NAME_X, 47.f), "LVL", LABEL_MM));
		addChild(createPanelText(Vec(ROW_NAME_X, 58.f), "OFF", LABEL_MM));
		addChild(createPanelText(Vec(ROW_NAME_X, 69.f), "SLW", LABEL_MM));
		addChild(createPanelText(Vec(ROW_NAME_X, 80.f), "INV", LABEL_MM));
		addChild(createPanelText(Vec(ROW_NAME_X, 97.f), "IN", LABEL_MM));

		// One column per lane: letter, four controls, input, output on its plate, two lamps.
		for (int lane = 0; lane < OneVoltCvOut::kLaneCount; ++lane) {
			const float laneX = LANE_X0 + lane * GRID_MM;
			addChild(createPanelText(Vec(laneX, 40.f), laneLetter(lane), LABEL_MM));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(laneX, 47.f)), module, OneVoltCvOut::LANE_PARAMS + lane * 4));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(laneX, 58.f)), module, OneVoltCvOut::LANE_PARAMS + lane * 4 + 1));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(laneX, 69.f)), module, OneVoltCvOut::LANE_PARAMS + lane * 4 + 2));
			addParam(createParamCentered<CKSS>(mm2px(Vec(laneX, 80.f)), module, OneVoltCvOut::LANE_PARAMS + lane * 4 + 3));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(laneX, 97.f)), module, lane));
			addPlatedOutput<PJ301MPort>(this, Vec(laneX, 114.f), "OUT", module, lane);
			addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(laneX - 2.6f, 124.f)), module, OneVoltCvOut::OUTPUT_LIGHTS + lane));
			addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(laneX + 2.6f, 124.f)), module, OneVoltCvOut::CLIP_LIGHTS + lane));
		}
	}
};

Model* modelOneVoltCvOut = createModel<OneVoltCvOut, OneVoltCvOutWidget>("OneVoltCvOut");
