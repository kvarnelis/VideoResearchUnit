#include "plugin.hpp"
#include "panel_style.hpp"
#include "video_bus.hpp"
#include "video_engine.hpp"

#include <cmath>

// FREQ knobs are 0..1 and map to 0.25..16 cycles; the tooltip shows the cycles.
struct RampsFrequencyQuantity : ParamQuantity {
	float getDisplayValue() override { return 0.25f * std::exp2(6.f * getValue()); }
	void setDisplayValue(float v) override { setValue(std::log2(clamp(v, 0.25f, 16.f) / 0.25f) / 6.f); }
};

struct Ramps : Module {
	enum ParamId { AXIS_A_PARAM, FREQ_A_PARAM, PHASE_A_PARAM, WAVE_A_PARAM,
		AXIS_B_PARAM, FREQ_B_PARAM, PHASE_B_PARAM, WAVE_B_PARAM, MIX_PARAM, PARAMS_LEN };
	enum InputId { FREQ_A_INPUT, PHASE_A_INPUT, WAVE_A_INPUT,
		FREQ_B_INPUT, PHASE_B_INPUT, WAVE_B_INPUT, INPUTS_LEN };
	enum OutputId { VIDEO_BUS_OUTPUT, OUTPUTS_LEN };
	VideoBusOwner videoBus;

	Ramps() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, 0);
		for (int layer = 0; layer < 2; ++layer) {
			const int base = layer * 4;
			const std::string name = layer == 0 ? " A" : " B";
			configSwitch(base, 0.f, 3.f, static_cast<float>(layer), "Axis" + name, {"Horizontal", "Vertical", "Radial", "Angular"});
			configParam<RampsFrequencyQuantity>(base + 1, 0.f, 1.f, 1.f / 3.f, "Frequency" + name, " cycles");
			configParam(base + 2, 0.f, 1.f, 0.f, "Phase" + name);
			configParam(base + 3, 0.f, 1.f, 0.f, "Wave" + name);
			configInput(layer * 3, "Frequency" + name + " CV");
			configInput(layer * 3 + 1, "Phase" + name + " CV");
			configInput(layer * 3 + 2, "Wave" + name + " CV");
		}
		configSwitch(MIX_PARAM, 0.f, 3.f, 0.f, "Mix", {"A", "A + B", "A - B", "A x B"});
		configOutput(VIDEO_BUS_OUTPUT, "Video bus token");
	}

	void process(const ProcessArgs&) override {
		RampsParams p;
		for (int layer = 0; layer < 2; ++layer) {
			const int base = layer * 4;
			const int cv = layer * 3;
			const int axis = static_cast<int>(clamp(std::round(params[base].getValue()), 0.f, 3.f));
			const float freq = 0.25f * std::exp2(6.f * clamp(params[base + 1].getValue() + inputs[cv].getVoltage() * 0.1f, 0.f, 1.f));
			float phase = params[base + 2].getValue() + inputs[cv + 1].getVoltage() * 0.1f;
			phase -= std::floor(phase);
			const float wave = clamp(params[base + 3].getValue() + inputs[cv + 2].getVoltage() * 0.1f, 0.f, 1.f);
			if (layer == 0) { p.axisA = axis; p.freqA = freq; p.phaseA = phase; p.waveA = wave; }
			else { p.axisB = axis; p.freqB = freq; p.phaseB = phase; p.waveB = wave; }
		}
		p.mix = static_cast<int>(clamp(std::round(params[MIX_PARAM].getValue()), 0.f, 3.f));
		videoBusPublishRamps(videoBus.id(), p);
		outputs[VIDEO_BUS_OUTPUT].setVoltage(videoBus.voltage());
	}

	// The bus already provides a race-free snapshot for the preview.
	RampsParams snapshot() const {
		VideoBusNode node;
		return videoBusReadNode(videoBus.id(), node) && node.type == VideoBusNodeType::Ramps ? node.ramps : RampsParams{};
	}
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		saveVideoBusId(rootJ, videoBus);
		return rootJ;
	}
	void dataFromJson(json_t* rootJ) override { loadVideoBusId(rootJ, videoBus, "outputBusVoltage"); }
};

struct RampsPreviewWidget : TransparentWidget {
	Ramps* module = nullptr;
	std::vector<unsigned char> rgba;
	int nvgImage = 0;

	void step() override {
		renderRamps(rgba, 128, 84, module ? module->snapshot() : RampsParams{});
		TransparentWidget::step();
	}
	void draw(const DrawArgs& args) override {
		if (rgba.empty()) return;
		if (!nvgImage) nvgImage = nvgCreateImageRGBA(args.vg, 128, 84, 0, rgba.data());
		else nvgUpdateImage(args.vg, nvgImage, rgba.data());
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, 3.f);
		nvgFillPaint(args.vg, nvgImagePattern(args.vg, 0.f, 0.f, box.size.x, box.size.y, 0.f, nvgImage, 1.f));
		nvgFill(args.vg);
	}
	void onRemove(const RemoveEvent& e) override {
		if (nvgImage && APP && APP->window) {
			nvgDeleteImage(APP->window->vg, nvgImage);
			nvgImage = 0;
		}
		TransparentWidget::onRemove(e);
	}
};

struct RampsWidget : ModuleWidget {
	RampsWidget(Ramps* module) {
		using namespace rasterlab_panel;
		const float W = 71.12f;
		const float P = 13.f;
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Ramps.svg")));
		addPanelHeader(this, W, "06", "RAMPS");
		auto* preview = new RampsPreviewWidget;
		preview->module = module;
		preview->box.pos = mm2px(Vec(W * 0.5f - 27.f, 19.f));
		preview->box.size = mm2px(Vec(54.f, 36.f));
		addChild(preview);
		for (int layer = 0; layer < 2; ++layer) {
			const float y = layer == 0 ? 66.f : 84.f;
			const char* labels[] = {layer == 0 ? "AXIS A" : "AXIS B", "FREQ", "PHASE", "WAVE"};
			for (int i = 0; i < 4; ++i) {
				const Vec pos(rowX(W, 5, i, P), y);
				addKnobLabel(this, pos, labels[i]);
				if (i == 0) addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(pos), module, layer * 4 + i));
				else addParam(createParamCentered<RoundBlackKnob>(mm2px(pos), module, layer * 4 + i));
			}
		}
		const Vec mixPos(rowX(W, 5, 4, P), 66.f);
		addKnobLabel(this, mixPos, "MIX");
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(mixPos), module, Ramps::MIX_PARAM));
		// Each CV jack sits in the column of the knob it modulates (FREQ, PHASE, WAVE in columns 1 to 3):
		// layer A on the first jack row (y 100), layer B on the second (y 116). Input i keeps cvLabels[i].
		const char* cvLabels[] = {"FREQ A", "PHS A", "WAVE A", "FREQ B", "PHS B", "WAVE B"};
		for (int i = 0; i < 6; ++i) {
			const Vec pos(rowX(W, 5, 1 + i % 3, P), i < 3 ? 100.f : 116.f);
			addJackLabel(this, pos, cvLabels[i]);
			addInput(createInputCentered<PJ301MPort>(mm2px(pos), module, i));
		}
		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 5, 4, P), 116), "BUS", module, Ramps::VIDEO_BUS_OUTPUT);
	}
};

Model* modelRamps = createModel<Ramps, RampsWidget>("Ramps");
