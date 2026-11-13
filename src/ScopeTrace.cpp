#include "plugin.hpp"
#include "cv_param.hpp"
#include "panel_style.hpp"
#include "scope_sweep.hpp"
#include "scope_timebase.hpp"
#include "signal_ring.hpp"
#include "video_bus.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

float normalizeInput(float voltage) {
	return clamp(voltage / 5.f, -1.5f, 1.5f);
}

// CV scale of the controls. Bipolar controls (position and rotation, -1..+1; the two scales, 0..2) move
// 1/5 of a unit per volt, so +-5 V adds +-1. Unipolar controls (TIME and HUE, 0..1) move 1/10 per volt,
// so 0..10 V sweeps the whole knob. The 0..2 knobs INTENSITY and BLOOM use the bipolar scale like the two
// scales do; BEAM, DECAY and DWELL (0..1) use the unipolar one. The DeflectionYoke uses the same two scales (bend and zoom at V/5,
// depth at 0.1 per volt).
constexpr float kPerVoltBipolar = 1.f / 5.f;
constexpr float kPerVoltUnipolar = 1.f / 10.f;
// The trigger level is already in volts, so its CV adds volts one for one.
constexpr float kPerVoltLevel = 1.f;

// How often a module that found every signal ring slot in use asks for one again.
constexpr float kSlotRetrySeconds = 1.f;

// TIME knob: 0..1 stored, displayed as the window length in milliseconds.
struct ScopeWindowParamQuantity : engine::ParamQuantity {
	float getDisplayValue() override {
		return 1000.f * scopeWindowSeconds(getValue());
	}

	void setDisplayValue(float displayValue) override {
		const float seconds = std::max(displayValue, 1e-6f) / 1000.f;
		setValue((std::log10(seconds) + 3.f) / 2.f);
	}

	// The number only: Rack's Quantity::getString() appends getUnit() to it, so including " ms" here
	// would show "ms ms" in the tooltip.
	std::string getDisplayValueString() override {
		char text[32];
		std::snprintf(text, sizeof(text), "%.2f", getDisplayValue());
		return text;
	}

	std::string getUnit() override {
		return " ms";
	}
};
}

