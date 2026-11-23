#pragma once
#include "plugin.hpp"
#include "video_clock.hpp"

#include <cstdio>
#include <string>

// The SLOW knob shared by Video Probe, Raster and Scan Sync (src/video_clock.hpp, "Slow scan"): a snapped step 0 to 7
// that means x1, x2, x4 ... x128 fields to a picture. Step 0, the default, is the plain clock.
struct SlowQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		const int k = vclock::slowFromStep(getValue());
		const vclock::Clock c(48000.0, 240, k);
		char text[128];
		std::snprintf(text, sizeof(text), "x%d (%.1f samples a line at 48 kHz, %.2f s a picture at 240 lines)", k, c.samplesPerLine(), c.fieldSeconds());
		return text;
	}
};

constexpr const char* kSlowDescription =
	"Slows the scan so a picture is sharper. At x1 a picture takes one field of the video clock and a line is only about 3 samples "
	"wide at 48 kHz. At xK a picture takes K fields: every line lasts K times longer and has about 3.05 x K samples, so the picture is "
	"about that many columns wide, and it comes round K times less often. At 48 kHz and 240 lines: x1 3 samples a line, 60 pictures a "
	"second; x4 12, 15 a second; x16 49, one every 0.27 s; x32 98, one every 0.53 s; x64 195, one every 1.07 s; x128 390, one every "
	"2.1 s. Sharper means slower. Set it to the same step on every module that scans or paints the same picture, with the same LINES.";

inline void configSlow(Module* m, int id) {
	auto* q = m->configParam<SlowQuantity>(id, 0.f, static_cast<float>(vclock::kSlowSteps - 1), 0.f, "Slow");
	q->snapEnabled = true;
	q->description = kSlowDescription;
}

// Call in the widget's constructor after the SLOW knob has been placed. Rack spreads a knob's whole range over the same
// length of drag however few steps it has (in its linear mode: pixels x 0.001 x speed x range x 2 pi / the knob's angle
// range), and a snapped knob moves only when a whole step has built up. With 8 steps that is about 110 pixels of drag
// for one step, so a normal short drag did nothing and the knob seemed stuck. This sets the knob's speed so one step
// takes kSlowPixelsPerStep pixels at Rack's default sensitivity; Ctrl and Shift still slow it down and speed it up.
constexpr float kSlowPixelsPerStep = 25.f;

inline void quickenSlowKnob(ModuleWidget* widget, int id) {
	auto* knob = dynamic_cast<app::Knob*>(widget->getParam(id));
	if (!knob || !(knob->maxAngle > knob->minAngle)) {
		return;
	}
	const float range = static_cast<float>(vclock::kSlowSteps - 1);
	const float rangeRatio = range * 2.f * static_cast<float>(M_PI) / (knob->maxAngle - knob->minAngle);
	knob->speed = 1.f / (0.001f * kSlowPixelsPerStep * rangeRatio);
}
