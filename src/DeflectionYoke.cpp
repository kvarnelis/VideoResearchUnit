#include "plugin.hpp"
#include "panel_style.hpp"
#include "hz_param.hpp"
#include "signal_ring.hpp"
#include "crt_beam.hpp"
#include "video_bus.hpp"
#include "deflection_yoke_math.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace {
// Full-scale deflection (+/-5 V) moves the image by this much in normalized units: half its width,
// so a full-scale excursion takes the picture entirely off the screen.
constexpr float kDeflectFullScale = 0.5f;
constexpr float kSampleClamp = 2.f;  // +/-10 V is already well past full scale

// The zoom oscillator is a slow modulator. The three coil oscillators (H, V and S) use the 0.1 Hz to
// 20 kHz mapping in deflection_yoke_math.hpp.
constexpr float kZoomMinHz = 0.01f;
constexpr float kZoomMaxHz = 20.f;

// Yoke cutoff for the coil signals (the coil's inductance rolls off high tones).
constexpr float kCoilMinHz = 500.f;
constexpr float kCoilMaxHz = 8000.f;
constexpr float kCoilDefaultHz = 3000.f;

// A fresh module's coil oscillators run at 60 Hz, a multiple of the field rate, which holds the pattern
// nearly still.
constexpr float kHomeHz = 60.f;

// Everything that changes slowly (knob-derived rates, LINES, SPOT, PERSIST, GLOW, CURL, TWIST, SIZE, FLIP,
// the COIL cutoff) is refreshed, and the node on the video bus is republished, every this many samples:
// 0.7 ms at 48 kHz, far finer than a video frame. Audio-rate paths (sources, V/OCT, amounts, DEPTH,
// phases) run every sample.
constexpr uint32_t kControlDivision = 32;

// A coil RATE knob stores 0..kRateTop and shows Hz. The shared ExpHzParamQuantity clamps its knob to 0..1
// (8 kHz), so this one maps through the unclamped helper in deflection_yoke_math.hpp.
struct DeflectionYokeRateQuantity : ExpHzParamQuantity {
	float getDisplayValue() override {
		return deflection_yoke::rateHzFromKnob(getValue());
	}

	void setDisplayValue(float displayValue) override {
		setValue(deflection_yoke::rateKnobFromHz(displayValue));
	}
};
}

struct DeflectionYoke : Module {
	// Ids are saved in patches: new ones go at the end, existing ones never move. The X_ and Y_ ids are the
	// H and V coils (the panel says H and V).
	enum ParamId {
		X_BEND_PARAM,     // H coil amount (-1..1)
		Y_BEND_PARAM,     // V coil amount (-1..1)
		DEPTH_PARAM,      // master depth of the three coils (0..1)
		RATE_PARAM,       // H coil oscillator, exponential Hz
		PHASE_PARAM,      // oscillator phase offset, all oscillators
		LINE_CURL_PARAM,  // static line bow
		TWIST_PARAM,      // static twist
		FLIP_H_PARAM,
		FLIP_V_PARAM,
		Y_RATE_PARAM,     // V coil oscillator, exponential Hz
		FINE_PARAM,       // fine detune of the H, V and S oscillators, +/-50 cents
		ZOOM_PARAM,       // zoom amount (-1..1): zoom = 1 + 2 x amount x signal, clamped to 0.02..4
		Z_RATE_PARAM,     // zoom oscillator, exponential Hz
		EDGE_PARAM,       // 0 clamp, 1 wrap, 2 black
		LINES_PARAM,      // visible scanlines per field, 120..600 (NTSC line rate; the field rate follows)
		SPOT_PARAM,       // spot size across the line, in units of the scanline pitch
		COIL_PARAM,       // yoke cutoff, exponential Hz: the coil lowpass
		PERSIST_PARAM,    // phosphor persistence, 0..1
		GLOW_PARAM,       // glow (bloom) amount, 0..2
		S_BEND_PARAM,     // S coil amount (-1..1)
		S_RATE_PARAM,     // S coil oscillator, exponential Hz
		H_SIZE_PARAM,     // size of the normal horizontal scan (0..1); 0 collapses it to a vertical line
		V_SIZE_PARAM,     // size of the normal vertical scan (0..1); 0 collapses it to a horizontal line
		X_WAVE_PARAM,     // H coil waveform: 0 sine, 1 triangle, 2 square, 3 DC
		Y_WAVE_PARAM,     // V coil waveform
		S_WAVE_PARAM,     // S coil waveform
		Y_PHASE_PARAM,    // V coil phase against the H coil, -0.5..0.5 cycles (0 keeps the quarter-cycle lead)
		S_PHASE_PARAM,    // S coil phase, -0.5..0.5 cycles
		PARAMS_LEN
	};
	enum InputId {
		VIDEO_BUS_INPUT,
		X_BEND_INPUT,     // H coil in: audio rate, +/-5 V full scale, replaces the oscillator
		Y_BEND_INPUT,     // V coil in
		DEPTH_INPUT,
		RATE_INPUT,       // H coil oscillator, 1 V/oct
		RESET_INPUT,
		ZOOM_INPUT,       // zoom in: audio rate, +/-5 V full scale, replaces the oscillator
		S_BEND_INPUT,     // S coil in
		Y_RATE_INPUT,     // V coil oscillator, 1 V/oct
		S_RATE_INPUT,     // S coil oscillator, 1 V/oct
		Z_RATE_INPUT,     // zoom oscillator, 1 V/oct
		X_AMOUNT_INPUT,   // coil amounts, +/-5 V = +/-1
		Y_AMOUNT_INPUT,
		S_AMOUNT_INPUT,
		FINE_INPUT,       // +/-5 V = +/-50 cents
		PHASE_INPUT,      // 5 V = one cycle
		Y_PHASE_INPUT,
		S_PHASE_INPUT,
		CURL_INPUT,       // +/-5 V = +/-1 (the total is limited to +/-3)
		TWIST_INPUT,
		H_SIZE_INPUT,     // +/-5 V = the knob's whole range
		V_SIZE_INPUT,
		LINES_INPUT,      // 5 V = one octave
		SPOT_INPUT,
		COIL_INPUT,       // 1 V/oct on the cutoff
		PERSIST_INPUT,
		GLOW_INPUT,
		FLIP_H_INPUT,     // gate: above 1 V inverts the switch
		FLIP_V_INPUT,
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
	dsp::SchmittTrigger resetTrigger;
	dsp::ClockDivider controlDivider;
	// One coil filter per coil channel (H, V and S), so each has its own state.
	crt::CoilFilter coilX;
	crt::CoilFilter coilY;
	crt::CoilFilter coilS;
	// Oscillator phases, 0..1. The phase offsets (PHASE, Y PHASE, S PHASE) are added when a sample is computed.
	double xPhase = 0.0;
	double yPhase = 0.0;
	double zPhase = 0.0;
	double sPhase = 0.0;
	int signalSlotId = 0;
	VideoBusOwner videoBus;