struct ScopeTrace : Module {
	// Saved patches and presets address params, inputs and outputs by number. New ids are only ever
	// appended; the asserts below pin the numbers so a reorder fails to compile.
	enum ParamId {
		MODE_PARAM,
		X_SCALE_PARAM,
		Y_SCALE_PARAM,
		X_POS_PARAM,
		Y_POS_PARAM,
		TIME_PARAM,
		THICKNESS_PARAM,
		DECAY_PARAM,
		HUE_PARAM,
		INTENSITY_PARAM,
		ROTATION_PARAM,
		BLOOM_PARAM,
		// Attenuverters for the CV jacks, -100% to +100%, default +100%.
		X_SCALE_CV_PARAM,
		Y_SCALE_CV_PARAM,
		ROTATION_CV_PARAM,
		X_POS_CV_PARAM,
		Y_POS_CV_PARAM,
		TIME_CV_PARAM,
		HUE_CV_PARAM,
		Z_CV_PARAM,
		TRIG_LEVEL_PARAM,
		DWELL_PARAM,
		// Attenuverters for the second set of CV jacks (appended, so older patches keep their ids).
		DWELL_CV_PARAM,
		INTENSITY_CV_PARAM,
		DECAY_CV_PARAM,
		BEAM_CV_PARAM,
		BLOOM_CV_PARAM,
		TRIG_LEVEL_CV_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		X_INPUT,
		Y_INPUT,
		Z_INPUT,
		TIME_INPUT,
		RESET_INPUT,
		TRIG_INPUT,
		X_SCALE_INPUT,
		Y_SCALE_INPUT,
		ROTATION_INPUT,
		X_POS_INPUT,
		Y_POS_INPUT,
		HUE_INPUT,
		DWELL_INPUT,
		INTENSITY_INPUT,
		DECAY_INPUT,
		BEAM_INPUT,
		BLOOM_INPUT,
		TRIG_LEVEL_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		VIDEO_BUS_OUTPUT,
		SWEEP_OUTPUT,
		SYNC_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		SIGNAL_LIGHT,
		LIGHTS_LEN
	};
	static_assert(MODE_PARAM == 0 && X_SCALE_PARAM == 1 && Y_SCALE_PARAM == 2 && X_POS_PARAM == 3 && Y_POS_PARAM == 4 &&
			TIME_PARAM == 5 && THICKNESS_PARAM == 6 && DECAY_PARAM == 7 && HUE_PARAM == 8 && INTENSITY_PARAM == 9 &&
			ROTATION_PARAM == 10 && BLOOM_PARAM == 11 && X_SCALE_CV_PARAM == 12 && Y_SCALE_CV_PARAM == 13 &&
			ROTATION_CV_PARAM == 14 && X_POS_CV_PARAM == 15 && Y_POS_CV_PARAM == 16 && TIME_CV_PARAM == 17 &&
			HUE_CV_PARAM == 18 && Z_CV_PARAM == 19 && TRIG_LEVEL_PARAM == 20 && DWELL_PARAM == 21 && DWELL_CV_PARAM == 22 &&
			INTENSITY_CV_PARAM == 23 && DECAY_CV_PARAM == 24 && BEAM_CV_PARAM == 25 && BLOOM_CV_PARAM == 26 &&
			TRIG_LEVEL_CV_PARAM == 27 && PARAMS_LEN == 28,
		"Scope Trace param ids are part of the patch format");
	static_assert(X_INPUT == 0 && Y_INPUT == 1 && Z_INPUT == 2 && TIME_INPUT == 3 && RESET_INPUT == 4 && TRIG_INPUT == 5 &&
			X_SCALE_INPUT == 6 && Y_SCALE_INPUT == 7 && ROTATION_INPUT == 8 && X_POS_INPUT == 9 && Y_POS_INPUT == 10 &&
			HUE_INPUT == 11 && DWELL_INPUT == 12 && INTENSITY_INPUT == 13 && DECAY_INPUT == 14 &&
			BEAM_INPUT == 15 && BLOOM_INPUT == 16 && TRIG_LEVEL_INPUT == 17 && INPUTS_LEN == 18,
		"Scope Trace input ids are part of the patch format");
	static_assert(VIDEO_BUS_OUTPUT == 0 && SWEEP_OUTPUT == 1 && SYNC_OUTPUT == 2 && OUTPUTS_LEN == 3,
		"Scope Trace output ids are part of the patch format");

	std::atomic<bool> hasSignal {false};
	dsp::SchmittTrigger resetTrigger;
	dsp::SchmittTrigger trigTrigger;      // the TRIG jack, when patched
	dsp::SchmittTrigger internalTrigger;  // the signal itself, when TRIG is not patched (drives the sweep only)
	ScopeSweep sweep;
	dsp::PulseGenerator syncPulse;
	uint32_t clearSerial = 0;  // counts RESET edges; the renderer wipes the phosphor when it changes
	int scopeTraceSlotId = 0;    // signal ring slot; 0 while none is free (see process())
	float slotRetryElapsed = 0.f;  // seconds since the last attempt to claim a ring slot
	double internalPhase = 0.0;  // fallback tone, 0..1 cycles
	VideoBusOwner videoBus;
	ScopeTraceParams lastScope;  // what process() last published; the tests read it

