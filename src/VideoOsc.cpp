#include "plugin.hpp"
#include "panel_style.hpp"
#include "video_bus.hpp"
#include "video_engine.hpp"
#include "video_osc_audio.hpp"
#include "video_osc_layout.hpp"

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
		FM_AMOUNT_PARAM,
		FREQ_C_PARAM,
		WAVE_C_PARAM,
		RANGE_C_PARAM,
		LOCK_C_PARAM,
		LEVEL_C_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		FREQUENCY_INPUT,
		HUE_INPUT,
		FEEDBACK_INPUT,
		SHAPE_INPUT,
		FREQ_B_INPUT,
		BEND_INPUT,
		LOCK_A_INPUT,
		LOCK_B_INPUT,
		IN_A_INPUT,
		IN_B_INPUT,
		FM_INPUT,
		VOCT_A_INPUT,
		VOCT_B_INPUT,
		FREQ_C_INPUT,
		LOCK_C_INPUT,
		IN_C_INPUT,
		VOCT_C_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		VIDEO_BUS_OUTPUT,
		AUDIO_A_OUTPUT,
		AUDIO_B_OUTPUT,
		AUDIO_C_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		FRAME_LIGHT,
		LIGHTS_LEN
	};

	static constexpr int kBarsAndRows = static_cast<int>(VideoPattern::BarsAndRows);

	// Bars and Rows counts cycles: 0.5 x 2^(knob + 0.4 per volt), from a quarter cycle to 512. A and B use the
	// same curve, so the A knob and the B knob read the same way.
	// V/OCT adds one octave a volt on top (zero when unpatched, which leaves the sum exactly as it was).
	static float pairCycles(float knob, float volts, float voct = 0.f) {
		return clamp(0.5f * std::exp2(knob + 0.4f * volts + voct), static_cast<float>(kPairMinCycles), static_cast<float>(kPairMaxCycles));
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
	oscaudio::Edges edgesA;
	oscaudio::Edges edgesB;
	oscaudio::Edges edgesC;
	VideoBusOwner videoBus;
	int signalSlotId = 0;

	VideoOsc() {
		signalSlotId = signalRingAllocate();
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam<FrequencyAQuantity>(FREQUENCY_PARAM, 0.f, 8.f, 4.f, "Frequency (A)");
		configParam(HUE_PARAM, 0.f, 1.f, 0.58f, "Hue");
		configParam(FEEDBACK_PARAM, 0.f, 0.98f, 0.2f, "Feedback");
		configParam(SHAPE_PARAM, 0.f, 1.f, 0.5f, "Wave (A)");
		configSwitch(PATTERN_PARAM, 0.f, 7.f, 0.f, "Pattern", {
			"Oscillator",
			"SMPTE 75%",
			"EBU 75%",
			"EBU 100%",
			"Grayscale",
			"Grid",
			"Bars and Rows",
			"Snow"
		});
		configParam(SATURATION_PARAM, 0.f, 2.f, 1.f, "Saturation");
		configParam(BRIGHTNESS_PARAM, 0.f, 2.f, 1.f, "Brightness");
		configParam(CONTRAST_PARAM, 0.f, 2.f, 1.f, "Contrast");
		configParam(FREQ_B_PARAM, 0.f, 8.f, 3.f, "Frequency (B)", " cycles", 2.f, 0.5f);
		configParam(WAVE_B_PARAM, 0.f, 1.f, 0.f, "Wave (B)");
		configSwitch(RANGE_A_PARAM, 0.f, 1.f, 1.f, "Range (A)", {"Rows down the picture", "Bars across the picture"});
		configSwitch(RANGE_B_PARAM, 0.f, 1.f, 0.f, "Range (B)", {"Rows down the picture", "Bars across the picture"});
		// LOCK was a two-position switch (0 free, 1 locked to the scan). The ids are the same, so saved patches
		// load: 0 stays fully free and 1 stays fully locked.
		configParam(LOCK_A_PARAM, 0.f, 1.f, 1.f, "Lock (A)");
		configParam(LOCK_B_PARAM, 0.f, 1.f, 1.f, "Lock (B)");
		configParam(BEND_PARAM, 0.f, 4.f, 0.f, "Bend", " cycles");
		configSwitch(MIX_PARAM, 0.f, 3.f, 1.f, "Mix", {"A", "A + B", "A - B", "A x B"});
		configParam(SPREAD_PARAM, 0.f, 1.f, 0.33f, "Spread");
		configParam(FM_AMOUNT_PARAM, -1.f, 1.f, 0.f, "FM amount");
		// Oscillator C, added in 2.4.0: the same four controls as A and B, and a LEVEL that is 0 on a new module, so
		// C is not in the picture until it is turned up.
		configParam(FREQ_C_PARAM, 0.f, 8.f, 3.f, "Frequency (C)", " cycles", 2.f, 0.5f);
		configParam(WAVE_C_PARAM, 0.f, 1.f, 0.f, "Wave (C)");
		configSwitch(RANGE_C_PARAM, 0.f, 1.f, 0.f, "Range (C)", {"Rows down the picture", "Bars across the picture"});
		configParam(LOCK_C_PARAM, 0.f, 1.f, 1.f, "Lock (C)");
		configParam(LEVEL_C_PARAM, 0.f, 1.f, 0.f, "Level (C)");
		configInput(FREQUENCY_INPUT, "Frequency (A) CV");
		configInput(HUE_INPUT, "Hue CV");
		configInput(FEEDBACK_INPUT, "Feedback CV");
		configInput(SHAPE_INPUT, "Wave (A) CV");
		configInput(FREQ_B_INPUT, "Frequency (B) CV");
		configInput(BEND_INPUT, "Bend CV");
		configInput(LOCK_A_INPUT, "Lock (A) CV");
		configInput(LOCK_B_INPUT, "Lock (B) CV");
		configInput(IN_A_INPUT, "In A");
		configInput(IN_B_INPUT, "In B");
		configInput(FM_INPUT, "FM In (video bus)");
		configInput(VOCT_A_INPUT, "V/OCT (A)");
		configInput(VOCT_B_INPUT, "V/OCT (B)");
		configInput(FREQ_C_INPUT, "Frequency (C) CV");
		configInput(LOCK_C_INPUT, "Lock (C) CV");
		configInput(IN_C_INPUT, "In C");
		configInput(VOCT_C_INPUT, "V/OCT (C)");
		configOutput(VIDEO_BUS_OUTPUT, "Video bus token");
		configOutput(AUDIO_A_OUTPUT, "Sound (A)");
		configOutput(AUDIO_B_OUTPUT, "Sound (B)");
		configOutput(AUDIO_C_OUTPUT, "Sound (C)");

		paramQuantities[LOCK_A_PARAM]->description = "How firmly oscillator A is held to the television scan. All the way up, its pattern stands still at any frequency. All the way down, it runs free: bars lean and travel, rows roll. In between, each line pulls it part of the way back into step: near a whole number of cycles the pattern catches and stands, further off it tears into slanted bands and slips.";
		paramQuantities[LOCK_B_PARAM]->description = "How firmly oscillator B is held to the television scan. All the way up, its pattern stands still at any frequency. All the way down, it runs free: bars lean and travel, rows roll. In between, each line pulls it part of the way back into step.";
		inputInfos[LOCK_A_INPUT]->description = "10 V is the knob's whole range, added to the knob.";
		inputInfos[LOCK_B_INPUT]->description = "10 V is the knob's whole range, added to the knob.";
		inputInfos[IN_A_INPUT]->description = "Audio or CV that pushes the bars of oscillator A sideways, read at each point of the scan, not once a frame. 5 V moves them one cycle of A. A voice or a tone draws its own waveform down the bars.";
		inputInfos[IN_B_INPUT]->description = "Audio or CV that pushes the bars of oscillator B sideways, read at each point of the scan, not once a frame. 5 V moves them one cycle of B.";
		inputInfos[FM_INPUT]->description = "A second picture that bends oscillator A. In Bars and Rows its brightness, at each pixel, speeds A up (bright) or slows it down (dark), so A's bars bunch together where the picture is bright and spread apart where it is dark. Unpatched, or with FM AMT at zero, nothing changes. In the Oscillator pattern it shifts the carrier's phase. The test cards ignore it.";
		paramQuantities[FM_AMOUNT_PARAM]->description = "How far the FM IN picture bends oscillator A. 0 is none. Positive: bright parts of the picture bunch the bars. Negative: dark parts do. At full, A's rate runs from a quarter (black) to one and three quarters (white) of its own.";
		outputInfos[AUDIO_A_OUTPUT]->description = soundDescription("A");
		outputInfos[AUDIO_B_OUTPUT]->description = soundDescription("B");
		outputInfos[AUDIO_C_OUTPUT]->description = soundDescription("C");
		paramQuantities[LEVEL_C_PARAM]->description = "How much of oscillator C joins the picture, in Bars and Rows. 0 leaves the picture exactly as A and B draw it. Turned up, C is combined the way MIX combines A and B: on A it fades from A to C, on A + B it averages all three, on A - B it takes C away as well, on A x B it multiplies C in. SPREAD puts C's colour two steps round the colour wheel from A's. C's sound output is always on, whatever this knob says.";
		paramQuantities[LOCK_C_PARAM]->description = "How firmly oscillator C is held to the television scan, as LOCK on A and B. All the way up, its pattern stands still; all the way down it runs free.";
		inputInfos[FREQ_C_INPUT]->description = "0.4 octave per volt on oscillator C's cycles, added to the knob, as the FREQ jacks of A and B.";
		inputInfos[LOCK_C_INPUT]->description = "10 V is the knob's whole range, added to the knob.";
		inputInfos[IN_C_INPUT]->description = "Audio or CV that pushes the bars of oscillator C sideways, read at each point of the scan. 5 V moves them one cycle of C.";
		inputInfos[VOCT_C_INPUT]->description = "One volt per octave on oscillator C's frequency: +1 V doubles the number of cycles, -1 V halves it. Unpatched, nothing changes.";
		inputInfos[VOCT_A_INPUT]->description = "One volt per octave, added to oscillator A's frequency on top of the knob and the CV jack: +1 V doubles the number of cycles, -1 V halves it. Unpatched, nothing changes. In the Oscillator pattern it doubles the frequency per volt too.";
		inputInfos[VOCT_B_INPUT]->description = "One volt per octave, added to oscillator B's frequency on top of the knob and the CV jack: +1 V doubles the number of cycles, -1 V halves it. Unpatched, nothing changes.";
		inputInfos[BEND_INPUT]->description = "0.4 cycles per volt, added to the Bend knob. It is read at each point of the scan, so an audio signal here bends the bars along the picture.";
	}

	// The words on the sound jacks' tooltips: pitch, the BARS and ROWS rates, and what happens out of earshot.
	static std::string soundDescription(const char* name) {
		return std::string("Oscillator ") + name + " as sound, at its own frequency: the wave on its WAVE knob, 10 V peak to peak, "
			"with the edges of a square or ramp band-limited. On ROWS the pitch is the number of cycles times 59.94 Hz, so 100 cycles is about 6 kHz. "
			"On BARS it is the number of cycles times the line rate, 15 734 Hz, so only the lowest settings are heard (a quarter cycle is 3.9 kHz). "
			"Above half the sample rate the output is silent, not folded down to a lower note: it fades out between 0.4 and 0.5 of the sample rate. "
			"LOCK, BEND and the scan-time inputs shape the picture only; the sound is the free-running wave.";
	}

	~VideoOsc() override {
		signalRingRelease(signalSlotId);
	}

	void process(const ProcessArgs& args) override {
		const float voctA = inputs[VOCT_A_INPUT].isConnected() ? clamp(inputs[VOCT_A_INPUT].getVoltage(), -10.f, 10.f) : 0.f;
		const float voctC = inputs[VOCT_C_INPUT].isConnected() ? clamp(inputs[VOCT_C_INPUT].getVoltage(), -10.f, 10.f) : 0.f;
		const float voctB = inputs[VOCT_B_INPUT].isConnected() ? clamp(inputs[VOCT_B_INPUT].getVoltage(), -10.f, 10.f) : 0.f;
		float frequency = clamp(params[FREQUENCY_PARAM].getValue() + inputs[FREQUENCY_INPUT].getVoltage() * 0.4f, 0.05f, 24.f);
		if (voctA != 0.f) {
			frequency = clamp(frequency * std::exp2(voctA), 0.05f, 24.f);
		}
		const float hue = clamp(params[HUE_PARAM].getValue() + inputs[HUE_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		const float feedback = clamp(params[FEEDBACK_PARAM].getValue() + inputs[FEEDBACK_INPUT].getVoltage() * 0.1f, 0.f, 0.98f);
		const float shape = clamp(params[SHAPE_PARAM].getValue() + inputs[SHAPE_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		const int pattern = static_cast<int>(clamp(std::round(params[PATTERN_PARAM].getValue()), 0.f, 7.f));
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
		out.cyclesA = pairCycles(params[FREQUENCY_PARAM].getValue(), inputs[FREQUENCY_INPUT].getVoltage(), voctA);
		out.cyclesB = pairCycles(params[FREQ_B_PARAM].getValue(), inputs[FREQ_B_INPUT].getVoltage(), voctB);
		out.waveB = clamp(params[WAVE_B_PARAM].getValue(), 0.f, 1.f);
		out.cyclesC = pairCycles(params[FREQ_C_PARAM].getValue(), inputs[FREQ_C_INPUT].getVoltage(), voctC);
		out.waveC = clamp(params[WAVE_C_PARAM].getValue(), 0.f, 1.f);
		out.barsC = params[RANGE_C_PARAM].getValue() > 0.5f;
		out.lockC = clamp(params[LOCK_C_PARAM].getValue() + inputs[LOCK_C_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		out.levelC = clamp(params[LEVEL_C_PARAM].getValue(), 0.f, 1.f);
		// BEND is the knob alone here; the BEND CV travels in the signal ring and is added at each pixel's scan time.
		out.bend = clamp(params[BEND_PARAM].getValue(), -8.f, 8.f);
		out.mix = static_cast<int>(clamp(std::round(params[MIX_PARAM].getValue()), 0.f, 3.f));
		out.spread = clamp(params[SPREAD_PARAM].getValue(), 0.f, 1.f);
		out.barsA = params[RANGE_A_PARAM].getValue() > 0.5f;
		out.barsB = params[RANGE_B_PARAM].getValue() > 0.5f;
		out.lockA = clamp(params[LOCK_A_PARAM].getValue() + inputs[LOCK_A_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
		out.lockB = clamp(params[LOCK_B_PARAM].getValue() + inputs[LOCK_B_INPUT].getVoltage() * 0.1f, 0.f, 1.f);

		// Scan-time inputs: pushed into the ring once per sample (no allocation, no lock). With none of the
		// three jacks patched the renderer ignores the ring and draws as it did before they existed.
		signalRingSetSampleRate(signalSlotId, args.sampleRate);
		float samples[kSignalRingChannels] = {};
		samples[kVideoOscInA] = clamp(inputs[IN_A_INPUT].getVoltage() * 0.2f, -8.f, 8.f);
		samples[kVideoOscInB] = clamp(inputs[IN_B_INPUT].getVoltage() * 0.2f, -8.f, 8.f);
		samples[kVideoOscInC] = clamp(inputs[IN_C_INPUT].getVoltage() * 0.2f, -8.f, 8.f);
		samples[kVideoOscBend] = clamp(inputs[BEND_INPUT].getVoltage() * 0.4f, -8.f, 8.f);
		signalRingPushAt(signalSlotId, samples, args.frame);
		out.signalSlotId = signalSlotId;
		out.scanInputs = inputs[IN_A_INPUT].isConnected() || inputs[IN_B_INPUT].isConnected() || inputs[IN_C_INPUT].isConnected() || inputs[BEND_INPUT].isConnected();
		pairClock.advanceAt(args.frame, args.sampleRate, out.cyclesA, out.barsA, out.cyclesB, out.barsB, out.cyclesC, out.barsC);
		out.latchedA = pairClock.latchedA;
		out.latchedB = pairClock.latchedB;
		out.latchedC = pairClock.latchedC;
		out.fieldOdd = pairClock.fieldOdd;
		out.snowTick = pairClock.fieldCount;

		// Sound outputs: the free-running phase the scan clock already keeps for each oscillator, at the audio sample.
		// Only worked out when a cable is in the jack.
		if (outputs[AUDIO_A_OUTPUT].isConnected()) {
			const double step = PairClock::cyclesPerLine(out.cyclesA, out.barsA) * vclock::kLineRateHz * args.sampleTime;
			outputs[AUDIO_A_OUTPUT].setVoltage(oscaudio::sample(edgesA, pairClock.phaseA, step, shape));
		}
		if (outputs[AUDIO_B_OUTPUT].isConnected()) {
			const double step = PairClock::cyclesPerLine(out.cyclesB, out.barsB) * vclock::kLineRateHz * args.sampleTime;
			outputs[AUDIO_B_OUTPUT].setVoltage(oscaudio::sample(edgesB, pairClock.phaseB, step, out.waveB));
		}

		if (outputs[AUDIO_C_OUTPUT].isConnected()) {
			const double step = PairClock::cyclesPerLine(out.cyclesC, out.barsC) * vclock::kLineRateHz * args.sampleTime;
			outputs[AUDIO_C_OUTPUT].setVoltage(oscaudio::sample(edgesC, pairClock.phaseC, step, out.waveC));
		}

		// FM IN: another picture bends oscillator A. Unpatched, a bus with no picture, or this module's own bus: none.
		if (inputs[FM_INPUT].isConnected()) {
			const int fmBus = videoBusIdFromVoltage(inputs[FM_INPUT].getVoltage());
			if (fmBus != videoBus.id() && videoBusIsActive(fmBus)) {
				out.fmBusId = fmBus;
				out.fmAmount = clamp(params[FM_AMOUNT_PARAM].getValue(), -1.f, 1.f);
			}
		}

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

	// The items of src/video_osc_layout.hpp, one function per kind.
	void item_KNOB(VideoOsc* module, float x, float y, const char* label, int id, float) {
		addKnobOf<RoundBlackKnob>(module, x, y, label, id, rasterlab_panel::R_KNOB);
	}
	void item_SNAP(VideoOsc* module, float x, float y, const char* label, int id, float) {
		addKnobOf<RoundBlackSnapKnob>(module, x, y, label, id, rasterlab_panel::R_KNOB);
	}
	void item_SMALL(VideoOsc* module, float x, float y, const char* label, int id, float) {
		addKnobOf<RoundSmallBlackKnob>(module, x, y, label, id, rasterlab_panel::R_SMALL);
	}
	template <class TKnob>
	void addKnobOf(VideoOsc* module, float x, float y, const char* label, int id, float radiusMm) {
		if (label[0]) {
			rasterlab_panel::addKnobLabel(this, Vec(x, y), label, radiusMm);
		}
		addParam(createParamCentered<TKnob>(mm2px(Vec(x, y)), module, id));
	}
	// The RANGE switch: lever up is bars. BARS sits above it and ROWS below, centred, so neither word reaches the
	// knob beside it; the name (given on the top row only) is above BARS.
	void item_SWITCH(VideoOsc* module, float x, float y, const char* label, int id, float) {
		using namespace rasterlab_panel;
		if (label[0]) {
			addChild(createPanelText(Vec(x, y - 8.4f), label, LABEL_MM));
		}
		addChild(createPanelText(Vec(x, y - 5.4f), "BARS", NOTE_MM));
		addChild(createPanelText(Vec(x, y + 5.4f), "ROWS", NOTE_MM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(x, y)), module, id));
	}
	void item_JACK(VideoOsc* module, float x, float y, const char* label, int id, float) {
		addCvJack(module, x, y, label, id);
	}
	void item_CELL(VideoOsc* module, float x, float y, const char*, int id, float) {
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, y)), module, id));
	}
	void item_OUT(VideoOsc* module, float x, float y, const char* label, int id, float plateWidthMm) {
		rasterlab_panel::addPlatedOutput<PJ301MPort>(this, Vec(x, y), label, module, id, plateWidthMm);
	}
	void item_LIGHT(VideoOsc* module, float x, float y, const char* label, int id, float) {
		rasterlab_panel::addJackLabel(this, Vec(x, y), label);
		addChild(createLightCentered<MediumLight<BlueLight>>(mm2px(Vec(x, y)), module, id));
	}
	void item_TEXT(VideoOsc*, float x, float y, const char* label, int, float sizeMm) {
		addChild(rasterlab_panel::createPanelText(Vec(x, y), label, sizeMm));
	}
	void item_LEFT(VideoOsc*, float x, float y, const char* label, int, float sizeMm) {
		addChild(rasterlab_panel::createPanelTextAt(Vec(x, y), label, sizeMm, NVG_ALIGN_LEFT));
	}
	void item_RULE(VideoOsc*, float x, float y, const char*, int, float x2) {
		addChild(rasterlab_panel::createPanelRule(x, x2, y));
	}

	VideoOscWidget(VideoOsc* module) {
		using namespace rasterlab_panel;
		const float W = kVideoOscPanelWidth;
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/VideoOsc.svg")));
		addPanelHeader(this, W, "05", "VIDEO OSCILLATOR");

		// Preview screen, same 3:2 frame as the 128x84 preview image.
		auto* preview = new VideoPreviewWidget();
		preview->module = module;
		preview->box.pos = mm2px(Vec(4.f, 19.f));
		preview->box.size = mm2px(Vec(54.f, 36.f));
		addChild(preview);

#define VIDEO_OSC_PLACE(kind, x, y, label, id, extra) item_##kind(module, x, y, label, id, extra);
		VIDEO_OSC_LAYOUT(VIDEO_OSC_PLACE)
#undef VIDEO_OSC_PLACE
	}
};

Model* modelVideoOsc = createModel<VideoOsc, VideoOscWidget>("VideoOsc");
