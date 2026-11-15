#include "plugin.hpp"
#include "panel_style.hpp"
#include "video_bus.hpp"
#include "video_mixer_math.hpp"

#include <cmath>

// TIME is stored 0..1 and shown as the seconds a whole take lasts, 0.02 s to 20 s.
struct VideoMixerTimeQuantity : ParamQuantity {
	float getDisplayValue() override { return videoMixerTakeSeconds(getValue()); }
	void setDisplayValue(float seconds) override {
		setValue(std::log(clamp(seconds, 0.02f, 20.f) / 0.02f) / std::log(1000.f));
	}
};

// VRU-22 Video Mixer: four pictures, one live and one next, a fader (or a cut, or a timed take) from one to the
// other, a key layer over the result, and a fade to black. The control logic is in video_mixer_math.hpp, the picture
// maths in the renderer (VideoGraphRenderer::renderVideoMixerFrame).
struct VideoMixer : Module {
	enum ParamId {
		HUE_PARAM,                    // A to D
		COLOR_PARAM = HUE_PARAM + 4,  // A to D
		SAT_PARAM = COLOR_PARAM + 4,
		NEXT_PARAM,
		FADER_PARAM,
		CUT_PARAM,
		AUTO_PARAM,
		ADVANCE_PARAM,
		TIME_PARAM,
		MODE_PARAM,
		SOFT_PARAM,
		KEY_ON_PARAM,
		KEY_THRESH_PARAM,
		KEY_SOFT_PARAM,
		KEY_INVERT_PARAM,
		KEY_HUE_PARAM,
		FILL_PARAM,
		BLACK_PARAM,
		SPAN_PARAM,
		LEVEL_PARAM,                  // A to D, appended so older patches load unchanged
		ALL_PARAM = LEVEL_PARAM + 4,  // the ALL button: every patched picture live at its LEVEL
		PARAMS_LEN
	};
	enum InputId {
		PICTURE_INPUT,                      // A to D
		PATTERN_INPUT = PICTURE_INPUT + 4,
		KEY_INPUT,
		MATTE_INPUT,
		FADER_INPUT,
		CUT_INPUT,
		AUTO_INPUT,
		NEXT_INPUT,
		NEXT_CV_INPUT,
		SAT_INPUT,
		SHIFT_INPUT,
		BLACK_INPUT,
		THRESH_INPUT,
		KEY_ON_INPUT,
		SPAN_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		VIDEO_BUS_OUTPUT,
		LIVE_OUTPUT,  // A to D, a gate while that input is live
		OUTPUTS_LEN = LIVE_OUTPUT + 4
	};
	enum LightId {
		PICTURE_LIGHT,                        // A to D, two lights each: the first is green (next), the second red (live)
		COLOR_LIGHT = PICTURE_LIGHT + 8,      // A to D
		CUT_LIGHT = COLOR_LIGHT + 4,
		AUTO_LIGHT,
		NEXT_LIGHT,
		ADVANCE_LIGHT,
		KEY_ON_LIGHT,
		KEY_INVERT_LIGHT,
		SIGNAL_LIGHT,
		COLOR_STATE_LIGHT,  // A to D, three lights each (red, green, blue): off, hue (white), gradient (amber)
		ALL_LIGHT = COLOR_STATE_LIGHT + 12,  // the ALL button
		LIGHTS_LEN
	};

	VideoBusOwner videoBus;
	VideoMixerState state;
	dsp::SchmittTrigger cutTrigger, autoTrigger, nextTrigger;
	int colorStates[kVideoMixerInputs] = {0, 0, 0, 0};
	dsp::BooleanTrigger cutButton, autoButton, nextButton;
	bool allAverages = false;  // ALL mode divides by the number of patched pictures instead of adding them

