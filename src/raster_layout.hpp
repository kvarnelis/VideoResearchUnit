#pragma once

// The Raster panel (VRU-29), as data (see src/panel_items.hpp for the item kinds). RasterWidget places everything from
// this list and scripts/check-panel.py reads the same list to draw the panel to scale and to fail on any overlap.
// Inputs are above, the one output is on its dark plate at the bottom. Edit positions here and nowhere else.

constexpr float kRasterPanelWidth = 50.8f;   // 10 HP

#define RASTER_LAYOUT(X) \
	X(SNAP,   9.5f, 31.f,  "LINES",  Raster::LINES_PARAM, 0) \
	X(SNAP,  22.5f, 31.f,  "SLOW",   Raster::SLOW_PARAM, 0) \
	X(SW2,   36.f,  31.f,  "FILL|JOIN|HOLD", Raster::FILL_PARAM, 0) \
	X(JACK,  10.2f, 56.f,  "BRIGHT", Raster::BRIGHT_INPUT, 0) \
	X(JACK,  25.4f, 56.f,  "X",      Raster::X_INPUT, 0) \
	X(JACK,  40.6f, 56.f,  "Y",      Raster::Y_INPUT, 0) \
	X(JACK,  10.2f, 76.f,  "R",      Raster::R_INPUT, 0) \
	X(JACK,  25.4f, 76.f,  "G",      Raster::G_INPUT, 0) \
	X(JACK,  40.6f, 76.f,  "B",      Raster::B_INPUT, 0) \
	X(OUT,   25.4f, 118.f, "BUS",    Raster::BUS_OUTPUT, 12.f)
