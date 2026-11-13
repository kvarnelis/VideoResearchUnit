#include "plugin.hpp"
#include "panel_style.hpp"
#include "video_bus.hpp"

#include <atomic>
#include <cmath>

struct BusMixer:Module{ enum ParamId{MIX_PARAM,MODE_PARAM,MATTE_PARAM,INV_PARAM,PARAMS_LEN}; enum InputId{BUS_A_INPUT,BUS_B_INPUT,MATTE_BUS_INPUT,MIX_INPUT,MATTE_INPUT,INPUTS_LEN}; enum OutputId{VIDEO_BUS_OUTPUT,OUTPUTS_LEN}; enum LightId{SIGNAL_LIGHT,LIGHTS_LEN}; VideoBusOwner videoBus; BusMixer(){config(PARAMS_LEN,INPUTS_LEN,OUTPUTS_LEN,LIGHTS_LEN); configParam(MIX_PARAM,0.f,1.f,0.5f,"Mix"); configSwitch(MODE_PARAM,0.f,6.f,0.f,"Blend mode",{"Mix","Add","Multiply","Screen","Difference","Max","Matte"}); configParam(MATTE_PARAM,0.f,1.f,0.f,"Matte amount"); configSwitch(INV_PARAM,0.f,1.f,0.f,"Invert matte",{"Off","On"}); configInput(BUS_A_INPUT,"Video bus A token"); configInput(BUS_B_INPUT,"Video bus B token"); configInput(MATTE_BUS_INPUT,"Matte bus token"); configInput(MIX_INPUT,"Mix CV"); configInput(MATTE_INPUT,"Matte amount CV"); configOutput(VIDEO_BUS_OUTPUT,"Video bus token");}
	void process(const ProcessArgs& args) override{
		// Unpatched B and MAT BUS mean "absent" (not bus 1). A/input follows the processor default (bus 1).
		const int a=inputs[BUS_A_INPUT].isConnected()?videoBusIdFromVoltage(inputs[BUS_A_INPUT].getVoltage()):1;
		const int b=inputs[BUS_B_INPUT].isConnected()?videoBusIdFromVoltage(inputs[BUS_B_INPUT].getVoltage()):0;
		const int m=inputs[MATTE_BUS_INPUT].isConnected()?videoBusIdFromVoltage(inputs[MATTE_BUS_INPUT].getVoltage()):0;
		const int out=videoBus.id();
		const bool active=videoBusIsActive(a)&&a!=out;
		BusMixerParams p; p.busB=b; p.matteBus=m; p.mix=clamp(params[MIX_PARAM].getValue()+inputs[MIX_INPUT].getVoltage()*0.1f,0.f,1.f); p.mode=(int)std::round(params[MODE_PARAM].getValue()); p.matteAmount=clamp(params[MATTE_PARAM].getValue()+inputs[MATTE_INPUT].getVoltage()*0.1f,0.f,1.f); p.invertMatte=params[INV_PARAM].getValue()>0.5f;
		if(active) videoBusPublishBusMixer(out,a,p); else videoBusClear(out);
		outputs[VIDEO_BUS_OUTPUT].setVoltage(videoBus.voltage()); lights[SIGNAL_LIGHT].setBrightnessSmooth(active?1.f:0.f,args.sampleTime);}
	json_t* dataToJson() override{json_t* r=json_object(); saveVideoBusId(r, videoBus); return r;} void dataFromJson(json_t* r) override{ loadVideoBusId(r, videoBus, "outputBusVoltage"); }};
struct BusMixerWidget : ModuleWidget {
	BusMixerWidget(BusMixer* module) {
		using namespace rasterlab_panel;
		const float W = 40.64f;	// 8 HP
		const float P = 13.0f;	// column pitch, so three 3.8 mm labels fit side by side
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/BusMixer.svg")));
		addPanelHeader(this, W, "13", "BUS MIXER");

		addKnobLabel(this, Vec(W * 0.5f, 32), "MIX");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(W * 0.5f, 32)), module, BusMixer::MIX_PARAM));

		addKnobLabel(this, Vec(rowX(W, 3, 0, P), 54), "MODE", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 3, 0, P), 54)), module, BusMixer::MODE_PARAM));
		addKnobLabel(this, Vec(rowX(W, 3, 1, P), 54), "MATTE", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 3, 1, P), 54)), module, BusMixer::MATTE_PARAM));
		addKnobLabel(this, Vec(rowX(W, 3, 2, P), 54), "INV M", R_SMALL);
		addParam(createParamCentered<CKSS>(mm2px(Vec(rowX(W, 3, 2, P), 54)), module, BusMixer::INV_PARAM));

		addJackLabel(this, Vec(rowX(W, 3, 0, P), 74), "BUS A");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 0, P), 74)), module, BusMixer::BUS_A_INPUT));
		addJackLabel(this, Vec(rowX(W, 3, 1, P), 74), "BUS B");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 1, P), 74)), module, BusMixer::BUS_B_INPUT));
		addJackLabel(this, Vec(rowX(W, 3, 2, P), 74), "MATTE\nBUS");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 2, P), 74)), module, BusMixer::MATTE_BUS_INPUT));

		addJackLabel(this, Vec(rowX(W, 3, 0, P), 92), "MIX");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 0, P), 92)), module, BusMixer::MIX_INPUT));
		addJackLabel(this, Vec(rowX(W, 3, 1, P), 92), "MATTE");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 1, P), 92)), module, BusMixer::MATTE_INPUT));

		addPlatedOutput<PJ301MPort>(this, Vec(W * 0.5f, 110), "BUS", module, BusMixer::VIDEO_BUS_OUTPUT);
		addJackLabel(this, Vec(W * 0.5f + GRID_MM, 110), "SIG");
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(W * 0.5f + GRID_MM, 110)), module, BusMixer::SIGNAL_LIGHT));
	}
};

Model* modelBusMixer=createModel<BusMixer,BusMixerWidget>("BusMixer");