	ScopeTrace() {
		scopeTraceSlotId = signalRingAllocate();
		if (scopeTraceSlotId == 0) {
			// All kSignalRingSlotCount rings are in use. Every ring call ignores slot 0, so without
			// a slot the module records nothing and draws a blank trace. Say so once here (this runs
			// on the UI thread, where logging is safe); process() keeps asking for a slot quietly.
			WARN("Scope Trace: no signal ring slot is free (all %d are in use), so it draws nothing; "
				"it will keep trying about once a second", kSignalRingSlotCount);
		}
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configSwitch(MODE_PARAM, 0.f, 3.f, 0.f, "Mode", {"XY Lissajous", "Oscilloscope", "Dual trace", "Polar"});
		configParam(X_SCALE_PARAM, 0.f, 2.f, 0.85f, "X scale");
		configParam(Y_SCALE_PARAM, 0.f, 2.f, 0.85f, "Y scale");
		configParam(X_POS_PARAM, -1.f, 1.f, 0.f, "X position");
		configParam(Y_POS_PARAM, -1.f, 1.f, 0.f, "Y position");
		configParam<ScopeWindowParamQuantity>(TIME_PARAM, 0.f, 1.f, 0.35f, "Timebase");
		configParam(THICKNESS_PARAM, 0.f, 1.f, 0.4f, "Beam width");
		configParam(DECAY_PARAM, 0.f, 1.f, 0.65f, "Persistence");
		configParam(HUE_PARAM, 0.f, 1.f, 0.32f, "Hue");
		configParam(INTENSITY_PARAM, 0.f, 2.f, 1.2f, "Intensity");
		configParam(ROTATION_PARAM, -1.f, 1.f, 0.f, "Rotation (turns all the way around; with CV, 10 V = one full turn)");
		configParam(BLOOM_PARAM, 0.f, 2.f, 0.f, "Bloom");
		configParam(X_SCALE_CV_PARAM, -1.f, 1.f, 1.f, "X scale CV amount", "%", 0.f, 100.f);
		configParam(Y_SCALE_CV_PARAM, -1.f, 1.f, 1.f, "Y scale CV amount", "%", 0.f, 100.f);
		configParam(ROTATION_CV_PARAM, -1.f, 1.f, 1.f, "Rotation CV amount", "%", 0.f, 100.f);
		configParam(X_POS_CV_PARAM, -1.f, 1.f, 1.f, "X position CV amount", "%", 0.f, 100.f);
		configParam(Y_POS_CV_PARAM, -1.f, 1.f, 1.f, "Y position CV amount", "%", 0.f, 100.f);
		configParam(TIME_CV_PARAM, -1.f, 1.f, 1.f, "Timebase CV amount", "%", 0.f, 100.f);
		configParam(HUE_CV_PARAM, -1.f, 1.f, 1.f, "Hue CV amount", "%", 0.f, 100.f);
		configParam(Z_CV_PARAM, -1.f, 1.f, 1.f, "Brightness CV amount", "%", 0.f, 100.f);
		configParam(TRIG_LEVEL_PARAM, -5.f, 5.f, 0.f, "Trigger level", " V");
		configParam(DWELL_PARAM, 0.f, 1.f, 0.5f, "Beam dwell (brighter where the beam moves slowly)");
		configParam(DWELL_CV_PARAM, -1.f, 1.f, 1.f, "Beam dwell CV amount", "%", 0.f, 100.f);
		configParam(INTENSITY_CV_PARAM, -1.f, 1.f, 1.f, "Intensity CV amount", "%", 0.f, 100.f);
		configParam(DECAY_CV_PARAM, -1.f, 1.f, 1.f, "Persistence CV amount", "%", 0.f, 100.f);
		configParam(BEAM_CV_PARAM, -1.f, 1.f, 1.f, "Beam width CV amount", "%", 0.f, 100.f);
		configParam(BLOOM_CV_PARAM, -1.f, 1.f, 1.f, "Bloom CV amount", "%", 0.f, 100.f);
		configParam(TRIG_LEVEL_CV_PARAM, -1.f, 1.f, 1.f, "Trigger level CV amount", "%", 0.f, 100.f);
		configInput(X_INPUT, "X in / trace A");
		configInput(Y_INPUT, "Y in / trace B");
		configInput(Z_INPUT, "Brightness");
		configInput(TIME_INPUT, "Timebase CV");
		configInput(RESET_INPUT, "Trace reset");
		configInput(TRIG_INPUT, "Trigger");
		configInput(X_SCALE_INPUT, "X scale CV");
		configInput(Y_SCALE_INPUT, "Y scale CV");
		configInput(ROTATION_INPUT, "Rotation CV (turns all the way around: 10 V = one full turn, so a rising CV spins the picture)");
		configInput(X_POS_INPUT, "X position CV");
		configInput(Y_POS_INPUT, "Y position CV");
		configInput(HUE_INPUT, "Hue CV");
		configInput(DWELL_INPUT, "Beam dwell CV (0 to 10 V sweeps the whole knob)");
		configInput(INTENSITY_INPUT, "Intensity CV (+-5 V moves it by 1, a half of the knob's 0 to 2 range)");
		configInput(DECAY_INPUT, "Persistence CV (0 to 10 V sweeps the whole knob)");
		configInput(BEAM_INPUT, "Beam width CV (0 to 10 V sweeps the whole knob)");
		configInput(BLOOM_INPUT, "Bloom CV (+-5 V moves it by 1, a half of the knob's 0 to 2 range)");
		configInput(TRIG_LEVEL_INPUT, "Trigger level CV (adds volts to the level knob, 1 V per volt, limited to +-5 V)");
		configOutput(VIDEO_BUS_OUTPUT, "Video bus token");
		configOutput(SWEEP_OUTPUT, "Sweep ramp (-5 to +5 V over one window)");
		configOutput(SYNC_OUTPUT, "Sync pulse (10 V, 1 ms, or half a window if that is shorter, at the start of each sweep)");
	}

