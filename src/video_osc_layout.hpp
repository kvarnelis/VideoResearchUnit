#pragma once

// The Video Oscillator panel, as data. VideoOscWidget (src/VideoOsc.cpp) places every control, jack, label and rule
// from this list, and scripts/check-video-osc-panel.py reads the same list to draw the panel to scale and to
// fail if any two things overlap or leave the panel. Edit positions here and nowhere else.
//
// X(kind, x, y, "label", id, extra), all in mm from the panel's top left.
//   KNOB   black knob (9.6 mm) with its label above; id is a VideoOsc::ParamId
//   SNAP   the same with a stepped knob
//   SMALL  small knob (7.68 mm)
//   SWITCH two-position switch (4.74 x 6.99 mm); BARS above and ROWS below it; label (may be "") above BARS
//   JACK   input jack (8.03 mm) with its label above; id is a VideoOsc::InputId
//   CELL   input jack with no label, named by the table's row and column text
//   OUT    output jack on its dark plate (extra is the plate width), label inside the plate; id is a VideoOsc::OutputId
//   LIGHT  the FRAME light with its label above
//   TEXT   panel text centred at x, y (extra is the size in mm: 3.8 label, 4.9 title)
//   LEFT   panel text whose left edge is at x
//   RULE   hairline from x to extra at y
// ids are written without the VideoOsc:: prefix; 0 where there is none.

constexpr float kVideoOscPanelWidth = 147.32f;   // 29 HP

