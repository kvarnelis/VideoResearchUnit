#pragma once

#include "video_clock.hpp"

#include <algorithm>
#include <cstdint>

// VRU-28 Scan Sync: the video clock (src/video_clock.hpp) as ordinary Rack signals. No Rack types here, so the
// module and its tests share one definition. Every output is a pure function of the audio frame number, the sample
// rate and the line count, so it needs no state and can never drift from the picture a scan module draws.
//
//   top        10 V pulse on the first sample at or after the top of the picture, 1 ms long
//   down       0 V at the top of the picture rising to 10 V at the bottom of the last visible line, then held at
//              10 V through the flyback (the 22.5 lines the beam is off), then back to 0 V
//   line       10 V pulse at the start of every line, 1 ms or half a line, whichever is shorter
//   across     0 V at the left of each line rising to 10 V at its right
//
// The line outputs count every line of the field, flyback lines included, so there are 262.5 line starts a field at
// 240 lines. A line is only about three samples long at Rack's sample rate (3.05 samples at 48 kHz), so the two line
// outputs are coarse: LINE is a pulse about every third sample and ACROSS is a three-step ramp.
namespace scan_sync {

constexpr float kHigh = 10.f;
constexpr double kPulseSeconds = 0.001;
constexpr int kMinLines = 120;
constexpr int kMaxLines = 600;

struct Outputs {
	float top = 0.f;
	float down = 0.f;
	float line = 0.f;
	float across = 0.f;
};

inline Outputs at(const vclock::Clock& clock, int64_t frame) {
	const vclock::VideoTime t = clock.fromFrame(static_cast<double>(frame));
	const double sinceField = t.fieldPhase * clock.samplesPerField();  // samples since the top of the picture
	const double sinceLine = t.linePhase * clock.samplesPerLine();     // samples since the start of this line
	const double pulse = kPulseSeconds * clock.sampleRate();
	Outputs o;
	o.top = sinceField < std::min(pulse, 0.5 * clock.samplesPerField()) ? kHigh : 0.f;
	o.down = static_cast<float>(kHigh * std::min(1.0, t.fieldPhase / clock.visibleFraction()));
	o.line = sinceLine < std::min(pulse, 0.5 * clock.samplesPerLine()) ? kHigh : 0.f;
	o.across = static_cast<float>(kHigh * t.linePhase);
	return o;
}

}  // namespace scan_sync