	VideoMixer() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		static const char* names[] = {"A", "B", "C", "D"};
		// The four hues start a quarter turn apart, so a first patch with COLOR on shows four different colors.
		static const float hues[] = {0.f, 0.25f, 0.5f, 0.75f};
		for (int i = 0; i < kVideoMixerInputs; ++i) {
			const std::string name = std::string("Video in ") + names[i];
			configParam(HUE_PARAM + i, 0.f, 1.f, hues[i], name + " hue", "°", 0.f, 360.f);
			configSwitch(COLOR_PARAM + i, 0.f, 2.f, 0.f, name + " color: click to step Off, Hue (one color), Gradient (several colors)", {"Off", "Hue", "Gradient"});
			configInput(PICTURE_INPUT + i, name + " (video bus token)");
			configOutput(LIVE_OUTPUT + i, name + " live")->description = "10 V while this input is the picture going out";
			// One two-color light shows both states, and Rack reads its hover text from the first of the pair, so both
			// carry the same words.
			for (int c = 0; c < 2; ++c) {
				LightInfo* info = configLight(PICTURE_LIGHT + 2 * i + c, name + " status");
				info->description = "Red means live. Green means next.";
			}
			for (int c = 0; c < 3; ++c) configLight(COLOR_STATE_LIGHT + 3 * i + c, name + " color: off, white for Hue, amber for Gradient");
			configParam(LEVEL_PARAM + i, 0.f, 1.f, 1.f, name + " level. Level while ALL is on", "%", 0.f, 100.f);
		}
		configSwitch(ALL_PARAM, 0.f, 1.f, 0.f, "All: every patched picture is live at its LEVEL. Mode, fader, cut, auto and next do nothing while this is on.", {"Off", "On"});
		configParam(SAT_PARAM, 0.f, 1.f, 1.f, "Saturation of every colored picture", "%", 0.f, 100.f);
		configButton(NEXT_PARAM, "Next: step to the following picture");
		configParam(FADER_PARAM, 0.f, 1.f, 0.f, "Fader", "%", 0.f, 100.f);
		configButton(CUT_PARAM, "Cut: next goes live now");
		configButton(AUTO_PARAM, "Auto: next goes live over TIME");
		configSwitch(ADVANCE_PARAM, 0.f, 1.f, 0.f, "Advance: the following picture is next after each transition", {"Off", "On"});
		configParam<VideoMixerTimeQuantity>(TIME_PARAM, 0.f, 1.f, 0.57f, "Take time", " s");
		configSwitch(MODE_PARAM, 0.f, 2.f, 0.f, "Transition", {"Dissolve", "Add", "Wipe"});
		configParam(SOFT_PARAM, 0.f, 1.f, 0.15f, "Wipe edge softness", "%", 0.f, 100.f);
		configSwitch(KEY_ON_PARAM, 0.f, 1.f, 0.f, "Key layer", {"Off", "On"});
		configParam(KEY_THRESH_PARAM, 0.f, 1.f, 0.5f, "Key threshold", "%", 0.f, 100.f);
		configParam(KEY_SOFT_PARAM, 0.f, 1.f, 0.12f, "Key edge softness", "%", 0.f, 100.f);
		configSwitch(KEY_INVERT_PARAM, 0.f, 1.f, 0.f, "Key invert (cut where the matte is dark)", {"Off", "On"});
		configParam(SPAN_PARAM, 0.f, 1.f, 0.f, "Gradient span: how far round the colors each Gradient input steps from its own hue", "°", 0.f, 360.f * kVideoMixerSpanTurns);
		configParam(KEY_HUE_PARAM, 0.f, 1.f, 0.33f, "Key flat color hue", "°", 0.f, 360.f);
		configSwitch(FILL_PARAM, 0.f, 1.f, 1.f, "Key fill", {"Flat color", "Key picture"});
		configParam(BLACK_PARAM, 0.f, 1.f, 0.f, "Fade to black", "%", 0.f, 100.f);

