#pragma once

#include <memory>
#include <string>
#include <vector>

namespace video_player {

struct DecodedFrame {
	int width = 0;                     // display size, after rotation metadata is applied
	int height = 0;
	std::vector<unsigned char> rgba;   // width * height * 4 bytes, top row first, no padding
	double presentationSeconds = 0.0;  // time of this frame within the clip
};

// Reads one local video file and yields frames in presentation order.
// Not thread-safe: create, use and destroy on a single thread.
class FrameSource {
public:
	virtual ~FrameSource() = default;
	// Opens the file. Frames are scaled so the longer side is at most maxLongSide pixels (never upscaled).
	// Returns false on failure; lastError() then gives a short reason.
	virtual bool open(const std::string& path, int maxLongSide) = 0;
	// Fills out with the next frame. Returns false at the end of the clip or on error.
	// lastError() is empty at the normal end of the clip and non-empty on error.
	virtual bool nextFrame(DecodedFrame& out) = 0;
	// Starts again at the first frame, reading the whole clip. Returns false on failure.
	virtual bool rewind() = 0;
	// Jumps to `seconds`, in the same time base as DecodedFrame::presentationSeconds. The next
	// nextFrame() gives the first frame at or after that time. When the clip has no frame there,
	// nextFrame() returns false with an empty lastError(), as at the normal end of the clip.
	// Returns false on failure; lastError() then gives a short reason. A later rewind() still
	// starts at the first frame.
	virtual bool seek(double seconds) = 0;
	virtual double durationSeconds() const = 0;  // 0 when unknown
	virtual int width() const = 0;               // display size of the opened file; 0 before a successful open
	virtual int height() const = 0;
	virtual std::string lastError() const = 0;
};

// true on macOS builds, false elsewhere.
bool platformSupportsPlayback();
// AVFoundation decoder on macOS. On other platforms, a source whose open() always returns false.
std::unique_ptr<FrameSource> createFrameSource();

}  // namespace video_player
