#include "plugin.hpp"
#include "panel_style.hpp"
#include "video_bus.hpp"

#include <atomic>
#include <cmath>

struct WaveShaper:Module{ enum ParamId{DRIVE_PARAM,BIAS_PARAM,FOLD_PARAM,QUANT_PARAM,MIRROR_PARAM,CONTRAST_PARAM,MODE_PARAM,MIX_PARAM,PARAMS_LEN}; enum InputId{VIDEO_BUS_INPUT,DRIVE_INPUT,BIAS_INPUT,FOLD_INPUT,MIX_INPUT,INPUTS_LEN}; enum OutputId{VIDEO_BUS_OUTPUT,OUTPUTS_LEN}; enum LightId{SIGNAL_LIGHT,LIGHTS_LEN}; VideoBusOwner videoBus; WaveShaper(){config(PARAMS_LEN,INPUTS_LEN,OUTPUTS_LEN,LIGHTS_LEN); configParam(DRIVE_PARAM,0.f,4.f,1.f,"Drive"); configParam(BIAS_PARAM,-1.f,1.f,0.f,"Bias"); configParam(FOLD_PARAM,0.f,1.f,0.f,"Fold"); configParam(QUANT_PARAM,0.f,1.f,0.f,"Quantize"); configParam(MIRROR_PARAM,0.f,1.f,0.f,"Mirror"); configParam(CONTRAST_PARAM,0.f,2.f,1.f,"Contrast"); configSwitch(MODE_PARAM,0.f,1.f,0.f,"Mode",{"RGB","Luma"}); configParam(MIX_PARAM,0.f,1.f,1.f,"Mix"); configInput(VIDEO_BUS_INPUT,"Video bus token"); configInput(DRIVE_INPUT,"Drive CV"); configInput(BIAS_INPUT,"Bias CV"); configInput(FOLD_INPUT,"Fold CV"); configInput(MIX_INPUT,"Mix CV"); configOutput(VIDEO_BUS_OUTPUT,"Video bus token");}
	void process(const ProcessArgs& args) override{ const int in=inputs[VIDEO_BUS_INPUT].isConnected()?videoBusIdFromVoltage(inputs[VIDEO_BUS_INPUT].getVoltage()):1; const int out=videoBus.id(); const bool active=videoBusIsActive(in)&&in!=out; WaveShaperParams p; p.drive=clamp(params[DRIVE_PARAM].getValue()+inputs[DRIVE_INPUT].getVoltage()*0.2f,0.f,4.f); p.bias=clamp(params[BIAS_PARAM].getValue()+inputs[BIAS_INPUT].getVoltage()*0.1f,-1.f,1.f); p.fold=clamp(params[FOLD_PARAM].getValue()+inputs[FOLD_INPUT].getVoltage()*0.1f,0.f,1.f); p.quantize=clamp(params[QUANT_PARAM].getValue(),0.f,1.f); p.mirror=clamp(params[MIRROR_PARAM].getValue(),0.f,1.f); p.contrast=clamp(params[CONTRAST_PARAM].getValue(),0.f,2.f); p.mode=(int)std::round(params[MODE_PARAM].getValue()); p.mix=clamp(params[MIX_PARAM].getValue()+inputs[MIX_INPUT].getVoltage()*0.1f,0.f,1.f); if(active) videoBusPublishWaveShaper(out,in,p); else videoBusClear(out); outputs[VIDEO_BUS_OUTPUT].setVoltage(videoBus.voltage()); lights[SIGNAL_LIGHT].setBrightnessSmooth(active?1.f:0.f,args.sampleTime);}
	json_t* dataToJson() override{json_t* r=json_object(); saveVideoBusId(r, videoBus); return r;} void dataFromJson(json_t* r) override{ loadVideoBusId(r, videoBus, "outputBusVoltage"); }};
struct WaveShaperWidget : ModuleWidget {
	WaveShaperWidget(WaveShaper* module) {
		using namespace rasterlab_panel;
		const float W = 40.64f;	// 8 HP
		const float P = 13.0f;	// column pitch, so QUANT, MIRROR and CONTR clear each other
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/WaveShaper.svg")));
		addPanelHeader(this, W, "11", "WAVE SHAPER");

		addKnobLabel(this, Vec(rowX(W, 3, 0, P), 32), "DRIVE");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(rowX(W, 3, 0, P), 32)), module, WaveShaper::DRIVE_PARAM));
		addKnobLabel(this, Vec(rowX(W, 3, 1, P), 32), "BIAS");
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 3, 1, P), 32)), module, WaveShaper::BIAS_PARAM));
		addKnobLabel(this, Vec(rowX(W, 3, 2, P), 32), "FOLD");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(rowX(W, 3, 2, P), 32)), module, WaveShaper::FOLD_PARAM));

		addKnobLabel(this, Vec(rowX(W, 3, 0, P), 54), "QUANT", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 3, 0, P), 54)), module, WaveShaper::QUANT_PARAM));
		addKnobLabel(this, Vec(rowX(W, 3, 1, P), 54), "MIRROR", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 3, 1, P), 54)), module, WaveShaper::MIRROR_PARAM));
		addKnobLabel(this, Vec(rowX(W, 3, 2, P), 54), "CONTR", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 3, 2, P), 54)), module, WaveShaper::CONTRAST_PARAM));

		addKnobLabel(this, Vec(rowX(W, 3, 0, P), 74), "MODE", R_SMALL);
		addParam(createParamCentered<CKSS>(mm2px(Vec(rowX(W, 3, 0, P), 74)), module, WaveShaper::MODE_PARAM));
		addKnobLabel(this, Vec(rowX(W, 3, 1, P), 74), "MIX", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 3, 1, P), 74)), module, WaveShaper::MIX_PARAM));

		addJackLabel(this, Vec(rowX(W, 3, 0, P), 92), "BUS IN");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 0, P), 92)), module, WaveShaper::VIDEO_BUS_INPUT));
		addJackLabel(this, Vec(rowX(W, 3, 1, P), 92), "DRIVE");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 1, P), 92)), module, WaveShaper::DRIVE_INPUT));
		addJackLabel(this, Vec(rowX(W, 3, 2, P), 92), "BIAS");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 2, P), 92)), module, WaveShaper::BIAS_INPUT));

		addJackLabel(this, Vec(rowX(W, 3, 0, P), 106), "FOLD");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 0, P), 106)), module, WaveShaper::FOLD_INPUT));
		addJackLabel(this, Vec(rowX(W, 3, 1, P), 106), "MIX");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 1, P), 106)), module, WaveShaper::MIX_INPUT));

		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 3, 1, P), 116), "BUS", module, WaveShaper::VIDEO_BUS_OUTPUT);
		addJackLabel(this, Vec(rowX(W, 3, 2, P), 116), "SIG");
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(rowX(W, 3, 2, P), 116)), module, WaveShaper::SIGNAL_LIGHT));
	}
};

Model* modelWaveShaper=createModel<WaveShaper,WaveShaperWidget>("WaveShaper");
