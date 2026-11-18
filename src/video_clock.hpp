#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// The video clock: one place that says where a Rack audio sample falls in video time.
// See docs/architecture.md, "Video clock".
//
// Video time is a pure function of the audio frame number (Rack's `ProcessArgs::frame`, which counts
// samples since the engine's first sample and is the same number in every module), the sample rate and
// the line count. The clock holds no state that anything writes: it is a value, so reading it from the
// audio thread (no lock, no allocation) and from the render side are the same call and cannot disagree.
//
//   - A field is (lines + 22.5) lines long, 262.5 at the default 240 lines, at a line rate of exactly
//     15 750 000 / 1001 Hz. That makes the default field rate exactly 60 000 / 1001 = 59.94006 Hz.
//   - A field is a FRACTIONAL number of samples (800.8 at 48 kHz and 240 lines; the old window was rounded
//     to 801). It is never rounded.
//   - Field n starts at audio frame n x samplesPerField(), a fractional frame. Frame 0 is the start of
//     field 0. Nothing ever moves that origin (there is no reset), so every module and every reader
//     that asks about the same frame gets the same answer.
//   - Inside a field the visible lines come first (line 0 at the top), then the flyback, which is
//     22.5 lines with the beam off. The last line of a field is a half line when the count ends in .5.
//
// The picture a scan module draws for field n shows the audio from frame n x samplesPerField() for one
// field; its top left corner is that frame. A pulse that goes high on the first whole frame at or after
// that point (see ScanSync) therefore marks the top of that picture to within one sample.

namespace vclock {

constexpr double kLineRateHz = 15750000.0 / 1001.0;  // 15734.2657...; 262.5 lines at 60000/1001 Hz
constexpr double kBlankingLines = 22.5;               // lines per field with the beam off
constexpr double kDefaultFieldRateHz = 60000.0 / 1001.0;
constexpr int kDefaultLines = 240;

// Slow scan. With SLOW = K a picture takes K fields of the clock instead of one, at the same line count: every line lasts
// K times longer, so it holds K times as many samples (3.05 x K at 48 kHz) and a picture takes K x (lines + 22.5) lines
// of time. Picture n starts at field n x K, i.e. at frame n x K x samplesPerField() (the clock's own field, so every module
// that uses the same LINES and SLOW agrees on where a picture starts, and K = 1 is the clock exactly as before).
// A SLOW knob stores a step 0 to 7; K = 2 to the step: x1, x2, x4, x8, x16, x32, x64, x128.
constexpr int kSlowSteps = 8;
inline int slowFromStep(float step) {
	if (!(step == step)) return 1;
	const int s = std::min(std::max(static_cast<int>(std::lround(step)), 0), kSlowSteps - 1);
	return 1 << s;
}

// Where a point of audio time sits in the picture.
struct VideoTime {
	int64_t field = 0;       // field number, counted from frame 0
	double fieldPhase = 0.0; // 0 at the top of the picture, 1 at the start of the next field (the whole field, flyback included)
	int line = 0;            // line within the field, 0 for the top line; 240 and up are flyback lines at 240 lines
	double linePhase = 0.0;  // 0 at the left of the line, 1 at the start of the next line
	bool visible = true;     // false in the flyback
};

// Rounding slack, in samples: frames this close to a field or line start count as being on it.
constexpr double kSnap = 1e-7;

class Clock {
public:
	Clock() : Clock(48000.0, kDefaultLines) {}
	// slow: the SLOW factor K (1 for the plain clock). Then "a field" below is a slow picture of K ordinary fields.
	Clock(double sampleRate, int visibleLines, int slow = 1)
		: rate(sampleRate > 0.0 ? sampleRate : 48000.0), lines(std::max(visibleLines, 1)), slowK(std::max(slow, 1)) {
		linesInField = static_cast<double>(lines) + kBlankingLines;
		lineSec = static_cast<double>(slowK) / kLineRateHz;
		fieldSec = linesInField * lineSec;
		samplesField = fieldSec * rate;
		samplesLine = lineSec * rate;
	}

	double sampleRate() const { return rate; }
	int visibleLines() const { return lines; }
	int slow() const { return slowK; }
	double linesPerField() const { return linesInField; }
	double lineSeconds() const { return lineSec; }
	double fieldSeconds() const { return fieldSec; }
	double fieldRateHz() const { return 1.0 / fieldSec; }
	// Fractional, never rounded. 800.8 at 48 kHz and 240 lines (48000 x 1001 / 60000).
	double samplesPerField() const { return samplesField; }
	double samplesPerLine() const { return samplesLine; }
	// Where the visible part of the picture ends, as a fraction of the field (the rest is flyback).
	double visibleFraction() const { return static_cast<double>(lines) / linesInField; }

	// Audio frame (fractional) to video time.
	VideoTime fromFrame(double frame) const {
		VideoTime t;
		double n = std::floor(frame / samplesField);
		double inField = frame - n * samplesField;  // in [0, samplesField) up to rounding
		if (inField < 0.0) {
			n -= 1.0;
			inField += samplesField;
		}
		// A frame that is a field start in exact arithmetic (frame 4004 is field 5 at 48 kHz) can land a rounding
		// error short of it in doubles. Treat anything within kSnap of the next start as that start.
		if (inField >= samplesField - kSnap) {
			n += 1.0;
			inField = std::max(0.0, inField - samplesField);
		}
		t.field = static_cast<int64_t>(n);
		t.fieldPhase = inField / samplesField;
		const double lineF = inField / samplesLine;
		double whole = std::floor(lineF + kSnap);
		t.line = static_cast<int>(std::min(whole, std::ceil(linesInField) - 1.0));
		t.linePhase = std::clamp(lineF - static_cast<double>(t.line), 0.0, 1.0);
		t.visible = t.line < lines;
		return t;
	}

	// Video time to audio frame. The inverse of fromFrame (field and fieldPhase decide it; line and
	// linePhase are the same position said another way).
	double toFrame(const VideoTime& t) const {
		return static_cast<double>(t.field) * samplesField + t.fieldPhase * samplesField;
	}
	// The same from a line and a position along it.
	double toFrame(int64_t field, int line, double linePhase) const {
		return static_cast<double>(field) * samplesField + (static_cast<double>(line) + linePhase) * samplesLine;
	}
	// The (fractional) frame at which a field starts: the top left of its picture.
	double fieldStartFrame(int64_t field) const { return static_cast<double>(field) * samplesField; }
	// The field a whole frame belongs to.
	int64_t fieldOfFrame(int64_t frame) const {
		return static_cast<int64_t>(std::floor(static_cast<double>(frame) / samplesField));
	}
	// Scan time in seconds from the start of a field to a position in samples, and back.
	double secondsToSamples(double seconds) const { return seconds * rate; }

private:
	double rate;
	int lines;
	int slowK = 1;
	double linesInField = 262.5;
	double lineSec = 0.0;
	double fieldSec = 0.0;
	double samplesField = 0.0;
	double samplesLine = 0.0;
};

}  // namespace vclock
