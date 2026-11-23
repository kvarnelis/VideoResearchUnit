#include "plugin.hpp"
#include "panel_style.hpp"
#include "panel_items.hpp"
#include "raster_layout.hpp"
#include "slow_param.hpp"
#include "raster_scan.hpp"
#include "signal_ring.hpp"
#include "video_bus.hpp"

#include <cmath>
#include <cstdio>

// VRU-29 Raster: ordinary Rack signals become a picture. Every audio sample is one place in the picture, taken from the
// video clock (docs/architecture.md, "Raster rule"), so BRIGHT (or R, G and B) is painted along each line, left to right,
// top to bottom, one picture per field of the clock. Patch X and/or Y and those axes are taken from the signals instead.
//
// process() only pushes one frame-stamped sample into this module's signal ring per audio sample (the Deflection Yoke's
// way: signalRingPushAt) and publishes the node. The picture is drawn on the render side from the newest complete
// field of the ring (src/raster_scan.hpp). No allocation, no lock.
//
// What the ring holds (volts / 10): R, G, B; or, with X or Y patched, X, Y and a grey brightness (the three colour
// signals are mixed to one grey with the Keyer's Rec. 709 weights, because the ring has four channels).
struct RasterLinesQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		const int lines = static_cast<int>(std::lround(getValue()));
		char text[96];
		std::snprintf(text, sizeof(text), "%d (about %.0f samples a picture at 48 kHz)", lines, raster_scan::samplesPerPicture(48000.0, lines));
		return text;
	}
};

struct Raster : Module {
	enum ParamId { LINES_PARAM, FILL_PARAM, SLOW_PARAM, PARAMS_LEN };
	enum InputId { BRIGHT_INPUT, X_INPUT, Y_INPUT, R_INPUT, G_INPUT, B_INPUT, INPUTS_LEN };
	enum OutputId { BUS_OUTPUT, OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	VideoBusOwner videoBus;
	int signalSlotId = 0;
	int sinceSlotRetry = 0;
	int sincePublish = 0;
	RasterParams published;
	bool havePublished = false;

	Raster() {
		signalSlotId = signalRingAllocate();
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		auto* lines = configParam<RasterLinesQuantity>(LINES_PARAM, static_cast<float>(raster_scan::kMinLines),
			static_cast<float>(raster_scan::kMaxLines), static_cast<float>(raster_scan::kDefaultLines), "Lines");
		lines->snapEnabled = true;
		lines->description = "How many lines the picture has: the same meaning as Scan Sync's LINES and Video Probe's, and it must match the "
			"module that made the signal. LINES sets the picture's height. At SLOW x1 a line lasts 63.55 us whatever LINES is, so there are "
			"only about 3 samples to a line at 48 kHz and the picture is about 3 columns wide; SLOW makes the lines longer and the picture "
			"sharper. A picture takes (LINES + 22.5) lines of time, 22.5 of them the flyback, which carries no picture.";
		configSwitch(FILL_PARAM, 0.f, 1.f, 0.f, "Fill", {"Hold: each sample is a flat block (or a dot, with X or Y patched)",
			"Join: straight joins between samples (or lines between points, with X or Y patched)"});
		paramQuantities[FILL_PARAM]->description = "Hold paints each sample as a flat block as wide as the stretch of line it stands for, so a low "
			"line count gives solid rows. Join runs a straight join from each sample to the next along the line, which looks smoother at 3 samples a line. "
			"With X or Y patched, Hold plots dots and Join draws a line from point to point.";
		configSlow(this, SLOW_PARAM);
		configInput(BRIGHT_INPUT, "Brightness: 0 V is black, 10 V is white, anything outside clips. Feeds red, green and blue unless they have their own cable.");
		configInput(X_INPUT, "X position of each sample (+/-5 V spans the picture). With no cable, X follows the video clock across each line.");
		configInput(Y_INPUT, "Y position of each sample (+/-5 V spans the picture, +5 V is the top). With no cable, Y follows the video clock down the picture.");
		configInput(R_INPUT, "Red: 0 V black, 10 V full. With no cable it takes BRIGHT.");
		configInput(G_INPUT, "Green: 0 V black, 10 V full. With no cable it takes BRIGHT.");
		configInput(B_INPUT, "Blue: 0 V black, 10 V full. With no cable it takes BRIGHT.");
		configOutput(BUS_OUTPUT, "Video bus: the picture built from the signals, one per field of the video clock");
	}

	~Raster() override {
		signalRingRelease(signalSlotId);
	}

	void process(const ProcessArgs& args) override {
		if (signalSlotId == 0 && ++sinceSlotRetry >= static_cast<int>(args.sampleRate)) {
			sinceSlotRetry = 0;
			signalSlotId = signalRingTryAllocate();
		}
		const int lines = raster_scan::clampLines(params[LINES_PARAM].getValue());
		RasterParams p;
		p.slotId = signalSlotId;
		p.lines = lines;
		p.xPlaced = inputs[X_INPUT].isConnected() ? 1 : 0;
		p.yPlaced = inputs[Y_INPUT].isConnected() ? 1 : 0;
		p.join = params[FILL_PARAM].getValue() > .5f ? 1 : 0;
		p.slow = vclock::slowFromStep(params[SLOW_PARAM].getValue());

		const float bright = inputs[BRIGHT_INPUT].getVoltage();
		const float r = inputs[R_INPUT].isConnected() ? inputs[R_INPUT].getVoltage() : bright;
		const float g = inputs[G_INPUT].isConnected() ? inputs[G_INPUT].getVoltage() : bright;
		const float b = inputs[B_INPUT].isConnected() ? inputs[B_INPUT].getVoltage() : bright;
		float samples[kSignalRingChannels] = {};
		if (p.xPlaced || p.yPlaced) {
			// Placed: x, y (volts / 10) and one grey (Rec. 709 weights, as the Keyer's brightness).
			samples[0] = inputs[X_INPUT].getVoltage() * .1f;
			samples[1] = inputs[Y_INPUT].getVoltage() * .1f;
			samples[2] = (.2126f * r + .7152f * g + .0722f * b) * .1f;
		}
		else {
			samples[0] = r * .1f;
			samples[1] = g * .1f;
			samples[2] = b * .1f;
		}
		for (int i = 0; i < 3; ++i) {
			if (!(samples[i] == samples[i])) samples[i] = 0.f;
		}
		signalRingSetSampleRate(signalSlotId, args.sampleRate);
		signalRingPushAt(signalSlotId, samples, args.frame);

		const int busId = videoBus.id();
		const bool changed = !havePublished || p.slotId != published.slotId || p.lines != published.lines ||
			p.xPlaced != published.xPlaced || p.yPlaced != published.yPlaced || p.join != published.join || p.slow != published.slow;
		if (changed || ++sincePublish >= 64) {
			sincePublish = 0;
			videoBusPublishRaster(busId, p);
			published = p;
			havePublished = true;
		}
		outputs[BUS_OUTPUT].setVoltage(videoBus.voltage());
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		saveVideoBusId(rootJ, videoBus);
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		loadVideoBusId(rootJ, videoBus, "outputBusVoltage");
		havePublished = false;
	}
};

struct RasterWidget : ModuleWidget {
	RasterWidget(Raster* module) {
		using namespace rasterlab_panel;
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Raster.svg")));
		addPanelHeader(this, kRasterPanelWidth, "29", "RASTER");
		PANEL_PLACE_ALL(RASTER_LAYOUT)
		quickenSlowKnob(this, Raster::SLOW_PARAM);
	}
};

Model* modelRaster = createModel<Raster, RasterWidget>("Raster");