#define VIDEO_OSC_LAYOUT(X) \
	/* the picture's own controls, beside the preview screen (54 x 36 mm at 4, 19) */ \
	X(SNAP,   66.5f,  30.5f, "PATTERN", VideoOsc::PATTERN_PARAM, 0) \
	X(KNOB,   79.5f,  30.5f, "HUE",     VideoOsc::HUE_PARAM, 0) \
	X(KNOB,   92.5f,  30.5f, "SPREAD",  VideoOsc::SPREAD_PARAM, 0) \
	X(SMALL,  66.5f,  47.f,  "SAT",     VideoOsc::SATURATION_PARAM, 0) \
	X(SMALL,  79.5f,  47.f,  "BRI",     VideoOsc::BRIGHTNESS_PARAM, 0) \
	X(SMALL,  92.5f,  47.f,  "CON",     VideoOsc::CONTRAST_PARAM, 0) \
	/* three oscillator rows: the same four controls at the same x; the column names are written once, over row A */ \
	X(TEXT,    7.5f,  72.f,  "A", 0, 4.9f) \
	X(KNOB,   19.f,   72.f,  "FREQ",  VideoOsc::FREQUENCY_PARAM, 0) \
	X(KNOB,   33.5f,  72.f,  "WAVE",  VideoOsc::SHAPE_PARAM, 0) \
	X(SWITCH, 46.f,   72.f,  "RANGE", VideoOsc::RANGE_A_PARAM, 0) \
	X(KNOB,   59.f,   72.f,  "LOCK",  VideoOsc::LOCK_A_PARAM, 0) \
	X(TEXT,    7.5f,  90.f,  "B", 0, 4.9f) \
	X(KNOB,   19.f,   90.f,  "",      VideoOsc::FREQ_B_PARAM, 0) \
	X(KNOB,   33.5f,  90.f,  "",      VideoOsc::WAVE_B_PARAM, 0) \
	X(SWITCH, 46.f,   90.f,  "",      VideoOsc::RANGE_B_PARAM, 0) \
	X(KNOB,   59.f,   90.f,  "",      VideoOsc::LOCK_B_PARAM, 0) \
	X(TEXT,    7.5f,  108.f, "C", 0, 4.9f) \
	X(KNOB,   19.f,   108.f, "",      VideoOsc::FREQ_C_PARAM, 0) \
	X(KNOB,   33.5f,  108.f, "",      VideoOsc::WAVE_C_PARAM, 0) \
	X(SWITCH, 46.f,   108.f, "",      VideoOsc::RANGE_C_PARAM, 0) \
	X(KNOB,   59.f,   108.f, "",      VideoOsc::LOCK_C_PARAM, 0) \
	/* what is not the same for all three, to the right of the rows */ \
	X(KNOB,   73.5f,  72.f,  "BEND",  VideoOsc::BEND_PARAM, 0) \
	X(SNAP,   87.f,   72.f,  "MIX",   VideoOsc::MIX_PARAM, 0) \
	X(KNOB,   73.5f,  90.f,  "FB",    VideoOsc::FEEDBACK_PARAM, 0) \
	X(KNOB,   87.f,   90.f,  "AMT",   VideoOsc::FM_AMOUNT_PARAM, 0) \
	X(KNOB,   73.5f,  108.f, "LEVEL", VideoOsc::LEVEL_C_PARAM, 0) \
	/* the input table: columns A, B, C; rows named once at the left */ \
	X(TEXT,  117.5f,  26.3f, "A", 0, 3.8f) \
	X(TEXT,  128.5f,  26.3f, "B", 0, 3.8f) \
	X(TEXT,  139.5f,  26.3f, "C", 0, 3.8f) \
	X(LEFT,  100.5f,  34.f,  "FREQ",  0, 3.8f) \
	X(CELL,  117.5f,  34.f,  "", VideoOsc::FREQUENCY_INPUT, 0) \
	X(CELL,  128.5f,  34.f,  "", VideoOsc::FREQ_B_INPUT, 0) \
	X(CELL,  139.5f,  34.f,  "", VideoOsc::FREQ_C_INPUT, 0) \
	X(LEFT,  100.5f,  44.f,  "V/OCT", 0, 3.8f) \
	X(CELL,  117.5f,  44.f,  "", VideoOsc::VOCT_A_INPUT, 0) \
	X(CELL,  128.5f,  44.f,  "", VideoOsc::VOCT_B_INPUT, 0) \
	X(CELL,  139.5f,  44.f,  "", VideoOsc::VOCT_C_INPUT, 0) \
	X(LEFT,  100.5f,  54.f,  "LOCK",  0, 3.8f) \
	X(CELL,  117.5f,  54.f,  "", VideoOsc::LOCK_A_INPUT, 0) \
	X(CELL,  128.5f,  54.f,  "", VideoOsc::LOCK_B_INPUT, 0) \
	X(CELL,  139.5f,  54.f,  "", VideoOsc::LOCK_C_INPUT, 0) \
	X(LEFT,  100.5f,  64.f,  "IN",    0, 3.8f) \
	X(CELL,  117.5f,  64.f,  "", VideoOsc::IN_A_INPUT, 0) \
	X(CELL,  128.5f,  64.f,  "", VideoOsc::IN_B_INPUT, 0) \
	X(CELL,  139.5f,  64.f,  "", VideoOsc::IN_C_INPUT, 0) \
	/* the other inputs, under a rule: one oscillator's or the whole picture's */ \
	X(RULE,  99.5f,   70.7f, "", 0, 144.f) \
	X(JACK,  117.5f,  80.f,  "WAVE A", VideoOsc::SHAPE_INPUT, 0) \
	X(JACK,  128.5f,  80.f,  "FB",     VideoOsc::FEEDBACK_INPUT, 0) \
	X(JACK,  139.5f,  80.f,  "HUE",    VideoOsc::HUE_INPUT, 0) \
	X(JACK,  117.5f,  93.5f, "BEND",   VideoOsc::BEND_INPUT, 0) \
	X(JACK,  128.5f,  93.5f, "FM IN",  VideoOsc::FM_INPUT, 0) \
	/* the outputs, one block at the bottom: SOUND A, B, C under their columns, and BUS with its FRAME light */ \
	X(LIGHT, 106.5f,  107.f, "FRAME",  VideoOsc::FRAME_LIGHT, 0) \
	X(TEXT,  128.5f,  111.7f, "SOUND", 0, 3.8f) \
	X(OUT,   106.5f,  118.5f, "BUS",   VideoOsc::VIDEO_BUS_OUTPUT, 9.2f) \
	X(OUT,   117.5f,  118.5f, "A", VideoOsc::AUDIO_A_OUTPUT, 9.2f) \
	X(OUT,   128.5f,  118.5f, "B", VideoOsc::AUDIO_B_OUTPUT, 9.2f) \
	X(OUT,   139.5f,  118.5f, "C", VideoOsc::AUDIO_C_OUTPUT, 9.2f)
