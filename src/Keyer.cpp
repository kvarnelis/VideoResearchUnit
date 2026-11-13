#include "plugin.hpp"
#include "panel_style.hpp"
#include "video_bus.hpp"

#include <atomic>
#include <cmath>

struct Keyer : Module {
	enum ParamId { THRESH_PARAM, SOFT_PARAM, GAIN_PARAM, MODE_PARAM, INVERT_PARAM, HUE_PARAM, MIX_PARAM, PARAMS_LEN };
	enum InputId { VIDEO_BUS_INPUT, THRESH_INPUT, SOFT_INPUT, GAIN_INPUT, MIX_INPUT, INPUTS_LEN };
	enum OutputId { VIDEO_BUS_OUTPUT, OUTPUTS_LEN }; enum LightId { SIGNAL_LIGHT, LIGHTS_LEN }; VideoBusOwner videoBus;
	Keyer(){ config(PARAMS_LEN,INPUTS_LEN,OUTPUTS_LEN,LIGHTS_LEN); configParam(THRESH_PARAM,0.f,1.f,0.5f,"Threshold"); configParam(SOFT_PARAM,0.001f,1.f,0.12f,"Softness"); configParam(GAIN_PARAM,0.f,2.f,1.f,"Matte gain"); configSwitch(MODE_PARAM,0.f,2.f,0.f,"Output mode",{"Matte","Color","Fill"}); configSwitch(INVERT_PARAM,0.f,1.f,0.f,"Invert matte",{"Off","On"}); configParam(HUE_PARAM,0.f,1.f,0.33f,"Key color hue"); configParam(MIX_PARAM,0.f,1.f,1.f,"Mix"); configInput(VIDEO_BUS_INPUT,"Video bus token"); configInput(THRESH_INPUT,"Threshold CV"); configInput(SOFT_INPUT,"Softness CV"); configInput(GAIN_INPUT,"Gain CV"); configInput(MIX_INPUT,"Mix CV"); configOutput(VIDEO_BUS_OUTPUT,"Video bus token"); }
	void process(const ProcessArgs& args) override { const int inBus=inputs[VIDEO_BUS_INPUT].isConnected()?videoBusIdFromVoltage(inputs[VIDEO_BUS_INPUT].getVoltage()):1; const int outBus=videoBus.id(); const bool active=videoBusIsActive(inBus)&&inBus!=outBus; KeyerParams p; p.threshold=clamp(params[THRESH_PARAM].getValue()+inputs[THRESH_INPUT].getVoltage()*0.1f,0.f,1.f); p.softness=clamp(params[SOFT_PARAM].getValue()+inputs[SOFT_INPUT].getVoltage()*0.1f,0.001f,1.f); p.gain=clamp(params[GAIN_PARAM].getValue()+inputs[GAIN_INPUT].getVoltage()*0.1f,0.f,2.f); p.mode=(int)std::round(params[MODE_PARAM].getValue()); p.invert=params[INVERT_PARAM].getValue()>0.5f; p.hue=clamp(params[HUE_PARAM].getValue(),0.f,1.f); p.mix=clamp(params[MIX_PARAM].getValue()+inputs[MIX_INPUT].getVoltage()*0.1f,0.f,1.f); if(active) videoBusPublishKeyer(outBus,inBus,p); else videoBusClear(outBus); outputs[VIDEO_BUS_OUTPUT].setVoltage(videoBus.voltage()); lights[SIGNAL_LIGHT].setBrightnessSmooth(active?1.f:0.f,args.sampleTime); }
	json_t* dataToJson() override { json_t* r=json_object(); saveVideoBusId(r, videoBus); return r; } void dataFromJson(json_t* r) override { loadVideoBusId(r, videoBus, "outputBusVoltage"); }
};
struct KeyerWidget : ModuleWidget {
	KeyerWidget(Keyer* module) {
		using namespace rasterlab_panel;
		const float W = 50.8f;	// 10 HP
		const float P = 12.2f;	// column pitch, so four 3.8 mm labels fit side by side
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Keyer.svg")));
		addPanelHeader(this, W, "12", "KEYER");

		addKnobLabel(this, Vec(rowX(W, 4, 0, P), 32), "INV");
		addParam(createParamCentered<CKSS>(mm2px(Vec(rowX(W, 4, 0, P), 32)), module, Keyer::INVERT_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 1, P), 32), "THRESH");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(rowX(W, 4, 1, P), 32)), module, Keyer::THRESH_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 2, P), 32), "GAIN");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(rowX(W, 4, 2, P), 32)), module, Keyer::GAIN_PARAM));

		addKnobLabel(this, Vec(rowX(W, 4, 0, P), 54), "MODE", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 0, P), 54)), module, Keyer::MODE_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 1, P), 54), "SOFT", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 1, P), 54)), module, Keyer::SOFT_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 2, P), 54), "HUE", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 2, P), 54)), module, Keyer::HUE_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 3, P), 54), "MIX", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 3, P), 54)), module, Keyer::MIX_PARAM));

		addJackLabel(this, Vec(rowX(W, 4, 0, P), 74), "BUS IN");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 0, P), 74)), module, Keyer::VIDEO_BUS_INPUT));
		addJackLabel(this, Vec(rowX(W, 4, 1, P), 74), "THR");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 1, P), 74)), module, Keyer::THRESH_INPUT));
		addJackLabel(this, Vec(rowX(W, 4, 2, P), 74), "SOFT");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 2, P), 74)), module, Keyer::SOFT_INPUT));
		addJackLabel(this, Vec(rowX(W, 4, 3, P), 74), "GAIN");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 3, P), 74)), module, Keyer::GAIN_INPUT));

		addJackLabel(this, Vec(rowX(W, 4, 0, P), 92), "MIX");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 0, P), 92)), module, Keyer::MIX_INPUT));

		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 4, 1, P), 114), "BUS", module, Keyer::VIDEO_BUS_OUTPUT);
		addJackLabel(this, Vec(rowX(W, 4, 2, P), 114), "SIG");
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(rowX(W, 4, 2, P), 114)), module, Keyer::SIGNAL_LIGHT));
	}
};

Model* modelKeyer=createModel<Keyer,KeyerWidget>("Keyer");