		configInput(PATTERN_INPUT, "Pattern picture for wipes (video bus token)");
		configInput(KEY_INPUT, "Key picture (video bus token)");
		configInput(MATTE_INPUT, "Matte picture (video bus token)");
		configInput(FADER_INPUT, "Fader CV (10 V is full travel)");
		configInput(CUT_INPUT, "Cut trigger");
		configInput(AUTO_INPUT, "Auto take trigger");
		configInput(NEXT_INPUT, "Next trigger");
		configInput(NEXT_CV_INPUT, "Next CV (0 to 10 V picks A to D in four equal bands)");
		configInput(SAT_INPUT, "Saturation CV (10 V is full range)");
		configInput(SHIFT_INPUT, "Hue shift CV (10 V is once round the colors)");
		configInput(SPAN_INPUT, "Gradient span CV (10 V is full range)");
		configInput(BLACK_INPUT, "Fade to black CV (10 V is full range)");
		configInput(THRESH_INPUT, "Key threshold CV (10 V is full range)");
		configInput(KEY_ON_INPUT, "Key on gate");
		configOutput(VIDEO_BUS_OUTPUT, "Video bus token");
		configLight(CUT_LIGHT, "Cut");
		configLight(AUTO_LIGHT, "Auto take running");
		configLight(NEXT_LIGHT, "Next");
		configLight(ADVANCE_LIGHT, "Advance on");
		configLight(KEY_ON_LIGHT, "Key layer on");
		configLight(KEY_INVERT_LIGHT, "Key invert on");
		configLight(SIGNAL_LIGHT, "Signal");
		configLight(ALL_LIGHT, "All on");
	}

	void onReset() override { state.reset(); }

	void process(const ProcessArgs& args) override {
		auto cv = [&](int param, int input, float lo, float hi) {
			return clamp(params[param].getValue() + inputs[input].getVoltage() * 0.1f, lo, hi);
		};
		const int outBus = videoBus.id();
		// A picture input is a bus when it is patched, reads a valid token, is not this mixer's own bus and has a
		// source behind it. Anything else is "no picture": nothing patched is absent, never a default bus.
		auto busOf = [&](int input) {
			if (!inputs[input].isConnected()) return 0;
			const int bus = videoBusIdFromVoltage(inputs[input].getVoltage());
			return bus != outBus && videoBusIsActive(bus) ? bus : 0;
		};

		VideoMixerParams p;
		unsigned patched = 0u;
		for (int i = 0; i < kVideoMixerInputs; ++i) {
			p.inputBus[i] = busOf(PICTURE_INPUT + i);
			if (p.inputBus[i] > 0) patched |= 1u << i;
		}
		p.patternBus = busOf(PATTERN_INPUT);
		p.keyBus = busOf(KEY_INPUT);
		p.matteBus = busOf(MATTE_INPUT);

		// A jack trigger and a button press both count. Each detector runs every sample, so neither misses an edge.
		const bool cutJack = cutTrigger.process(inputs[CUT_INPUT].getVoltage(), 0.1f, 2.f);
		const bool cutPress = cutButton.process(params[CUT_PARAM].getValue() > 0.5f);
		const bool takeJack = autoTrigger.process(inputs[AUTO_INPUT].getVoltage(), 0.1f, 2.f);
		const bool takePress = autoButton.process(params[AUTO_PARAM].getValue() > 0.5f);
		const bool stepJack = nextTrigger.process(inputs[NEXT_INPUT].getVoltage(), 0.1f, 2.f);
		const bool stepPress = nextButton.process(params[NEXT_PARAM].getValue() > 0.5f);
		const bool cut = cutJack || cutPress;
		const bool take = takeJack || takePress;
		const bool step = stepJack || stepPress;

		VideoMixerState::Inputs in;
		in.lever = cv(FADER_PARAM, FADER_INPUT, 0.f, 1.f);
		in.cut = cut;
		in.take = take;
		in.step = step;
		in.select = inputs[NEXT_CV_INPUT].isConnected() ? videoMixerInputFromVolts(inputs[NEXT_CV_INPUT].getVoltage()) : -1;
		in.advance = params[ADVANCE_PARAM].getValue() > 0.5f;
		in.takeSeconds = videoMixerTakeSeconds(params[TIME_PARAM].getValue());
		in.patched = patched;
		state.step(in, args.sampleTime);

		const float shift = inputs[SHIFT_INPUT].getVoltage();
		for (int i = 0; i < kVideoMixerInputs; ++i) {
			const int colorState = static_cast<int>(clamp(std::round(params[COLOR_PARAM + i].getValue()), 0.f, 2.f));
			colorStates[i] = colorState;
			p.colorOn[i] = colorState == 1;
			p.gradientOn[i] = colorState == 2;
			p.hue[i] = videoMixerShiftedHue(params[HUE_PARAM + i].getValue(), shift);
		}
		p.onAir = state.onAir;
		p.next = state.next;
		p.fader = state.mix;
		// ALL overrides MODE: while its button is on, the mode is 3 and the switch does nothing to the picture.
		const bool allMode = params[ALL_PARAM].getValue() > 0.5f;
		p.mode = allMode ? 3 : static_cast<int>(clamp(std::round(params[MODE_PARAM].getValue()), 0.f, 2.f));
		for (int i = 0; i < kVideoMixerInputs; ++i) p.level[i] = params[LEVEL_PARAM + i].getValue();
		p.average = allAverages;
		p.soft = params[SOFT_PARAM].getValue();
		p.black = cv(BLACK_PARAM, BLACK_INPUT, 0.f, 1.f);
		p.sat = cv(SAT_PARAM, SAT_INPUT, 0.f, 1.f);
		p.span = cv(SPAN_PARAM, SPAN_INPUT, 0.f, 1.f) * kVideoMixerSpanTurns;
		p.keyOn = params[KEY_ON_PARAM].getValue() > 0.5f || inputs[KEY_ON_INPUT].getVoltage() >= 1.f;
		p.keyThresh = cv(KEY_THRESH_PARAM, THRESH_INPUT, 0.f, 1.f);
		p.keySoft = params[KEY_SOFT_PARAM].getValue();
		p.keyHue = params[KEY_HUE_PARAM].getValue();
		// The FILL switch is KEY (picture) up and FLAT down, and a CKSS reads 1 when up; the renderer's 0 is a picture.
		p.keyFill = params[FILL_PARAM].getValue() > 0.5f ? 0 : 1;
		p.keyInvert = params[KEY_INVERT_PARAM].getValue() > 0.5f;

		// Nothing to show until at least one picture is patched; then the mixer is a bus like any other.
		const bool active = patched != 0u;
		if (active) videoBusPublishVideoMixer(outBus, p);
		else videoBusClear(outBus);
		outputs[VIDEO_BUS_OUTPUT].setVoltage(videoBus.voltage());

		// In ALL every patched picture is live, so every connected light is red and every LIVE gate is up.
		for (int i = 0; i < kVideoMixerInputs; ++i) {
			const bool connected = (patched >> i) & 1u;
			const bool live = allMode ? connected : i == state.onAir;
			outputs[LIVE_OUTPUT + i].setVoltage(live ? 10.f : 0.f);
			lights[PICTURE_LIGHT + 2 * i].setBrightnessSmooth(!allMode && i == state.next && i != state.onAir ? 1.f : 0.f, args.sampleTime);
			lights[PICTURE_LIGHT + 2 * i + 1].setBrightnessSmooth(live ? 1.f : 0.f, args.sampleTime);
			const float hueLight = colorStates[i] == 1 ? 1.f : 0.f;
			const float gradientLight = colorStates[i] == 2 ? 1.f : 0.f;
			lights[COLOR_STATE_LIGHT + 3 * i].setBrightness(hueLight + gradientLight);
			lights[COLOR_STATE_LIGHT + 3 * i + 1].setBrightness(hueLight + 0.55f * gradientLight);
			lights[COLOR_STATE_LIGHT + 3 * i + 2].setBrightness(hueLight);
		}
		lights[CUT_LIGHT].setBrightnessSmooth(cut ? 1.f : 0.f, args.sampleTime, 10.f);
		lights[AUTO_LIGHT].setBrightnessSmooth(state.taking ? 1.f : 0.f, args.sampleTime, 10.f);
		lights[NEXT_LIGHT].setBrightnessSmooth(step ? 1.f : 0.f, args.sampleTime, 10.f);
		lights[ADVANCE_LIGHT].setBrightness(in.advance ? 1.f : 0.f);
		lights[KEY_ON_LIGHT].setBrightness(p.keyOn ? 1.f : 0.f);
		lights[KEY_INVERT_LIGHT].setBrightness(p.keyInvert ? 1.f : 0.f);
		lights[ALL_LIGHT].setBrightness(allMode ? 1.f : 0.f);
		lights[SIGNAL_LIGHT].setBrightnessSmooth(active ? 1.f : 0.f, args.sampleTime);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		saveVideoBusId(rootJ, videoBus);
		json_object_set_new(rootJ, "onAir", json_integer(state.onAir));
		json_object_set_new(rootJ, "next", json_integer(state.next));
		json_object_set_new(rootJ, "flipped", json_boolean(state.flipped));
		json_object_set_new(rootJ, "armed", json_boolean(state.armed));
		json_object_set_new(rootJ, "allAverages", json_boolean(allAverages));
		return rootJ;
	}
	void dataFromJson(json_t* rootJ) override {
		loadVideoBusId(rootJ, videoBus, "outputBusVoltage");
		if (json_t* j = json_object_get(rootJ, "onAir")) {
			if (json_is_integer(j)) state.onAir = clamp(static_cast<int>(json_integer_value(j)), 0, kVideoMixerInputs - 1);
		}
		if (json_t* j = json_object_get(rootJ, "next")) {
			if (json_is_integer(j)) state.next = clamp(static_cast<int>(json_integer_value(j)), 0, kVideoMixerInputs - 1);
		}
		if (json_t* j = json_object_get(rootJ, "flipped")) state.flipped = json_is_true(j);
		if (json_t* j = json_object_get(rootJ, "armed")) state.armed = json_is_true(j);
		if (json_t* j = json_object_get(rootJ, "allAverages")) allAverages = json_is_true(j);
	}
};

