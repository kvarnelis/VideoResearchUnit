#include "plugin.hpp"
#include "panel_style.hpp"
#include "signal_ring.hpp"
#include "crt_beam.hpp"
#include "video_bus.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace {
// +/-5 V full scale moves the picture by half its width (ramp units), as the DeflectionYoke does.
constexpr float kDeflectFullScale = 0.5f;
constexpr float kSampleClamp = 2.f;  // +/-10 V is already well past full scale
// Yoke cutoff for the deflection inputs. Fixed: the coil rolls off high tones.
constexpr float kCoilHz = 3000.f;
// Scanline counts for the LINES switch. 128 and 256 read at Rack size; 525 and 1050 are the broadcast counts.
constexpr int kLineChoices[4] = {128, 256, 525, 1050};
}

struct ScanProcessor : Module {
	enum ParamId {
		LUMA_GAIN_PARAM,   // vertical lift per unit luma, bipolar (-1..1), in screen heights
		H_POS_PARAM,       // horizontal offset (-1..1)
		H_WIDTH_PARAM,     // horizontal scale (-2..2); negative mirrors the picture
		V_POS_PARAM,       // vertical offset (-1..1)
		V_HEIGHT_PARAM,    // vertical scale (0..2); 0 collapses the lines to one thin line
		LINES_PARAM,       // 0..3: 128, 256, 525, 1050 lines
		INTENSITY_PARAM,   // beam energy, 0..3
		ROTATE_PARAM,      // ROTATE 90
		COLOR_PARAM,       // beam colour from the source
		Z_INTENSITY_PARAM, // beam brightens with luma, 0..1
		PERSIST_PARAM,     // phosphor persistence, 0..1
		GLOW_PARAM,        // glow, 0..2
		PARAMS_LEN
	};
	enum InputId {
		VIDEO_BUS_INPUT,
		LUMA_GAIN_INPUT,   // LUMA GAIN CV, +/-5 V = +/-1
		DEFLECT_X_INPUT,   // audio rate, +/-5 V full scale
		DEFLECT_Y_INPUT,   // audio rate, +/-5 V full scale
		BLANK_INPUT,       // beam off while above 1 V
		INTENSITY_INPUT,   // INTENSITY CV, 10 V = full knob travel
		H_POS_INPUT,       // horizontal position CV, +/-5 V = +/-1 screen width
		V_POS_INPUT,       // vertical position CV, +/-5 V = +/-1 screen height
		Z_INPUT,           // beam brightness at audio rate, -5 V off, 0 V unchanged, +5 V double
		INPUTS_LEN
	};
	enum OutputId {
		VIDEO_BUS_OUTPUT,
		VIDEO_BUS_2_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		SIGNAL_LIGHT,
		LIGHTS_LEN
	};

	std::atomic<int> sourceBus {1};
	std::atomic<bool> hasSignal {false};
	// One coil filter per deflection channel, so each axis keeps its own state.
	crt::CoilFilter coilX;
	crt::CoilFilter coilY;
	int signalSlotId = 0;
	VideoBusOwner videoBus;

