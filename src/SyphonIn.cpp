#include "plugin.hpp"
#include "panel_style.hpp"
#include "syphon_bridge.hpp"
#include "video_bus.hpp"
#include "video_external_frame.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

struct SyphonIn : Module {
	enum ParamId {
		ENABLE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		INPUTS_LEN
	};
	enum OutputId {
		VIDEO_BUS_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		BRIDGE_LIGHT,
		FRAME_LIGHT,
		SIGNAL_LIGHT,
		LIGHTS_LEN
	};

	std::atomic<bool> bridgeReady {false};
	std::atomic<bool> hasFrame {false};
	std::atomic<bool> outputActive {false};
	std::atomic<int> latestWidth {0};
	std::atomic<int> latestHeight {0};
	VideoBusOwner videoBus;
	int externalFrameId = 0;
	// Server chosen from the right-click menu. Empty means no server is received.
	// Only the UI thread reads or writes this string.
	std::string selectedServerName;

	SyphonIn() {
		externalFrameId = videoExternalFrameAllocate();
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configSwitch(ENABLE_PARAM, 0.f, 1.f, 1.f, "Receive", {"Off", "On"});
		configOutput(VIDEO_BUS_OUTPUT, "Video bus token");
	}

	~SyphonIn() override {
		videoExternalFrameRelease(externalFrameId);
	}

	void process(const ProcessArgs& args) override {
		const bool enabled = params[ENABLE_PARAM].getValue() > 0.5f;
		const bool active = enabled && hasFrame.load(std::memory_order_relaxed);
		const int outputBusId = videoBus.id();
		if (active) {
			videoBusPublishExternalFrame(outputBusId, externalFrameId);
		}
		else {
			videoBusClear(outputBusId);
		}

		outputActive.store(active, std::memory_order_relaxed);
		outputs[VIDEO_BUS_OUTPUT].setVoltage(videoBus.voltage());
		lights[BRIDGE_LIGHT].setBrightnessSmooth(bridgeReady.load(std::memory_order_relaxed) ? 1.f : 0.f, args.sampleTime);
		lights[FRAME_LIGHT].setBrightnessSmooth(hasFrame.load(std::memory_order_relaxed) ? 1.f : 0.f, args.sampleTime);
		lights[SIGNAL_LIGHT].setBrightnessSmooth(active ? 1.f : 0.f, args.sampleTime);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		saveVideoBusId(rootJ, videoBus);
		json_object_set_new(rootJ, "selectedServerName", json_string(selectedServerName.c_str()));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		loadVideoBusId(rootJ, videoBus, "outputBusVoltage");

		json_t* selectedServerNameJ = json_object_get(rootJ, "selectedServerName");
		if (selectedServerNameJ && json_is_string(selectedServerNameJ)) {
			selectedServerName = json_string_value(selectedServerNameJ);
		}
	}

	bool isReceiveEnabled() {
		return params[ENABLE_PARAM].getValue() > 0.5f;
	}
};

struct SyphonInReceiverWidget : widget::OpenGlWidget {
	SyphonIn* module = nullptr;
	std::unique_ptr<SyphonInputBridge> bridge;
	std::vector<unsigned char> rgba;
	std::string connectedServerName;

	SyphonInReceiverWidget() {
		bridge = createSyphonInputBridge();
	}

	void drawFramebuffer() override {
		if (!module || !bridge) {
			return;
		}

		const bool bridgeAvailable = bridge->isAvailable();
		module->bridgeReady.store(bridgeAvailable, std::memory_order_relaxed);
		if (!module->isReceiveEnabled() || !bridgeAvailable) {
			module->hasFrame.store(false, std::memory_order_relaxed);
			connectedServerName.clear();
			return;
		}

		// No automatic choice: the user picks a server from the right-click menu.
		const std::string requestedServer = module->selectedServerName;
		if (requestedServer.empty()) {
			module->hasFrame.store(false, std::memory_order_relaxed);
			connectedServerName.clear();
			return;
		}

		if (connectedServerName != requestedServer) {
			if (!bridge->connect(requestedServer)) {
				module->hasFrame.store(false, std::memory_order_relaxed);
				connectedServerName.clear();
				return;
			}
			connectedServerName = requestedServer;
		}

		SyphonFrameInfo info;
		if (!bridge->copyLatestRgbaFrame(rgba, info) || rgba.empty()) {
			// No frame this draw, for example because the server has quit. The status
			// falls back to LINKED until a frame arrives again.
			module->hasFrame.store(false, std::memory_order_relaxed);
			return;
		}

		videoExternalFramePublish(module->externalFrameId, rgba.data(), info.width, info.height, info.timestampSeconds);
		module->hasFrame.store(true, std::memory_order_relaxed);
	}
};