	// Control-rate state, refreshed every kControlDivision samples (and at once after load or a sample
	// rate change). The knob rates are cached in Hz so the audio path needs no pow() for a knob.
	bool refreshNow = true;
	float xKnobHz = kHomeHz;
	float yKnobHz = kHomeHz;
	float sKnobHz = kHomeHz;
	float zKnobHz = 1.f;
	float fineKnobRatio = 1.f;
	DeflectionYokeParams wobble;
	bool lastActive = false;
	int lastInputBusId = 0;
	int lastOutputBusId = 0;

	DeflectionYoke() {
		signalSlotId = signalRingAllocate();
		controlDivider.setDivision(kControlDivision);
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		// A fresh module is set up to work at once: DEPTH 0.5, the H coil at 0.75 and all three coil
		// oscillators at 60 Hz. Saved patches store their own values, and the new params default to neutral
		// (the S coil and both SIZE knobs, which are 1, do nothing), so old patches sound as they did.
		configParam(X_BEND_PARAM, -1.f, 1.f, 0.75f, "H coil amount");
		configParam(Y_BEND_PARAM, -1.f, 1.f, 0.f, "V coil amount");
		configParam(S_BEND_PARAM, -1.f, 1.f, 0.f, "S coil amount");
		configParam(DEPTH_PARAM, 0.f, 1.f, 0.5f, "Depth");
		configRate(RATE_PARAM, "H coil rate");
		configRate(Y_RATE_PARAM, "V coil rate");
		configRate(S_RATE_PARAM, "S coil rate");
		configParam(FINE_PARAM, -1.f, 1.f, 0.f, "Fine tune", " ct", 0.f, 50.f);
		configParam(PHASE_PARAM, 0.f, 1.f, 0.f, "Phase (all oscillators)", " deg", 0.f, 360.f);
		configParam(Y_PHASE_PARAM, -0.5f, 0.5f, 0.f, "V coil phase", " deg", 0.f, 360.f);
		configParam(S_PHASE_PARAM, -0.5f, 0.5f, 0.f, "S coil phase", " deg", 0.f, 360.f);
		const std::vector<std::string> waves = {"Sine", "Triangle", "Square", "DC"};
		configSwitch(X_WAVE_PARAM, 0.f, 3.f, 0.f, "H coil wave", waves);
		configSwitch(Y_WAVE_PARAM, 0.f, 3.f, 0.f, "V coil wave", waves);
		configSwitch(S_WAVE_PARAM, 0.f, 3.f, 0.f, "S coil wave", waves);
		configParam(H_SIZE_PARAM, 0.f, 1.f, 1.f, "H size", "%", 0.f, 100.f);
		configParam(V_SIZE_PARAM, 0.f, 1.f, 1.f, "V size", "%", 0.f, 100.f);
		configParam(ZOOM_PARAM, -1.f, 1.f, 0.f, "Zoom amount");
		auto* zRate = configParam<ExpHzParamQuantity>(Z_RATE_PARAM, 0.f, 1.f, normalizedFromExpHz(0.5f, kZoomMinHz, kZoomMaxHz), "Zoom oscillator rate");
		zRate->minHz = kZoomMinHz;
		zRate->maxHz = kZoomMaxHz;
		configParam(LINE_CURL_PARAM, -1.f, 1.f, 0.f, "Line curl");
		configParam(TWIST_PARAM, -1.f, 1.f, 0.f, "Twist");
		configSwitch(FLIP_H_PARAM, 0.f, 1.f, 0.f, "Flip horizontal", {"Off", "On"});
		configSwitch(FLIP_V_PARAM, 0.f, 1.f, 0.f, "Flip vertical", {"Off", "On"});
		// Black is the default for new modules. Saved patches keep the value they were saved with.
		configSwitch(EDGE_PARAM, 0.f, 2.f, 2.f, "Edge", {"Clamp", "Wrap", "Black"});
		configParam(LINES_PARAM, static_cast<float>(crt::kMinLines), static_cast<float>(crt::kMaxLines), static_cast<float>(crt::kDefaultLines), "Scanlines", " lines");
		configParam(SPOT_PARAM, 0.1f, 0.6f, 0.20f, "Spot size (fraction of line pitch)");
		auto* coil = configParam<ExpHzParamQuantity>(COIL_PARAM, 0.f, 1.f, normalizedFromExpHz(kCoilDefaultHz, kCoilMinHz, kCoilMaxHz), "Coil cutoff");
		coil->minHz = kCoilMinHz;
		coil->maxHz = kCoilMaxHz;
		configParam(PERSIST_PARAM, 0.f, 1.f, 0.35f, "Persistence");
		configParam(GLOW_PARAM, 0.f, 2.f, 0.4f, "Glow");

		describeParam(X_BEND_PARAM, "How hard the H coil pushes the beam sideways. Negative reverses the push.");
		describeParam(Y_BEND_PARAM, "How hard the V coil pushes the beam up and down. Negative reverses the push.");
		describeParam(S_BEND_PARAM, "How far the S coil turns the picture's deflection about the screen centre. The angle follows the coil signal, so lines bend into an S.");
		describeParam(DEPTH_PARAM, "Master level of the three coils. Also scales line curl and twist.");
		describeParam(RATE_PARAM, "H coil oscillator rate, 0.1 Hz to 20 kHz. 30, 60 and 90 Hz sit near multiples of the field rate and hold almost still.");
		describeParam(Y_RATE_PARAM, "V coil oscillator rate, 0.1 Hz to 20 kHz.");
		describeParam(S_RATE_PARAM, "S coil oscillator rate, 0.1 Hz to 20 kHz.");
		describeParam(FINE_PARAM, "Detunes the H, V and S coil rates together, up to 50 cents either way.");
		describeParam(PHASE_PARAM, "Phase offset added to every oscillator, the zoom oscillator included.");
		describeParam(Y_PHASE_PARAM, "Shifts the V coil against the H coil. At 0 the V coil leads by a quarter cycle, so equal rates draw a circle.");
		describeParam(S_PHASE_PARAM, "Shifts the S coil oscillator.");
		const char* waveText = "Oscillator waveform. DC holds the coil steady: it shifts the raster, or turns it by a fixed angle for the S coil. A cable in the jack below replaces the oscillator.";
		describeParam(X_WAVE_PARAM, waveText);
		describeParam(Y_WAVE_PARAM, waveText);
		describeParam(S_WAVE_PARAM, waveText);
		describeParam(H_SIZE_PARAM, "Size of the normal horizontal scan. Turned down, the raster collapses to a vertical line while the coils keep working on it.");
		describeParam(V_SIZE_PARAM, "Size of the normal vertical scan. Turned down, the raster collapses to a horizontal line while the coils keep working on it.");
		describeParam(ZOOM_PARAM, "How far the zoom oscillator scales the picture.");
		describeParam(Z_RATE_PARAM, "Zoom oscillator rate, 0.01 to 20 Hz.");
		describeParam(LINE_CURL_PARAM, "Bows the scanlines. Scaled by Depth.");
		describeParam(TWIST_PARAM, "Twists the picture about its centre. Scaled by Depth.");
		describeParam(EDGE_PARAM, "What shows past the picture border: its edge pixels stretched (Clamp), the picture repeated (Wrap) or black.");
		describeParam(LINES_PARAM, "Scanlines per field, 120 to 600. The field rate follows the line count.");
		describeParam(SPOT_PARAM, "Width of the beam spot as a fraction of the line spacing.");
		describeParam(COIL_PARAM, "Cutoff of the filter on the coil signals, 500 Hz to 8 kHz. Lower values round off fast bends.");
		describeParam(PERSIST_PARAM, "How long the trail of earlier fields lasts.");
		describeParam(GLOW_PARAM, "Bloom around bright areas.");

		configInput(VIDEO_BUS_INPUT, "Video bus token");
		configInput(X_BEND_INPUT, "H coil in (audio rate)");
		configInput(Y_BEND_INPUT, "V coil in (audio rate)");
		configInput(S_BEND_INPUT, "S coil in (audio rate)");
		configInput(DEPTH_INPUT, "Depth CV");
		configInput(RATE_INPUT, "H coil rate (1 V/oct)");
		configInput(Y_RATE_INPUT, "V coil rate (1 V/oct)");
		configInput(S_RATE_INPUT, "S coil rate (1 V/oct)");
		configInput(Z_RATE_INPUT, "Zoom rate (1 V/oct)");
		configInput(RESET_INPUT, "Phase reset");
		configInput(ZOOM_INPUT, "Zoom in (audio rate)");
		configInput(X_AMOUNT_INPUT, "H coil amount CV");
		configInput(Y_AMOUNT_INPUT, "V coil amount CV");
		configInput(S_AMOUNT_INPUT, "S coil amount CV");
		configInput(FINE_INPUT, "Fine tune CV");
		configInput(PHASE_INPUT, "Phase CV (all oscillators)");
		configInput(Y_PHASE_INPUT, "V coil phase CV");
		configInput(S_PHASE_INPUT, "S coil phase CV");
		configInput(CURL_INPUT, "Line curl CV");
		configInput(TWIST_INPUT, "Twist CV");
		configInput(H_SIZE_INPUT, "H size CV");
		configInput(V_SIZE_INPUT, "V size CV");
		configInput(LINES_INPUT, "Scanlines CV (5 V per octave)");
		configInput(SPOT_INPUT, "Spot size CV");
		configInput(COIL_INPUT, "Coil cutoff CV (1 V/oct)");
		configInput(PERSIST_INPUT, "Persistence CV");
		configInput(GLOW_INPUT, "Glow CV");
		configInput(FLIP_H_INPUT, "Flip horizontal gate");
		configInput(FLIP_V_INPUT, "Flip vertical gate");
		configOutput(VIDEO_BUS_OUTPUT, "Video bus token");

		describeInput(X_BEND_INPUT, "Audio-rate signal for the H coil, +/-5 V full scale. A cable here replaces the oscillator.");
		describeInput(Y_BEND_INPUT, "Audio-rate signal for the V coil, +/-5 V full scale. A cable here replaces the oscillator.");
		describeInput(S_BEND_INPUT, "Audio-rate signal for the S coil, +/-5 V full scale. A cable here replaces the oscillator.");
		describeInput(ZOOM_INPUT, "Audio-rate signal for the zoom, +/-5 V full scale. A cable here replaces the zoom oscillator.");
		describeInput(DEPTH_INPUT, "0.1 per volt, added to the Depth knob.");
		describeInput(X_AMOUNT_INPUT, "+/-5 V sweeps the amount from -1 to 1, added to the knob.");
		describeInput(Y_AMOUNT_INPUT, "+/-5 V sweeps the amount from -1 to 1, added to the knob.");
		describeInput(S_AMOUNT_INPUT, "+/-5 V sweeps the amount from -1 to 1, added to the knob.");
		describeInput(FINE_INPUT, "+/-5 V is the knob's full +/-50 cents.");
		describeInput(PHASE_INPUT, "5 V is one cycle, added to the knob.");
		describeInput(Y_PHASE_INPUT, "5 V is one cycle, added to the knob.");
		describeInput(S_PHASE_INPUT, "5 V is one cycle, added to the knob.");
		describeInput(CURL_INPUT, "+/-5 V is +/-1, added to the knob. The total is limited to +/-3.");
		describeInput(TWIST_INPUT, "+/-5 V is +/-1, added to the knob. The total is limited to +/-3.");
		describeInput(H_SIZE_INPUT, "+/-5 V sweeps the knob's whole range, added to the knob.");
		describeInput(V_SIZE_INPUT, "+/-5 V sweeps the knob's whole range, added to the knob.");
		describeInput(LINES_INPUT, "5 V doubles the line count, rounded to whole lines.");
		describeInput(SPOT_INPUT, "+/-5 V sweeps the knob's whole range, added to the knob.");
		describeInput(COIL_INPUT, "1 V per octave on the cutoff.");
		describeInput(PERSIST_INPUT, "+/-5 V sweeps the knob's whole range, added to the knob.");
		describeInput(GLOW_INPUT, "+/-5 V sweeps the knob's whole range, added to the knob.");
		describeInput(FLIP_H_INPUT, "Above 1 V inverts the Flip horizontal switch.");
		describeInput(FLIP_V_INPUT, "Above 1 V inverts the Flip vertical switch.");
	}

