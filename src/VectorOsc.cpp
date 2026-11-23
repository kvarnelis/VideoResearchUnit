#include "plugin.hpp"
#include "panel_style.hpp"
#include "panel_items.hpp"
#include "vector_osc_math.hpp"

// VRU-25 Vector Osc: a CV and audio source for Scope Trace's X IN and Y IN. No video bus.
struct VectorOsc : Module {
	// Ids are append-only: the CV attenuverters come after LEVEL_PARAM, the newer CV jacks after SYNC_INPUT.
	enum ParamId { FREQ_PARAM, RATIO_PARAM, FINE_PARAM, PHASE_PARAM, VOICES_PARAM, DETUNE_PARAM,
		WAVE_PARAM, WAVE_Y_PARAM, LEVEL_PARAM,
		RATIO_CV_PARAM, PHASE_CV_PARAM, DETUNE_CV_PARAM, WAVE_CV_PARAM,
		FINE_CV_PARAM, VOICES_CV_PARAM, WAVE_Y_CV_PARAM, LEVEL_CV_PARAM, PARAMS_LEN };
	enum InputId { VOCT_INPUT, RATIO_INPUT, PHASE_INPUT, DETUNE_INPUT, WAVE_INPUT, SYNC_INPUT,
		FINE_INPUT, VOICES_INPUT, WAVE_Y_INPUT, LEVEL_INPUT, INPUTS_LEN };
	enum OutputId { X_OUTPUT, Y_OUTPUT, Z_OUTPUT, OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	vector_osc::Engine engine;
	dsp::SchmittTrigger syncTrigger;

	VectorOsc() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(FREQ_PARAM, -12.f, 5.f, -2.f, "Frequency of X", " Hz", 2.f, dsp::FREQ_C4);
		configSwitch(RATIO_PARAM, 0.f, vector_osc::kRatioCount - 1.f, vector_osc::kRatioUnity, "Ratio of Y to X (Y speed : X speed)",
			std::vector<std::string>(vector_osc::kRatioNames, vector_osc::kRatioNames + vector_osc::kRatioCount));
		configParam(FINE_PARAM, -1.f, 1.f, 0.f, "Fine ratio: slightly off the chosen ratio so the figure turns slowly", " semitones", 0.f, 1.f);
		configParam(PHASE_PARAM, 0.f, 1.f, .25f, "Phase of Y against X", " degrees", 0.f, 360.f);
		configParam(VOICES_PARAM, 1.f, 8.f, 3.f, "Voices per side");
		paramQuantities[VOICES_PARAM]->snapEnabled = true;
		configParam(DETUNE_PARAM, 0.f, 1.f, .3f, "Detune: spread of the voices (0 is one clean line)", "%", 0.f, 100.f);
		configParam(WAVE_PARAM, 0.f, vector_osc::kWaveMax, 0.f, "Wave shape: sine, triangle, saw, square");
		configParam(WAVE_Y_PARAM, -1.f, 1.f, 0.f, "Wave shape of Y compared with X");
		configParam(LEVEL_PARAM, 0.f, 1.f, 1.f, "Level of X and Y", "%", 0.f, 100.f);
		// The attenuverters start at 100%, which is what each jack did before it had one.
		static const char* const cvNames[] = {"Ratio", "Phase", "Detune", "Wave", "Fine", "Voices", "Y wave", "Level"};
		for (int i = 0; i < 8; ++i)
			configParam(RATIO_CV_PARAM + i, -1.f, 1.f, 1.f, std::string(cvNames[i]) + " CV amount", "%", 0.f, 100.f);
		configInput(VOCT_INPUT, "Pitch (1 V per octave)");
		configInput(RATIO_INPUT, "Ratio step CV (1 V per step along the ratio list)");
		configInput(PHASE_INPUT, "Phase of Y CV (10 V is one full turn)");
		configInput(DETUNE_INPUT, "Detune CV (10 V is full)");
		configInput(WAVE_INPUT, "Wave shape CV (10 V is full)");
		configInput(SYNC_INPUT, "Sync: a rising edge restarts every voice");
		configInput(FINE_INPUT, "Fine ratio CV (5 V is one semitone)");
		configInput(VOICES_INPUT, "Voices CV (10 V sweeps 1 to 8)");
		configInput(WAVE_Y_INPUT, "Y wave CV (5 V is full)");
		configInput(LEVEL_INPUT, "Level CV (10 V is full)");
		configOutput(X_OUTPUT, "X (+/-5 V)");
		configOutput(Y_OUTPUT, "Y (+/-5 V)");
		configOutput(Z_OUTPUT, "Z brightness (0 to 10 V, bright where the trace moves slowly)");
	}
	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		engine.reset();
	}
	void process(const ProcessArgs& args) override {
		using namespace vector_osc;
		if (syncTrigger.process(inputs[SYNC_INPUT].getVoltage(), .1f, 2.f)) engine.reset();
		auto knob = [&](int id) { return params[id].getValue(); };
		auto volts = [&](int id) { return inputs[id].getVoltage(); };
		vector_osc::Settings s;
		const float pitch = sane(knob(FREQ_PARAM) + sane(volts(VOCT_INPUT), -12.f, 12.f), -12.f, 5.f);
		s.freq = dsp::FREQ_C4 * std::exp2(pitch);
		const int step = ratioStepWithCv(knob(RATIO_PARAM), volts(RATIO_INPUT), knob(RATIO_CV_PARAM));
		s.ratio = kRatios[step] * fineFactor(fineWithCv(knob(FINE_PARAM), volts(FINE_INPUT), knob(FINE_CV_PARAM)));
		s.phase = phaseWithCv(knob(PHASE_PARAM), volts(PHASE_INPUT), knob(PHASE_CV_PARAM));
		s.voices = voicesWithCv(knob(VOICES_PARAM), volts(VOICES_INPUT), knob(VOICES_CV_PARAM));
		s.detune = detuneWithCv(knob(DETUNE_PARAM), volts(DETUNE_INPUT), knob(DETUNE_CV_PARAM));
		s.wave = waveWithCv(knob(WAVE_PARAM), volts(WAVE_INPUT), knob(WAVE_CV_PARAM));
		s.waveOffsetY = waveYWithCv(knob(WAVE_Y_PARAM), volts(WAVE_Y_INPUT), knob(WAVE_Y_CV_PARAM));
		s.level = levelWithCv(knob(LEVEL_PARAM), volts(LEVEL_INPUT), knob(LEVEL_CV_PARAM));
		const vector_osc::Output o = engine.process(s, args.sampleTime);
		outputs[X_OUTPUT].setVoltage(o.x);
		outputs[Y_OUTPUT].setVoltage(o.y);
		outputs[Z_OUTPUT].setVoltage(o.z);
	}
};

#include "vector_osc_layout.hpp"

struct VectorOscWidget : ModuleWidget {
	VectorOscWidget(VectorOsc* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/VectorOsc.svg")));
		rasterlab_panel::addPanelHeader(this, kVectorOscPanelWidth, "25", "VECTOR OSC");
		PANEL_PLACE_ALL(VECTOR_OSC_LAYOUT)
	}
};

Model* modelVectorOsc = createModel<VectorOsc, VectorOscWidget>("VectorOsc");
