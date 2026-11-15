#include "plugin.hpp"
#include "panel_style.hpp"
#include "vector_osc_math.hpp"

// VRU-25 Vector Osc: a CV and audio source for Scope Trace's X IN and Y IN. No video bus.
struct VectorOsc : Module {
	enum ParamId { FREQ_PARAM, RATIO_PARAM, FINE_PARAM, PHASE_PARAM, VOICES_PARAM, DETUNE_PARAM,
		WAVE_PARAM, WAVE_Y_PARAM, LEVEL_PARAM, PARAMS_LEN };
	enum InputId { VOCT_INPUT, RATIO_INPUT, PHASE_INPUT, DETUNE_INPUT, WAVE_INPUT, SYNC_INPUT, INPUTS_LEN };
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
		configParam(WAVE_PARAM, 0.f, 1.f, 0.f, "Wave shape: sine, triangle, saw, square");
		configParam(WAVE_Y_PARAM, -1.f, 1.f, 0.f, "Wave shape of Y compared with X");
		configParam(LEVEL_PARAM, 0.f, 1.f, 1.f, "Level of X and Y", "%", 0.f, 100.f);
		configInput(VOCT_INPUT, "Pitch (1 V per octave)");
		configInput(RATIO_INPUT, "Ratio step (1 V per step along the ratio list)");
		configInput(PHASE_INPUT, "Phase of Y (10 V is one full turn)");
		configInput(DETUNE_INPUT, "Detune (10 V is full)");
		configInput(WAVE_INPUT, "Wave shape (10 V is full)");
		configInput(SYNC_INPUT, "Sync: a rising edge restarts every voice");
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
		vector_osc::Settings s;
		const float pitch = sane(params[FREQ_PARAM].getValue() + sane(inputs[VOCT_INPUT].getVoltage(), -12.f, 12.f), -12.f, 5.f);
		s.freq = dsp::FREQ_C4 * std::exp2(pitch);
		const int step = ratioIndex(params[RATIO_PARAM].getValue() + std::round(sane(inputs[RATIO_INPUT].getVoltage(), -20.f, 20.f)));
		s.ratio = kRatios[step] * fineFactor(params[FINE_PARAM].getValue());
		s.phase = params[PHASE_PARAM].getValue() + sane(inputs[PHASE_INPUT].getVoltage(), -20.f, 20.f) * .1f;
		s.voices = static_cast<int>(std::lround(params[VOICES_PARAM].getValue()));
		s.detune = sane(params[DETUNE_PARAM].getValue() + sane(inputs[DETUNE_INPUT].getVoltage(), -20.f, 20.f) * .1f, 0.f, 1.f);
		s.wave = sane(params[WAVE_PARAM].getValue() + sane(inputs[WAVE_INPUT].getVoltage(), -20.f, 20.f) * .1f, 0.f, 1.f);
		s.waveOffsetY = params[WAVE_Y_PARAM].getValue();
		s.level = params[LEVEL_PARAM].getValue();
		const vector_osc::Output o = engine.process(s, args.sampleTime);
		outputs[X_OUTPUT].setVoltage(o.x);
		outputs[Y_OUTPUT].setVoltage(o.y);
		outputs[Z_OUTPUT].setVoltage(o.z);
	}
};

struct VectorOscWidget : ModuleWidget {
	VectorOscWidget(VectorOsc* module) {
		using namespace rasterlab_panel;
		constexpr float W = 60.96f; // 12 HP
		constexpr float P = 18.f;   // column pitch, so a label like VOICES clears its neighbours
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/VectorOsc.svg")));
		addPanelHeader(this, W, "25", "VECTOR OSC");
		auto knob = [&](int col, float y, const char* label, int id, bool snap = false) {
			const Vec p(rowX(W, 3, col, P), y);
			addKnobLabel(this, p, label);
			if (snap) addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(p), module, id));
			else addParam(createParamCentered<RoundBlackKnob>(mm2px(p), module, id));
		};
		knob(0, 28.f, "FREQ", VectorOsc::FREQ_PARAM);
		knob(1, 28.f, "RATIO", VectorOsc::RATIO_PARAM, true);
		knob(2, 28.f, "FINE", VectorOsc::FINE_PARAM);
		knob(0, 49.f, "PHASE", VectorOsc::PHASE_PARAM);
		knob(1, 49.f, "VOICES", VectorOsc::VOICES_PARAM, true);
		knob(2, 49.f, "DETUNE", VectorOsc::DETUNE_PARAM);
		knob(0, 70.f, "WAVE", VectorOsc::WAVE_PARAM);
		knob(1, 70.f, "Y WAVE", VectorOsc::WAVE_Y_PARAM);
		knob(2, 70.f, "LEVEL", VectorOsc::LEVEL_PARAM);
		auto jack = [&](int col, float y, const char* label, int id) {
			const Vec p(rowX(W, 3, col, P), y);
			addJackLabel(this, p, label);
			addInput(createInputCentered<PJ301MPort>(mm2px(p), module, id));
		};
		jack(0, 88.f, "V/OCT", VectorOsc::VOCT_INPUT);
		jack(1, 88.f, "RATIO", VectorOsc::RATIO_INPUT);
		jack(2, 88.f, "PHASE", VectorOsc::PHASE_INPUT);
		jack(0, 103.f, "DETUNE", VectorOsc::DETUNE_INPUT);
		jack(1, 103.f, "WAVE", VectorOsc::WAVE_INPUT);
		jack(2, 103.f, "SYNC", VectorOsc::SYNC_INPUT);
		const char* names[] = {"X", "Y", "Z"};
		for (int i = 0; i < 3; ++i)
			addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 3, i, P), 120.f), names[i], module, VectorOsc::X_OUTPUT + i);
	}
};

Model* modelVectorOsc = createModel<VectorOsc, VectorOscWidget>("VectorOsc");
