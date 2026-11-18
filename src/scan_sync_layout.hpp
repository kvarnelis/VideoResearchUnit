#pragma once

// The Scan Sync panel (VRU-28), as data (see src/panel_items.hpp for the item kinds). ScanSyncWidget places everything from
// this list and scripts/check-panel.py reads the same list to draw the panel to scale and to fail on any overlap.
// Controls above, the four outputs on dark plates in one block at the bottom.

constexpr float kScanSyncPanelWidth = 50.8f;   // 10 HP

#define SCAN_SYNC_LAYOUT(X) \
	X(SNAP, 25.4f, 42.f,  "LINES", ScanSync::LINES_PARAM, 0) \
	X(SNAP, 25.4f, 70.f,  "SLOW",  ScanSync::SLOW_PARAM, 0) \
	X(OUT,   7.7f, 118.f, "TOP",    ScanSync::TOP_OUTPUT, 11.2f) \
	X(OUT,  19.5f, 118.f, "DOWN",   ScanSync::DOWN_OUTPUT, 11.2f) \
	X(OUT,  31.3f, 118.f, "LINE",   ScanSync::LINE_OUTPUT, 11.2f) \
	X(OUT,  43.1f, 118.f, "ACROSS", ScanSync::ACROSS_OUTPUT, 11.2f)
