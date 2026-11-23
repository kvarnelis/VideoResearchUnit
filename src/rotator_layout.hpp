#pragma once

// The Rotator panel (VRU-20), as data (see src/panel_items.hpp for the item kinds). RotatorWidget places everything from
// this list and scripts/check-panel.py reads the same list to draw the panel to scale and to fail on any overlap.
// Six columns 13.6 mm apart, two rows, every place used. Each knob has its CV jack below it. The first column of the
// top row holds the two trigger jacks, NOTCH over RESET, straight above STEP and next to GLIDE, which they work with;
// PIVOT X and PIVOT Y sit under X and Y. The setting of STEP, GLIDE, FOLDS and EDGE is written under the knob
// (RotatorReadout, 4 mm high, centred 8.4 mm below it; not in this table), which is why the second row's jacks sit
// lower. Edit positions here and nowhere else.

constexpr float kRotatorPanelWidth = 86.36f;   // 17 HP

#define ROTATOR_LAYOUT(X) \
	X(JACK,  9.18f, 29.2f, "NOTCH", Rotator::NOTCH_INPUT, 0) \
	X(JACK,  9.18f, 46.f,  "RESET", Rotator::RESET_INPUT, 0) \
	X(KNOB,  22.78f, 30.f,  "ANGLE", Rotator::ANGLE_PARAM, 0) \
	X(KNOB,  36.38f, 30.f,  "SPIN", Rotator::SPIN_PARAM, 0) \
	X(KNOB,  49.98f, 30.f,  "ZOOM", Rotator::ZOOM_PARAM, 0) \
	X(KNOB,  63.58f, 30.f,  "X", Rotator::X_PARAM, 0) \
	X(KNOB,  77.18f, 30.f,  "Y", Rotator::Y_PARAM, 0) \
	X(JACK,  22.78f, 46.f,  "CV", Rotator::ANGLE_INPUT, 0) \
	X(JACK,  36.38f, 46.f,  "CV", Rotator::SPIN_INPUT, 0) \
	X(JACK,  49.98f, 46.f,  "CV", Rotator::ZOOM_INPUT, 0) \
	X(JACK,  63.58f, 46.f,  "CV", Rotator::X_INPUT, 0) \
	X(JACK,  77.18f, 46.f,  "CV", Rotator::Y_INPUT, 0) \
	X(RULE,  4.f, 55.f, "", 0, 82.36f) \
	X(SNAP,  9.18f, 68.f,  "STEP", Rotator::STEP_PARAM, 0) \
	X(KNOB,  22.78f, 68.f,  "GLIDE", Rotator::GLIDE_PARAM, 0) \
	X(SNAP,  36.38f, 68.f,  "FOLDS", Rotator::FOLDS_PARAM, 0) \
	X(SNAP,  49.98f, 68.f,  "EDGE", Rotator::EDGE_PARAM, 0) \
	X(KNOB,  63.58f, 68.f,  "PIVOT X", Rotator::PIVOT_X_PARAM, 0) \
	X(KNOB,  77.18f, 68.f,  "PIVOT Y", Rotator::PIVOT_Y_PARAM, 0) \
	X(JACK,  9.18f, 92.f,  "CV", Rotator::STEP_INPUT, 0) \
	X(JACK,  22.78f, 92.f,  "CV", Rotator::GLIDE_INPUT, 0) \
	X(JACK,  36.38f, 92.f,  "CV", Rotator::FOLDS_INPUT, 0) \
	X(JACK,  49.98f, 92.f,  "CV", Rotator::EDGE_INPUT, 0) \
	X(JACK,  63.58f, 92.f,  "CV", Rotator::PIVOT_X_INPUT, 0) \
	X(JACK,  77.18f, 92.f,  "CV", Rotator::PIVOT_Y_INPUT, 0) \
	X(RULE,  4.f, 101.f, "", 0, 82.36f) \
	X(JACK,  9.18f, 117.f, "BUS IN", Rotator::VIDEO_BUS_INPUT, 0) \
	X(LIGHT, 43.18f, 117.f, "SIG", Rotator::SIGNAL_LIGHT, 0) \
	X(OUT,   77.18f, 117.f, "BUS", Rotator::VIDEO_BUS_OUTPUT, 12.f)