	~ScopeTrace() override {
		signalRingRelease(scopeTraceSlotId);
	}

	void process(const ProcessArgs& args) override {
		using namespace rasterlab_cv;

		// No ring slot (the constructor found all of them in use and logged it): ask again about once a
		// second, without waiting on the pool's lock, and start recording the moment one frees up.
		if (scopeTraceSlotId == 0) {
			slotRetryElapsed += args.sampleTime;
			if (slotRetryElapsed >= kSlotRetrySeconds) {
				slotRetryElapsed = 0.f;
				scopeTraceSlotId = signalRingTryAllocate();
			}
		}

		const bool resetEdge = inputs[RESET_INPUT].isConnected() && resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 2.f);
		if (resetEdge) {
			signalRingClear(scopeTraceSlotId);
			internalPhase = 0.0;
			sweep.restart();
			clearSerial++;  // published below; the renderer wipes the phosphor when it changes
		}

		// Each CV jack adds attenuverter x volts x (knob units per volt) to its knob. An unpatched jack
		// reads 0 V, so with the attenuverters at their default +100% a patch without CV is unchanged.
		const float timebase = modulated(params[TIME_PARAM].getValue(), inputs[TIME_INPUT].getVoltage(),
			params[TIME_CV_PARAM].getValue(), kPerVoltUnipolar, 0.f, 1.f);
		const int mode = static_cast<int>(clamp(std::round(params[MODE_PARAM].getValue()), 0.f, 3.f));

		// Full-rate capture: one sample per audio sample goes into the ring, so the ring is not decimated.
		// TIME sets the window length. (The renderer does decimate when it draws: a long window is thinned
		// to at most 4096 points, one in every stride samples.)
		// The fallback tone advances once per sample at its Hz (scopeFallbackHz), so it runs at the
		// frequency the code says, not a fraction of it. Its shape depends on the mode.
		internalPhase = advanceScopeFallbackPhase(internalPhase, timebase, args.sampleTime);
		const bool hasX = inputs[X_INPUT].isConnected();
		const bool hasY = inputs[Y_INPUT].isConnected();
		float fallbackX = 0.f;
		float fallbackY = 0.f;
		scopeFallbackSignal(mode, internalPhase, fallbackX, fallbackY);
		const float x = hasX ? normalizeInput(inputs[X_INPUT].getVoltage()) : fallbackX;
		const float y = hasY ? normalizeInput(inputs[Y_INPUT].getVoltage()) : fallbackY;
		const float z = zModulation(inputs[Z_INPUT].getVoltage(), params[Z_CV_PARAM].getValue(), inputs[Z_INPUT].isConnected());
		const float sample[kSignalRingChannels] = {x, y, z};
		signalRingSetSampleRate(scopeTraceSlotId, args.sampleRate);
		signalRingPush(scopeTraceSlotId, sample);

