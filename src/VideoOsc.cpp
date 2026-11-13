#include "plugin.hpp"
#include "panel_style.hpp"
#include "video_bus.hpp"
#include "video_engine.hpp"

#include <memory>

struct VideoOsc : Module {
	// New parameters and inputs are added at the end of each list. The numbers are saved in patches and presets.
	enum ParamId {
		FREQUENCY_PARAM,
		HUE_PARAM,
		FEEDBACK_PARAM,
		SHAPE_PARAM,
		PATTERN_PARAM,
		SATURATION_PARAM,
		BRIGHTNESS_PARAM,
		CONTRAST_PARAM,
		FREQ_B_PARAM,
		WAVE_B_PARAM,
		RANGE_A_PARAM,
		RANGE_B_PARAM,
		LOCK_A_PARAM,
		LOCK_B_PARAM,
		BEND_PARAM,
		MIX_PARAM,
		SPREAD_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		FREQUENCY_INPUT,
		HUE_INPUT,
		FEEDBACK_INPUT,
		SHAPE_INPUT,
		FREQ_B_INPUT,
		BEND_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		VIDEO_BUS_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		FRAME_LIGHT,
		LIGHTS_LEN
	};

	static constexpr int kBarsAndRows = static_cast<int>(VideoPattern::BarsAndRows);

	// Bars and Rows counts cycles: 0.5 x 2^(knob + 0.4 per volt), from a quarter cycle to 512. A and B use the
	// same curve, so the A knob and the B knob read the same way.
	static float pairCycles(float knob, float volts) {
		return clamp(0.5f * std::exp2(knob + 0.4f * volts), static_cast<float>(kPairMinCycles), static_cast<float>(kPairMaxCycles));
	}

	// The A frequency knob keeps its 0 to 8 reading in the other patterns. In Bars and Rows it reads in cycles,
	// the same way the B knob does, and typing a number there means cycles.
	struct FrequencyAQuantity : ParamQuantity {
		bool inCycles() {
			return module && static_cast<int>(std::round(module->params[PATTERN_PARAM].getValue())) == kBarsAndRows;
		}
		float getDisplayValue() override {
			return inCycles() ? 0.5f * std::exp2(getValue()) : ParamQuantity::getDisplayValue();
		}
		void setDisplayValue(float displayValue) override {
			if (!inCycles()) {
				ParamQuantity::setDisplayValue(displayValue);
				return;
			}
			setImmediateValue(clamp(std::log2(std::max(displayValue, 0.001f) / 0.5f), minValue, maxValue));
		}
		std::string getUnit() override {
			return inCycles() ? " cycles" : ParamQuantity::getUnit();
		}
	};

	float phase = 0.f;
	PairClock pairClock;
	VideoBusOwner videoBus;

