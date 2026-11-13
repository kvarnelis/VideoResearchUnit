#include "plugin.hpp"
#include "gl_video_texture.hpp"
#include "panel_style.hpp"
#include "syphon_bridge.hpp"
#include "video_bus.hpp"
#include "video_graph_renderer.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

// Published frames are always 4:3, independent of the Display module.
struct SyphonFrameSize {
	int width;
	int height;
	const char* label;
};

static const SyphonFrameSize kSyphonOutSizes[] = {
	{1024, 768, "1024 x 768"},
	{1440, 1080, "1440 x 1080"},
};
static const int kSyphonOutSizeCount = 2;
static const char* kSyphonServerPrefix = "VRU \xe2\x80\x93 ";  // "VRU – "

struct SyphonOut : Module {
	enum ParamId {
		ENABLE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		VIDEO_BUS_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUTPUTS_LEN
	};
	enum LightId {
		SIGNAL_LIGHT,
		BRIDGE_LIGHT,
		PUBLISH_LIGHT,
		LIGHTS_LEN
	};

	std::atomic<int> selectedBus {1};
	std::atomic<bool> hasSignal {false};
	std::atomic<bool> bridgeReady {false};
	std::atomic<bool> framePublished {false};
	std::atomic<int> frameSizeIndex {0};
	// Optional user label for the server name. Empty means the module id is used.
	std::string serverLabel;

	SyphonOut() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configSwitch(ENABLE_PARAM, 0.f, 1.f, 1.f, "Publish", {"Off", "On"});
		configInput(VIDEO_BUS_INPUT, "Video bus token");
	}

	void process(const ProcessArgs& args) override {
		const int busId = inputs[VIDEO_BUS_INPUT].isConnected() ? videoBusIdFromVoltage(inputs[VIDEO_BUS_INPUT].getVoltage()) : 1;
		const bool enabled = params[ENABLE_PARAM].getValue() > 0.5f;
		const bool active = enabled && videoBusIsActive(busId);

		selectedBus.store(busId, std::memory_order_relaxed);
		hasSignal.store(active, std::memory_order_relaxed);
		lights[SIGNAL_LIGHT].setBrightnessSmooth(active ? 1.f : 0.f, args.sampleTime);
		lights[BRIDGE_LIGHT].setBrightnessSmooth(bridgeReady.load(std::memory_order_relaxed) ? 1.f : 0.f, args.sampleTime);
		lights[PUBLISH_LIGHT].setBrightnessSmooth(framePublished.load(std::memory_order_relaxed) ? 1.f : 0.f, args.sampleTime);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "serverLabel", json_string(serverLabel.c_str()));
		json_object_set_new(rootJ, "frameSizeIndex", json_integer(frameSizeIndex.load()));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* serverLabelJ = json_object_get(rootJ, "serverLabel");
		if (serverLabelJ && json_is_string(serverLabelJ)) {
			serverLabel = json_string_value(serverLabelJ);
		}
		json_t* frameSizeJ = json_object_get(rootJ, "frameSizeIndex");
		if (frameSizeJ && json_is_integer(frameSizeJ)) {
			frameSizeIndex.store(clamp(static_cast<int>(json_integer_value(frameSizeJ)), 0, kSyphonOutSizeCount - 1));
		}
	}

	int snapshotBusId() const {
		return selectedBus.load(std::memory_order_relaxed);
	}

	bool isPublishEnabled() {
		return params[ENABLE_PARAM].getValue() > 0.5f;
	}

	const SyphonFrameSize& frameSize() const {
		return kSyphonOutSizes[clamp(frameSizeIndex.load(), 0, kSyphonOutSizeCount - 1)];
	}

	// The part of the server name after the prefix: the user label, or the module id.
	std::string serverDetail() const {
		return serverLabel.empty() ? std::to_string(id) : serverLabel;
	}

	// Server name as Syphon clients list it: "VRU – <label or module id>".
	std::string serverName() const {
		return kSyphonServerPrefix + serverDetail();
	}
};

struct SyphonOutRenderWidget : widget::OpenGlWidget {
	SyphonOut* module = nullptr;
	std::unique_ptr<VideoGraphRenderer> renderer;
	std::unique_ptr<SyphonOutputBridge> bridge;
	OpenGlVideoTexture texture;

	SyphonOutRenderWidget() {
		renderer = std::make_unique<VideoGraphRenderer>();
		bridge = createSyphonOutputBridge();
	}

	void drawFramebuffer() override {
		if (!module || !bridge) {
			return;
		}

		const bool bridgeAvailable = bridge->isAvailable();
		module->bridgeReady.store(bridgeAvailable, std::memory_order_relaxed);
		if (!module->isPublishEnabled()) {
			module->framePublished.store(false, std::memory_order_relaxed);
			return;
		}

		const SyphonFrameSize& size = module->frameSize();
		const std::vector<unsigned char>& rgba = renderer->renderBus(module->snapshotBusId(), size.width, size.height);
		if (!renderer->hasSignal() || rgba.empty()) {
			module->framePublished.store(false, std::memory_order_relaxed);
			return;
		}

		const bool uploaded = texture.uploadRgba(rgba.data(), size.width, size.height, true);
		bool published = false;
		if (uploaded && bridgeAvailable) {
			bridge->setServerName(module->serverName());
			published = bridge->publishTexture(texture.info());
		}
		module->framePublished.store(published, std::memory_order_relaxed);
	}