		// Trigger level, in volts. A rising crossing of the level with a 10 mV Schmitt band, so a 0/10 V gate
		// still fires on every gate at the default 0 V. TRIG patched: its edge stamps this sample, so the
		// time-based modes start their window here, and it paces the sweep. TRIG not patched: the renderer
		// finds its own crossing, and a second trigger on the signal in channel 0 (in volts) paces the sweep.
		// The level moves one volt per volt (it is already in volts) and stays within the knob's +-5 V.
		const float triggerVolts = modulated(params[TRIG_LEVEL_PARAM].getValue(), inputs[TRIG_LEVEL_INPUT].getVoltage(),
			params[TRIG_LEVEL_CV_PARAM].getValue(), kPerVoltLevel, -5.f, 5.f);
		const bool trigPatched = inputs[TRIG_INPUT].isConnected();
		bool triggerEdge = false;
		if (trigPatched) {
			if (trigTrigger.process(inputs[TRIG_INPUT].getVoltage(), triggerVolts, triggerVolts + 0.01f)) {
				signalRingMarkTrigger(scopeTraceSlotId);
				triggerEdge = true;
			}
		}
		else {
			triggerEdge = internalTrigger.process(x * 5.f, triggerVolts, triggerVolts + 0.01f);
		}

		// SWEEP and SYNC. The ramp runs in every mode. The pulse is 1 ms, but never longer than half a
		// window, so at the shortest timebase two sweeps in a row stay two separate pulses.
		const float windowSeconds = scopeWindowSeconds(timebase);
		if (sweep.process(args.sampleTime, windowSeconds, triggerEdge)) {
			syncPulse.trigger(std::min(1e-3f, 0.5f * windowSeconds));
		}
		outputs[SWEEP_OUTPUT].setVoltage(sweep.rampVolts());
		outputs[SYNC_OUTPUT].setVoltage(syncPulse.process(args.sampleTime) ? 10.f : 0.f);

		ScopeTraceParams scope;
		scope.mode = mode;
		scope.scaleX = modulated(params[X_SCALE_PARAM].getValue(), inputs[X_SCALE_INPUT].getVoltage(),
			params[X_SCALE_CV_PARAM].getValue(), kPerVoltBipolar, 0.f, 2.f);
		scope.scaleY = modulated(params[Y_SCALE_PARAM].getValue(), inputs[Y_SCALE_INPUT].getVoltage(),
			params[Y_SCALE_CV_PARAM].getValue(), kPerVoltBipolar, 0.f, 2.f);
		scope.offsetX = modulated(params[X_POS_PARAM].getValue(), inputs[X_POS_INPUT].getVoltage(),
			params[X_POS_CV_PARAM].getValue(), kPerVoltBipolar, -1.f, 1.f);
		scope.offsetY = modulated(params[Y_POS_PARAM].getValue(), inputs[Y_POS_INPUT].getVoltage(),
			params[Y_POS_CV_PARAM].getValue(), kPerVoltBipolar, -1.f, 1.f);
		scope.timebase = timebase;
		scope.thickness = modulated(params[THICKNESS_PARAM].getValue(), inputs[BEAM_INPUT].getVoltage(),
			params[BEAM_CV_PARAM].getValue(), kPerVoltUnipolar, 0.f, 1.f);
		scope.decay = modulated(params[DECAY_PARAM].getValue(), inputs[DECAY_INPUT].getVoltage(),
			params[DECAY_CV_PARAM].getValue(), kPerVoltUnipolar, 0.f, 1.f);
		scope.hue = modulatedWrapped(params[HUE_PARAM].getValue(), inputs[HUE_INPUT].getVoltage(),
			params[HUE_CV_PARAM].getValue(), kPerVoltUnipolar);
		scope.intensity = modulated(params[INTENSITY_PARAM].getValue(), inputs[INTENSITY_INPUT].getVoltage(),
			params[INTENSITY_CV_PARAM].getValue(), kPerVoltBipolar, 0.f, 2.f);
		// ROTATE wraps instead of clamping: -1..+1 is -180 to +180 degrees, and past either end it comes round
		// again, so a rising CV (10 V of travel is one full turn) spins the picture and the knob sets the start.
		scope.rotation = modulatedTurn(params[ROTATION_PARAM].getValue(), inputs[ROTATION_INPUT].getVoltage(),
			params[ROTATION_CV_PARAM].getValue(), kPerVoltBipolar);
		scope.bloom = modulated(params[BLOOM_PARAM].getValue(), inputs[BLOOM_INPUT].getVoltage(),
			params[BLOOM_CV_PARAM].getValue(), kPerVoltBipolar, 0.f, 2.f);
		scope.triggerPatched = trigPatched;
		scope.triggerLevel = triggerVolts / 5.f;  // the same units as the X channel (volts / 5)
		scope.dwell = modulated(params[DWELL_PARAM].getValue(), inputs[DWELL_INPUT].getVoltage(),
			params[DWELL_CV_PARAM].getValue(), kPerVoltUnipolar, 0.f, 1.f);
		scope.clearSerial = clearSerial;
		lastScope = scope;