// A zone heading: the word at the left and a hairline from the end of the word to the right-hand edge of the box.
// The word is measured when it is drawn, so the line always starts a fixed gap after it.
struct VideoMixerZoneTitle : TransparentWidget {
	std::string text;

	void draw(const DrawArgs& args) override {
		using namespace rasterlab_panel;
		useLabelFont(args.vg);
		nvgFontSize(args.vg, mm2px(LABEL_MM));
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, nvgRGB(27, 27, 27));
		const float middle = box.size.y * 0.5f;
		nvgText(args.vg, 0.f, middle, text.c_str(), nullptr);
		const float start = nvgTextBounds(args.vg, 0.f, 0.f, text.c_str(), nullptr, nullptr) + mm2px(2.2f);
		if (start < box.size.x) {
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, start, middle);
			nvgLineTo(args.vg, box.size.x, middle);
			nvgStrokeColor(args.vg, nvgRGB(27, 27, 27));
			nvgStrokeWidth(args.vg, mm2px(0.15f));
			nvgStroke(args.vg);
		}
	}
};

struct VideoMixerWidget : ModuleWidget {
	using PushButton = VCVLightButton<MediumSimpleLight<WhiteLight>>;
	using LatchButton = VCVLightLatch<MediumSimpleLight<WhiteLight>>;
	using ColorButton = VCVLightLatch<MediumSimpleLight<RedGreenBlueLight>>;

