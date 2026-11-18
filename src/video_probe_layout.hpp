#pragma once

// The Video Probe panel, as data (see src/panel_items.hpp for the item kinds). VideoProbeWidget places every control,
// jack, label and plate from this list, and scripts/check-panel.py reads the same list to draw the panel to scale
// and to fail if any two things overlap or leave the panel. The picture preview (58.7 x 44 mm at x 21.45, y 19) is
// not an item; the checker knows it from kVideoProbePreview. Edit positions here and nowhere else.

constexpr float kVideoProbePanelWidth = 101.6f;   // 20 HP
// Preview screen: x, y, width, height in mm.
constexpr float kVideoProbePreview[4] = {21.45f, 19.f, 58.7f, 44.f};

#define VIDEO_PROBE_LAYOUT(X) \
	X(KNOB,  14.8f,  76.f,  "X",      VideoProbe::X_PARAM, 0) \
	X(KNOB,  32.8f,  76.f,  "Y",      VideoProbe::Y_PARAM, 0) \
	X(KNOB,  50.8f,  76.f,  "SIZE",   VideoProbe::SIZE_PARAM, 0) \
	X(KNOB,  68.8f,  76.f,  "SMOOTH", VideoProbe::SMOOTH_PARAM, 0) \
	X(KNOB,  86.8f,  76.f,  "LEVEL",  VideoProbe::LEVEL_PARAM, 0) \
	X(TRIM,  14.8f,  90.f,  "AMT",    VideoProbe::X_AMT_PARAM, 0) \
	X(TRIM,  32.8f,  90.f,  "AMT",    VideoProbe::Y_AMT_PARAM, 0) \
	X(TRIM,  50.8f,  90.f,  "SLOW",   VideoProbe::SLOW_PARAM, 0) \
	X(TRIM,  68.8f,  90.f,  "LINES",  VideoProbe::LINES_PARAM, 0) \
	X(LIGHT, 86.8f,  93.f,  "SIG",    VideoProbe::SIGNAL_LIGHT, 0) \
	X(JACK,  14.8f,  103.f, "X",      VideoProbe::X_INPUT, 0) \
	X(JACK,  32.8f,  103.f, "Y",      VideoProbe::Y_INPUT, 0) \
	X(JACK,  50.8f,  103.f, "BUS IN", VideoProbe::VIDEO_BUS_INPUT, 0) \
	X(SW2,   68.8f,  103.f, "SCAN|ON|OFF", VideoProbe::SCAN_PARAM, 0) \
	X(OUT,   9.5f,   119.f, "LUMA",   VideoProbe::LUMA_OUTPUT, 11.2f) \
	X(OUT,   21.3f,  119.f, "R",      VideoProbe::R_OUTPUT, 11.2f) \
	X(OUT,   33.1f,  119.f, "G",      VideoProbe::G_OUTPUT, 11.2f) \
	X(OUT,   44.9f,  119.f, "B",      VideoProbe::B_OUTPUT, 11.2f) \
	X(OUT,   56.7f,  119.f, "AVG",    VideoProbe::AVG_OUTPUT, 11.2f) \
	X(OUT,   68.5f,  119.f, "MOTION", VideoProbe::MOTION_OUTPUT, 11.2f) \
	X(OUT,   80.3f,  119.f, "GATE",   VideoProbe::GATE_OUTPUT, 11.2f) \
	X(OUT,   92.1f,  119.f, "FRAME",  VideoProbe::FRAME_OUTPUT, 11.2f)