	~DeflectionYoke() override {
		signalRingRelease(signalSlotId);
	}

	void configRate(int paramId, const char* name) {
		auto* rate = configParam<DeflectionYokeRateQuantity>(paramId, 0.f, deflection_yoke::kRateTop, deflection_yoke::rateKnobFromHz(kHomeHz), name);
		rate->minHz = deflection_yoke::kRateMinHz;
		rate->maxHz = deflection_yoke::kRateMaxHz;
	}

	void describeParam(int paramId, const char* text) {
		paramQuantities[paramId]->description = text;
	}

	void describeInput(int portId, const char* text) {
		inputInfos[portId]->description = text;
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		Module::onSampleRateChange(e);
		refreshNow = true;
	}

	// An input's voltage, 0 when nothing is patched. A non-finite voltage reads as 0, so no NaN gets into
	// a phase, a filter or the ring.
	float cv(int inputId) {
		Input& in = inputs[inputId];
		if (!in.isConnected()) {
			return 0.f;
		}
		const float volts = in.getVoltage();
		return std::isfinite(volts) ? volts : 0.f;
	}

	// An audio-rate cable as a coil source: +/-5 V is +/-1, limited to +/-kSampleClamp.
	float cable(int inputId) {
		return clamp(cv(inputId) / 5.f, -kSampleClamp, kSampleClamp);
	}