	static constexpr float W = 152.4f;   // 30 HP

	// Eleven columns 12.7 mm apart in three groups: the four pictures, the transition (a fader and its buttons),
	// the key layer. Each gap between groups is 3 mm wider than the pitch.
	static float column(int k) {
		const float pitch = 12.7f, gutter = 3.f;
		const float span = 10.f * pitch + 2.f * gutter;
		return (W - span) * 0.5f + k * pitch + (k >= 4 ? gutter : 0.f) + (k >= 8 ? gutter : 0.f);
	}

	void addZoneTitle(float xLeft, float xRight, float y, const std::string& text) {
		using namespace rasterlab_panel;
		auto* title = new VideoMixerZoneTitle;
		title->text = text;
		const Vec size = mm2px(Vec(xRight - xLeft, LABEL_MM * 2.f));
		title->box.size = size;
		title->box.pos = mm2px(Vec(xLeft, y)).minus(Vec(0.f, size.y * 0.5f));
		addChild(title);
	}

	void addKnob(VideoMixer* module, Vec pos, const char* label, int paramId) {
		using namespace rasterlab_panel;
		addKnobLabel(this, pos, label, R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(pos), module, paramId));
	}
	void addJack(VideoMixer* module, Vec pos, const char* label, int inputId) {
		rasterlab_panel::addJackLabel(this, pos, label);
		addInput(createInputCentered<PJ301MPort>(mm2px(pos), module, inputId));
	}
	void addPush(VideoMixer* module, Vec pos, const char* label, int paramId, int lightId) {
		rasterlab_panel::addKnobLabel(this, pos, label, rasterlab_panel::R_BUTTON);
		addParam(createLightParamCentered<PushButton>(mm2px(pos), module, paramId, lightId));
	}
	void addLatch(VideoMixer* module, Vec pos, const char* label, int paramId, int lightId) {
		rasterlab_panel::addKnobLabel(this, pos, label, rasterlab_panel::R_BUTTON);
		addParam(createLightParamCentered<LatchButton>(mm2px(pos), module, paramId, lightId));
	}

	VideoMixerWidget(VideoMixer* module) {
		using namespace rasterlab_panel;
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/VideoMixer.svg")));
		addPanelHeader(this, W, "22", "VIDEO MIXER");

		float C[11];
		for (int k = 0; k < 11; ++k) C[k] = column(k);
		const float titleY = 19.5f;
		const float zoneLeft = C[0] - 4.f;
		addZoneTitle(zoneLeft, C[3] + 4.f, titleY, "VIDEO IN");
		addZoneTitle(C[4] - 4.8f, C[7] + 4.f, titleY, "TRANSITION");
		addZoneTitle(C[8] - 4.f, C[10] + 4.f, titleY, "KEY LAYER");

		// VIDEO IN. Each input has its status light (red: live, green: next), a hue knob and its COLOR button.
		static const char* names[] = {"A", "B", "C", "D"};
		static const char* hueNames[] = {"HUE", "HUE", "HUE", "HUE"};
		for (int i = 0; i < kVideoMixerInputs; ++i) {
			addJack(module, Vec(C[i], 30.f), names[i], VideoMixer::PICTURE_INPUT + i);
			addChild(createLightCentered<SmallLight<GreenRedLight>>(mm2px(Vec(C[i] + 5.2f, 26.2f)), module, VideoMixer::PICTURE_LIGHT + 2 * i));
			addKnob(module, Vec(C[i], 43.f), hueNames[i], VideoMixer::HUE_PARAM + i);
			addKnobLabel(this, Vec(C[i], 56.f), "COLOR", R_BUTTON);
			addParam(createLightParamCentered<ColorButton>(mm2px(Vec(C[i], 56.f)), module, VideoMixer::COLOR_PARAM + i, VideoMixer::COLOR_STATE_LIGHT + 3 * i));
			addKnob(module, Vec(C[i], 67.5f), "LEVEL", VideoMixer::LEVEL_PARAM + i);
		}
		const Vec sat(C[0], 81.f);
		addKnobLabel(this, sat, "SAT");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(sat), module, VideoMixer::SAT_PARAM));
		addJack(module, Vec(C[1], 81.f), "SAT CV", VideoMixer::SAT_INPUT);
		addKnob(module, Vec(C[2], 81.f), "SPAN", VideoMixer::SPAN_PARAM);
		addJack(module, Vec(C[3], 81.f), "SPAN CV", VideoMixer::SPAN_INPUT);
		addChild(createPanelRule(zoneLeft, C[3] + 4.f, 90.f));
		addPush(module, Vec(C[0], 101.f), "NEXT", VideoMixer::NEXT_PARAM, VideoMixer::NEXT_LIGHT);
		addJack(module, Vec(C[1], 101.f), "TRIG", VideoMixer::NEXT_INPUT);
		addJack(module, Vec(C[2], 101.f), "NEXT CV", VideoMixer::NEXT_CV_INPUT);
		addJack(module, Vec(C[3], 101.f), "SHIFT", VideoMixer::SHIFT_INPUT);

		// Transition. The MODE lever is a three-position switch: WIPE up, ADD in the middle, DISSOLVE down.
		const Vec mode(C[4], 31.f);
		addKnobLabel(this, mode, "MODE", 4.8f);
		static const char* modeWords[] = {"WIPE", "ADD", "DISSOLVE"};
		for (int j = 0; j < 3; ++j)
			addChild(createPanelTextAt(Vec(mode.x + TOGGLE_WORD_DX_MM, mode.y + (j - 1) * LINE_PITCH_MM), modeWords[j], NOTE_MM, NVG_ALIGN_LEFT));
		addParam(createParamCentered<CKSSThree>(mm2px(mode), module, VideoMixer::MODE_PARAM));
		// ALL: a lit latch under the MODE switch, left of the FADER label and above the CV jack.
		addLatch(module, Vec(C[4], 44.5f), "ALL", VideoMixer::ALL_PARAM, VideoMixer::ALL_LIGHT);
		addJack(module, Vec(C[6], 31.f), "PATTERN", VideoMixer::PATTERN_INPUT);
		addKnob(module, Vec(C[7], 31.f), "SOFT", VideoMixer::SOFT_PARAM);

		const float faderX = (C[4] + C[5]) * 0.5f;
		const Vec fader(faderX, 61.f);
		addKnobLabel(this, fader, "FADER", 9.12f);
		addParam(createParamCentered<RoundHugeBlackKnob>(mm2px(fader), module, VideoMixer::FADER_PARAM));
		addPush(module, Vec(C[6], 53.f), "CUT", VideoMixer::CUT_PARAM, VideoMixer::CUT_LIGHT);
		addJack(module, Vec(C[6], 66.f), "TRIG", VideoMixer::CUT_INPUT);
		addPush(module, Vec(C[7], 53.f), "AUTO", VideoMixer::AUTO_PARAM, VideoMixer::AUTO_LIGHT);
		addJack(module, Vec(C[7], 66.f), "TRIG", VideoMixer::AUTO_INPUT);
		addJack(module, Vec(faderX, 86.f), "CV", VideoMixer::FADER_INPUT);
		addLatch(module, Vec(C[6], 86.f), "ADVANCE", VideoMixer::ADVANCE_PARAM, VideoMixer::ADVANCE_LIGHT);
		addKnob(module, Vec(C[7], 86.f), "TIME", VideoMixer::TIME_PARAM);

		// Key layer: what to cut with (KEY, MATTE and the threshold), what to put in the hole, and when.
		addJack(module, Vec(C[8], 30.f), "KEY", VideoMixer::KEY_INPUT);
		addJack(module, Vec(C[9], 30.f), "MATTE", VideoMixer::MATTE_INPUT);
		addLatch(module, Vec(C[10], 30.f), "KEY ON", VideoMixer::KEY_ON_PARAM, VideoMixer::KEY_ON_LIGHT);
		addKnob(module, Vec(C[8], 44.f), "THRESH", VideoMixer::KEY_THRESH_PARAM);
		addKnob(module, Vec(C[9], 44.f), "SOFT", VideoMixer::KEY_SOFT_PARAM);
		addJack(module, Vec(C[10], 44.f), "GATE", VideoMixer::KEY_ON_INPUT);
		addJack(module, Vec(C[8], 58.f), "CV", VideoMixer::THRESH_INPUT);
		addLatch(module, Vec(C[9], 58.f), "INV", VideoMixer::KEY_INVERT_PARAM, VideoMixer::KEY_INVERT_LIGHT);
		addKnob(module, Vec(C[10], 58.f), "KEY HUE", VideoMixer::KEY_HUE_PARAM);
		const Vec fill(C[10], 73.f);
		addToggleLabels(this, fill, "FILL", "KEY", "FLAT", -1);
		addParam(createParamCentered<CKSS>(mm2px(fill), module, VideoMixer::FILL_PARAM));
		addChild(createPanelRule(C[8] - 4.f, C[10] + 4.f, 86.f));
		addKnob(module, Vec(C[8], 96.f), "BLACK", VideoMixer::BLACK_PARAM);
		addJack(module, Vec(C[9], 96.f), "CV", VideoMixer::BLACK_INPUT);

		// Output: one live gate for each picture, the signal light, and the bus.
		addZoneTitle(zoneLeft, W - 4.f, 108.f, "OUTPUT");
		for (int i = 0; i < kVideoMixerInputs; ++i)
			addPlatedOutput<PJ301MPort>(this, Vec(C[i], 120.f), "LIVE", module, VideoMixer::LIVE_OUTPUT + i, 10.4f);
		const Vec sig(C[9], 120.f);
		addJackLabel(this, sig, "SIG", 1.5f);
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(sig), module, VideoMixer::SIGNAL_LIGHT));
		addPlatedOutput<PJ301MPort>(this, Vec(C[10], 120.f), "BUS", module, VideoMixer::VIDEO_BUS_OUTPUT);
	}

	void appendContextMenu(ui::Menu* menu) override {
		VideoMixer* mixer = dynamic_cast<VideoMixer*>(module);
		if (!mixer) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createCheckMenuItem("ALL mode averages instead of adds", "", [=]() {
			return mixer->allAverages;
		}, [=]() {
			mixer->allAverages = !mixer->allAverages;
		}));
	}
};

Model* modelVideoMixer = createModel<VideoMixer, VideoMixerWidget>("VideoMixer");
