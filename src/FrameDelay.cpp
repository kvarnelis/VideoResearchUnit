#include "plugin.hpp"
#include "panel_style.hpp"
#include "cv_param.hpp"
#include "frame_ring.hpp"
#include "video_bus.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>

// VRU-27 Frame Delay: one picture in, the same picture a few frames later out. A patch loop is legal when it goes
// through one of these (docs/architecture.md, "Frame Delay"). The module only publishes the settings and a count of
// ticks; the stored pictures, and the drawing of the source, are the renderer's (renderFrameDelayLeaf and
// drawDelaySources in video_graph_renderer.cpp).
//
// A tick is 1/60 s of audio time, the same clock Feedback uses, so every Display, every picture size, the public API
// and the offline tool see the same delayed picture whatever their own frame rates are.

namespace {
constexpr double kTickSeconds = 1.0 / 60.0;
// Ten volts moves FRAMES across its whole range.
constexpr float kFramesPerVolt = static_cast<float>(kFrameDelayMaxFrames - 1) / 10.f;
}

struct FrameDelayQuantity : ParamQuantity {
	std::string getDescription() override;
	std::string getDisplayValueString() override {
		const int frames = static_cast<int>(std::round(getValue()));
		char text[64];
		std::snprintf(text, sizeof(text), "%d %s (%d ms)", frames, frames == 1 ? "frame" : "frames", static_cast<int>(std::round(frames * 1000.0 / 60.0)));
		return text;
	}
};

struct FrameDelay : Module {
	enum ParamId { FRAMES_PARAM, FRAMES_CV_PARAM, PARAMS_LEN };
	enum InputId { VIDEO_BUS_INPUT, FRAMES_INPUT, INPUTS_LEN };
	enum OutputId { VIDEO_BUS_OUTPUT, OUTPUTS_LEN };
	enum LightId { SIGNAL_LIGHT, LIGHTS_LEN };
	VideoBusOwner videoBus;
	int ringSlotId = 0;
	int frameCounter = 0;
	double tickTime = 0.0;
	// Changes when the module is reset; the renderer clears the stored pictures when it sees a new value.
	std::atomic<int> clearCounter {0};

	FrameDelay() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		ringSlotId = frameRingAllocateSlot();
		frameRingSetCapacity(ringSlotId, 2);
		auto* frames = configParam<FrameDelayQuantity>(FRAMES_PARAM, 1.f, static_cast<float>(kFrameDelayMaxFrames), 1.f, "Frames", " frames");
		frames->snapEnabled = true;
		frames->description = "How many frames old the picture is. A frame is 1/60 of a second. The delay starts out black, so a loop through it begins from nothing.";
		configParam(FRAMES_CV_PARAM, -1.f, 1.f, 1.f, "Frames CV amount", "%", 0.f, 100.f);
		configInput(VIDEO_BUS_INPUT, "Video bus token");
		configInput(FRAMES_INPUT, "Frames CV (10 V is the longest delay)");
		configOutput(VIDEO_BUS_OUTPUT, "Video bus token");
	}
	~FrameDelay() {
		frameRingReleaseSlot(ringSlotId);
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		const int next = clearCounter.load(std::memory_order_relaxed);
		clearCounter.store(next >= (1 << 30) ? 1 : next + 1, std::memory_order_relaxed);
	}

	void process(const ProcessArgs& args) override {
		tickTime += args.sampleTime;
		while (tickTime >= kTickSeconds) {
			tickTime -= kTickSeconds;
			frameCounter = frameCounter >= (1 << 30) ? 1 : frameCounter + 1;
		}
		const int inBus = inputs[VIDEO_BUS_INPUT].isConnected() ? videoBusIdFromVoltage(inputs[VIDEO_BUS_INPUT].getVoltage()) : 1;
		const int outBus = videoBus.id();
		const bool active = videoBusIsActive(inBus) && inBus != outBus;
		const float frames = rasterlab_cv::modulated(params[FRAMES_PARAM].getValue(), inputs[FRAMES_INPUT].getVoltage(),
			params[FRAMES_CV_PARAM].getValue(), kFramesPerVolt, 1.f, static_cast<float>(kFrameDelayMaxFrames));
		FrameDelayParams p;
		p.ringSlotId = ringSlotId;
		p.frameCounter = frameCounter;
		p.clearCounter = clearCounter.load(std::memory_order_relaxed);
		p.frames = static_cast<int>(std::lround(frames));
		if (active) videoBusPublishFrameDelay(outBus, inBus, p);
		else videoBusClear(outBus);
		outputs[VIDEO_BUS_OUTPUT].setVoltage(videoBus.voltage());
		lights[SIGNAL_LIGHT].setBrightnessSmooth(active ? 1.f : 0.f, args.sampleTime);
	}
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		saveVideoBusId(rootJ, videoBus);
		return rootJ;
	}
	void dataFromJson(json_t* rootJ) override {
		loadVideoBusId(rootJ, videoBus, "outputBusVoltage");
	}
};

static const char* const kByteStoreNote = "Stored as 8-bit: with full precision on, a float store of this size would pass 256 MB, so the pictures are rounded to 8 bits here.";

std::string FrameDelayQuantity::getDescription() {
	std::string text = ParamQuantity::getDescription();
	const FrameDelay* delay = dynamic_cast<const FrameDelay*>(module);
	if (delay && frameRingByteFallback(delay->ringSlotId)) {
		text += "\n";
		text += kByteStoreNote;
	}
	return text;
}

struct FrameDelayWidget : ModuleWidget {
	FrameDelayWidget(FrameDelay* module) {
		using namespace rasterlab_panel;
		const float W = 35.56f;  // 7 HP
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/FrameDelay.svg")));
		addPanelHeader(this, W, "27", "FRAME DELAY", 4.2f);

		const float x = W * 0.5f;
		const Vec knob(x, 40.f);
		addKnobLabel(this, knob, "FRAMES");
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(knob), module, FrameDelay::FRAMES_PARAM));
		const Vec trim(x, 56.f);
		addKnobLabel(this, trim, "CV AMT", R_TRIM);
		addParam(createParamCentered<Trimpot>(mm2px(trim), module, FrameDelay::FRAMES_CV_PARAM));
		const Vec cv(x, 71.f);
		addJackLabel(this, cv, "CV");
		addInput(createInputCentered<PJ301MPort>(mm2px(cv), module, FrameDelay::FRAMES_INPUT));

		const float rowY = 118.f;
		const Vec busIn(rowX(W, 3, 0, 12.f), rowY);
		addJackLabel(this, busIn, "BUS IN");
		addInput(createInputCentered<PJ301MPort>(mm2px(busIn), module, FrameDelay::VIDEO_BUS_INPUT));
		const Vec sig(rowX(W, 3, 1, 12.f), rowY);
		addJackLabel(this, sig, "SIG");
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(sig), module, FrameDelay::SIGNAL_LIGHT));
		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 3, 2, 12.f), rowY), "BUS", module, FrameDelay::VIDEO_BUS_OUTPUT);
	}

	void appendContextMenu(ui::Menu* menu) override {
		FrameDelay* delay = dynamic_cast<FrameDelay*>(module);
		if (!delay || !frameRingByteFallback(delay->ringSlotId)) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Stored as 8-bit (a float store would pass 256 MB)"));
	}
};

Model* modelFrameDelay = createModel<FrameDelay, FrameDelayWidget>("FrameDelay");