	// One coil's source: the cable if one is patched, else its oscillator at `phase` (cycles).
	float coilSource(int inputId, int waveParam, double phase) {
		if (inputs[inputId].isConnected()) {
			return cable(inputId);
		}
		return deflection_yoke::wave(static_cast<int>(std::lround(params[waveParam].getValue())), phase);
	}

	// One oscillator's frequency: the knob (and fine tune) in Hz, plus the 1 V/oct cable if patched.
	float oscillatorRate(float knobHz, int octaveInputId, float sampleRate) {
		if (inputs[octaveInputId].isConnected()) {
			knobHz *= deflection_yoke::octaveRatio(cv(octaveInputId));
		}
		return deflection_yoke::oscillatorHz(knobHz, sampleRate);
	}

	// The slow controls. Runs every kControlDivision samples, and at once after load or a sample rate change.
	void refreshControls(float sampleRate) {
		refreshNow = false;
		xKnobHz = deflection_yoke::rateHzFromKnob(params[RATE_PARAM].getValue());
		yKnobHz = deflection_yoke::rateHzFromKnob(params[Y_RATE_PARAM].getValue());
		sKnobHz = deflection_yoke::rateHzFromKnob(params[S_RATE_PARAM].getValue());
		zKnobHz = expHzFromNormalized(params[Z_RATE_PARAM].getValue(), kZoomMinHz, kZoomMaxHz);
		fineKnobRatio = deflection_yoke::fineRatio(params[FINE_PARAM].getValue(), 0.f);

		// The yoke cutoff is shared by the three coil channels.
		const float coilKnobHz = expHzFromNormalized(params[COIL_PARAM].getValue(), kCoilMinHz, kCoilMaxHz);
		const float cutoff = deflection_yoke::coilCutoffWithCv(coilKnobHz, cv(COIL_INPUT), sampleRate);
		coilX.setCutoff(cutoff, sampleRate);
		coilY.setCutoff(cutoff, sampleRate);
		coilS.setCutoff(cutoff, sampleRate);
		signalRingSetSampleRate(signalSlotId, sampleRate);

		wobble.lineCurl = deflection_yoke::shapeWithCv(params[LINE_CURL_PARAM].getValue(), cv(CURL_INPUT));
		wobble.twist = deflection_yoke::shapeWithCv(params[TWIST_PARAM].getValue(), cv(TWIST_INPUT));
		wobble.flipH = deflection_yoke::flipWithGate(params[FLIP_H_PARAM].getValue() > 0.5f, cv(FLIP_H_INPUT));
		wobble.flipV = deflection_yoke::flipWithGate(params[FLIP_V_PARAM].getValue() > 0.5f, cv(FLIP_V_INPUT));
		wobble.edge = static_cast<int>(clamp(std::round(params[EDGE_PARAM].getValue()), 0.f, 2.f));
		wobble.signalSlotId = signalSlotId;
		wobble.lines = deflection_yoke::linesWithCv(params[LINES_PARAM].getValue(), cv(LINES_INPUT), crt::kMinLines, crt::kMaxLines);
		wobble.spot = deflection_yoke::rangeWithCv(params[SPOT_PARAM].getValue(), cv(SPOT_INPUT), 0.1f, 0.6f);
		wobble.persist = deflection_yoke::rangeWithCv(params[PERSIST_PARAM].getValue(), cv(PERSIST_INPUT), 0.f, 1.f);
		wobble.glow = deflection_yoke::rangeWithCv(params[GLOW_PARAM].getValue(), cv(GLOW_INPUT), 0.f, 2.f);
		wobble.hSize = deflection_yoke::rangeWithCv(params[H_SIZE_PARAM].getValue(), cv(H_SIZE_INPUT), 0.f, 1.f);
		wobble.vSize = deflection_yoke::rangeWithCv(params[V_SIZE_PARAM].getValue(), cv(V_SIZE_INPUT), 0.f, 1.f);
	}