// NO SYPHON when the bridge is not linked, LINKED when it is but no frame arrives, and while frames
// arrive: the server name on line 1 and the sending app's name smaller on line 2 (the name is
// "App / Server"), then the frame size. Long names are shortened with "..." to fit the readout.
// Sizes are in mm (see panel_style.hpp): the headline is READOUT_MM and the two detail lines are
// LABEL_MM, so every line reads at least as large as a control label. At 28.56 mm wide the screen
// holds 26.5 mm of text; NO SYPHON at READOUT_MM is 22.7 mm.
struct SyphonInStatusWidget : TransparentWidget {
	SyphonIn* module = nullptr;

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, 3.f);
		nvgFillColor(args.vg, nvgRGB(8, 10, 13));
		nvgFill(args.vg);

		const bool bridge = module && module->bridgeReady.load(std::memory_order_relaxed);
		const bool frame = module && module->hasFrame.load(std::memory_order_relaxed);
		const int width = module ? module->latestWidth.load(std::memory_order_relaxed) : 0;
		const int height = module ? module->latestHeight.load(std::memory_order_relaxed) : 0;
		const float textWidth = box.size.x - 6.f;
		const NVGcolor lit = nvgRGB(190, 255, 170);
		const NVGcolor dim = nvgRGB(210, 210, 210);

		rasterlab_panel::useLabelFont(args.vg);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		if (!bridge || !frame) {
			nvgFontSize(args.vg, mm2px(rasterlab_panel::READOUT_MM));
			nvgFillColor(args.vg, dim);
			const char* label = bridge ? "LINKED" : "NO SYPHON";
			nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.5f, label, nullptr);
		}
		else {
			std::string server = module->selectedServerName;
			std::string app;
			const size_t split = server.find(" / ");
			if (split != std::string::npos) {
				app = server.substr(0, split);
				server = server.substr(split + 3);
			}

			nvgFontSize(args.vg, mm2px(rasterlab_panel::READOUT_MM));
			nvgFillColor(args.vg, lit);
			nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.27f, rasterlab_panel::fitToWidth(args.vg, server, textWidth).c_str(), nullptr);

			if (!app.empty()) {
				nvgFontSize(args.vg, mm2px(rasterlab_panel::LABEL_MM));
				nvgFillColor(args.vg, nvgRGBA(190, 255, 170, 170));
				nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.47f, rasterlab_panel::fitToWidth(args.vg, app, textWidth).c_str(), nullptr);
			}

			if (width > 0 && height > 0) {
				const std::string size = std::to_string(width) + "x" + std::to_string(height);
				nvgFontSize(args.vg, mm2px(rasterlab_panel::LABEL_MM));
				nvgFillColor(args.vg, dim);
				nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.72f, size.c_str(), nullptr);
			}
		}

		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.5f, 0.5f, box.size.x - 1.f, box.size.y - 1.f, 3.f);
		nvgStrokeColor(args.vg, nvgRGBA(255, 255, 255, 46));
		nvgStrokeWidth(args.vg, 1.f);
		nvgStroke(args.vg);
	}
};

struct SyphonInWidget : ModuleWidget {
	SyphonIn* syphonModule = nullptr;
	SyphonInReceiverWidget* receiver = nullptr;

	SyphonInWidget(SyphonIn* module) {
		using namespace rasterlab_panel;
		const float W = 35.56f;	// 7 HP
		setModule(module);
		syphonModule = module;
		setPanel(createPanel(asset::plugin(pluginInstance, "res/SyphonIn.svg")));
		addPanelHeader(this, W, "08", "SYPHON IN");

		receiver = new SyphonInReceiverWidget();
		receiver->module = module;
		receiver->box.pos = mm2px(Vec(2.f, 2.f));
		receiver->box.size = mm2px(Vec(1.f, 1.f));
		addChild(receiver);

// Status readout under the header.
auto* status = new SyphonInStatusWidget();
status->module = module;
status->box.pos = mm2px(Vec(W * 0.5f - 14.28f, 22.f));
status->box.size = mm2px(Vec(28.56f, 34.f));
addChild(status);

		addKnobLabel(this, Vec(W * 0.5f, 70), "RECEIVE");
		addParam(createParamCentered<CKSS>(mm2px(Vec(W * 0.5f, 70)), module, SyphonIn::ENABLE_PARAM));

		addJackLabel(this, Vec(rowX(W, 3, 0), 90), "LINK");
		addChild(createLightCentered<MediumLight<BlueLight>>(mm2px(Vec(rowX(W, 3, 0), 90)), module, SyphonIn::BRIDGE_LIGHT));
		addJackLabel(this, Vec(rowX(W, 3, 1), 90), "FRM");
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(rowX(W, 3, 1), 90)), module, SyphonIn::FRAME_LIGHT));
		addJackLabel(this, Vec(rowX(W, 3, 2), 90), "OUT");
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(rowX(W, 3, 2), 90)), module, SyphonIn::SIGNAL_LIGHT));

		addPlatedOutput<PJ301MPort>(this, Vec(W * 0.5f, 108), "BUS", module, SyphonIn::VIDEO_BUS_OUTPUT);
	}

	// Lists the Syphon servers running now. The list is rebuilt each time the menu opens.
	void appendContextMenu(ui::Menu* menu) override {
		if (!syphonModule || !receiver || !receiver->bridge) {
			return;
		}
		menu->addChild(createMenuLabel("Syphon server"));
		if (!receiver->bridge->isAvailable()) {
			menu->addChild(createMenuItem("Syphon not linked in this build", "", []() {}, true));
			return;
		}

		const std::vector<std::string> names = receiver->bridge->serverNames();
		if (names.empty()) {
			menu->addChild(createMenuItem("No Syphon servers running", "", []() {}, true));
		}
		for (const std::string& name : names) {
			menu->addChild(createCheckMenuItem(name, "", [=]() {
				return syphonModule->selectedServerName == name;
			}, [=]() {
				syphonModule->selectedServerName = name;
			}));
		}
		menu->addChild(createCheckMenuItem("None", "", [=]() {
			return syphonModule->selectedServerName.empty();
		}, [=]() {
			syphonModule->selectedServerName.clear();
		}));
	}
};

Model* modelSyphonIn = createModel<SyphonIn, SyphonInWidget>("SyphonIn");