	ScanProcessor() {
		signalSlotId = signalRingAllocate();
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(LUMA_GAIN_PARAM, -1.f, 1.f, 0.2f, "Luma gain", " screen heights per unit luma");
		configParam(H_POS_PARAM, -1.f, 1.f, 0.f, "Horizontal position", " screen widths");
		configParam(H_WIDTH_PARAM, -2.f, 2.f, 1.f, "Horizontal width");
		configParam(V_POS_PARAM, -1.f, 1.f, 0.f, "Vertical position", " screen heights");
		configParam(V_HEIGHT_PARAM, 0.f, 2.f, 1.f, "Vertical height");
		configSwitch(LINES_PARAM, 0.f, 3.f, 0.f, "Scanlines", {"128", "256", "525", "1050"});
		configParam(INTENSITY_PARAM, 0.f, 3.f, 0.6f, "Intensity");
		configSwitch(ROTATE_PARAM, 0.f, 1.f, 0.f, "Rotate 90", {"Off", "On"});
		configSwitch(COLOR_PARAM, 0.f, 1.f, 0.f, "Colour from source", {"Phosphor", "Source"});
		configParam(Z_INTENSITY_PARAM, 0.f, 1.f, 0.f, "Z-intensity");
		configParam(PERSIST_PARAM, 0.f, 1.f, 0.35f, "Persistence");
		configParam(GLOW_PARAM, 0.f, 2.f, 0.25f, "Glow");
		configInput(VIDEO_BUS_INPUT, "Video bus token");
		configInput(LUMA_GAIN_INPUT, "Luma gain CV");
		configInput(DEFLECT_X_INPUT, "Deflect X (audio rate)");
		configInput(DEFLECT_Y_INPUT, "Deflect Y (audio rate)");
		configInput(BLANK_INPUT, "Blank (beam off above 1 V)");
		configInput(INTENSITY_INPUT, "Intensity CV (10 V = full knob travel)");
		configInput(H_POS_INPUT, "Horizontal position CV (+/-5 V = +/-1 screen width)");
		configInput(V_POS_INPUT, "Vertical position CV (+/-5 V = +/-1 screen height)");
		configInput(Z_INPUT, "Z: beam brightness, audio rate (-5 V off, 0 V unchanged, +5 V double)");
		configOutput(VIDEO_BUS_OUTPUT, "Video bus token");
		configOutput(VIDEO_BUS_2_OUTPUT, "Video bus token (same picture as BUS 1)");
	}

	~ScanProcessor() override {
		signalRingRelease(signalSlotId);
	}

