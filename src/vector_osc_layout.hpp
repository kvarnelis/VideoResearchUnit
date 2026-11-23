#pragma once

// The Vector Osc panel (VRU-25), as data (see src/panel_items.hpp for the item kinds). VectorOscWidget places everything
// from this list and scripts/check-panel.py reads the same list to draw the panel to scale and to fail on any overlap.
// Two rows of five columns. A column is a knob, its CV attenuverter (AMT) and its CV jack. FREQ has no attenuverter:
// its jack is V/OCT, which has to stay 1 V per octave. The last column of the second row holds only SYNC.
// Edit positions here and nowhere else.

constexpr float kVectorOscPanelWidth = 81.28f;   // 16 HP

#define VECTOR_OSC_LAYOUT(X) \
	X(KNOB,   9.44f, 30.f,  "FREQ",   VectorOsc::FREQ_PARAM, 0) \
	X(SNAP,  25.04f, 30.f,  "RATIO",  VectorOsc::RATIO_PARAM, 0) \
	X(KNOB,  40.64f, 30.f,  "FINE",   VectorOsc::FINE_PARAM, 0) \
	X(KNOB,  56.24f, 30.f,  "PHASE",  VectorOsc::PHASE_PARAM, 0) \
	X(SNAP,  71.84f, 30.f,  "VOICES", VectorOsc::VOICES_PARAM, 0) \
	X(TRIM,  25.04f, 43.f,  "AMT",    VectorOsc::RATIO_CV_PARAM, 0) \
	X(TRIM,  40.64f, 43.f,  "AMT",    VectorOsc::FINE_CV_PARAM, 0) \
	X(TRIM,  56.24f, 43.f,  "AMT",    VectorOsc::PHASE_CV_PARAM, 0) \
	X(TRIM,  71.84f, 43.f,  "AMT",    VectorOsc::VOICES_CV_PARAM, 0) \
	X(JACK,   9.44f, 55.5f, "V/OCT",  VectorOsc::VOCT_INPUT, 0) \
	X(JACK,  25.04f, 55.5f, "CV",     VectorOsc::RATIO_INPUT, 0) \
	X(JACK,  40.64f, 55.5f, "CV",     VectorOsc::FINE_INPUT, 0) \
	X(JACK,  56.24f, 55.5f, "CV",     VectorOsc::PHASE_INPUT, 0) \
	X(JACK,  71.84f, 55.5f, "CV",     VectorOsc::VOICES_INPUT, 0) \
	X(KNOB,   9.44f, 74.f,  "DETUNE", VectorOsc::DETUNE_PARAM, 0) \
	X(KNOB,  25.04f, 74.f,  "WAVE",   VectorOsc::WAVE_PARAM, 0) \
	X(KNOB,  40.64f, 74.f,  "Y WAVE", VectorOsc::WAVE_Y_PARAM, 0) \
	X(KNOB,  56.24f, 74.f,  "LEVEL",  VectorOsc::LEVEL_PARAM, 0) \
	X(TRIM,   9.44f, 87.f,  "AMT",    VectorOsc::DETUNE_CV_PARAM, 0) \
	X(TRIM,  25.04f, 87.f,  "AMT",    VectorOsc::WAVE_CV_PARAM, 0) \
	X(TRIM,  40.64f, 87.f,  "AMT",    VectorOsc::WAVE_Y_CV_PARAM, 0) \
	X(TRIM,  56.24f, 87.f,  "AMT",    VectorOsc::LEVEL_CV_PARAM, 0) \
	X(JACK,   9.44f, 99.5f, "CV",     VectorOsc::DETUNE_INPUT, 0) \
	X(JACK,  25.04f, 99.5f, "CV",     VectorOsc::WAVE_INPUT, 0) \
	X(JACK,  40.64f, 99.5f, "CV",     VectorOsc::WAVE_Y_INPUT, 0) \
	X(JACK,  56.24f, 99.5f, "CV",     VectorOsc::LEVEL_INPUT, 0) \
	X(JACK,  71.84f, 99.5f, "SYNC",   VectorOsc::SYNC_INPUT, 0) \
	X(OUT,   25.04f, 118.f, "X",      VectorOsc::X_OUTPUT, 13.f) \
	X(OUT,   40.64f, 118.f, "Y",      VectorOsc::Y_OUTPUT, 13.f) \
	X(OUT,   56.24f, 118.f, "Z",      VectorOsc::Z_OUTPUT, 13.f)
