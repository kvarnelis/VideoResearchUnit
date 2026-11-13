#pragma once

// Recording of the Display module's picture to a video file (the REC button).
//
// Rack-free so it can be tested on its own. Two parts:
//  - FrameSink: one video file being written. The macOS implementation (display_recorder_mac.mm)
//    uses AVFoundation; other platforms get a stub that reports "macOS only".
//  - Session: one recording. It owns a worker thread that paces frames at a constant rate, converts
//    them to the recording size, and hands them to a FrameSink. The caller (the UI thread) only
//    copies a frame into a small mailbox. Nothing here touches the disk or the encoder from the
//    caller's thread, and nothing ever makes the caller wait for the encoder.

#include "audio_ring.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace display_recorder {

// One video file being written. Not thread-safe: one thread (the Session's worker) uses it from
// open() to finish() or cancel().
class FrameSink {
public:
	virtual ~FrameSink() = default;
	// Creates the file at `path`. width and height are even. false: lastError() says why.
	virtual bool open(const std::string& path, int width, int height, int fps) = 0;
	// false only while the encoder is busy and cannot take another frame right now; the caller then
	// drops the frame. After an encoder failure it returns true, so that append() reports the error.
	virtual bool readyForFrame() = 0;
	// Adds one RGBA frame (top row first, rowBytes per row) of the opened size. ptsSeconds counts from
	// the start of the recording and must increase from one call to the next.
	virtual bool append(const unsigned char* rgba, int rowBytes, double ptsSeconds) = 0;
	// Ends the recording at endSeconds and closes the file. Blocks until the file is complete and
	// playable. false: lastError() says why.
	virtual bool finish(double endSeconds) = 0;
	// Abandons the recording and deletes the file. Safe to call at any time, and after finish().
	virtual void cancel() = 0;
	virtual std::string lastError() const = 0;

	// ---- Audio (optional) ----
	// Call before open(): asks for a stereo audio track at sampleRate Hz. false: this sink cannot (the movie
	// is then written without sound). hasAudio() says whether open() really made the track.
	virtual bool enableAudio(int sampleRate) { (void)sampleRate; return false; }
	virtual bool hasAudio() const { return false; }
	// false only while the encoder is busy; the caller keeps the samples and tries again.
	virtual bool audioReady() { return true; }
	// Adds interleaved stereo float frames (+-1.0 full scale) whose first frame is at ptsSeconds. Audio must
	// be contiguous: each call starts where the last one ended.
	virtual bool appendAudio(const float* stereo, int frames, double ptsSeconds) {
		(void)stereo; (void)frames; (void)ptsSeconds;
		return false;
	}
};

// true on macOS builds, false elsewhere.
bool platformSupportsRecording();
// AVFoundation H.264 writer on macOS. On other platforms, a sink whose open() always fails.
std::unique_ptr<FrameSink> createFrameSink();

enum class SessionState {
	Starting,   // the worker is choosing a file name and opening the encoder
	Recording,
	Finishing,  // stop was requested; the file is being closed
	Finished,   // the file is complete
	Failed,     // nothing usable was written; error() says why
};

struct SessionStats {
	std::uint32_t written = 0;   // frames handed to the encoder
	std::uint32_t dropped = 0;   // frame slots skipped because the worker or the encoder was behind
	std::uint32_t repeated = 0;  // frames written again because the picture had not changed
	bool hasAudio = false;                 // the movie has an audio track
	std::uint64_t audioFrames = 0;         // audio sample frames handed to the encoder
	float audioPeak = 0.f;                 // largest absolute sample the audio ring delivered (0: the source was silent)
	std::uint64_t audioDropped = 0;        // audio frames lost because the ring overflowed (replaced by silence)
	bool stoppedForRateChange = false;     // the engine sample rate changed, so the recording was ended
};

struct SessionOptions {
	int fps = 30;
	// The following three run on the worker thread. They must not use anything the caller may destroy,
	// so capture values, not pointers to the caller's objects.
	std::function<std::unique_ptr<FrameSink>()> makeSink;
	// Picks the file to write: the folder, creating it, and a free name. false: error says why.
	std::function<bool(std::string& path, std::string& error)> choosePath;
	// Called once with the chosen path when the session is over, whatever happened (to release it).
	std::function<void(const std::string& path)> pathDone;
	// Audio. With `audio` null (or audioSampleRate 0) the movie has no sound and the timing is exactly the
	// picture's own. Otherwise the movie gets a stereo track from the ring, and the audio sample count is the
	// master clock: each picture is stamped with the time the samples have reached when it is written, so
	// sound and picture cannot drift apart. The ring's producer must already be running at audioSampleRate.
	// If the ring's rate later differs, or the engine stops sending samples, see Session below.
	std::shared_ptr<AudioRing> audio;
	int audioSampleRate = 0;
};

struct SessionShared;

// One recording, from the first frame to a closed file.
//
// Timing: frames are written on a constant timeline, kFps per second, starting when the encoder is
// open. At each tick the worker writes the newest frame it was given, or the previous one again when
// nothing new arrived (a static picture), so the movie plays at the speed the picture changed on
// screen. If the worker or the encoder falls behind, the ticks that were missed are skipped: the
// previous frame simply stays on screen for longer. The caller is never blocked.
//
// Audio: with an AudioRing the worker drains it, discarding what arrived before the encoder was open, and
// writes it contiguously. Picture n is written when the audio clock reaches n / fps, so a paused engine
// (no samples arriving) pauses the movie, and the movie never gets longer than the audio. If the engine
// sample rate changes during the recording the worker closes the file at that point
// (stats().stoppedForRateChange). If the ring overflows, the missing frames become silence.
//
// Size: fixed by the first frame, rounded down to even numbers (H.264 needs them). A later frame of
// that size, or one pixel larger in either direction, is used as it is (cropped by that pixel). Any
// other size is scaled to the recording size (bilinear) on the worker thread.
class Session {
public:
	static constexpr int kFps = 30;

	// Starts the worker. firstRgba is copied before the constructor returns.
	Session(SessionOptions options, const unsigned char* firstRgba, int width, int height);
	// Asks the worker to finish, waits for it for a limited time (about 15 s), then lets go of it.
	~Session();
	Session(const Session&) = delete;
	Session& operator=(const Session&) = delete;

	// Caller's thread only. Copies the frame into the mailbox and returns; at most about 70 frames a
	// second are taken, the rest are ignored. A no-op once stop was requested or the session failed.
	void submit(const unsigned char* rgba, int width, int height);
	// Asks the worker to write the last frame and close the file. Returns at once.
	void requestStop();
	// Waits until the worker has ended (Finished or Failed). false on timeout.
	bool waitUntilDone(int timeoutMs);

	SessionState state() const;
	// Starting or Recording: frames are still being accepted.
	bool active() const;
	std::string path() const;   // "" until the file name is chosen
	std::string error() const;  // "" unless the state is Failed
	SessionStats stats() const;
	int outputWidth() const;
	int outputHeight() const;

private:
	std::shared_ptr<SessionShared> shared;
	std::thread worker;
	std::chrono::steady_clock::time_point lastSubmit;
};

}  // namespace display_recorder