	VideoOsc() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam<FrequencyAQuantity>(FREQUENCY_PARAM, 0.f, 8.f, 4.f, "Frequency (A)");
		configParam(HUE_PARAM, 0.f, 1.f, 0.58f, "Hue");
		configParam(FEEDBACK_PARAM, 0.f, 0.98f, 0.2f, "Feedback");
		configParam(SHAPE_PARAM, 0.f, 1.f, 0.5f, "Wave (A)");
		configSwitch(PATTERN_PARAM, 0.f, 6.f, 0.f, "Pattern", {
			"Oscillator",
			"SMPTE 75%",
			"EBU 75%",
			"EBU 100%",
			"Grayscale",
			"Grid",
			"Bars and Rows"
		});
		configParam(SATURATION_PARAM, 0.f, 2.f, 1.f, "Saturation");
		configParam(BRIGHTNESS_PARAM, 0.f, 2.f, 1.f, "Brightness");
		configParam(CONTRAST_PARAM, 0.f, 2.f, 1.f, "Contrast");
		configParam(FREQ_B_PARAM, 0.f, 8.f, 3.f, "Frequency (B)", " cycles", 2.f, 0.5f);
		configParam(WAVE_B_PARAM, 0.f, 1.f, 0.f, "Wave (B)");
		configSwitch(RANGE_A_PARAM, 0.f, 1.f, 1.f, "Range (A)", {"Rows down the picture", "Bars across the picture"});
		configSwitch(RANGE_B_PARAM, 0.f, 1.f, 0.f, "Range (B)", {"Rows down the picture", "Bars across the picture"});
		configSwitch(LOCK_A_PARAM, 0.f, 1.f, 1.f, "Lock (A)", {"Free", "Locked to the scan"});
		configSwitch(LOCK_B_PARAM, 0.f, 1.f, 1.f, "Lock (B)", {"Free", "Locked to the scan"});
		configParam(BEND_PARAM, 0.f, 4.f, 0.f, "Bend", " cycles");
		configSwitch(MIX_PARAM, 0.f, 3.f, 1.f, "Mix", {"A", "A + B", "A - B", "A x B"});
		configParam(SPREAD_PARAM, 0.f, 1.f, 0.33f, "Spread");
		configInput(FREQUENCY_INPUT, "Frequency (A) CV");
		configInput(HUE_INPUT, "Hue CV");
		configInput(FEEDBACK_INPUT, "Feedback CV");
		configInput(SHAPE_INPUT, "Wave (A) CV");
		configInput(FREQ_B_INPUT, "Frequency (B) CV");
		configInput(BEND_INPUT, "Bend CV");
		configOutput(VIDEO_BUS_OUTPUT, "Video bus token");
	}

	void process(const ProcessArgs& args) override {
		const float frequency = clamp(params[FREQUENCY_PARAM].getValue() + inputs[FREQUENCY_INPUT].getVoltage() * 0.4f, 0.05f, 24.f);
		const float hue = clamp(params[HUE_PARAM].getValue() + inputs[HUE_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		const float feedback = clamp(params[FEEDBACK_PARAM].getValue() + inputs[FEEDBACK_INPUT].getVoltage() * 0.1f, 0.f, 0.98f);
		const float shape = clamp(params[SHAPE_PARAM].getValue() + inputs[SHAPE_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		const int pattern = static_cast<int>(clamp(std::round(params[PATTERN_PARAM].getValue()), 0.f, 6.f));
		const float saturation = clamp(params[SATURATION_PARAM].getValue(), 0.f, 2.f);
		const float brightness = clamp(params[BRIGHTNESS_PARAM].getValue(), 0.f, 2.f);
		const float contrast = clamp(params[CONTRAST_PARAM].getValue(), 0.f, 2.f);

		phase += frequency * args.sampleTime * 0.1f;
		if (phase >= 1024.f) {
			phase -= 1024.f;
		}

		VideoParams out;
		out.phase = phase;
		out.frequency = frequency;
		out.hue = hue;
		out.feedback = feedback;
		out.shape = shape;
		out.saturation = saturation;
		out.brightness = brightness;
		out.contrast = contrast;
		out.pattern = pattern;

		// Bars and Rows. Every sample reads each control once. The scan clock runs in every pattern, so
		// changing to Bars and Rows picks up in step instead of starting from zero.
		out.cyclesA = pairCycles(params[FREQUENCY_PARAM].getValue(), inputs[FREQUENCY_INPUT].getVoltage());
		out.cyclesB = pairCycles(params[FREQ_B_PARAM].getValue(), inputs[FREQ_B_INPUT].getVoltage());
		out.waveB = clamp(params[WAVE_B_PARAM].getValue(), 0.f, 1.f);
		out.bend = clamp(params[BEND_PARAM].getValue() + inputs[BEND_INPUT].getVoltage() * 0.4f, -8.f, 8.f);
		out.mix = static_cast<int>(clamp(std::round(params[MIX_PARAM].getValue()), 0.f, 3.f));
		out.spread = clamp(params[SPREAD_PARAM].getValue(), 0.f, 1.f);
		out.barsA = params[RANGE_A_PARAM].getValue() > 0.5f;
		out.barsB = params[RANGE_B_PARAM].getValue() > 0.5f;
		out.lockA = params[LOCK_A_PARAM].getValue() > 0.5f;
		out.lockB = params[LOCK_B_PARAM].getValue() > 0.5f;
		pairClock.advance(args.sampleTime, out.cyclesA, out.barsA, out.cyclesB, out.barsB);
		out.latchedA = pairClock.latchedA;
		out.latchedB = pairClock.latchedB;
		out.fieldOdd = pairClock.fieldOdd;

		videoBusPublish(videoBus.id(), out);
		outputs[VIDEO_BUS_OUTPUT].setVoltage(videoBus.voltage());
		lights[FRAME_LIGHT].setBrightnessSmooth(std::sin(phase * 6.28318530718f) * 0.5f + 0.5f, args.sampleTime);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "phase", json_real(phase));
		saveVideoBusId(rootJ, videoBus);
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* phaseJ = json_object_get(rootJ, "phase");
		if (phaseJ) {
			phase = json_number_value(phaseJ);
		}

		loadVideoBusId(rootJ, videoBus, "busVoltage");
	}

	// The picture the preview draws is what the bus carries: the last set of parameters process() published.
	bool readPublished(VideoParams& out) const {
		return videoBusRead(videoBus.id(), out);
	}
};

struct VideoPreviewWidget : TransparentWidget {
	VideoOsc* module = nullptr;
	std::unique_ptr<PreviewVideoEngine> engine;
	VideoParams lastParams;
	int nvgImage = 0;
	int imageWidth = 128;
	int imageHeight = 84;

	VideoPreviewWidget() {
		engine = std::make_unique<PreviewVideoEngine>();
	}

	~VideoPreviewWidget() override = default;

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0.f, 0.f, box.size.x, box.size.y);
		nvgFillColor(args.vg, nvgRGB(6, 8, 12));
		nvgFill(args.vg);

		if (!module) {
			drawLabel(args, "NO MODULE");
			return;
		}

		VideoParams params;
		if (module->readPublished(params)) {
			lastParams = params;
		}
		const std::vector<unsigned char>& rgba = engine->render(imageWidth, imageHeight, lastParams);
		if (!rgba.empty()) {
			if (!nvgImage) {
				nvgImage = nvgCreateImageRGBA(args.vg, imageWidth, imageHeight, 0, rgba.data());
			}
			else {
				nvgUpdateImage(args.vg, nvgImage, rgba.data());
			}

			NVGpaint paint = nvgImagePattern(args.vg, 0.f, 0.f, box.size.x, box.size.y, 0.f, nvgImage, 1.f);
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, 3.f);
			nvgFillPaint(args.vg, paint);
			nvgFill(args.vg);
		}

		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.5f, 0.5f, box.size.x - 1.f, box.size.y - 1.f, 3.f);
		nvgStrokeColor(args.vg, nvgRGBA(255, 255, 255, 38));
		nvgStrokeWidth(args.vg, 1.f);
		nvgStroke(args.vg);
	}

	void onRemove(const RemoveEvent& e) override {
		if (nvgImage && APP && APP->window) {
			nvgDeleteImage(APP->window->vg, nvgImage);
			nvgImage = 0;
		}
		TransparentWidget::onRemove(e);
	}

	void drawLabel(const DrawArgs& args, const char* label) {
		// Same face and size as a control label on the panel (3.8 mm); this placeholder was 10 px, about 3.4 mm.
		rasterlab_panel::useLabelFont(args.vg);
		nvgFontSize(args.vg, mm2px(rasterlab_panel::LABEL_MM));
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, nvgRGB(196, 196, 196));
		nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.5f, label, nullptr);
	}
};

