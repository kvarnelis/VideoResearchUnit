#include "plugin.hpp"
#include "panel_style.hpp"
#include "video_bus.hpp"

#include <atomic>
#include <cmath>
#include "freeze_frame_store.hpp"

struct FreezeFrame:Module{ enum ParamId{HOLD_PARAM,BLEND_PARAM,STROBE_PARAM,CLEAR_PARAM,PARAMS_LEN}; enum InputId{VIDEO_BUS_INPUT,HOLD_INPUT,TRIG_INPUT,BLEND_INPUT,STROBE_INPUT,INPUTS_LEN}; enum OutputId{VIDEO_BUS_OUTPUT,OUTPUTS_LEN}; enum LightId{SIGNAL_LIGHT,HOLD_LIGHT,LIGHTS_LEN}; VideoBusOwner videoBus; int freezeSlotId=1; int captureCounter=0; float strobePhase=0.f; dsp::SchmittTrigger captureTrigger; FreezeFrame(){config(PARAMS_LEN,INPUTS_LEN,OUTPUTS_LEN,LIGHTS_LEN); freezeSlotId=freezeFrameAllocateSlot(); configParam(HOLD_PARAM,0.f,1.f,0.f,"Hold"); configParam(BLEND_PARAM,0.f,1.f,1.f,"Blend"); configParam(STROBE_PARAM,0.f,12.f,0.f,"Strobe rate"," Hz"); configButton(CLEAR_PARAM,"Clear frame"); configInput(VIDEO_BUS_INPUT,"Video bus token"); configInput(HOLD_INPUT,"Hold CV"); configInput(TRIG_INPUT,"Capture trigger"); configInput(BLEND_INPUT,"Blend CV"); configInput(STROBE_INPUT,"Strobe CV"); configOutput(VIDEO_BUS_OUTPUT,"Video bus token");} ~FreezeFrame(){freezeFrameReleaseSlot(freezeSlotId);} void process(const ProcessArgs& args) override{ const int in=inputs[VIDEO_BUS_INPUT].isConnected()?videoBusIdFromVoltage(inputs[VIDEO_BUS_INPUT].getVoltage()):1; const int out=videoBus.id(); const bool active=videoBusIsActive(in)&&in!=out; const bool trig=inputs[TRIG_INPUT].isConnected()&&captureTrigger.process(inputs[TRIG_INPUT].getVoltage(),0.1f,2.f); const float strobe=clamp(params[STROBE_PARAM].getValue()+inputs[STROBE_INPUT].getVoltage()*0.25f,0.f,24.f); if(trig) captureCounter++; if(strobe>0.0001f){ strobePhase+=strobe*args.sampleTime; if(strobePhase>=1.f){ strobePhase-=std::floor(strobePhase); captureCounter++; }} else strobePhase=0.f; FreezeFrameParams p; p.freezeSlotId=freezeSlotId; p.captureCounter=captureCounter; p.hold=clamp(params[HOLD_PARAM].getValue()+inputs[HOLD_INPUT].getVoltage()*0.1f,0.f,1.f); p.blend=clamp(params[BLEND_PARAM].getValue()+inputs[BLEND_INPUT].getVoltage()*0.1f,0.f,1.f); p.strobe=strobe; p.clear=params[CLEAR_PARAM].getValue()>0.5f; if(active) videoBusPublishFreezeFrame(out,in,p); else videoBusClear(out); outputs[VIDEO_BUS_OUTPUT].setVoltage(videoBus.voltage()); lights[SIGNAL_LIGHT].setBrightnessSmooth(active?1.f:0.f,args.sampleTime); lights[HOLD_LIGHT].setBrightnessSmooth(p.hold,args.sampleTime);} json_t* dataToJson() override{json_t* r=json_object(); saveVideoBusId(r, videoBus); return r;} void dataFromJson(json_t* r) override{ loadVideoBusId(r, videoBus, "outputBusVoltage"); }};
struct FreezeFrameWidget : ModuleWidget {
	FreezeFrameWidget(FreezeFrame* module) {
		using namespace rasterlab_panel;
		const float W = 40.64f;	// 8 HP
		const float P = 13.0f;	// column pitch, so three 3.8 mm labels fit side by side
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/FreezeFrame.svg")));
		addPanelHeader(this, W, "15", "FREEZE FRAME");

		addKnobLabel(this, Vec(rowX(W, 3, 0, P), 32), "HOLD");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(rowX(W, 3, 0, P), 32)), module, FreezeFrame::HOLD_PARAM));
		addKnobLabel(this, Vec(rowX(W, 3, 1, P), 32), "STROBE");
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 3, 1, P), 32)), module, FreezeFrame::STROBE_PARAM));
		addKnobLabel(this, Vec(rowX(W, 3, 2, P), 32), "BLEND");
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(rowX(W, 3, 2, P), 32)), module, FreezeFrame::BLEND_PARAM));

		addKnobLabel(this, Vec(rowX(W, 3, 1, P), 54), "CLEAR", R_BUTTON);
		addParam(createParamCentered<LEDButton>(mm2px(Vec(rowX(W, 3, 1, P), 54)), module, FreezeFrame::CLEAR_PARAM));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(rowX(W, 3, 1, P), 54)), module, FreezeFrame::HOLD_LIGHT));

		addJackLabel(this, Vec(rowX(W, 3, 0, P), 74), "BUS IN");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 0, P), 74)), module, FreezeFrame::VIDEO_BUS_INPUT));
		addJackLabel(this, Vec(rowX(W, 3, 1, P), 74), "HOLD");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 1, P), 74)), module, FreezeFrame::HOLD_INPUT));
		addJackLabel(this, Vec(rowX(W, 3, 2, P), 74), "TRIG");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 2, P), 74)), module, FreezeFrame::TRIG_INPUT));

		addJackLabel(this, Vec(rowX(W, 3, 0, P), 92), "BLEND");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 0, P), 92)), module, FreezeFrame::BLEND_INPUT));
		addJackLabel(this, Vec(rowX(W, 3, 1, P), 92), "STROBE");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 3, 1, P), 92)), module, FreezeFrame::STROBE_INPUT));

		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 3, 1, P), 116), "BUS", module, FreezeFrame::VIDEO_BUS_OUTPUT);
		addJackLabel(this, Vec(rowX(W, 3, 2, P), 116), "SIG");
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(rowX(W, 3, 2, P), 116)), module, FreezeFrame::SIGNAL_LIGHT));
	}
};

Model* modelFreezeFrame=createModel<FreezeFrame,FreezeFrameWidget>("FreezeFrame");