	void process(const ProcessArgs& args) override {
		const int inputBusId = inputs[VIDEO_BUS_INPUT].isConnected() ? videoBusIdFromVoltage(inputs[VIDEO_BUS_INPUT].getVoltage()) : 1;
		const bool active = videoBusIsActive(inputBusId) && inputBusId != videoBus.id();
		const int outputBusId = videoBus.id();

		if (inputs[RESET_INPUT].isConnected() && resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 2.f)) {
			xPhase = 0.0;
			yPhase = 0.0;
			zPhase = 0.0;
			sPhase = 0.0;
		}

		const bool tick = controlDivider.process() || refreshNow;
		if (tick) {
			refreshControls(args.sampleRate);
		}

		// Oscillator frequencies. Fine detune is the same ratio on H, V and S, so circles stay circles.
		const float fine = inputs[FINE_INPUT].isConnected()
			? deflection_yoke::fineRatio(params[FINE_PARAM].getValue(), cv(FINE_INPUT))
			: fineKnobRatio;
		const float xHz = oscillatorRate(xKnobHz * fine, RATE_INPUT, args.sampleRate);
		const float yHz = oscillatorRate(yKnobHz * fine, Y_RATE_INPUT, args.sampleRate);
		const float sHz = oscillatorRate(sKnobHz * fine, S_RATE_INPUT, args.sampleRate);
		const float zHz = oscillatorRate(zKnobHz, Z_RATE_INPUT, args.sampleRate);
		xPhase += static_cast<double>(xHz) * args.sampleTime;
		yPhase += static_cast<double>(yHz) * args.sampleTime;
		sPhase += static_cast<double>(sHz) * args.sampleTime;
		zPhase += static_cast<double>(zHz) * args.sampleTime;
		xPhase -= std::floor(xPhase);
		yPhase -= std::floor(yPhase);
		sPhase -= std::floor(sPhase);
		zPhase -= std::floor(zPhase);

		const float phaseOffset = deflection_yoke::phaseWithCv(params[PHASE_PARAM].getValue(), cv(PHASE_INPUT));
		const float yPhaseOffset = deflection_yoke::phaseWithCv(params[Y_PHASE_PARAM].getValue(), cv(Y_PHASE_INPUT));
		const float sPhaseOffset = deflection_yoke::phaseWithCv(params[S_PHASE_PARAM].getValue(), cv(S_PHASE_INPUT));
		const float xSource = coilSource(X_BEND_INPUT, X_WAVE_PARAM, xPhase + phaseOffset);
		// V is a quarter cycle ahead of H when it runs on its own: H = sin, V = cos, so circles and spirals.
		const float ySource = coilSource(Y_BEND_INPUT, Y_WAVE_PARAM, yPhase + phaseOffset + 0.25 + yPhaseOffset);
		const float sSource = coilSource(S_BEND_INPUT, S_WAVE_PARAM, sPhase + phaseOffset + sPhaseOffset);
		const float zSource = inputs[ZOOM_INPUT].isConnected()
			? cable(ZOOM_INPUT)
			: deflection_yoke::wave(deflection_yoke::kWaveSine, zPhase + phaseOffset);

		const float depth = deflection_yoke::depthWithCv(params[DEPTH_PARAM].getValue(), cv(DEPTH_INPUT));
		const float xAmount = deflection_yoke::amountWithCv(params[X_BEND_PARAM].getValue(), cv(X_AMOUNT_INPUT));
		const float yAmount = deflection_yoke::amountWithCv(params[Y_BEND_PARAM].getValue(), cv(Y_AMOUNT_INPUT));
		const float sAmount = deflection_yoke::amountWithCv(params[S_BEND_PARAM].getValue(), cv(S_AMOUNT_INPUT));
		const float zoomAmount = clamp(params[ZOOM_PARAM].getValue(), -1.f, 1.f);