	void onRemove(const RemoveEvent& e) override {
		texture.destroy();
		widget::OpenGlWidget::onRemove(e);
	}
};

// Shows NO SYPHON when the bridge is not linked, LINKED when the bridge is ready but there is no bus
// signal to publish, and while publishing the server name over two lines: VRU, then the label
// or module id (clients list the server as "VRU – <label or id>"). The whole name is wider than the
// screen, so it is split instead of shrunk. Sizes are in mm (panel_style.hpp):
// the headline is READOUT_MM, the id line is LABEL_MM and is shortened with "..." when a 16-digit id
// does not fit the 26.5 mm of text the 28.56 mm screen holds.
struct SyphonOutStatusWidget : TransparentWidget {
	SyphonOut* module = nullptr;

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, 3.f);
		nvgFillColor(args.vg, nvgRGB(8, 10, 13));
		nvgFill(args.vg);

		const bool bridge = module && module->bridgeReady.load(std::memory_order_relaxed);
		const bool published = module && module->framePublished.load(std::memory_order_relaxed);
		const float textWidth = box.size.x - 6.f;
		const NVGcolor lit = nvgRGB(190, 255, 170);
		const NVGcolor dim = nvgRGB(210, 210, 210);

		rasterlab_panel::useLabelFont(args.vg);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		if (bridge && published) {
			nvgFontSize(args.vg, mm2px(rasterlab_panel::READOUT_MM));
			nvgFillColor(args.vg, lit);
			nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.40f, "VRU", nullptr);

			nvgFontSize(args.vg, mm2px(rasterlab_panel::LABEL_MM));
			nvgFillColor(args.vg, nvgRGBA(190, 255, 170, 170));
			nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.60f, rasterlab_panel::fitToWidth(args.vg, module->serverDetail(), textWidth).c_str(), nullptr);
		}
		else {
			nvgFontSize(args.vg, mm2px(rasterlab_panel::READOUT_MM));
			nvgFillColor(args.vg, dim);
			nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.5f, bridge ? "LINKED" : "NO SYPHON", nullptr);
		}

		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.5f, 0.5f, box.size.x - 1.f, box.size.y - 1.f, 3.f);
		nvgStrokeColor(args.vg, nvgRGBA(255, 255, 255, 46));
		nvgStrokeWidth(args.vg, 1.f);
		nvgStroke(args.vg);
	}
};

struct SyphonOutWidget : ModuleWidget {
	SyphonOut* syphonModule = nullptr;

	SyphonOutWidget(SyphonOut* module) {
		using namespace rasterlab_panel;
		const float W = 35.56f;	// 7 HP
		setModule(module);
		syphonModule = module;
		setPanel(createPanel(asset::plugin(pluginInstance, "res/SyphonOut.svg")));
		addPanelHeader(this, W, "17", "SYPHON OUT");

		auto* render = new SyphonOutRenderWidget();
		render->module = module;
		render->box.pos = mm2px(Vec(2.f, 2.f));
		render->box.size = mm2px(Vec(1.f, 1.f));
		addChild(render);

		// Status readout under the header.
		auto* status = new SyphonOutStatusWidget();
		status->module = module;
		status->box.pos = mm2px(Vec(W * 0.5f - 14.28f, 22.f));
		status->box.size = mm2px(Vec(28.56f, 34.f));
		addChild(status);

		addKnobLabel(this, Vec(W * 0.5f, 70), "PUBLISH");
		addParam(createParamCentered<CKSS>(mm2px(Vec(W * 0.5f, 70)), module, SyphonOut::ENABLE_PARAM));

		addJackLabel(this, Vec(W * 0.5f, 90), "BUS IN");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(W * 0.5f, 90)), module, SyphonOut::VIDEO_BUS_INPUT));

		addJackLabel(this, Vec(rowX(W, 3, 0), 108), "SIG");
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(rowX(W, 3, 0), 108)), module, SyphonOut::SIGNAL_LIGHT));
		addJackLabel(this, Vec(rowX(W, 3, 1), 108), "LINK");
		addChild(createLightCentered<MediumLight<BlueLight>>(mm2px(Vec(rowX(W, 3, 1), 108)), module, SyphonOut::BRIDGE_LIGHT));
		addJackLabel(this, Vec(rowX(W, 3, 2), 108), "OUT");
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(rowX(W, 3, 2), 108)), module, SyphonOut::PUBLISH_LIGHT));
	}

	void appendContextMenu(ui::Menu* menu) override {
		if (!syphonModule) {
			return;
		}
		menu->addChild(createMenuLabel("Published frame (4:3)"));
		for (int i = 0; i < kSyphonOutSizeCount; ++i) {
			menu->addChild(createCheckMenuItem(kSyphonOutSizes[i].label, "", [=]() {
				return syphonModule->frameSizeIndex.load() == i;
			}, [=]() {
				syphonModule->frameSizeIndex.store(i);
			}));
		}
		menu->addChild(createMenuLabel("Server: " + syphonModule->serverName()));
	}
};

Model* modelSyphonOut = createModel<SyphonOut, SyphonOutWidget>("SyphonOut");