		const int outputBusId = videoBus.id();
		videoBusPublishScopeTrace(outputBusId, scopeTraceSlotId, scope);
		outputs[VIDEO_BUS_OUTPUT].setVoltage(videoBus.voltage());
		const bool active = inputs[X_INPUT].isConnected() || inputs[Y_INPUT].isConnected();
		hasSignal.store(active, std::memory_order_relaxed);
		lights[SIGNAL_LIGHT].setBrightnessSmooth(active ? 1.f : 0.35f, args.sampleTime);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "phase", json_real(internalPhase));
		saveVideoBusId(rootJ, videoBus);
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* phaseJ = json_object_get(rootJ, "phase");
		if (phaseJ) {
			internalPhase = json_number_value(phaseJ);
			internalPhase -= std::floor(internalPhase);
		}

		// clearSerial is not restored or bumped here: loading a patch must not wipe the phosphor.
		loadVideoBusId(rootJ, videoBus, "outputBusVoltage");
	}

	// Rack restores only the param ids that are in the saved file. A patch saved before DWELL existed has no
	// id 21, so it would load DWELL at its 0.5 default and draw slow stretches brighter than the patch was made
	// with. Start from 0, the even law those patches were saved with; an id the file does have still overwrites
	// it. A new module and "Initialize" do not come through here and keep 0.5.
	void paramsFromJson(json_t* rootJ) override {
		params[DWELL_PARAM].setValue(0.f);
		Module::paramsFromJson(rootJ);
	}
};

// Current mode name, drawn on the panel under the MODE knob. Reads the knob every frame.
struct ScopeModeReadout : TransparentWidget {
	ScopeTrace* module = nullptr;

	void draw(const DrawArgs& args) override {
		static const char* const kNames[4] = {"XY LISSAJOUS", "OSCILLOSCOPE", "DUAL TRACE", "POLAR"};
		int mode = 0;
		if (module) {
			mode = static_cast<int>(clamp(std::round(module->params[ScopeTrace::MODE_PARAM].getValue()), 0.f, 3.f));
		}
		auto font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
		if (font) {
			nvgFontFaceId(args.vg, font->handle);
		}
		nvgFontSize(args.vg, mm2px(1.8f));
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, nvgRGB(27, 27, 27));
		nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.5f, kNames[mode], nullptr);
	}
};

