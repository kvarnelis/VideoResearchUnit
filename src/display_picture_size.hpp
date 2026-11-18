#pragma once

#include <algorithm>
#include <cmath>

// The Display's "Picture size" choice, shared by the module and the offline tests. No Rack dependencies.
// The choice is the most rows (lines) the picture may have; the width follows at 4:3.

constexpr int kDisplayLinesChoices[] = {1080, 720, 540};
constexpr int kDisplayLinesOldPatch = 1080;  // a patch saved before the choice existed: its exact old output
constexpr int kDisplayLinesNew = 720;        // a Display added now
constexpr int kDisplayMinRenderLines = 480;
constexpr int kDisplayMaxRenderWidth = 2048;
constexpr float kDisplayScreenAspect = 4.f / 3.f;

// Snaps to a listed choice; anything else (a hand-edited patch) is the old-patch value.
inline int displayClampLines(long long lines) {
	for (int choice : kDisplayLinesChoices) {
		if (lines == choice) {
			return choice;
		}
	}
	return kDisplayLinesOldPatch;
}

// The picture size for a screen `wantedRows` device pixels tall: rows clamped to [480, maxLines], width at 4:3,
// and the width capped (the rows then follow it).
inline void displayPictureSize(float wantedRows, int maxLines, int& width, int& height) {
	height = std::clamp(static_cast<int>(std::lround(wantedRows)), kDisplayMinRenderLines, std::max(maxLines, kDisplayMinRenderLines));
	width = std::max(1, static_cast<int>(std::lround(height * kDisplayScreenAspect)));
	if (width > kDisplayMaxRenderWidth) {
		width = kDisplayMaxRenderWidth;
		height = std::max(1, static_cast<int>(std::lround(width / kDisplayScreenAspect)));
	}
}

// The size a choice makes at full height (the right-hand text of its menu item), e.g. "1440x1080".
inline void displayChoiceSize(int lines, int& width, int& height) {
	displayPictureSize(static_cast<float>(lines), lines, width, height);
}