		// Coil and zoom signals for this sample, stored for the renderer's scan-time lookup. The coil
		// filters the three coil channels only. The zoom term is 1 + value, clamped by the renderer to
		// 0.02..4, so the raster can collapse to a point or overscan. The S channel is a turn: the renderer
		// scales it to radians.
		float samples[kSignalRingChannels] = {};
		samples[kDeflectionYokeDeflectX] = coilX.process(depth * xAmount * xSource * kDeflectFullScale);
		samples[kDeflectionYokeDeflectY] = coilY.process(depth * yAmount * ySource * kDeflectFullScale);
		samples[kDeflectionYokeZoom] = 2.f * zoomAmount * zSource;
		samples[kDeflectionYokeSCoil] = coilS.process(depth * sAmount * sSource);
		signalRingPush(signalSlotId, samples);

		sourceBus.store(inputBusId, std::memory_order_relaxed);
		hasSignal.store(active, std::memory_order_relaxed);

		// Publish with the control-rate refresh, and at once when the bus wiring changes.
		const bool wiringChanged = active != lastActive || inputBusId != lastInputBusId || outputBusId != lastOutputBusId;
		if (tick || wiringChanged) {
			wobble.depth = depth;
			if (active) {
				videoBusPublishDeflectionYoke(outputBusId, inputBusId, wobble);
			}
			else {
				videoBusClear(outputBusId);
			}
			lastActive = active;
			lastInputBusId = inputBusId;
			lastOutputBusId = outputBusId;
		}

		outputs[VIDEO_BUS_OUTPUT].setVoltage(videoBus.voltage());
		lights[SIGNAL_LIGHT].setBrightnessSmooth(active ? 1.f : 0.f, args.sampleTime);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "phase", json_real(xPhase));
		json_object_set_new(rootJ, "yPhase", json_real(yPhase));
		json_object_set_new(rootJ, "zPhase", json_real(zPhase));
		json_object_set_new(rootJ, "sPhase", json_real(sPhase));
		saveVideoBusId(rootJ, videoBus);
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		if (json_t* phaseJ = json_object_get(rootJ, "phase")) {
			xPhase = json_number_value(phaseJ);
			xPhase -= std::floor(xPhase);
		}
		if (json_t* phaseJ = json_object_get(rootJ, "yPhase")) {
			yPhase = json_number_value(phaseJ);
			yPhase -= std::floor(yPhase);
		}
		if (json_t* phaseJ = json_object_get(rootJ, "zPhase")) {
			zPhase = json_number_value(phaseJ);
			zPhase -= std::floor(zPhase);
		}
		if (json_t* phaseJ = json_object_get(rootJ, "sPhase")) {
			sPhase = json_number_value(phaseJ);
			sPhase -= std::floor(sPhase);
		}

		loadVideoBusId(rootJ, videoBus, "outputBusVoltage");
		refreshNow = true;
	}
};

// A small knob that snaps to whole numbers, for the WAVE selectors (0 sine, 1 triangle, 2 square, 3 DC).
struct DeflectionYokeWaveKnob : RoundSmallBlackKnob {
	DeflectionYokeWaveKnob() {
		snap = true;
	}
};

// The panel is 20 HP and every knob has its jack directly under it. A rule at x = 50.8 splits it in two.
//   Left block: one column for each coil (H, V, S) and an ALL column for the settings the three share.
//   The rows are amount, rate, wave and phase, so one coil can be compared with the next by reading across.
//   Right block: four titled bands for what the beam does to the picture (RASTER), the zoom and the line
//   shape (SHAPE), the tube (TUBE) and the video connections (VIDEO).
// A jack under its knob that takes plain CV has no label (its tooltip names it). A jack that takes
// something else has one: V/OCT, IN, RESET and BUS IN.
// The rules are in res/DeflectionYoke.svg: the header rule, the rule at x = 50.8 and one beside each band
// title, at the title's y.
struct DeflectionYokeWidget : ModuleWidget {
	// A knob or switch with its label above it. radiusMm is half the control's height (R_KNOB, R_SMALL,
	// R_SWITCH), which sets how far above the control the label sits.
	template <typename TControl>
	void addLabelledControl(Vec centreMm, const std::string& label, float radiusMm, int paramId) {
		rasterlab_panel::addKnobLabel(this, centreMm, label, radiusMm);
		addParam(createParamCentered<TControl>(mm2px(centreMm), module, paramId));
	}

	// An input jack, with a label above it unless label is empty.
	void addLabelledInput(Vec centreMm, const std::string& label, int inputId) {
		if (!label.empty()) {
			rasterlab_panel::addJackLabel(this, centreMm, label);
		}
		addInput(createInputCentered<PJ301MPort>(mm2px(centreMm), module, inputId));
	}

