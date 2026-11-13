#pragma once

#include "video_player_core.hpp"
#include "video_player_decoder.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Video Player engine. Pure C++17 with no Rack dependency, so tests/video_player_engine_test.cpp
// can run it under `make test` with a fake decoder and a fake yt-dlp.
//
// One worker thread owns everything slow: opening files, running yt-dlp, decoding and the playback
// clock. The audio thread only touches the lock-free calls marked below, so it never waits on a
// lock, a file or a child process. Frames go to the video bus through an external frame slot
// (video_external_frame.hpp), referenced by id, so no pixels travel inside the bus node.

namespace video_player {

using FrameSourceFactory = std::function<std::unique_ptr<FrameSource>()>;

struct EngineOptions {
	// Where downloaded YouTube videos are kept: one file per video ID, reused on later loads.
	std::string cacheDir;
	// Longest side of decoded frames, in pixels. Larger clips are scaled down, never up.
	int maxLongSide = 960;
	// Frames decoded ahead of the playback clock. At a loop wrap the decoder rewinds while these
	// frames are still on their way to the screen, so the wrap shows no pause.
	int lookaheadFrames = 8;
	// Where to look for yt-dlp. Left false, the usual order applies: /opt/homebrew/bin,
	// /usr/local/bin, then PATH. Tests turn this on to point at a fake yt-dlp.
	bool useCustomYtDlpDirs = false;
	std::vector<std::string> ytDlpDirs;
	// Empty: the AVFoundation decoder on macOS (and a decoder that always fails elsewhere).
	FrameSourceFactory factory;
	// Called on the worker thread after each frame goes out. Tests use it to watch the frame rhythm.
	std::function<void(const DecodedFrame&)> onFramePublished;
};

// What the panel shows. A copy, so the UI thread can ask every frame.
struct Snapshot {
	TransportState state = TransportState::NoSource;
	std::string sourceName;        // file name, or youtube.com/watch?v=ID; empty with no source
	std::string message;           // plain-words status or error; empty when there is nothing to say
	bool messageIsError = false;
	double positionSeconds = 0.0;  // time of the frame on screen
	double durationSeconds = 0.0;  // 0 when unknown
	int width = 0;
	int height = 0;
};

class Engine {
public:
	// externalFrameId comes from videoExternalFrameAllocate(). The caller releases it after
	// destroying the engine. With id 0 (no free slot) everything works except the picture.
	Engine(int externalFrameId, const EngineOptions& options);
	// Stops the worker. A running yt-dlp is stopped too.
	~Engine();
	Engine(const Engine&) = delete;
	Engine& operator=(const Engine&) = delete;

	// ---- UI thread. Each call returns at once; the work happens on the worker thread. ----

	// Opens a local file and remembers the path for saving. A missing or unsupported file shows
	// a status message and does not crash.
	void loadFile(const std::string& path);
	// Text typed or pasted into the link field. A YouTube link downloads (or reuses the cached
	// copy) and plays. A path is opened like loadFile. Anything else only shows a message and
	// leaves the current video alone. Empty text does nothing.
	void loadLink(const std::string& text);
	// Removes the video and forgets the saved source.
	void clearSource();
	// Applies saved settings after a patch loads. Never starts playback by itself.
	void restore(const Settings& settings);
	Settings settings() const;
	Snapshot snapshot() const;
	// Jumps to `seconds` from the start of the clip, clamped to the clip. The newest target wins:
	// targets posted faster than the worker can seek are dropped, so a drag stays smooth. Playing
	// keeps playing from there. Paused stays paused and shows the new frame. Ready and Ended become
	// Paused, so Play continues from the sought spot. Ignored while no video is loaded, and dropped
	// when a new video starts loading. A seek never fires the end-of-clip pulse and never cancels a
	// download.
	void requestSeek(double seconds);

	// ---- Audio thread. Lock-free. ----

	// Play when stopped or paused, pause when playing, play again from the start after the end.
	void requestPlayToggle() {
		playRequests_.fetch_add(1, std::memory_order_relaxed);
	}
	// Back to the first frame, held there.
	void requestStop() {
		stopRequests_.fetch_add(1, std::memory_order_relaxed);
	}
	// On: wrap at the end. Off: stop at the end and hold the last frame.
	void setLoop(bool on) {
		loop_.store(on, std::memory_order_relaxed);
	}
	// 0.25 to 2.0, where 1.0 is normal speed.
	void setSpeed(float speed) {
		speed_.store(speed < 0.25f ? 0.25f : (speed > 2.f ? 2.f : speed), std::memory_order_relaxed);
	}
	bool hasFrame() const {
		return hasFrame_.load(std::memory_order_relaxed);
	}
	bool isPlaying() const {
		return playing_.load(std::memory_order_relaxed);
	}
	// Counts every end of the clip, including loop wraps. The audio thread turns each new count
	// into one trigger pulse.
	uint32_t endCount() const {
		return endCount_.load(std::memory_order_relaxed);
	}

private:
	using Clock = std::chrono::steady_clock;

