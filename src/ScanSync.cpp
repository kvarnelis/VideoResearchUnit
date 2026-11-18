#include "plugin.hpp"
#include "panel_style.hpp"
#include "scan_sync_math.hpp"
#include "slow_param.hpp"
#include "scan_sync_layout.hpp"
#include "panel_items.hpp"

#include <cmath>
#include <cstdio>

// VRU-28 Scan Sync: signals that follow the picture. A pulse and a ramp that run once per picture, and a pulse and a
// ramp that run once per line, all from the video clock (docs/architecture.md, "Video clock"). Patch the ramps or
// pulses into anything whose pattern should stand still on a scan module's picture. No video bus.
//
// Everything is worked out from Rack's frame number in process(), with no state shared with any other thread,
// no lock and no allocation.
struct ScanSyncQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		const int lines = static_cast<int>(std::lround(getValue()));
		const double field = 1.0 / vclock::Clock(48000.0, lines).fieldSeconds();
		char text[64];
		std::snprintf(text, sizeof(text), "%d (%.2f pictures a second)", lines, field);
		return text;
	}
};

struct ScanSync : Module {
	enum ParamId { LINES_PARAM, SLOW_PARAM, PARAMS_LEN };
	enum InputId { INPUTS_LEN };
	enum OutputId { TOP_OUTPUT, DOWN_OUTPUT, LINE_OUTPUT, ACROSS_OUTPUT, OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	vclock::Clock clock;

	ScanSync() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		auto* lines = configParam<ScanSyncQuantity>(LINES_PARAM, scan_sync::kMinLines, scan_sync::kMaxLines, vclock::kDefaultLines, "Lines");
		lines->snapEnabled = true;
		lines->description = "How many visible lines a picture has. Set it to the same number as the scan module you are driving: "
			"the picture rate follows it (240 lines is 59.94 pictures a second, 120 is about 110, 600 is about 25).";
		configSlow(this, SLOW_PARAM);
		configOutput(TOP_OUTPUT, "Top of picture: a 10 V, 1 ms pulse on the first sample at or after the top of each picture");
		configOutput(DOWN_OUTPUT, "Down the picture: a ramp from 0 V at the top to 10 V at the bottom, once per picture; "
			"it stays at 10 V while the beam is off between pictures");
		configOutput(LINE_OUTPUT, "Start of each line: a 10 V pulse, 1 ms or half a line if that is shorter. "
			"At SLOW x1 a line is only about three samples long, so this is coarse; SLOW makes the line longer");
		configOutput(ACROSS_OUTPUT, "Across each line: a ramp from 0 V at the left to 10 V at the right, once per line. "
			"At SLOW x1 a line is only about three samples long, so this is a ramp of three steps; SLOW gives it more");
	}

	void process(const ProcessArgs& args) override {
		const int lines = static_cast<int>(std::lround(std::clamp(params[LINES_PARAM].getValue(),
			static_cast<float>(scan_sync::kMinLines), static_cast<float>(scan_sync::kMaxLines))));
		const int slow = vclock::slowFromStep(params[SLOW_PARAM].getValue());
		if (clock.visibleLines() != lines || clock.slow() != slow || clock.sampleRate() != static_cast<double>(args.sampleRate)) {
			clock = vclock::Clock(static_cast<double>(args.sampleRate), lines, slow);
		}
		const scan_sync::Outputs o = scan_sync::at(clock, args.frame);
		outputs[TOP_OUTPUT].setVoltage(o.top);
		outputs[DOWN_OUTPUT].setVoltage(o.down);
		outputs[LINE_OUTPUT].setVoltage(o.line);
		outputs[ACROSS_OUTPUT].setVoltage(o.across);
	}
};

struct ScanSyncWidget : ModuleWidget {
	ScanSyncWidget(ScanSync* module) {
		using namespace rasterlab_panel;
		constexpr float W = kScanSyncPanelWidth;  // 10 HP
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/ScanSync.svg")));
		addPanelHeader(this, W, "28", "SCAN SYNC");
		PANEL_PLACE_ALL(SCAN_SYNC_LAYOUT)
	}
};

Model* modelScanSync = createModel<ScanSync, ScanSyncWidget>("ScanSync");