struct VideoOscWidget : ModuleWidget {
	template <class TKnob>
	void addKnob(VideoOsc* module, float x, float y, const char* label, int paramId, float radiusMm = rasterlab_panel::R_KNOB) {
		rasterlab_panel::addKnobLabel(this, Vec(x, y), label, radiusMm);
		addParam(createParamCentered<TKnob>(mm2px(Vec(x, y)), module, paramId));
	}

	void addCvJack(VideoOsc* module, float x, float y, const char* label, int inputId) {
		rasterlab_panel::addJackLabel(this, Vec(x, y), label);
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, y)), module, inputId));
	}

	// One oscillator's row: its tag, FREQ, WAVE, the RANGE switch (lever up is bars) and the LOCK switch
	// (lever up is locked to the scan).
	void addOscillatorRow(VideoOsc* module, float y, const char* tag, int freqId, int waveId, int rangeId, int lockId) {
		using namespace rasterlab_panel;
		addChild(createPanelText(Vec(7.5f, y), tag, TITLE_MM));
		addKnob<RoundBlackKnob>(module, 17.5f, y, "FREQ", freqId);
		addKnob<RoundBlackKnob>(module, 33.f, y, "WAVE", waveId);
		addToggleLabels(this, Vec(45.f, y), "RANGE", "BARS", "ROWS");
		addParam(createParamCentered<CKSS>(mm2px(Vec(45.f, y)), module, rangeId));
		addToggleLabels(this, Vec(61.f, y), "LOCK", "SCAN", "FREE");
		addParam(createParamCentered<CKSS>(mm2px(Vec(61.f, y)), module, lockId));
	}

	VideoOscWidget(VideoOsc* module) {
		using namespace rasterlab_panel;
		const float W = 101.6f;	// 20 HP
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/VideoOsc.svg")));
		addPanelHeader(this, W, "05", "VIDEO OSCILLATOR");

		// Preview screen, same 3:2 frame as the 128x84 preview image.
		auto* preview = new VideoPreviewWidget();
		preview->module = module;
		preview->box.pos = mm2px(Vec(4.f, 19.f));
		preview->box.size = mm2px(Vec(54.f, 36.f));
		addChild(preview);

		// To the right of the screen: the picture's own controls.
		addKnob<RoundBlackSnapKnob>(module, 68.f, 30.5f, "PATTERN", VideoOsc::PATTERN_PARAM);
		addKnob<RoundBlackKnob>(module, 81.f, 30.5f, "HUE", VideoOsc::HUE_PARAM);
		addKnob<RoundBlackKnob>(module, 93.f, 30.5f, "SPREAD", VideoOsc::SPREAD_PARAM);
		addKnob<RoundSmallBlackKnob>(module, 68.f, 47.f, "SAT", VideoOsc::SATURATION_PARAM, R_SMALL);
		addKnob<RoundSmallBlackKnob>(module, 81.f, 47.f, "BRI", VideoOsc::BRIGHTNESS_PARAM, R_SMALL);
		addKnob<RoundSmallBlackKnob>(module, 93.f, 47.f, "CON", VideoOsc::CONTRAST_PARAM, R_SMALL);

		// Below the screen: oscillator A, then oscillator B, then what they do to each other.
		addOscillatorRow(module, 66.f, "A", VideoOsc::FREQUENCY_PARAM, VideoOsc::SHAPE_PARAM, VideoOsc::RANGE_A_PARAM, VideoOsc::LOCK_A_PARAM);
		addOscillatorRow(module, 84.f, "B", VideoOsc::FREQ_B_PARAM, VideoOsc::WAVE_B_PARAM, VideoOsc::RANGE_B_PARAM, VideoOsc::LOCK_B_PARAM);
		addKnob<RoundBlackKnob>(module, 79.f, 66.f, "BEND", VideoOsc::BEND_PARAM);
		addKnob<RoundBlackSnapKnob>(module, 90.f, 66.f, "MIX", VideoOsc::MIX_PARAM);
		addKnob<RoundBlackKnob>(module, 84.5f, 84.f, "FB", VideoOsc::FEEDBACK_PARAM);

		// The six CV inputs in one row, 15.2 mm apart; FRAME and BUS sit under the last two.
		const float jackX[6] = {10.5f, 25.7f, 40.9f, 56.1f, 71.3f, 86.5f};
		addCvJack(module, jackX[0], 100.f, "FREQ A", VideoOsc::FREQUENCY_INPUT);
		addCvJack(module, jackX[1], 100.f, "WAVE A", VideoOsc::SHAPE_INPUT);
		addCvJack(module, jackX[2], 100.f, "FREQ B", VideoOsc::FREQ_B_INPUT);
		addCvJack(module, jackX[3], 100.f, "BEND", VideoOsc::BEND_INPUT);
		addCvJack(module, jackX[4], 100.f, "HUE", VideoOsc::HUE_INPUT);
		addCvJack(module, jackX[5], 100.f, "FB", VideoOsc::FEEDBACK_INPUT);

		addJackLabel(this, Vec(jackX[4], 116.f), "FRAME");
		addChild(createLightCentered<MediumLight<BlueLight>>(mm2px(Vec(jackX[4], 116.f)), module, VideoOsc::FRAME_LIGHT));
		addPlatedOutput<PJ301MPort>(this, Vec(jackX[5], 116.f), "BUS", module, VideoOsc::VIDEO_BUS_OUTPUT);
	}
};

Model* modelVideoOsc = createModel<VideoOsc, VideoOscWidget>("VideoOsc");