	DeflectionYokeWidget(DeflectionYoke* module) {
		using namespace rasterlab_panel;
		const float W = 101.6f;	// 20 HP
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/DeflectionYoke.svg")));
		addPanelHeader(this, W, "01", "DEFLECTION YOKE", TITLE_MM, 3.f);

		// ---- Left block: the three coils and the settings they share ----
		const float colH = 7.8f;
		const float colV = 20.0f;
		const float colS = 32.2f;
		const float colAll = 44.4f;
		const float headY = 18.4f;
		addChild(createPanelText(Vec(colH, headY), "H COIL", LABEL_MM));
		addChild(createPanelText(Vec(colV, headY), "V COIL", LABEL_MM));
		addChild(createPanelText(Vec(colS, headY), "S COIL", LABEL_MM));
		addChild(createPanelText(Vec(colAll, headY), "ALL", LABEL_MM));

		// Row 1: how far each coil moves the beam, and the DEPTH that scales all three.
		const float amtY = 30.f;
		const float amtJackY = 40.6f;
		addLabelledControl<RoundBlackKnob>(Vec(colH, amtY), "AMT", R_KNOB, DeflectionYoke::X_BEND_PARAM);
		addLabelledInput(Vec(colH, amtJackY), "", DeflectionYoke::X_AMOUNT_INPUT);
		addLabelledControl<RoundBlackKnob>(Vec(colV, amtY), "AMT", R_KNOB, DeflectionYoke::Y_BEND_PARAM);
		addLabelledInput(Vec(colV, amtJackY), "", DeflectionYoke::Y_AMOUNT_INPUT);
		addLabelledControl<RoundBlackKnob>(Vec(colS, amtY), "AMT", R_KNOB, DeflectionYoke::S_BEND_PARAM);
		addLabelledInput(Vec(colS, amtJackY), "", DeflectionYoke::S_AMOUNT_INPUT);
		addLabelledControl<RoundBlackKnob>(Vec(colAll, amtY), "DEPTH", R_KNOB, DeflectionYoke::DEPTH_PARAM);
		addLabelledInput(Vec(colAll, amtJackY), "", DeflectionYoke::DEPTH_INPUT);

		// Row 2: how fast each coil runs, and the FINE detune of all three.
		const float rateY = 55.17f;
		const float rateJackY = 69.0f;
		addLabelledControl<RoundBlackKnob>(Vec(colH, rateY), "RATE", R_KNOB, DeflectionYoke::RATE_PARAM);
		addLabelledInput(Vec(colH, rateJackY), "V/OCT", DeflectionYoke::RATE_INPUT);
		addLabelledControl<RoundBlackKnob>(Vec(colV, rateY), "RATE", R_KNOB, DeflectionYoke::Y_RATE_PARAM);
		addLabelledInput(Vec(colV, rateJackY), "V/OCT", DeflectionYoke::Y_RATE_INPUT);
		addLabelledControl<RoundBlackKnob>(Vec(colS, rateY), "RATE", R_KNOB, DeflectionYoke::S_RATE_PARAM);
		addLabelledInput(Vec(colS, rateJackY), "V/OCT", DeflectionYoke::S_RATE_INPUT);
		addLabelledControl<RoundBlackKnob>(Vec(colAll, rateY), "FINE", R_KNOB, DeflectionYoke::FINE_PARAM);
		addLabelledInput(Vec(colAll, rateJackY), "", DeflectionYoke::FINE_INPUT);

		// Row 3: the waveform of each coil, with the jack that replaces its oscillator, and the PHASE
		// that offsets all three.
		const float waveY = 82.4f;
		const float waveJackY = 95.1f;
		addLabelledControl<DeflectionYokeWaveKnob>(Vec(colH, waveY), "WAVE", R_SMALL, DeflectionYoke::X_WAVE_PARAM);
		addLabelledInput(Vec(colH, waveJackY), "IN", DeflectionYoke::X_BEND_INPUT);
		addLabelledControl<DeflectionYokeWaveKnob>(Vec(colV, waveY), "WAVE", R_SMALL, DeflectionYoke::Y_WAVE_PARAM);
		addLabelledInput(Vec(colV, waveJackY), "IN", DeflectionYoke::Y_BEND_INPUT);
		addLabelledControl<DeflectionYokeWaveKnob>(Vec(colS, waveY), "WAVE", R_SMALL, DeflectionYoke::S_WAVE_PARAM);
		addLabelledInput(Vec(colS, waveJackY), "IN", DeflectionYoke::S_BEND_INPUT);
		addLabelledControl<RoundSmallBlackKnob>(Vec(colAll, waveY), "PHASE", R_SMALL, DeflectionYoke::PHASE_PARAM);
		addLabelledInput(Vec(colAll, waveJackY), "", DeflectionYoke::PHASE_INPUT);

		// Row 4: the V and S phase against H (H is the reference, so its cell is empty), and RESET.
		const float phaseY = 107.9f;
		const float phaseJackY = 117.54f;
		addLabelledControl<RoundSmallBlackKnob>(Vec(colV, phaseY), "PHASE", R_SMALL, DeflectionYoke::Y_PHASE_PARAM);
		addLabelledInput(Vec(colV, phaseJackY), "", DeflectionYoke::Y_PHASE_INPUT);
		addLabelledControl<RoundSmallBlackKnob>(Vec(colS, phaseY), "PHASE", R_SMALL, DeflectionYoke::S_PHASE_PARAM);
		addLabelledInput(Vec(colS, phaseJackY), "", DeflectionYoke::S_PHASE_INPUT);
		addLabelledInput(Vec(colAll, phaseJackY), "RESET", DeflectionYoke::RESET_INPUT);

		// ---- Right block: four bands, four columns ----
		const float c1 = 57.1f;
		const float c2 = 69.5f;
		const float c3 = 81.9f;
		const float c4 = 94.3f;
		const float bandX = 52.4f;	// left edge of the band titles

		// RASTER: the size of the scanned area and the mirror switches, each with a jack under it.
		const float rasterTitleY = 18.4f;
		const float rasterY = 28.14f;
		const float rasterJackY = 37.38f;
		addChild(createPanelTextAt(Vec(bandX, rasterTitleY), "RASTER", LABEL_MM, NVG_ALIGN_LEFT));
		addLabelledControl<RoundSmallBlackKnob>(Vec(c1, rasterY), "H SIZE", R_SMALL, DeflectionYoke::H_SIZE_PARAM);
		addLabelledInput(Vec(c1, rasterJackY), "", DeflectionYoke::H_SIZE_INPUT);
		addLabelledControl<RoundSmallBlackKnob>(Vec(c2, rasterY), "V SIZE", R_SMALL, DeflectionYoke::V_SIZE_PARAM);
		addLabelledInput(Vec(c2, rasterJackY), "", DeflectionYoke::V_SIZE_INPUT);
		addLabelledControl<CKSS>(Vec(c3, rasterY), "FLIP H", R_SMALL, DeflectionYoke::FLIP_H_PARAM);
		addLabelledInput(Vec(c3, rasterJackY), "", DeflectionYoke::FLIP_H_INPUT);
		addLabelledControl<CKSS>(Vec(c4, rasterY), "FLIP V", R_SMALL, DeflectionYoke::FLIP_V_PARAM);
		addLabelledInput(Vec(c4, rasterJackY), "", DeflectionYoke::FLIP_V_INPUT);

		// SHAPE: zoom with its own oscillator, then the line shape. The four jacks share one row.
		const float shapeTitleY = 44.14f;
		const float shapeY = 53.88f;
		const float shapeJackY = 66.5f;
		addChild(createPanelTextAt(Vec(bandX, shapeTitleY), "SHAPE", LABEL_MM, NVG_ALIGN_LEFT));
		addLabelledControl<RoundSmallBlackKnob>(Vec(c1, shapeY), "ZOOM", R_SMALL, DeflectionYoke::ZOOM_PARAM);
		addLabelledInput(Vec(c1, shapeJackY), "IN", DeflectionYoke::ZOOM_INPUT);
		addLabelledControl<RoundSmallBlackKnob>(Vec(c2, shapeY), "Z RATE", R_SMALL, DeflectionYoke::Z_RATE_PARAM);
		addLabelledInput(Vec(c2, shapeJackY), "V/OCT", DeflectionYoke::Z_RATE_INPUT);
		addLabelledControl<RoundSmallBlackKnob>(Vec(c3, shapeY), "CURL", R_SMALL, DeflectionYoke::LINE_CURL_PARAM);
		addLabelledInput(Vec(c3, shapeJackY), "", DeflectionYoke::CURL_INPUT);
		addLabelledControl<RoundSmallBlackKnob>(Vec(c4, shapeY), "TWIST", R_SMALL, DeflectionYoke::TWIST_PARAM);
		addLabelledInput(Vec(c4, shapeJackY), "", DeflectionYoke::TWIST_INPUT);

		// TUBE: spot size, yoke cutoff, phosphor trail and bloom.
		const float tubeTitleY = 72.82f;
		const float tubeY = 82.56f;
		const float tubeJackY = 91.8f;
		addChild(createPanelTextAt(Vec(bandX, tubeTitleY), "TUBE", LABEL_MM, NVG_ALIGN_LEFT));
		addLabelledControl<RoundSmallBlackKnob>(Vec(c1, tubeY), "SPOT", R_SMALL, DeflectionYoke::SPOT_PARAM);
		addLabelledInput(Vec(c1, tubeJackY), "", DeflectionYoke::SPOT_INPUT);
		addLabelledControl<RoundSmallBlackKnob>(Vec(c2, tubeY), "COIL", R_SMALL, DeflectionYoke::COIL_PARAM);
		addLabelledInput(Vec(c2, tubeJackY), "", DeflectionYoke::COIL_INPUT);
		addLabelledControl<RoundSmallBlackKnob>(Vec(c3, tubeY), "PERSIST", R_SMALL, DeflectionYoke::PERSIST_PARAM);
		addLabelledInput(Vec(c3, tubeJackY), "", DeflectionYoke::PERSIST_INPUT);
		addLabelledControl<RoundSmallBlackKnob>(Vec(c4, tubeY), "GLOW", R_SMALL, DeflectionYoke::GLOW_PARAM);
		addLabelledInput(Vec(c4, tubeJackY), "", DeflectionYoke::GLOW_INPUT);

		// VIDEO: scanline count, the edge rule, and the bus in and out. EDGE is a taller switch than the
		// knobs are, so it sits a little lower and its label stays on the row's line.
		const float videoTitleY = 98.56f;
		const float videoY = 108.3f;
		const float edgeY = 109.26f;
		const float sigY = 108.7f;
		const float videoJackY = 117.54f;
		addChild(createPanelTextAt(Vec(bandX, videoTitleY), "VIDEO", LABEL_MM, NVG_ALIGN_LEFT));
		addLabelledControl<RoundSmallBlackKnob>(Vec(c1, videoY), "LINES", R_SMALL, DeflectionYoke::LINES_PARAM);
		addLabelledInput(Vec(c1, videoJackY), "", DeflectionYoke::LINES_INPUT);
		addLabelledControl<CKSSThree>(Vec(c2, edgeY), "EDGE", R_KNOB, DeflectionYoke::EDGE_PARAM);
		addLabelledInput(Vec(c3, videoJackY), "BUS IN", DeflectionYoke::VIDEO_BUS_INPUT);
		addKnobLabel(this, Vec(c4, videoY), "SIG", R_SMALL);
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(c4, sigY)), module, DeflectionYoke::SIGNAL_LIGHT));
		addPlatedOutput<PJ301MPort>(this, Vec(c4, videoJackY), "BUS", module, DeflectionYoke::VIDEO_BUS_OUTPUT);
	}
};

Model* modelDeflectionYoke = createModel<DeflectionYoke, DeflectionYokeWidget>("DeflectionYoke");
