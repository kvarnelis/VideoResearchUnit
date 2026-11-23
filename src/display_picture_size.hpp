#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

// The Display's "Picture size" choice, shared by the module and the offline tests. No Rack dependencies.
// The choice is the most rows (lines) the picture may have; the width follows at 4:3.

constexpr int kDisplayLinesChoices[] = {1080, 720, 540};
constexpr int kDisplayDefaultLines = 720;
inline std::atomic<int> displayPreferredLines{kDisplayDefaultLines};
constexpr int kDisplayMinRenderLines = 480;
constexpr int kDisplayMaxRenderWidth = 2048;
constexpr float kDisplayScreenAspect = 4.f / 3.f;

// Missing or unsupported choices use the caller's default.
inline int displayClampLines(long long lines, int fallback) {
	for (int choice : kDisplayLinesChoices) {
		if (lines == choice) {
			return choice;
		}
	}
	return fallback;
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