	void process(const ProcessArgs& args) override {
		const int inputBusId = inputs[VIDEO_BUS_INPUT].isConnected() ? videoBusIdFromVoltage(inputs[VIDEO_BUS_INPUT].getVoltage()) : 1;
		const bool active = videoBusIsActive(inputBusId) && inputBusId != videoBus.id();

		// Deflection goes through the yoke filter. Blank is a gate: it is not filtered.
		coilX.setCutoff(kCoilHz, args.sampleRate);
		coilY.setCutoff(kCoilHz, args.sampleRate);
		const float xIn = inputs[DEFLECT_X_INPUT].isConnected() ? std::clamp(inputs[DEFLECT_X_INPUT].getVoltage() / 5.f, -kSampleClamp, kSampleClamp) : 0.f;
		const float yIn = inputs[DEFLECT_Y_INPUT].isConnected() ? std::clamp(inputs[DEFLECT_Y_INPUT].getVoltage() / 5.f, -kSampleClamp, kSampleClamp) : 0.f;
		const bool blanked = inputs[BLANK_INPUT].isConnected() && inputs[BLANK_INPUT].getVoltage() > 1.f;
		// Z is audio rate, so it goes straight into the ring with no coil filter. Unpatched: 0, the beam is unchanged.
		const float zIn = inputs[Z_INPUT].isConnected() ? std::clamp(inputs[Z_INPUT].getVoltage() / 5.f, -1.f, 2.f) : 0.f;
		const float samples[kSignalRingChannels] = {
			coilX.process(kDeflectFullScale * xIn),
			coilY.process(kDeflectFullScale * yIn),
			blanked ? 1.f : 0.f,
			zIn,
		};
		signalRingSetSampleRate(signalSlotId, args.sampleRate);
		signalRingPush(signalSlotId, samples);

		const float lumaCv = inputs[LUMA_GAIN_INPUT].isConnected() ? inputs[LUMA_GAIN_INPUT].getVoltage() / 5.f : 0.f;
		const float intensityCv = inputs[INTENSITY_INPUT].isConnected() ? inputs[INTENSITY_INPUT].getVoltage() : 0.f;
		const float hPosCv = inputs[H_POS_INPUT].isConnected() ? inputs[H_POS_INPUT].getVoltage() / 5.f : 0.f;
		const float vPosCv = inputs[V_POS_INPUT].isConnected() ? inputs[V_POS_INPUT].getVoltage() / 5.f : 0.f;
		const int lineIndex = static_cast<int>(std::clamp(std::round(params[LINES_PARAM].getValue()), 0.f, 3.f));

		ScanProcessorParams scanProc;
		scanProc.lumaGain = std::clamp(params[LUMA_GAIN_PARAM].getValue() + lumaCv, -1.f, 1.f);
		scanProc.hPos = std::clamp(params[H_POS_PARAM].getValue() + hPosCv, -1.f, 1.f);
		scanProc.hWidth = params[H_WIDTH_PARAM].getValue();
		scanProc.vPos = std::clamp(params[V_POS_PARAM].getValue() + vPosCv, -1.f, 1.f);
		scanProc.vHeight = params[V_HEIGHT_PARAM].getValue();
		scanProc.lines = kLineChoices[lineIndex];
		scanProc.intensity = std::clamp(params[INTENSITY_PARAM].getValue() + intensityCv * 0.3f, 0.f, 3.f);
		scanProc.rotate = params[ROTATE_PARAM].getValue() > 0.5f;
		scanProc.color = params[COLOR_PARAM].getValue() > 0.5f;
		scanProc.zIntensity = params[Z_INTENSITY_PARAM].getValue();
		scanProc.persist = params[PERSIST_PARAM].getValue();
		scanProc.glow = params[GLOW_PARAM].getValue();
		scanProc.signalSlotId = signalSlotId;

		sourceBus.store(inputBusId, std::memory_order_relaxed);
		hasSignal.store(active, std::memory_order_relaxed);

		const int outputBusId = videoBus.id();
		if (active) {
			videoBusPublishScanProcessor(outputBusId, inputBusId, scanProc);
		}
		else {
			videoBusClear(outputBusId);
		}

		outputs[VIDEO_BUS_OUTPUT].setVoltage(videoBus.voltage());
		outputs[VIDEO_BUS_2_OUTPUT].setVoltage(videoBus.voltage());
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

struct ScanProcessorWidget : ModuleWidget {
	ScanProcessorWidget(ScanProcessor* module) {
		using namespace rasterlab_panel;
		const float W = 66.04f;	// 13 HP
		// Four columns 15.24 mm apart (1.5 grid steps) and six rows: the scan, where the picture sits, how the
		// beam looks, the CV jacks in two rows, then the bus jacks and the signal light.
		const float P = 15.24f;
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/ScanProcessor.svg")));
		addPanelHeader(this, W, "02", "SCAN PROCESSOR");

		// Row 1: the scan. LUMA GAIN sets how far each line lifts with luma, LINES the line count, INTENSITY
		// the beam energy, and ROTATE turns the picture 90 degrees. The labels share the large-knob baseline.
		const float row1 = 29.f;
		addKnobLabel(this, Vec(rowX(W, 4, 0, P), row1), "LUMA GAIN", R_KNOB);
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(rowX(W, 4, 0, P), row1)), module, ScanProcessor::LUMA_GAIN_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 1, P), row1), "LINES", R_KNOB);
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(rowX(W, 4, 1, P), row1)), module, ScanProcessor::LINES_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 2, P), row1), "INTENSITY", R_KNOB);
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(rowX(W, 4, 2, P), row1)), module, ScanProcessor::INTENSITY_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 3, P), row1), "ROTATE", R_KNOB);
		addParam(createParamCentered<CKSS>(mm2px(Vec(rowX(W, 4, 3, P), row1)), module, ScanProcessor::ROTATE_PARAM));

		// Row 2: where the picture sits. H WIDTH and V HEIGHT scale it, H POS and V POS move it.
		const float row2 = 46.f;
		addKnobLabel(this, Vec(rowX(W, 4, 0, P), row2), "H WIDTH", R_KNOB);
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(rowX(W, 4, 0, P), row2)), module, ScanProcessor::H_WIDTH_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 1, P), row2), "H POS", R_KNOB);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 1, P), row2)), module, ScanProcessor::H_POS_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 2, P), row2), "V HEIGHT", R_KNOB);
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(rowX(W, 4, 2, P), row2)), module, ScanProcessor::V_HEIGHT_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 3, P), row2), "V POS", R_KNOB);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 3, P), row2)), module, ScanProcessor::V_POS_PARAM));

		// Row 3: how the beam looks. Z-INT brightens it with luma, COLOR takes its colour from the source,
		// PERSIST sets the phosphor trail and GLOW the bloom.
		const float row3 = 63.f;
		addKnobLabel(this, Vec(rowX(W, 4, 0, P), row3), "Z-INT", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 0, P), row3)), module, ScanProcessor::Z_INTENSITY_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 1, P), row3), "COLOR", R_SMALL);
		addParam(createParamCentered<CKSS>(mm2px(Vec(rowX(W, 4, 1, P), row3)), module, ScanProcessor::COLOR_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 2, P), row3), "PERSIST", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 2, P), row3)), module, ScanProcessor::PERSIST_PARAM));
		addKnobLabel(this, Vec(rowX(W, 4, 3, P), row3), "GLOW", R_SMALL);
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(rowX(W, 4, 3, P), row3)), module, ScanProcessor::GLOW_PARAM));

		// Row 4: CV for the knobs above, each jack under the knob it drives: LUMA GAIN, H POS, INTENSITY, V POS.
		const float row4 = 80.f;
		addJackLabel(this, Vec(rowX(W, 4, 0, P), row4), "LUMA");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 0, P), row4)), module, ScanProcessor::LUMA_GAIN_INPUT));
		addJackLabel(this, Vec(rowX(W, 4, 1, P), row4), "H POS");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 1, P), row4)), module, ScanProcessor::H_POS_INPUT));
		addJackLabel(this, Vec(rowX(W, 4, 2, P), row4), "INTENSITY");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 2, P), row4)), module, ScanProcessor::INTENSITY_INPUT));
		addJackLabel(this, Vec(rowX(W, 4, 3, P), row4), "V POS");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 3, P), row4)), module, ScanProcessor::V_POS_INPUT));

		// Row 5: audio-rate CV. X and Y deflect the beam, Z sets its brightness, and BLANK switches it off
		// while it is above 1 V.
		const float row5 = 98.f;
		addJackLabel(this, Vec(rowX(W, 4, 0, P), row5), "X");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 0, P), row5)), module, ScanProcessor::DEFLECT_X_INPUT));
		addJackLabel(this, Vec(rowX(W, 4, 1, P), row5), "Y");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 1, P), row5)), module, ScanProcessor::DEFLECT_Y_INPUT));
		addJackLabel(this, Vec(rowX(W, 4, 2, P), row5), "Z");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 2, P), row5)), module, ScanProcessor::Z_INPUT));
		addJackLabel(this, Vec(rowX(W, 4, 3, P), row5), "BLANK");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 3, P), row5)), module, ScanProcessor::BLANK_INPUT));

		// Row 6: bus. BUS IN takes a video bus token. BUS 1 and BUS 2 carry the same token, and SIG lights
		// while a signal is in.
		const float rowIO = 115.f;
		addJackLabel(this, Vec(rowX(W, 4, 0, P), rowIO), "BUS IN");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rowX(W, 4, 0, P), rowIO)), module, ScanProcessor::VIDEO_BUS_INPUT));
		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 4, 1, P), rowIO), "BUS 1", module, ScanProcessor::VIDEO_BUS_OUTPUT);
		addPlatedOutput<PJ301MPort>(this, Vec(rowX(W, 4, 2, P), rowIO), "BUS 2", module, ScanProcessor::VIDEO_BUS_2_OUTPUT);
		addJackLabel(this, Vec(rowX(W, 4, 3, P), rowIO), "SIG");
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(rowX(W, 4, 3, P), rowIO)), module, ScanProcessor::SIGNAL_LIGHT));
	}
};

Model* modelScanProcessor = createModel<ScanProcessor, ScanProcessorWidget>("ScanProcessor");