struct ScopeTraceWidget : ModuleWidget {
	ScopeTraceWidget(ScopeTrace* module) {
		using namespace rasterlab_panel;
		const float W = 101.6f;	// 20 HP
		const float P8 = 12.7f;	// eight-column pitch (2.5 HP): the small knob, attenuverter and CV rows
		const float P4 = 25.4f;	// four-column pitch (5 HP): the large knobs, each centred over a pair of the eight columns
		const float SWEEP_PLATE_MM = 11.4f;	// the word SWEEP is wider than the standard 9.2 mm output plate
		// Row centres, in mm from the top of the panel.
		const float yBig = 30.f;	// large knobs
		const float yKnob = 50.f;	// small knobs
		const float yTrim = 68.f;	// attenuverters
		const float yCv = 80.f;	// CV jacks, one under each attenuverter
		const float yLow = 97.5f;	// trigger level and bloom (the two-line TRIG LVL label must clear the CV jacks above)
		const float yTrim2 = 94.f;	// second set of attenuverters, columns 2 to 7
		const float yCv2 = 106.f;	// their CV jacks
		const float yJack = 119.f;	// signal jacks and outputs
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/ScopeTrace.svg")));
		addPanelHeader(this, W, "03", "SCOPE TRACE");

		// Column x positions in mm: col8(k) is the k-th of eight, col4(k) the k-th of four.
		auto col8 = [&](int k) { return rowX(W, 8, k, P8); };
		auto col4 = [&](int k) { return rowX(W, 4, k, P4); };
		auto bigKnob = [&](int col, const char* label, int paramId) {
			addKnobLabel(this, Vec(col4(col), yBig), label);
			addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(col4(col), yBig)), module, paramId));
		};
		auto smallKnob = [&](int col, float y, const char* label, int paramId) {
			addKnobLabel(this, Vec(col8(col), y), label, R_SMALL);
			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(col8(col), y)), module, paramId));
		};
		// An attenuverter with its CV jack under it. Every jack carries the same short label, so the
		// label belongs to the column: the attenuverter's own label above it names the control.
		auto cvColumn = [&](int col, const char* label, int attenuverterId, int jackId) {
			addKnobLabel(this, Vec(col8(col), yTrim), label, R_TRIM);
			addParam(createParamCentered<Trimpot>(mm2px(Vec(col8(col), yTrim)), module, attenuverterId));
			addJackLabel(this, Vec(col8(col), yCv), "CV");
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(col8(col), yCv)), module, jackId));
		};
		auto signalInput = [&](int col, const char* label, int inputId) {
			addJackLabel(this, Vec(col8(col), yJack), label);
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(col8(col), yJack)), module, inputId));
		};

		// Row 1, large knobs. MODE is a snap knob with the mode name drawn under it (knob centre 30 mm,
		// the next row's labels sit at 43.7 mm). The readout box is 22 mm wide and centred on the knob,
		// so its left edge is 1.7 mm from the panel edge.
		addKnobLabel(this, Vec(col4(0), yBig), "MODE");
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(col4(0), yBig)), module, ScopeTrace::MODE_PARAM));
		{
			auto* readout = new ScopeModeReadout();
			readout->module = module;
			const Vec centre = mm2px(Vec(col4(0), 36.5f));
			readout->box.size = mm2px(Vec(22.f, 3.f));
			readout->box.pos = Vec(centre.x - readout->box.size.x * 0.5f, centre.y - readout->box.size.y * 0.5f);
			addChild(readout);
		}
		bigKnob(1, "X SCALE", ScopeTrace::X_SCALE_PARAM);
		bigKnob(2, "Y SCALE", ScopeTrace::Y_SCALE_PARAM);
		bigKnob(3, "ROTATE", ScopeTrace::ROTATION_PARAM);

		// Row 2, small knobs, all eight columns. Labels on neighbouring columns must fit side by side in
		// the 12.7 mm pitch. Widths at the label size (Nunito-Bold, 3.8 mm): the full word INTENSITY is
		// 15.8 mm and DWELL 10.6 mm, which would overlap by 0.5 mm, so the knob is labelled INTENS
		// (11.2 mm), the tightest pair at 1.8 mm apart. Every other pair is at least 3.1 mm apart.
		smallKnob(0, yKnob, "X POS", ScopeTrace::X_POS_PARAM);
		smallKnob(1, yKnob, "Y POS", ScopeTrace::Y_POS_PARAM);
		smallKnob(2, yKnob, "TIME", ScopeTrace::TIME_PARAM);
		smallKnob(3, yKnob, "DWELL", ScopeTrace::DWELL_PARAM);
		smallKnob(4, yKnob, "INTENS", ScopeTrace::INTENSITY_PARAM);
		smallKnob(5, yKnob, "HUE", ScopeTrace::HUE_PARAM);
		smallKnob(6, yKnob, "DECAY", ScopeTrace::DECAY_PARAM);
		smallKnob(7, yKnob, "BEAM", ScopeTrace::THICKNESS_PARAM);

		// Rows 3 and 4: an attenuverter (-100% to +100%, starting at +100%) with a CV jack under it, one
		// pair per modulated control. The pairs are not under the knobs they modulate; each label names
		// its control.
		cvColumn(0, "X SCL", ScopeTrace::X_SCALE_CV_PARAM, ScopeTrace::X_SCALE_INPUT);
		cvColumn(1, "Y SCL", ScopeTrace::Y_SCALE_CV_PARAM, ScopeTrace::Y_SCALE_INPUT);
		cvColumn(2, "ROT", ScopeTrace::ROTATION_CV_PARAM, ScopeTrace::ROTATION_INPUT);
		cvColumn(3, "X POS", ScopeTrace::X_POS_CV_PARAM, ScopeTrace::X_POS_INPUT);
		cvColumn(4, "Y POS", ScopeTrace::Y_POS_CV_PARAM, ScopeTrace::Y_POS_INPUT);
		cvColumn(5, "TIME", ScopeTrace::TIME_CV_PARAM, ScopeTrace::TIME_INPUT);
		cvColumn(6, "HUE", ScopeTrace::HUE_CV_PARAM, ScopeTrace::HUE_INPUT);
		cvColumn(7, "Z", ScopeTrace::Z_CV_PARAM, ScopeTrace::Z_INPUT);

		// Rows 5 and 6: the second set of attenuverters and CV jacks, columns 2 to 7, in the same style as
		// rows 3 and 4. They sit to the right of the TRIG LVL and BLOOM knobs, which stay in columns 0 and 1.
		auto cvColumn2 = [&](int col, const char* label, int attenuverterId, int jackId) {
			addKnobLabel(this, Vec(col8(col), yTrim2), label, R_TRIM);
			addParam(createParamCentered<Trimpot>(mm2px(Vec(col8(col), yTrim2)), module, attenuverterId));
			addJackLabel(this, Vec(col8(col), yCv2), "CV");
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(col8(col), yCv2)), module, jackId));
		};
		cvColumn2(2, "DWELL", ScopeTrace::DWELL_CV_PARAM, ScopeTrace::DWELL_INPUT);
		cvColumn2(3, "INTENS", ScopeTrace::INTENSITY_CV_PARAM, ScopeTrace::INTENSITY_INPUT);
		cvColumn2(4, "DECAY", ScopeTrace::DECAY_CV_PARAM, ScopeTrace::DECAY_INPUT);
		cvColumn2(5, "BEAM", ScopeTrace::BEAM_CV_PARAM, ScopeTrace::BEAM_INPUT);
		cvColumn2(6, "BLOOM", ScopeTrace::BLOOM_CV_PARAM, ScopeTrace::BLOOM_INPUT);
		cvColumn2(7, "LEVEL", ScopeTrace::TRIG_LEVEL_CV_PARAM, ScopeTrace::TRIG_LEVEL_INPUT);

		// Row 5, small knobs. TRIG LVL is stacked on two lines: on one line it is 13.3 mm wide and
		// would run off the panel edge from the first column. The second set of attenuverters sits to their
		// right; the TILT slot (beam tilt, a later phase) is gone, a new control takes the next free spot.
		smallKnob(0, yLow, "TRIG\nLVL", ScopeTrace::TRIG_LEVEL_PARAM);
		smallKnob(1, yLow, "BLOOM", ScopeTrace::BLOOM_PARAM);

		// Row 6, signal jacks: inputs on the left, the status light, then the outputs. X IN and Y IN are the
		// module's main signal inputs, so each sits in an outlined plate (the output plate's size, unfilled)
		// with its label inside; that sets them apart from the X SCL and Y SCL CV jacks two rows up in the
		// same columns, which they were mistaken for. Output plates behind SWEEP, SYNC and BUS; the jacks
		// draw on top of every plate.
		addPlatedInput<PJ301MPort>(this, Vec(col8(0), yJack), "X IN", module, ScopeTrace::X_INPUT);
		addPlatedInput<PJ301MPort>(this, Vec(col8(1), yJack), "Y IN", module, ScopeTrace::Y_INPUT);
		signalInput(2, "TRIG", ScopeTrace::TRIG_INPUT);
		signalInput(3, "RESET", ScopeTrace::RESET_INPUT);
		addJackLabel(this, Vec(col8(4), yJack), "SIG");
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(col8(4), yJack)), module, ScopeTrace::SIGNAL_LIGHT));
		addPlatedOutput<PJ301MPort>(this, Vec(col8(5), yJack), "SWEEP", module, ScopeTrace::SWEEP_OUTPUT, SWEEP_PLATE_MM);
		addPlatedOutput<PJ301MPort>(this, Vec(col8(6), yJack), "SYNC", module, ScopeTrace::SYNC_OUTPUT);
		addPlatedOutput<PJ301MPort>(this, Vec(col8(7), yJack), "BUS", module, ScopeTrace::VIDEO_BUS_OUTPUT);
	}
};

Model* modelScopeTrace = createModel<ScopeTrace, ScopeTraceWidget>("ScopeTrace");