	struct Request {
		enum class Kind { None, File, YouTube, Clear };
		Kind kind = Kind::None;
		std::string text;
	};

	struct Queued {
		double pts = 0.0;       // when to show it, on the playback timeline
		double localPts = 0.0;  // time within the clip
		int pass = 0;           // which trip through the clip it belongs to
		bool published = false; // already on the video bus (the first frame, shown while stopped)
		DecodedFrame frame;
	};

	struct PassEnd {
		int pass = 0;
		double startTimeline = 0.0;
		double endTimeline = 0.0;
	};

	enum class Download { Done, Failed, Cancelled };

	void run();
	std::chrono::microseconds step();
	bool requestPending();
	void pollNotice();

	void beginLoad(bool youtube, const std::string& text);
	void loadSource(bool youtube, const std::string& text);
	bool openPath(const std::string& path, std::string& error);
	Download download(const std::string& exe, const std::string& videoId, const std::string& cachePath,
		std::string& error);
	void fail(const std::string& message);
	void unloadSource();
	void enterError(const std::string& message);

	void handleTransport();
	void handleSeek();
	void dropPendingSeek();
	void applyEvent(TransportEvent event);
	bool restartFromBeginning();
	void startClock();
	double mediaNow() const;
	void updateClockForSpeed();

	bool decodeSome();
	bool decodeOne(bool allowEmpty = false);
	bool wrapDecoder();
	void onDecodeEnd();
	void resetTimeline();
	void present();
	bool checkEnd();

	DecodedFrame acquireFrame();
	void recycle(DecodedFrame&& frame);
	void flushRing();
	void publishFrame(const DecodedFrame& frame);
	void clearOutput();
	void publishSnapshot(bool force);

	// ---- Shared between threads, guarded by mutex_ ----
	mutable std::mutex mutex_;
	std::condition_variable cv_;
	Request request_;
	// The newest seek target and whether the worker has yet to act on it. Not a Request: a seek
	// must not cancel a download, so requestPending() does not look at it.
	bool seekPending_ = false;
	double seekSeconds_ = 0.0;
	std::string notice_;
	bool noticePending_ = false;
	Settings settings_;
	Snapshot snapshot_;

	// ---- Lock-free, shared with the audio thread ----
	std::atomic<bool> quit_{false};
	std::atomic<uint32_t> playRequests_{0};
	std::atomic<uint32_t> stopRequests_{0};
	std::atomic<bool> loop_{false};
	std::atomic<float> speed_{1.f};
	std::atomic<bool> hasFrame_{false};
	std::atomic<bool> playing_{false};
	std::atomic<uint32_t> endCount_{0};

	// ---- Worker thread only ----
	const int frameId_;
	EngineOptions options_;
	bool customFactory_ = false;

	std::unique_ptr<FrameSource> source_;
	std::string currentPath_;
	std::string sourceName_;
	std::string message_;
	bool messageIsError_ = false;
	bool noticeActive_ = false;
	Clock::time_point noticeUntil_;
	std::string savedMessage_;
	bool savedMessageIsError_ = false;
	TransportState state_ = TransportState::NoSource;
	double durationSeconds_ = 0.0;
	int width_ = 0;
	int height_ = 0;
	Clock::time_point lastSnapshot_;

	uint32_t playSeen_ = 0;
	uint32_t stopSeen_ = 0;

	// Playback clock: media time = anchorMedia_ + (now - anchorWall_) * anchorSpeed_ while playing.
	Clock::time_point anchorWall_;
	double anchorMedia_ = 0.0;
	double anchorSpeed_ = 1.0;
	double pausedMedia_ = 0.0;

	// Decoding. Every frame gets a spot on one timeline that keeps counting across loop wraps.
	std::deque<Queued> ring_;
	std::deque<PassEnd> passEnds_;
	std::vector<DecodedFrame> pool_;
	int decodePass_ = 0;
	double decodePassOffset_ = 0.0;
	double lastPassEnd_ = 0.0;
	bool sourceAtEnd_ = false;
	bool haveFirstOfPass_ = false;
	double passFirstPts_ = 0.0;
	// Presentation time of the clip's first frame, as the decoder reports it. Often 0, not always.
	// A seek counts from here, and the pass it starts uses it as passFirstPts_, so positions stay
	// relative to the clip start. Set when a trip through the whole clip begins; not reset by
	// resetTimeline(), which a seek calls.
	double clipFirstPts_ = 0.0;
	double prevLocalPts_ = 0.0;
	int passFrames_ = 0;
	double frameDur_ = 1.0 / 30.0;
	double presentedLocal_ = 0.0;

	// Declared last so the thread starts after every other member exists.
	std::thread thread_;
};

}  // namespace video_player
