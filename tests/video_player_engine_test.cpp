// Offline checks for the Video Player engine. Standalone, with no framework and no Rack SDK.
// make test builds this with src/video_player_core.cpp, src/video_player_engine.cpp and
// src/video_external_frame.cpp, then runs it from the repository root.
//
// A fake decoder stands in for AVFoundation and small shell scripts stand in for yt-dlp, so the
// checks need no yt-dlp, no ffmpeg, no video file and no network. The engine is pointed at the
// scripts through EngineOptions, so the real /opt/homebrew/bin and /usr/local/bin are never used.
// Scratch files go under build/video_player_engine_test_tmp.
//
// Each failure prints one line, then the summary "video_player_engine_test: N checks, F failed".
// Exits 0 only when F is 0.

#include "video_external_frame.hpp"
#include "video_player_core.hpp"
#include "video_player_decoder.hpp"
#include "video_player_engine.hpp"

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace video_player;

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// The audio thread calls these on the engine, so they must never take a lock.
static_assert(std::atomic<bool>::is_always_lock_free, "bool atomics must be lock-free");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "counter atomics must be lock-free");
static_assert(std::atomic<float>::is_always_lock_free, "float atomics must be lock-free");

// This test never links a real decoder. These stand in for src/video_player_decoder_mac.mm and
// src/video_player_decoder_stub.cpp, so the test builds the same way on every platform. They also
// let one check cover the engine when the platform cannot play video.
namespace video_player {
bool platformSupportsPlayback() {
	return false;
}
std::unique_ptr<FrameSource> createFrameSource() {
	return nullptr;
}
}  // namespace video_player

namespace {

int checks = 0;
int failures = 0;
std::string currentTest;

void check(bool ok, const char* what, int line) {
	++checks;
	if (!ok) {
		++failures;
		std::printf("  FAIL [%s] line %d: %s\n", currentTest.c_str(), line, what);
	}
}

void checkText(const std::string& actual, const std::string& expected, const char* what, int line) {
	++checks;
	if (actual != expected) {
		++failures;
		std::printf("  FAIL [%s] line %d: %s is \"%s\", expected \"%s\"\n", currentTest.c_str(), line, what,
			actual.c_str(), expected.c_str());
	}
}

void checkContains(const std::string& text, const std::string& needle, const char* what, int line) {
	++checks;
	if (text.find(needle) == std::string::npos) {
		++failures;
		std::printf("  FAIL [%s] line %d: %s does not contain \"%s\": \"%s\"\n", currentTest.c_str(), line, what,
			needle.c_str(), text.c_str());
	}
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_TEXT(actual, expected) checkText((actual), (expected), #actual, __LINE__)
#define CHECK_CONTAINS(text, needle) checkContains((text), (needle), #text, __LINE__)

void sleepMs(int ms) {
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

double secondsSince(Clock::time_point start) {
	return std::chrono::duration<double>(Clock::now() - start).count();
}

bool waitFor(const std::function<bool()>& condition, double timeoutSeconds = 5.0) {
	const Clock::time_point start = Clock::now();
	while (!condition()) {
		if (secondsSince(start) > timeoutSeconds) {
			return false;
		}
		sleepMs(2);
	}
	return true;
}

bool near(double a, double b, double tolerance) {
	return std::fabs(a - b) <= tolerance;
}

double median(std::vector<double> values) {
	if (values.empty()) {
		return 0.0;
	}
	std::sort(values.begin(), values.end());
	return values[values.size() / 2];
}

// ---------------------------------------------------------------------------------------------
// Files

std::string gRoot;

void writeFile(const std::string& path, const std::string& text, bool executable = false) {
	std::ofstream out(path, std::ios::binary);
	out << text;
	out.close();
	if (executable) {
		chmod(path.c_str(), 0755);
	}
}

bool fileHasContent(const std::string& path) {
	struct stat info;
	return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0;
}

bool fileExists(const std::string& path) {
	struct stat info;
	return stat(path.c_str(), &info) == 0;
}

std::vector<std::string> readLines(const std::string& path) {
	std::vector<std::string> lines;
	std::ifstream in(path);
	std::string line;
	while (std::getline(in, line)) {
		lines.push_back(line);
	}
	return lines;
}

// A file for the engine to open. The fake decoder does not read it; the engine only checks that it exists.
std::string makeMedia(const std::string& name) {
	const std::string path = gRoot + "/media/" + name;
	writeFile(path, "not really a video");
	return path;
}

// ---------------------------------------------------------------------------------------------
// Fake decoder

struct Probe {
	// Describes the clip. Set before the first load; the worker reads them afterwards.
	int frames = 30;
	double fps = 50.0;
	double ptsOffset = 0.0;
	// Added to the duration the file reports, as for a file whose video track ends before its audio.
	double extraDuration = 0.0;
	int width = 8;
	int height = 6;
	// May change between loads.
	std::atomic<bool> failOpen{false};
	std::atomic<int> failAtFrame{-1};
	std::atomic<int> rewindSleepMs{0};
	std::atomic<bool> failSeek{false};
	std::atomic<int> seekSleepMs{0};

	// What the engine did.
	std::atomic<int> rewinds{0};
	std::atomic<int> seeks{0};
	std::atomic<double> lastSeekTarget{-1.0};
	struct Shown {
		int index;
		Clock::time_point when;
	};
	std::mutex mutex;
	std::vector<std::string> opened;
	std::vector<Shown> shown;

	void onShown(const DecodedFrame& frame) {
		const int index = frame.rgba.size() >= 2 ? frame.rgba[0] + 256 * frame.rgba[1] : -1;
		std::lock_guard<std::mutex> lock(mutex);
		shown.push_back({index, Clock::now()});
	}
	size_t shownCount() {
		std::lock_guard<std::mutex> lock(mutex);
		return shown.size();
	}
	int lastShown() {
		std::lock_guard<std::mutex> lock(mutex);
		return shown.empty() ? -1 : shown.back().index;
	}
	std::vector<Shown> shownCopy() {
		std::lock_guard<std::mutex> lock(mutex);
		return shown;
	}
	std::vector<int> indices() {
		std::lock_guard<std::mutex> lock(mutex);
		std::vector<int> out;
		for (const Shown& s : shown) {
			out.push_back(s.index);
		}
		return out;
	}
	std::vector<std::string> openedCopy() {
		std::lock_guard<std::mutex> lock(mutex);
		return opened;
	}
};

// Frame number N carries N in its first two bytes and shows at N / fps seconds.
class FakeSource : public FrameSource {
public:
	explicit FakeSource(Probe* probe) : probe_(probe) {}

	bool open(const std::string& path, int /*maxLongSide*/) override {
		{
			std::lock_guard<std::mutex> lock(probe_->mutex);
			probe_->opened.push_back(path);
		}
		if (probe_->failOpen.load()) {
			error_ = "fake open failure";
			return false;
		}
		opened_ = true;
		return true;
	}

	bool nextFrame(DecodedFrame& out) override {
		if (index_ >= probe_->frames) {
			return false;  // the normal end: lastError() stays empty
		}
		if (index_ == probe_->failAtFrame.load()) {
			error_ = "fake decode failure";
			return false;
		}
		out.width = probe_->width;
		out.height = probe_->height;
		out.rgba.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4u, 0);
		out.rgba[0] = static_cast<unsigned char>(index_ & 255);
		out.rgba[1] = static_cast<unsigned char>((index_ >> 8) & 255);
		out.rgba[3] = 255;
		out.presentationSeconds = probe_->ptsOffset + static_cast<double>(index_) / probe_->fps;
		++index_;
		return true;
	}

	bool rewind() override {
		probe_->rewinds.fetch_add(1);
		const int pause = probe_->rewindSleepMs.load();
		if (pause > 0) {
			sleepMs(pause);  // a decoder takes real time to start over
		}
		index_ = 0;
		error_.clear();
		return true;
	}

	// The next frame is the first one at or after `seconds`. Past the last frame there is none, and
	// that is not an error, as at the end of a real clip.
	bool seek(double seconds) override {
		probe_->seeks.fetch_add(1);
		probe_->lastSeekTarget.store(seconds);
		const int pause = probe_->seekSleepMs.load();
		if (pause > 0) {
			sleepMs(pause);  // a decoder takes real time to find a spot
		}
		if (probe_->failSeek.load()) {
			error_ = "fake seek failure";
			return false;
		}
		const double wanted = std::ceil((seconds - probe_->ptsOffset) * probe_->fps - 1e-6);
		index_ = static_cast<int>(std::max(0.0, std::min(wanted, 1e9)));
		error_.clear();
		return true;
	}

	double durationSeconds() const override {
		return static_cast<double>(probe_->frames) / probe_->fps + probe_->extraDuration;
	}
	int width() const override {
		return opened_ ? probe_->width : 0;
	}
	int height() const override {
		return opened_ ? probe_->height : 0;
	}
	std::string lastError() const override {
		return error_;
	}

private:
	Probe* probe_;
	bool opened_ = false;
	int index_ = 0;
	std::string error_;
};

// ---------------------------------------------------------------------------------------------
// Test rig: engine, fake decoder, frame slot

struct Setup {
	std::string cacheDir;
	std::vector<std::string> ytDlpDirs;
	int lookahead = 8;
	bool useSlot = true;
};

class Rig {
public:
	explicit Rig(const Setup& setup = Setup(), int frames = 20, double fps = 50.0) {
		probe.frames = frames;
		probe.fps = fps;
		if (setup.useSlot) {
			frameId = videoExternalFrameAllocate();
		}
		EngineOptions options;
		options.cacheDir = setup.cacheDir;
		options.useCustomYtDlpDirs = true;
		options.ytDlpDirs = setup.ytDlpDirs;
		options.lookaheadFrames = setup.lookahead;
		Probe* p = &probe;
		options.factory = [p]() { return std::unique_ptr<FrameSource>(new FakeSource(p)); };
		options.onFramePublished = [p](const DecodedFrame& frame) { p->onShown(frame); };
		engine.reset(new Engine(frameId, options));
	}
	~Rig() {
		engine.reset();
		if (frameId > 0) {
			videoExternalFrameRelease(frameId);
		}
	}
	Rig(const Rig&) = delete;
	Rig& operator=(const Rig&) = delete;

	Probe probe;
	int frameId = 0;
	std::unique_ptr<Engine> engine;
};

bool waitState(Engine& engine, TransportState state, double timeoutSeconds = 5.0) {
	return waitFor([&] { return engine.snapshot().state == state; }, timeoutSeconds);
}

bool waitOpened(Rig& rig, size_t count, double timeoutSeconds = 5.0) {
	return waitFor([&] { return rig.probe.openedCopy().size() >= count; }, timeoutSeconds);
}

// Where, in the list of frames shown so far, the first frame numbered `value` sits when looking from
// position `from` on. -1 when it has not been shown (yet).
int findShown(Rig& rig, size_t from, int value) {
	const std::vector<int> all = rig.probe.indices();
	for (size_t i = from; i < all.size(); ++i) {
		if (all[i] == value) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

// The frame numbers from position `from` on.
std::vector<int> tailFrom(const std::vector<int>& values, size_t from) {
	if (from >= values.size()) {
		return std::vector<int>();
	}
	return std::vector<int>(values.begin() + static_cast<std::ptrdiff_t>(from), values.end());
}

// Each frame is shown at most once per trip through the clip, in order.
void checkIncreasing(const std::vector<int>& values, const char* what, int line) {
	bool ok = true;
	for (size_t i = 1; i < values.size(); ++i) {
		ok = ok && values[i] > values[i - 1];
	}
	++checks;
	if (!ok) {
		++failures;
		std::string sequence;
		for (int value : values) {
			sequence += std::to_string(value) + " ";
		}
		std::printf("  FAIL [%s] line %d: %s is not increasing: %s\n", currentTest.c_str(), line, what,
			sequence.c_str());
	}
}

#define CHECK_INCREASING(values) checkIncreasing((values), #values, __LINE__)

// ---------------------------------------------------------------------------------------------
// Fake yt-dlp. Every run logs "<tag> <arguments>" to $FAKE_YTDLP_LOG, then does what `body` says.

std::string fakeYtDlpScript(const std::string& tag, const std::string& body) {
	return "#!/bin/sh\n"
		   "echo \"" + tag + " $*\" >> \"$FAKE_YTDLP_LOG\"\n"
		   "out=\"\"\n"
		   "while [ $# -gt 0 ]; do\n"
		   "  if [ \"$1\" = \"-o\" ]; then out=\"$2\"; fi\n"
		   "  shift\n"
		   "done\n" + body;
}

const char* const kBodyOk = "printf 'fake video bytes' > \"$out\"\nexit 0\n";
const char* const kBodyFail =
	"printf 'partial' > \"$out\"\n"
	"echo 'ERROR: [youtube] VRUtest0000: Video unavailable' >&2\n"
	"exit 1\n";
const char* const kBodyNoFormat =
	"echo 'ERROR: [youtube] VRUtest0000: Requested format is not available. Use --list-formats for a list of available formats' >&2\n"
	"exit 1\n";
const char* const kBodyNoFile = "exit 0\n";
const char* const kBodyHang = "echo $$ > \"$FAKE_YTDLP_PID\"\nexec sleep 30\n";
const char* const kBodySlowOk = "sleep 0.6\nprintf 'fake video bytes' > \"$out\"\nexit 0\n";

// Makes <root>/<dirName>/yt-dlp and returns the folder.
std::string makeYtDlpDir(const std::string& dirName, const std::string& tag, const std::string& body,
	bool executable = true) {
	const std::string dir = gRoot + "/" + dirName;
	std::error_code ec;
	fs::create_directories(dir, ec);
	writeFile(dir + "/yt-dlp", fakeYtDlpScript(tag, body), executable);
	return dir;
}

std::string logPath() {
	return gRoot + "/ytdlp.log";
}

std::string pidPath() {
	return gRoot + "/ytdlp.pid";
}

void resetFakeLog() {
	std::remove(logPath().c_str());
	std::remove(pidPath().c_str());
	setenv("FAKE_YTDLP_LOG", logPath().c_str(), 1);
	setenv("FAKE_YTDLP_PID", pidPath().c_str(), 1);
}

bool processIsGone(long pid) {
	return kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH;
}

long readPid() {
	const std::vector<std::string> lines = readLines(pidPath());
	return lines.empty() ? 0 : std::atol(lines[0].c_str());
}

const std::string kId = "VRUtest0000";
const std::string kWatchUrl = "https://www.youtube.com/watch?v=VRUtest0000";

// ---------------------------------------------------------------------------------------------
// Tests

void testIdleEngine() {
	currentTest = "idle engine";
	Rig rig;
	const Snapshot s = rig.engine->snapshot();
	CHECK(s.state == TransportState::NoSource);
	CHECK_TEXT(s.message, "");
	CHECK(!rig.engine->hasFrame());
	CHECK(!rig.engine->isPlaying());
	CHECK(rig.engine->endCount() == 0);
	CHECK(rig.engine->settings().source == SourceKind::None);

	// Buttons with no video do nothing, and empty text is ignored.
	rig.engine->requestPlayToggle();
	rig.engine->requestStop();
	rig.engine->loadLink("   ");
	rig.engine->loadFile("");
	sleepMs(100);
	CHECK(rig.engine->snapshot().state == TransportState::NoSource);
	CHECK(rig.engine->settings().source == SourceKind::None);
	CHECK(rig.probe.openedCopy().empty());
}

void testLoadFile() {
	currentTest = "load a file";
	Rig rig;
	rig.probe.ptsOffset = 1.0;  // clips need not start at time zero
	const std::string path = makeMedia("clip.mp4");
	rig.engine->loadFile(path);
	CHECK(waitState(*rig.engine, TransportState::Ready));
	const Snapshot s = rig.engine->snapshot();
	CHECK_TEXT(s.sourceName, "clip.mp4");
	CHECK_TEXT(s.message, "");
	CHECK(!s.messageIsError);
	CHECK(s.width == 8 && s.height == 6);
	CHECK(near(s.durationSeconds, 0.4, 1e-6));
	CHECK(s.positionSeconds < 0.001);
	CHECK(rig.engine->hasFrame());
	CHECK(rig.probe.openedCopy().size() == 1 && rig.probe.openedCopy()[0] == path);

	// The first frame is in the slot the video bus points at.
	ExternalVideoFrame frame;
	CHECK(videoExternalFrameRead(rig.frameId, frame));
	CHECK(frame.width == 8 && frame.height == 6);
	CHECK(frame.rgba.size() == 8u * 6u * 4u);
	CHECK(!frame.rgba.empty() && frame.rgba[0] == 0 && frame.rgba[1] == 0);

	// The path is kept for the patch.
	const Settings saved = rig.engine->settings();
	CHECK(saved.source == SourceKind::File);
	CHECK_TEXT(saved.filePath, path);

	// Ready holds the first frame. Nothing new is shown until Play.
	sleepMs(150);
	CHECK(rig.probe.shownCount() == 1);
	CHECK(rig.engine->snapshot().state == TransportState::Ready);
	CHECK(rig.engine->endCount() == 0);
}

void testPlayToEnd() {
	currentTest = "play to the end, loop off";
	Rig rig;
	const std::string path = makeMedia("end.mp4");
	rig.engine->loadFile(path);
	CHECK(waitState(*rig.engine, TransportState::Ready));

	const Clock::time_point pressed = Clock::now();
	rig.engine->requestPlayToggle();
	CHECK(waitFor([&] { return rig.engine->endCount() == 1; }, 4.0));
	const double took = secondsSince(pressed);
	CHECK(took > 0.30 && took < 1.5);  // 20 frames at 50 fps is 0.4 s
	CHECK(waitState(*rig.engine, TransportState::Ended));
	CHECK(!rig.engine->isPlaying());

	// The last frame stays on screen and nothing more is shown.
	CHECK(rig.probe.lastShown() == 19);
	const size_t shownAtEnd = rig.probe.shownCount();
	sleepMs(200);
	CHECK(rig.probe.shownCount() == shownAtEnd);
	CHECK(rig.engine->endCount() == 1);
	CHECK(rig.engine->hasFrame());
	CHECK(rig.probe.rewinds.load() == 0);
	const Snapshot ended = rig.engine->snapshot();
	CHECK(near(ended.positionSeconds, 0.4, 0.05));
	CHECK_INCREASING(rig.probe.indices());

	// Stop after the end goes back to the first frame and holds it.
	rig.engine->requestStop();
	CHECK(waitState(*rig.engine, TransportState::Ready));
	CHECK(rig.probe.lastShown() == 0);
	CHECK(rig.engine->snapshot().positionSeconds < 0.001);
	CHECK(rig.engine->endCount() == 1);
}

void testPlayAgainFromEnded() {
	currentTest = "play again after the end";
	Rig rig;
	rig.engine->loadFile(makeMedia("again.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	rig.engine->requestPlayToggle();
	CHECK(waitState(*rig.engine, TransportState::Ended));
	CHECK(rig.engine->endCount() == 1);
	const size_t before = rig.probe.shownCount();

	rig.engine->requestPlayToggle();  // from Ended, Play starts over
	CHECK(waitFor([&] { return rig.engine->endCount() == 2; }, 4.0));
	CHECK(waitState(*rig.engine, TransportState::Ended));
	const std::vector<int> all = rig.probe.indices();
	CHECK(all.size() > before);
	CHECK(all.size() > before && all[before] == 0);
	CHECK(rig.probe.lastShown() == 19);
	CHECK(rig.probe.rewinds.load() >= 1);
}

void testPauseAndResume() {
	currentTest = "pause and resume";
	Rig rig(Setup(), 100, 50.0);
	rig.engine->loadFile(makeMedia("pause.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	rig.engine->requestPlayToggle();
	CHECK(waitFor([&] { return rig.probe.shownCount() >= 8; }));
	rig.engine->requestPlayToggle();
	CHECK(waitState(*rig.engine, TransportState::Paused));

	const size_t shownAtPause = rig.probe.shownCount();
	const int indexAtPause = rig.probe.lastShown();
	CHECK(indexAtPause > 0 && indexAtPause < 99);
	sleepMs(250);
	CHECK(rig.probe.shownCount() == shownAtPause);
	CHECK(!rig.engine->isPlaying());
	CHECK(near(rig.engine->snapshot().positionSeconds, indexAtPause * 0.02, 0.011));

	// Resume continues where it stopped. The paused time does not count.
	rig.engine->requestPlayToggle();
	CHECK(waitFor([&] { return rig.probe.shownCount() >= shownAtPause + 5; }));
	const std::vector<int> all = rig.probe.indices();
	CHECK(all.size() > shownAtPause);
	if (all.size() > shownAtPause) {
		CHECK(all[shownAtPause] > indexAtPause);
		CHECK(all[shownAtPause] <= indexAtPause + 6);
	}
	CHECK_INCREASING(all);
	CHECK(rig.engine->isPlaying());
}

void testStopWhilePlaying() {
	currentTest = "stop while playing";
	Rig rig(Setup(), 100, 50.0);
	rig.engine->loadFile(makeMedia("stop.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	rig.engine->requestPlayToggle();
	CHECK(waitFor([&] { return rig.probe.shownCount() >= 6; }));
	rig.engine->requestStop();
	CHECK(waitState(*rig.engine, TransportState::Ready));
	CHECK(rig.probe.lastShown() == 0);
	CHECK(rig.engine->snapshot().positionSeconds < 0.001);
	CHECK(!rig.engine->isPlaying());
	CHECK(rig.probe.rewinds.load() >= 1);
	const size_t shownAfterStop = rig.probe.shownCount();
	sleepMs(200);
	CHECK(rig.probe.shownCount() == shownAfterStop);
	CHECK(rig.engine->endCount() == 0);

	// Play after Stop starts from the first frame.
	rig.engine->requestPlayToggle();
	CHECK(waitFor([&] { return rig.probe.shownCount() >= shownAfterStop + 3; }));
	const std::vector<int> all = rig.probe.indices();
	CHECK(all.size() > shownAfterStop);
	if (all.size() > shownAfterStop) {
		CHECK(all[shownAfterStop] <= 3);
	}
}

void testLoopWrap() {
	currentTest = "loop on wraps without a gap";
	Setup setup;
	Rig rig(setup, 20, 50.0);  // 20 ms per frame, 0.4 s per trip
	rig.probe.rewindSleepMs = 8;  // the decoder takes a moment to start over
	rig.engine->loadFile(makeMedia("loop.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	rig.engine->setLoop(true);
	rig.engine->requestPlayToggle();
	CHECK(waitFor([&] { return rig.engine->endCount() >= 4; }, 8.0));
	CHECK(rig.engine->snapshot().state == TransportState::Playing);
	CHECK(rig.engine->isPlaying());

	const std::vector<Probe::Shown> shown = rig.probe.shownCopy();
	std::vector<double> wrapGapsMs;
	int wraps = 0;
	for (size_t i = 1; i < shown.size(); ++i) {
		if (shown[i].index < shown[i - 1].index) {
			++wraps;
			CHECK(shown[i - 1].index >= 15);  // the trip ran to its end
			CHECK(shown[i].index <= 3);       // and the next one began at the start
			wrapGapsMs.push_back(
				std::chrono::duration<double, std::milli>(shown[i].when - shown[i - 1].when).count());
		}
	}
	CHECK(wraps >= 3);
	CHECK(rig.probe.rewinds.load() >= wraps);
	// One frame every 20 ms is normal. A wrap that waits for the decoder would add its rewind time.
	const double typicalWrapGap = median(wrapGapsMs);
	CHECK(typicalWrapGap > 12.0 && typicalWrapGap < 34.0);

	// Loop off during playback: the trip in progress is the last one, then the last frame is held.
	rig.engine->setLoop(false);
	CHECK(waitState(*rig.engine, TransportState::Ended, 4.0));
	CHECK(rig.probe.lastShown() == 19);
	CHECK(!rig.engine->isPlaying());
	const size_t shownAtEnd = rig.probe.shownCount();
	sleepMs(150);
	CHECK(rig.probe.shownCount() == shownAtEnd);
}

void testLoopSwitchedOnLate() {
	currentTest = "loop switched on during the last part";
	Rig rig(Setup(), 50, 50.0);  // 1 s clip
	rig.engine->loadFile(makeMedia("late.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	rig.engine->requestPlayToggle();
	CHECK(waitFor([&] { return rig.probe.lastShown() >= 38; }));
	rig.engine->setLoop(true);  // after the decoder has already reached the end
	CHECK(waitFor([&] { return rig.engine->endCount() >= 1; }, 3.0));
	sleepMs(150);
	CHECK(rig.engine->snapshot().state == TransportState::Playing);
	const std::vector<int> all = rig.probe.indices();
	bool wrapped = false;
	for (size_t i = 1; i < all.size(); ++i) {
		wrapped = wrapped || all[i] < all[i - 1];
	}
	CHECK(wrapped);
}

void testSpeed() {
	currentTest = "speed";
	Rig rig(Setup(), 25, 50.0);  // 0.5 s at normal speed
	rig.engine->loadFile(makeMedia("speed.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	rig.engine->setSpeed(2.f);
	const Clock::time_point pressed = Clock::now();
	rig.engine->requestPlayToggle();
	CHECK(waitFor([&] { return rig.engine->endCount() == 1; }, 4.0));
	const double took = secondsSince(pressed);
	CHECK(took > 0.15 && took < 0.42);  // about 0.25 s

	// Out-of-range values are clamped, so the engine never gets a speed of zero or less.
	rig.engine->setSpeed(0.f);
	rig.engine->setSpeed(-3.f);
	rig.engine->setSpeed(50.f);
	rig.engine->requestStop();
	CHECK(waitState(*rig.engine, TransportState::Ready));
	rig.engine->requestPlayToggle();
	CHECK(waitState(*rig.engine, TransportState::Ended, 4.0));
}

void testMissingFile() {
	currentTest = "missing file";
	Rig rig;
	const std::string path = gRoot + "/media/does-not-exist.mp4";
	std::remove(path.c_str());
	rig.engine->loadFile(path);
	CHECK(waitState(*rig.engine, TransportState::Error));
	const Snapshot s = rig.engine->snapshot();
	CHECK_TEXT(s.message, msgFileNotFound(path));
	CHECK(s.messageIsError);
	CHECK_TEXT(s.sourceName, "does-not-exist.mp4");
	CHECK(!rig.engine->hasFrame());
	CHECK(rig.probe.openedCopy().empty());  // the decoder is never asked to open it
	// The path is still saved, so the patch finds the file again if it comes back.
	CHECK(rig.engine->settings().source == SourceKind::File);
	CHECK_TEXT(rig.engine->settings().filePath, path);

	// The transport buttons do nothing and do not crash.
	rig.engine->requestPlayToggle();
	rig.engine->requestStop();
	sleepMs(120);
	CHECK(rig.engine->snapshot().state == TransportState::Error);
	CHECK(!rig.engine->isPlaying());

	// Loading a real file afterwards recovers.
	const std::string good = makeMedia("recovered.mp4");
	rig.engine->loadFile(good);
	CHECK(waitState(*rig.engine, TransportState::Ready));
	CHECK_TEXT(rig.engine->snapshot().message, "");
	CHECK(rig.engine->hasFrame());
}

void testUnsupportedContainer() {
	currentTest = "mkv and webm";
	Rig rig;
	const std::string mkv = makeMedia("movie.mkv");
	rig.engine->loadFile(mkv);
	CHECK(waitState(*rig.engine, TransportState::Error));
	CHECK_TEXT(rig.engine->snapshot().message, msgUnsupportedContainer("mkv"));
	CHECK_CONTAINS(rig.engine->snapshot().message, "MKV and WebM");

	const std::string webm = makeMedia("movie.WEBM");
	rig.engine->loadFile(webm);
	CHECK(waitFor([&] {
		const Snapshot s = rig.engine->snapshot();
		return s.state == TransportState::Error && s.sourceName == "movie.WEBM";
	}));
	CHECK_TEXT(rig.engine->snapshot().message, msgUnsupportedContainer("webm"));
	CHECK(rig.probe.openedCopy().empty());
}

void testDecoderProblems() {
	currentTest = "decoder problems";
	Rig rig;
	const std::string path = makeMedia("broken.mp4");

	// The decoder refuses the file.
	rig.probe.failOpen = true;
	rig.engine->loadFile(path);
	CHECK(waitState(*rig.engine, TransportState::Error));
	CHECK_TEXT(rig.engine->snapshot().message, msgDecodeFailed(path));
	CHECK(rig.engine->snapshot().messageIsError);
	CHECK(!rig.engine->hasFrame());
	rig.probe.failOpen = false;

	// A file that opens but has no frames.
	Rig empty(Setup(), 0, 50.0);
	empty.engine->loadFile(path);
	CHECK(waitState(*empty.engine, TransportState::Error));
	CHECK_TEXT(empty.engine->snapshot().message, msgDecodeFailed(path));
	CHECK(!empty.engine->hasFrame());

	// A decode error in the middle of the clip stops playback with a message, and the engine recovers.
	Rig middle(Setup(), 40, 50.0);
	middle.probe.failAtFrame = 25;
	middle.engine->loadFile(path);
	CHECK(waitState(*middle.engine, TransportState::Ready));
	middle.engine->requestPlayToggle();
	CHECK(waitState(*middle.engine, TransportState::Error, 4.0));
	CHECK_TEXT(middle.engine->snapshot().message, msgDecodeFailed(path));
	CHECK(!middle.engine->isPlaying());
	middle.engine->requestPlayToggle();
	sleepMs(80);
	CHECK(middle.engine->snapshot().state == TransportState::Error);
	middle.probe.failAtFrame = -1;
	middle.engine->loadFile(path);
	CHECK(waitFor([&] { return middle.probe.openedCopy().size() >= 2; }));
	CHECK(waitState(*middle.engine, TransportState::Ready));
	CHECK_TEXT(middle.engine->snapshot().message, "");
}

void testLinkSorting() {
	currentTest = "file path or link";
	Rig rig;
	const std::string path = makeMedia("typed.mp4");

	// A file path typed into the link field opens as a file. Spaces around it are ignored.
	rig.engine->loadLink("  " + path + "\n");
	CHECK(waitState(*rig.engine, TransportState::Ready));
	CHECK(rig.engine->settings().source == SourceKind::File);
	CHECK_TEXT(rig.engine->settings().filePath, path);
	CHECK(rig.probe.openedCopy().size() == 1);

	// A link that is not YouTube shows a message and leaves the loaded video alone.
	rig.engine->loadLink("https://example.com/clip.mp4");
	CHECK(waitFor([&] { return rig.engine->snapshot().message == msgUnsupportedUrl(); }));
	const Snapshot s = rig.engine->snapshot();
	CHECK(s.messageIsError);
	CHECK(s.state == TransportState::Ready);
	CHECK_TEXT(s.sourceName, "typed.mp4");
	CHECK(rig.engine->hasFrame());
	CHECK(rig.engine->settings().source == SourceKind::File);
	CHECK_TEXT(rig.engine->settings().filePath, path);
	CHECK(rig.probe.openedCopy().size() == 1);

	// Other text that looks like a link, such as a playlist page, gets the same message.
	rig.engine->loadLink("https://www.youtube.com/playlist?list=PL123");
	sleepMs(80);
	CHECK_TEXT(rig.engine->snapshot().message, msgUnsupportedUrl());
	CHECK(rig.engine->snapshot().state == TransportState::Ready);
}

void testYtDlpMissing() {
	currentTest = "yt-dlp not installed";
	const std::string emptyDir = gRoot + "/no-ytdlp";
	std::error_code ec;
	fs::create_directories(emptyDir, ec);
	Setup setup;
	setup.cacheDir = gRoot + "/cache-missing";
	setup.ytDlpDirs = {emptyDir, gRoot + "/folder-that-does-not-exist"};
	Rig rig(setup);
	resetFakeLog();
	rig.engine->loadLink("https://youtu.be/VRUtest0000");
	CHECK(waitState(*rig.engine, TransportState::Error));
	const Snapshot s = rig.engine->snapshot();
	CHECK_TEXT(s.message, msgYtDlpMissing());
	CHECK_CONTAINS(s.message, "yt-dlp is not installed");
	CHECK(s.messageIsError);
	CHECK_TEXT(s.sourceName, "youtube.com/watch?v=VRUtest0000");
	CHECK(rig.probe.openedCopy().empty());
	CHECK(!fileExists(logPath()));
	// The link is kept in the patch, so it works after yt-dlp is installed.
	CHECK(rig.engine->settings().source == SourceKind::YouTube);
	CHECK_TEXT(rig.engine->settings().youtubeUrl, kWatchUrl);

	// A script that is not executable does not count as installed.
	const std::string stiff = makeYtDlpDir("stiff", "stiff", kBodyOk, false);
	Setup stiffSetup = setup;
	stiffSetup.ytDlpDirs = {stiff};
	Rig stiffRig(stiffSetup);
	stiffRig.engine->loadLink(kWatchUrl);
	CHECK(waitState(*stiffRig.engine, TransportState::Error));
	CHECK_TEXT(stiffRig.engine->snapshot().message, msgYtDlpMissing());
	CHECK(!fileExists(logPath()));
}

void testYtDlpDownloadAndCache() {
	currentTest = "YouTube download and cache";
	const std::string dir = makeYtDlpDir("ytdlp-ok", "ok-tag", kBodyOk);
	Setup setup;
	setup.cacheDir = gRoot + "/cache-ok/nested/deeper";
	setup.ytDlpDirs = {dir};
	std::error_code ec;
	fs::remove_all(gRoot + "/cache-ok", ec);
	resetFakeLog();
	Rig rig(setup);
	const std::string cachePath = cachePathForId(setup.cacheDir, kId);

	// Extra parts of the pasted link never reach yt-dlp.
	rig.engine->loadLink("https://www.youtube.com/watch?v=VRUtest0000&list=PL1234&t=30s");
	CHECK(waitState(*rig.engine, TransportState::Ready, 10.0));
	const std::vector<std::string> lines = readLines(logPath());
	CHECK(lines.size() == 1);
	const std::string line = lines.empty() ? std::string() : lines[0];
	CHECK_CONTAINS(line, "ok-tag ");
	CHECK_CONTAINS(line, "--no-playlist");
	CHECK_CONTAINS(line, "-f best[ext=mp4][vcodec^=avc1][height<=1080]");
	CHECK_CONTAINS(line, "-o " + cachePath);
	CHECK_CONTAINS(line, kWatchUrl);
	CHECK(line.find("list=") == std::string::npos);
	CHECK(line.find("t=30s") == std::string::npos);
	CHECK(fileHasContent(cachePath));
	CHECK(rig.probe.openedCopy().size() == 1 && rig.probe.openedCopy()[0] == cachePath);
	CHECK_TEXT(rig.engine->snapshot().sourceName, "youtube.com/watch?v=VRUtest0000");
	CHECK_TEXT(rig.engine->snapshot().message, "");
	CHECK(rig.engine->hasFrame());
	CHECK(rig.engine->settings().source == SourceKind::YouTube);
	CHECK_TEXT(rig.engine->settings().youtubeUrl, kWatchUrl);
	CHECK_TEXT(rig.engine->settings().filePath, "");

	// The same video again is played from the cache. yt-dlp does not run.
	rig.engine->loadLink("https://youtu.be/VRUtest0000?si=abc");
	CHECK(waitOpened(rig, 2));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	CHECK(readLines(logPath()).size() == 1);
	CHECK(rig.probe.openedCopy().size() == 2 && rig.probe.openedCopy()[1] == cachePath);

	// A different video is downloaded, under its own ID.
	rig.engine->loadLink("https://youtu.be/aaaaaaaaaaa");
	CHECK(waitOpened(rig, 3, 10.0));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	const std::vector<std::string> after = readLines(logPath());
	CHECK(after.size() == 2);
	CHECK(after.size() == 2 && after[1].find("aaaaaaaaaaa") != std::string::npos);
	CHECK(fileHasContent(cachePathForId(setup.cacheDir, "aaaaaaaaaaa")));
	CHECK(fileHasContent(cachePath));

	// The downloaded file plays like any other.
	rig.engine->requestPlayToggle();
	CHECK(waitState(*rig.engine, TransportState::Ended, 4.0));
	CHECK(rig.engine->endCount() == 1);
}

void testYtDlpSearchOrder() {
	currentTest = "yt-dlp search order";
	std::error_code ec;
	fs::remove_all(gRoot + "/cache-order", ec);
	const std::string first = makeYtDlpDir("order-first", "FIRST", kBodyOk);
	const std::string second = makeYtDlpDir("order-second", "SECOND", kBodyOk);
	const std::string notExecutable = makeYtDlpDir("order-stiff", "STIFF", kBodyOk, false);
	Setup setup;
	setup.cacheDir = gRoot + "/cache-order";

	// The first folder that has a usable yt-dlp wins.
	resetFakeLog();
	setup.ytDlpDirs = {gRoot + "/order-missing", first, second};
	{
		Rig rig(setup);
		rig.engine->loadLink(kWatchUrl);
		CHECK(waitState(*rig.engine, TransportState::Ready, 10.0));
		const std::vector<std::string> lines = readLines(logPath());
		CHECK(lines.size() == 1);
		CHECK(lines.size() == 1 && lines[0].compare(0, 5, "FIRST") == 0);
	}

	// A copy that is not executable is skipped.
	fs::remove_all(gRoot + "/cache-order", ec);
	resetFakeLog();
	setup.ytDlpDirs = {notExecutable, second, first};
	{
		Rig rig(setup);
		rig.engine->loadLink(kWatchUrl);
		CHECK(waitState(*rig.engine, TransportState::Ready, 10.0));
		const std::vector<std::string> lines = readLines(logPath());
		CHECK(lines.size() == 1);
		CHECK(lines.size() == 1 && lines[0].compare(0, 6, "SECOND") == 0);
	}
}

void testYtDlpFailures() {
	currentTest = "yt-dlp failures";
	std::error_code ec;
	fs::remove_all(gRoot + "/cache-fail", ec);
	Setup setup;
	setup.cacheDir = gRoot + "/cache-fail";
	const std::string cachePath = cachePathForId(setup.cacheDir, kId);

	// yt-dlp fails and leaves half a file behind. The reason is shown and the half file is removed.
	setup.ytDlpDirs = {makeYtDlpDir("ytdlp-fail", "fail", kBodyFail)};
	resetFakeLog();
	{
		Rig rig(setup);
		rig.engine->loadLink(kWatchUrl);
		CHECK(waitState(*rig.engine, TransportState::Error, 10.0));
		const Snapshot s = rig.engine->snapshot();
		CHECK(s.messageIsError);
		CHECK_CONTAINS(s.message, "YouTube download failed.");
		CHECK_CONTAINS(s.message, "Video unavailable");
		CHECK(!fileExists(cachePath));
		CHECK(rig.probe.openedCopy().empty());
		CHECK(!rig.engine->hasFrame());
		CHECK(rig.engine->settings().source == SourceKind::YouTube);
	}

	// The video has no single-file H.264 MP4 version.
	setup.ytDlpDirs = {makeYtDlpDir("ytdlp-noformat", "noformat", kBodyNoFormat)};
	resetFakeLog();
	{
		Rig rig(setup);
		rig.engine->loadLink(kWatchUrl);
		CHECK(waitState(*rig.engine, TransportState::Error, 10.0));
		CHECK_TEXT(rig.engine->snapshot().message, msgYouTubeNoMp4());
		CHECK(!fileExists(cachePath));
	}

	// yt-dlp says it is done but saved nothing.
	setup.ytDlpDirs = {makeYtDlpDir("ytdlp-nofile", "nofile", kBodyNoFile)};
	resetFakeLog();
	{
		Rig rig(setup);
		rig.engine->loadLink(kWatchUrl);
		CHECK(waitState(*rig.engine, TransportState::Error, 10.0));
		CHECK_CONTAINS(rig.engine->snapshot().message, "YouTube download failed.");
		CHECK(rig.probe.openedCopy().empty());
	}

	// An empty cache file from an earlier crash is not trusted: the video downloads again.
	setup.ytDlpDirs = {makeYtDlpDir("ytdlp-retry", "retry", kBodyOk)};
	resetFakeLog();
	writeFile(cachePath, "");
	{
		Rig rig(setup);
		rig.engine->loadLink(kWatchUrl);
		CHECK(waitState(*rig.engine, TransportState::Ready, 10.0));
		CHECK(readLines(logPath()).size() == 1);
	}
}

void testYtDlpProgressAndButtons() {
	currentTest = "download progress, buttons during download";
	std::error_code ec;
	fs::remove_all(gRoot + "/cache-slow", ec);
	Setup setup;
	setup.cacheDir = gRoot + "/cache-slow";
	setup.ytDlpDirs = {makeYtDlpDir("ytdlp-slow", "slow", kBodySlowOk)};
	resetFakeLog();
	Rig rig(setup);
	rig.engine->loadLink(kWatchUrl);
	CHECK(waitFor([&] {
		const Snapshot s = rig.engine->snapshot();
		return s.state == TransportState::Loading && s.message.find("Downloading from YouTube") != std::string::npos;
	}));
	CHECK(!rig.engine->snapshot().messageIsError);

	// Presses made while the download runs are dropped. The video does not start by itself afterwards.
	rig.engine->requestPlayToggle();
	rig.engine->requestPlayToggle();
	rig.engine->requestPlayToggle();
	rig.engine->requestStop();
	CHECK(waitState(*rig.engine, TransportState::Ready, 10.0));
	sleepMs(200);
	CHECK(rig.engine->snapshot().state == TransportState::Ready);
	CHECK(!rig.engine->isPlaying());
	CHECK(rig.probe.shownCount() == 1);
	CHECK_TEXT(rig.engine->snapshot().message, "");
}

void testYtDlpStopsWithEngine() {
	currentTest = "yt-dlp is stopped";
	std::error_code ec;
	fs::remove_all(gRoot + "/cache-hang", ec);
	Setup setup;
	setup.cacheDir = gRoot + "/cache-hang";
	setup.ytDlpDirs = {makeYtDlpDir("ytdlp-hang", "hang", kBodyHang)};

	// Removing the module while a download runs stops the download.
	resetFakeLog();
	{
		std::unique_ptr<Rig> rig(new Rig(setup));
		rig->engine->loadLink(kWatchUrl);
		CHECK(waitFor([&] { return fileHasContent(pidPath()); }));
		const long pid = readPid();
		CHECK(pid > 1);
		CHECK(!processIsGone(pid));
		const Clock::time_point started = Clock::now();
		rig.reset();
		CHECK(secondsSince(started) < 3.0);
		CHECK(processIsGone(pid));
	}

	// Loading something else while a download runs stops the download too.
	resetFakeLog();
	{
		Rig rig(setup);
		rig.engine->loadLink(kWatchUrl);
		CHECK(waitFor([&] { return fileHasContent(pidPath()); }));
		const long pid = readPid();
		CHECK(pid > 1);
		rig.engine->loadFile(makeMedia("instead.mp4"));
		CHECK(waitState(*rig.engine, TransportState::Ready, 5.0));
		CHECK(processIsGone(pid));
		CHECK(rig.engine->settings().source == SourceKind::File);
		CHECK(!fileExists(cachePathForId(setup.cacheDir, kId)));
	}
}

void testSaveAndRestore() {
	currentTest = "save and restore";
	const std::string path = makeMedia("saved.mp4");

	// File: path out, same path back in. The video is ready but not playing.
	Settings saved;
	{
		Rig rig;
		rig.engine->loadFile(path);
		CHECK(waitState(*rig.engine, TransportState::Ready));
		saved = rig.engine->settings();
	}
	const KeyValues keys = settingsToKeyValues(saved);
	const Settings loaded = settingsFromKeyValues(keys);
	CHECK(loaded.source == SourceKind::File);
	CHECK_TEXT(loaded.filePath, path);
	{
		Rig rig;
		rig.engine->restore(loaded);
		CHECK(waitState(*rig.engine, TransportState::Ready));
		sleepMs(150);
		CHECK(rig.engine->snapshot().state == TransportState::Ready);
		CHECK(!rig.engine->isPlaying());
		CHECK(rig.probe.shownCount() == 1);
		CHECK(rig.engine->hasFrame());
		CHECK_TEXT(rig.engine->snapshot().sourceName, "saved.mp4");
		CHECK_TEXT(rig.engine->settings().filePath, path);
		CHECK(rig.probe.openedCopy().size() == 1 && rig.probe.openedCopy()[0] == path);
	}

	// File that is gone: a message, no crash, and the path is kept for the next save.
	{
		Rig rig;
		Settings gone;
		gone.source = SourceKind::File;
		gone.filePath = gRoot + "/media/gone.mp4";
		std::remove(gone.filePath.c_str());
		rig.engine->restore(gone);
		CHECK(waitState(*rig.engine, TransportState::Error));
		CHECK_TEXT(rig.engine->snapshot().message, msgFileNotFound(gone.filePath));
		CHECK(rig.engine->settings().source == SourceKind::File);
		CHECK_TEXT(rig.engine->settings().filePath, gone.filePath);
		rig.engine->requestPlayToggle();
		sleepMs(80);
		CHECK(rig.engine->snapshot().state == TransportState::Error);
	}

	// Nothing saved: an empty player. Missing or unknown values give an empty player as well.
	{
		Rig rig;
		rig.engine->restore(Settings());
		sleepMs(100);
		CHECK(rig.engine->snapshot().state == TransportState::NoSource);
		CHECK(rig.engine->settings().source == SourceKind::None);
		Settings emptyPath;
		emptyPath.source = SourceKind::File;
		rig.engine->restore(emptyPath);
		sleepMs(100);
		CHECK(rig.engine->snapshot().state == TransportState::NoSource);
		CHECK(rig.engine->settings().source == SourceKind::None);
		CHECK(rig.probe.openedCopy().empty());
	}

	// YouTube with the video already in the cache: it opens without yt-dlp.
	{
		Setup setup;
		setup.cacheDir = gRoot + "/cache-restore";
		setup.ytDlpDirs = {gRoot + "/no-ytdlp"};
		std::error_code ec;
		fs::create_directories(setup.cacheDir, ec);
		const std::string cached = cachePathForId(setup.cacheDir, kId);
		writeFile(cached, "cached bytes");
		resetFakeLog();
		Rig rig(setup);
		Settings youtube;
		youtube.source = SourceKind::YouTube;
		youtube.youtubeUrl = kWatchUrl;
		rig.engine->restore(youtube);
		CHECK(waitState(*rig.engine, TransportState::Ready));
		CHECK(rig.probe.openedCopy().size() == 1 && rig.probe.openedCopy()[0] == cached);
		CHECK(!fileExists(logPath()));
		CHECK(rig.engine->settings().source == SourceKind::YouTube);
		CHECK_TEXT(rig.engine->settings().youtubeUrl, kWatchUrl);
		CHECK(!rig.engine->isPlaying());
	}

	// YouTube with the cache gone and no yt-dlp: the plain message, and the link is kept.
	{
		Setup setup;
		setup.cacheDir = gRoot + "/cache-restore-empty";
		setup.ytDlpDirs = {gRoot + "/no-ytdlp"};
		Rig rig(setup);
		Settings youtube;
		youtube.source = SourceKind::YouTube;
		youtube.youtubeUrl = kWatchUrl;
		rig.engine->restore(youtube);
		CHECK(waitState(*rig.engine, TransportState::Error));
		CHECK_TEXT(rig.engine->snapshot().message, msgYtDlpMissing());
		CHECK_TEXT(rig.engine->settings().youtubeUrl, kWatchUrl);
	}
}

void testClearAndReplace() {
	currentTest = "remove and replace video";
	Rig rig(Setup(), 100, 50.0);
	const std::string a = makeMedia("a.mp4");
	const std::string b = makeMedia("b.mp4");
	rig.engine->loadFile(a);
	CHECK(waitState(*rig.engine, TransportState::Ready));
	rig.engine->requestPlayToggle();
	CHECK(waitFor([&] { return rig.probe.shownCount() >= 5; }));

	// A new file replaces the playing one. It waits at its first frame.
	rig.engine->loadFile(b);
	CHECK(waitOpened(rig, 2));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	CHECK_TEXT(rig.engine->snapshot().sourceName, "b.mp4");
	CHECK(!rig.engine->isPlaying());
	CHECK(rig.probe.lastShown() == 0);
	const size_t shown = rig.probe.shownCount();
	sleepMs(150);
	CHECK(rig.probe.shownCount() == shown);
	CHECK(rig.engine->hasFrame());
	CHECK_TEXT(rig.engine->settings().filePath, b);

	// Remove video: empty player, no picture, nothing saved.
	rig.engine->clearSource();
	CHECK(waitState(*rig.engine, TransportState::NoSource));
	CHECK(!rig.engine->hasFrame());
	CHECK(rig.engine->settings().source == SourceKind::None);
	CHECK_TEXT(rig.engine->settings().filePath, "");
	CHECK_TEXT(rig.engine->snapshot().sourceName, "");
	CHECK_TEXT(rig.engine->snapshot().message, "");
	ExternalVideoFrame frame;
	CHECK(!videoExternalFrameRead(rig.frameId, frame));
	rig.engine->requestPlayToggle();
	sleepMs(80);
	CHECK(rig.engine->snapshot().state == TransportState::NoSource);
}

void testPlatformWithoutPlayback() {
	currentTest = "platform without playback";
	// No factory given, so the engine asks the platform decoder, which here says it cannot play video.
	const int frameId = videoExternalFrameAllocate();
	{
		EngineOptions options;
		options.cacheDir = gRoot + "/cache-noplatform";
		options.useCustomYtDlpDirs = true;
		Engine engine(frameId, options);
		const std::string path = makeMedia("platform.mp4");
		engine.loadFile(path);
		CHECK(waitState(engine, TransportState::Error));
		CHECK_TEXT(engine.snapshot().message, msgNotMacOs());
		CHECK(engine.snapshot().messageIsError);
		CHECK(!engine.hasFrame());
		engine.requestPlayToggle();
		sleepMs(60);
		CHECK(engine.snapshot().state == TransportState::Error);
	}
	if (frameId > 0) {
		videoExternalFrameRelease(frameId);
	}
}

void testNoFrameSlot() {
	currentTest = "no free frame slot";
	Setup setup;
	setup.useSlot = false;
	Rig rig(setup);
	CHECK(rig.frameId == 0);
	rig.engine->loadFile(makeMedia("noslot.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	CHECK(!rig.engine->hasFrame());  // the picture has nowhere to go, but playback still works
	rig.engine->requestPlayToggle();
	CHECK(waitState(*rig.engine, TransportState::Ended, 4.0));
	CHECK(rig.engine->endCount() == 1);
	CHECK(rig.probe.shownCount() >= 10);
	CHECK(!rig.engine->hasFrame());
}

// ---------------------------------------------------------------------------------------------
// Seeking. Unless a test says otherwise the fake clip runs at 50 frames per second, so frame N sits
// at N / 50 s, and a seek to a time shows the frame that starts at or after it.

void testSeekWhilePlaying() {
	currentTest = "seek while playing";
	Rig rig(Setup(), 250, 50.0);  // 5 s
	rig.engine->loadFile(makeMedia("seekplay.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	rig.engine->requestPlayToggle();
	CHECK(waitFor([&] { return rig.probe.shownCount() >= 5; }));

	// Forward. The frame at the target is the next one on screen, and playback goes on from there.
	const size_t before = rig.probe.shownCount();
	rig.engine->requestSeek(2.0);
	CHECK(waitFor([&] { return findShown(rig, before, 100) >= 0; }));
	const int jump = findShown(rig, before, 100);
	CHECK(waitFor([&] { return jump >= 0 && rig.probe.shownCount() >= static_cast<size_t>(jump) + 6; }));
	if (jump >= 0) {
		const std::vector<int> after = tailFrom(rig.probe.indices(), static_cast<size_t>(jump));
		CHECK(after.size() >= 6);
		if (after.size() >= 6) {
			CHECK_INCREASING(after);  // no frame twice, none out of order
			CHECK(after[5] <= 140);   // five more frames take 0.1 s, so about 105
		}
	}
	CHECK(rig.engine->snapshot().state == TransportState::Playing);
	CHECK(rig.engine->isPlaying());
	CHECK(rig.engine->snapshot().positionSeconds >= 1.999 && rig.engine->snapshot().positionSeconds < 2.6);

	// Backward, the same way.
	const size_t before2 = rig.probe.shownCount();
	rig.engine->requestSeek(0.5);
	CHECK(waitFor([&] { return findShown(rig, before2, 25) >= 0; }));
	const int jump2 = findShown(rig, before2, 25);
	CHECK(waitFor([&] { return jump2 >= 0 && rig.probe.shownCount() >= static_cast<size_t>(jump2) + 6; }));
	if (jump2 >= 0) {
		const std::vector<int> after = tailFrom(rig.probe.indices(), static_cast<size_t>(jump2));
		CHECK(after.size() >= 6);
		if (after.size() >= 6) {
			CHECK_INCREASING(after);
			CHECK(after[5] <= 65);
		}
	}
	CHECK(rig.engine->snapshot().state == TransportState::Playing);
	CHECK(rig.engine->isPlaying());

	// Seeking is not rewinding, and a seek is not the end of the clip.
	CHECK(rig.probe.seeks.load() == 2);
	CHECK(rig.probe.rewinds.load() == 0);
	CHECK(rig.engine->endCount() == 0);
}

void testSeekWhilePaused() {
	currentTest = "seek while paused";
	Rig rig(Setup(), 250, 50.0);
	rig.engine->loadFile(makeMedia("seekpause.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	rig.engine->requestPlayToggle();
	CHECK(waitFor([&] { return rig.probe.shownCount() >= 8; }));
	rig.engine->requestPlayToggle();
	CHECK(waitState(*rig.engine, TransportState::Paused));

	// Paused stays paused. The new frame is on screen, and only that one.
	size_t shown = rig.probe.shownCount();
	rig.engine->requestSeek(3.0);
	CHECK(waitFor([&] { return rig.probe.lastShown() == 150; }));
	sleepMs(200);
	CHECK(rig.engine->snapshot().state == TransportState::Paused);
	CHECK(!rig.engine->isPlaying());
	CHECK(rig.probe.shownCount() == shown + 1);
	CHECK(near(rig.engine->snapshot().positionSeconds, 3.0, 0.011));
	ExternalVideoFrame frame;
	CHECK(videoExternalFrameRead(rig.frameId, frame));  // and it is the picture on the video bus
	CHECK(frame.rgba.size() >= 2 && frame.rgba[0] == 150 && frame.rgba[1] == 0);

	// Backwards too.
	shown = rig.probe.shownCount();
	rig.engine->requestSeek(1.0);
	CHECK(waitFor([&] { return rig.probe.lastShown() == 50; }));
	sleepMs(150);
	CHECK(rig.engine->snapshot().state == TransportState::Paused);
	CHECK(rig.probe.shownCount() == shown + 1);
	CHECK(near(rig.engine->snapshot().positionSeconds, 1.0, 0.011));

	// Play goes on from the sought frame, not from where the video was paused.
	rig.engine->requestPlayToggle();
	CHECK(waitFor([&] { return rig.probe.shownCount() >= shown + 5; }));
	const std::vector<int> after = tailFrom(rig.probe.indices(), shown);
	CHECK(after.size() >= 5);
	if (after.size() >= 5) {
		CHECK(after[0] == 50);
		CHECK(after[1] > 50 && after[1] <= 56);
		CHECK_INCREASING(after);
	}
	CHECK(rig.engine->isPlaying());
	CHECK(rig.probe.rewinds.load() == 0);
}

void testSeekFromReady() {
	currentTest = "seek from ready";
	Rig rig(Setup(), 100, 50.0);
	rig.engine->loadFile(makeMedia("seekready.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	CHECK(rig.probe.shownCount() == 1);

	// Ready becomes Paused: the seek shows a frame but does not start the video.
	rig.engine->requestSeek(1.0);
	CHECK(waitState(*rig.engine, TransportState::Paused));
	CHECK(waitFor([&] { return rig.probe.lastShown() == 50; }));
	sleepMs(150);
	CHECK(rig.engine->snapshot().state == TransportState::Paused);
	CHECK(!rig.engine->isPlaying());
	CHECK(rig.probe.shownCount() == 2);  // the first frame, then the sought one
	CHECK(near(rig.engine->snapshot().positionSeconds, 1.0, 0.011));
	CHECK(rig.engine->hasFrame());
	ExternalVideoFrame frame;
	CHECK(videoExternalFrameRead(rig.frameId, frame));
	CHECK(frame.rgba.size() >= 2 && frame.rgba[0] == 50 && frame.rgba[1] == 0);

	// Play starts where the seek left it.
	rig.engine->requestPlayToggle();
	CHECK(waitFor([&] { return rig.probe.shownCount() >= 6; }));
	const std::vector<int> after = tailFrom(rig.probe.indices(), 1);
	CHECK(after.size() >= 5);
	if (after.size() >= 5) {
		CHECK(after[0] == 50);
		CHECK_INCREASING(after);
	}
	CHECK(rig.probe.rewinds.load() == 0);

	// Stop still goes back to the first frame.
	rig.engine->requestStop();
	CHECK(waitState(*rig.engine, TransportState::Ready));
	CHECK(rig.probe.lastShown() == 0);
	CHECK(rig.engine->snapshot().positionSeconds < 0.001);
	CHECK(rig.probe.rewinds.load() == 1);
}

void testSeekAfterEnd() {
	currentTest = "seek after the end";
	Rig rig(Setup(), 20, 50.0);  // 0.4 s
	rig.engine->loadFile(makeMedia("seekended.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	rig.engine->requestPlayToggle();
	CHECK(waitState(*rig.engine, TransportState::Ended, 4.0));
	CHECK(rig.engine->endCount() == 1);
	const size_t shown = rig.probe.shownCount();

	// Ended becomes Paused at the sought frame. No rewind, no pulse, one new frame.
	rig.engine->requestSeek(0.2);
	CHECK(waitState(*rig.engine, TransportState::Paused));
	CHECK(waitFor([&] { return rig.probe.lastShown() == 10; }));
	sleepMs(150);
	CHECK(rig.engine->snapshot().state == TransportState::Paused);
	CHECK(!rig.engine->isPlaying());
	CHECK(rig.probe.shownCount() == shown + 1);
	CHECK(rig.engine->endCount() == 1);
	CHECK(rig.probe.rewinds.load() == 0);
	CHECK(near(rig.engine->snapshot().positionSeconds, 0.2, 0.011));

	// Play continues from there. Reaching the end again is one more end of the clip, and only one.
	rig.engine->requestPlayToggle();
	CHECK(waitFor([&] { return rig.engine->endCount() == 2; }, 4.0));
	CHECK(waitState(*rig.engine, TransportState::Ended));
	sleepMs(100);
	CHECK(rig.engine->endCount() == 2);
	CHECK(rig.probe.rewinds.load() == 0);
	const std::vector<int> after = tailFrom(rig.probe.indices(), shown);
	CHECK(after.size() >= 2);
	if (after.size() >= 2) {
		CHECK(after[0] == 10);
		CHECK(after[1] > 10);
	}
	CHECK_INCREASING(after);
	CHECK(rig.probe.lastShown() == 19);
}

void testSeekCoalesces() {
	currentTest = "rapid seeks coalesce";
	Rig rig(Setup(), 250, 50.0);
	rig.probe.seekSleepMs = 200;  // a decoder takes real time to find a spot
	rig.engine->loadFile(makeMedia("seekrapid.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));

	// One seek is under way when six more arrive. Of those, only the newest is carried out.
	rig.engine->requestSeek(1.0);
	CHECK(waitFor([&] { return rig.probe.seeks.load() >= 1; }));
	for (int i = 1; i <= 6; ++i) {
		rig.engine->requestSeek(1.0 + 0.5 * i);  // 1.5 up to 4.0
	}
	CHECK(waitFor([&] { return rig.probe.lastShown() == 200; }, 4.0));
	sleepMs(300);
	CHECK(rig.probe.seeks.load() == 2);
	CHECK(near(rig.probe.lastSeekTarget.load(), 4.0, 1e-9));
	CHECK(rig.engine->snapshot().state == TransportState::Paused);
	bool onlyTheEnds = true;
	for (int index : rig.probe.indices()) {
		onlyTheEnds = onlyTheEnds && (index == 0 || index == 50 || index == 200);
	}
	CHECK(onlyTheEnds);  // the first frame, the seek that was under way, the newest target

	// A long run of targets in a tight loop: however many are carried out, the last one is.
	rig.probe.seekSleepMs = 0;
	for (int i = 0; i < 500; ++i) {
		rig.engine->requestSeek(0.5 + 0.004 * (i % 100));
	}
	rig.engine->requestSeek(2.5);
	CHECK(waitFor([&] { return rig.probe.lastShown() == 125; }, 4.0));
	sleepMs(200);
	CHECK(rig.probe.lastShown() == 125);
	CHECK(near(rig.probe.lastSeekTarget.load(), 2.5, 1e-9));
	CHECK(rig.engine->snapshot().state == TransportState::Paused);
	CHECK(near(rig.engine->snapshot().positionSeconds, 2.5, 0.011));
}

void testSeekToTheEnd() {
	currentTest = "seek to the end";
	Rig rig(Setup(), 50, 50.0);  // 1 s
	rig.engine->loadFile(makeMedia("seekend.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));

	// All the way to the right is the duration itself, where no frame starts. That is not an error.
	rig.engine->requestSeek(1.0);
	CHECK(waitState(*rig.engine, TransportState::Paused));
	CHECK(waitFor([&] { return rig.probe.shownCount() >= 2; }));
	Snapshot s = rig.engine->snapshot();
	CHECK(s.state == TransportState::Paused);
	CHECK_TEXT(s.message, "");
	CHECK(!s.messageIsError);
	CHECK(rig.probe.lastShown() >= 47 && rig.probe.lastShown() <= 49);
	CHECK(s.positionSeconds > 0.9 && s.positionSeconds < 1.0);
	CHECK(rig.engine->hasFrame());
	CHECK(rig.engine->endCount() == 0);

	// Further than the end is held to the same spot. Below zero is the first frame.
	rig.engine->requestSeek(1e9);
	CHECK(waitFor([&] { return rig.probe.shownCount() >= 3; }));
	CHECK(rig.probe.lastShown() >= 47 && rig.probe.lastShown() <= 49);
	rig.engine->requestSeek(-5.0);
	CHECK(waitFor([&] { return rig.probe.shownCount() >= 4; }));
	CHECK(rig.probe.lastShown() == 0);
	CHECK(rig.engine->snapshot().positionSeconds < 0.001);
	CHECK(rig.engine->snapshot().state == TransportState::Paused);

	// A number that is not a number is ignored.
	const int seeksBefore = rig.probe.seeks.load();
	rig.engine->requestSeek(std::numeric_limits<double>::quiet_NaN());
	rig.engine->requestSeek(std::numeric_limits<double>::infinity());
	sleepMs(100);
	CHECK(rig.probe.seeks.load() == seeksBefore);
	CHECK(rig.engine->snapshot().state == TransportState::Paused);
	CHECK(rig.probe.lastShown() == 0);

	// Play from the last frames reaches the end after a moment, as a normal end of the clip.
	rig.engine->requestSeek(1.0);
	CHECK(waitFor([&] { return rig.probe.lastShown() >= 47; }));
	CHECK(rig.engine->endCount() == 0);
	rig.engine->requestPlayToggle();
	CHECK(waitState(*rig.engine, TransportState::Ended, 3.0));
	CHECK(rig.engine->endCount() == 1);
	CHECK(rig.probe.lastShown() == 49);
	CHECK(rig.engine->snapshot().message.empty());

	// The duration the file reports can be longer than its video. A spot with no frame is not an
	// error either: the player looks a little further back until it finds a frame.
	Rig longer(Setup(), 50, 50.0);
	longer.probe.extraDuration = 1.0;  // the file says 2 s, the video has 1 s
	longer.engine->loadFile(makeMedia("seeklong.mp4"));
	CHECK(waitState(*longer.engine, TransportState::Ready));
	CHECK(near(longer.engine->snapshot().durationSeconds, 2.0, 1e-6));
	longer.engine->requestSeek(1.8);
	CHECK(waitState(*longer.engine, TransportState::Paused));
	CHECK(waitFor([&] { return longer.probe.shownCount() >= 2; }));
	CHECK(longer.engine->snapshot().message.empty());
	CHECK(!longer.engine->snapshot().messageIsError);
	CHECK(longer.probe.lastShown() == 28);  // 0.55 s, after trying 1.8 s and 1.55 s
	CHECK(longer.probe.seeks.load() == 3);
	CHECK(longer.probe.rewinds.load() == 0);
	CHECK(near(longer.engine->snapshot().positionSeconds, 0.56, 0.011));

	// With no frame to be found at all, the player goes back to the first frame instead of failing.
	Rig nothing(Setup(), 50, 50.0);
	nothing.probe.extraDuration = 100.0;  // the file says 101 s, the video has 1 s
	nothing.engine->loadFile(makeMedia("seeknothing.mp4"));
	CHECK(waitState(*nothing.engine, TransportState::Ready));
	nothing.engine->requestSeek(90.0);
	CHECK(waitState(*nothing.engine, TransportState::Paused));
	CHECK(waitFor([&] { return nothing.probe.shownCount() >= 2; }));
	CHECK(nothing.engine->snapshot().message.empty());
	CHECK(!nothing.engine->snapshot().messageIsError);
	CHECK(nothing.probe.lastShown() == 0);
	CHECK(nothing.probe.seeks.load() == 5);
	CHECK(nothing.probe.rewinds.load() == 1);
	CHECK(nothing.engine->snapshot().positionSeconds < 0.001);
}

void testSeekDecoderFailures() {
	currentTest = "seek the decoder cannot do";
	Rig rig(Setup(), 100, 50.0);
	rig.engine->loadFile(makeMedia("seekrefused.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));

	// A decoder that refuses the seek: back to the first frame, no error.
	rig.probe.failSeek = true;
	rig.engine->requestSeek(1.0);
	CHECK(waitState(*rig.engine, TransportState::Paused));
	CHECK(rig.probe.shownCount() == 2);  // the first frame again
	CHECK(rig.probe.lastShown() == 0);
	CHECK(rig.probe.rewinds.load() == 1);
	CHECK(rig.engine->snapshot().message.empty());
	CHECK(!rig.engine->snapshot().messageIsError);
	CHECK(rig.engine->snapshot().positionSeconds < 0.001);

	// The player is not left broken: the next seek works.
	rig.probe.failSeek = false;
	rig.engine->requestSeek(1.0);
	CHECK(waitFor([&] { return rig.probe.lastShown() == 50; }));
	CHECK(rig.engine->snapshot().state == TransportState::Paused);
	CHECK(near(rig.engine->snapshot().positionSeconds, 1.0, 0.011));

	// A real decode error at the sought spot is an error, with the usual message.
	Rig broken(Setup(), 100, 50.0);
	const std::string path = makeMedia("seekbroken.mp4");
	broken.probe.failAtFrame = 60;
	broken.engine->loadFile(path);
	CHECK(waitState(*broken.engine, TransportState::Ready));
	broken.engine->requestSeek(1.2);
	CHECK(waitState(*broken.engine, TransportState::Error));
	CHECK_TEXT(broken.engine->snapshot().message, msgDecodeFailed(path));
	CHECK(broken.engine->snapshot().messageIsError);
	CHECK(!broken.engine->isPlaying());

	// In the Error state a seek does nothing, as the buttons do nothing.
	broken.engine->requestSeek(0.2);
	sleepMs(100);
	CHECK(broken.engine->snapshot().state == TransportState::Error);
	CHECK(broken.probe.seeks.load() == 1);
}

void testSeekWithoutVideo() {
	currentTest = "seek with no video";
	Rig rig;
	rig.engine->requestSeek(1.0);
	sleepMs(100);
	CHECK(rig.engine->snapshot().state == TransportState::NoSource);
	CHECK(!rig.engine->hasFrame());
	CHECK(rig.probe.seeks.load() == 0);

	// The seek was not kept for later. A video that loads now waits at its first frame.
	rig.engine->loadFile(makeMedia("noseek.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	sleepMs(150);
	CHECK(rig.engine->snapshot().state == TransportState::Ready);
	CHECK(rig.probe.shownCount() == 1);
	CHECK(rig.probe.lastShown() == 0);
	CHECK(rig.probe.seeks.load() == 0);
	CHECK(rig.engine->snapshot().positionSeconds < 0.001);

	// A video that failed to load takes no seek either, and loading again recovers.
	const std::string missing = gRoot + "/media/seek-missing.mp4";
	std::remove(missing.c_str());
	rig.engine->loadFile(missing);
	CHECK(waitState(*rig.engine, TransportState::Error));
	const std::string message = rig.engine->snapshot().message;
	rig.engine->requestSeek(0.5);
	sleepMs(100);
	CHECK(rig.engine->snapshot().state == TransportState::Error);
	CHECK_TEXT(rig.engine->snapshot().message, message);
	CHECK(rig.probe.seeks.load() == 0);
	rig.engine->loadFile(makeMedia("noseek-again.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	sleepMs(100);
	CHECK(rig.engine->snapshot().state == TransportState::Ready);
	CHECK(rig.probe.seeks.load() == 0);
}

void testSeekDroppedByLoad() {
	currentTest = "seek dropped by a new video";
	Rig rig(Setup(), 100, 50.0);
	rig.probe.seekSleepMs = 300;  // the first seek keeps the worker busy while the rest is posted
	rig.engine->loadFile(makeMedia("seek-a.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));

	rig.engine->requestSeek(0.5);
	CHECK(waitFor([&] { return rig.probe.seeks.load() >= 1; }));
	rig.engine->requestSeek(1.5);  // waits behind the first one
	rig.engine->loadFile(makeMedia("seek-b.mp4"));
	CHECK(waitOpened(rig, 2));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	sleepMs(400);

	// The new video waits at its first frame. The seek aimed at the old one is gone.
	CHECK(rig.engine->snapshot().state == TransportState::Ready);
	CHECK_TEXT(rig.engine->snapshot().sourceName, "seek-b.mp4");
	CHECK(rig.probe.seeks.load() == 1);
	CHECK(rig.probe.lastShown() == 0);
	CHECK(near(rig.engine->snapshot().positionSeconds, 0.0, 0.001));
	bool noSecondSeek = true;
	for (int index : rig.probe.indices()) {
		noSecondSeek = noSecondSeek && index != 75;
	}
	CHECK(noSecondSeek);
	CHECK(!rig.engine->isPlaying());
}

void testSeekDuringDownload() {
	currentTest = "seek during a download";
	std::error_code ec;
	fs::remove_all(gRoot + "/cache-seek", ec);
	Setup setup;
	setup.cacheDir = gRoot + "/cache-seek";
	setup.ytDlpDirs = {makeYtDlpDir("ytdlp-seek", "seek", kBodySlowOk)};
	resetFakeLog();
	Rig rig(setup, 100, 50.0);
	rig.engine->loadLink(kWatchUrl);
	CHECK(waitFor([&] {
		const Snapshot s = rig.engine->snapshot();
		return s.state == TransportState::Loading && s.message.find("Downloading from YouTube") != std::string::npos;
	}));

	// The download carries on, and the video that arrives is not seeked: the seek was meant for
	// whatever was on screen before.
	rig.engine->requestSeek(0.5);
	rig.engine->requestSeek(1.0);
	CHECK(waitState(*rig.engine, TransportState::Ready, 10.0));
	sleepMs(200);
	CHECK(rig.engine->snapshot().state == TransportState::Ready);
	CHECK_TEXT(rig.engine->snapshot().message, "");
	CHECK(rig.probe.seeks.load() == 0);
	CHECK(rig.probe.shownCount() == 1);
	CHECK(rig.engine->snapshot().positionSeconds < 0.001);
	CHECK(readLines(logPath()).size() == 1);

	// A seek after that works as usual.
	rig.engine->requestSeek(1.0);
	CHECK(waitState(*rig.engine, TransportState::Paused));
	CHECK(waitFor([&] { return rig.probe.lastShown() == 50; }));
}

void testSeekThenLoopWrap() {
	currentTest = "loop wrap after a seek";
	Rig rig(Setup(), 40, 50.0);  // 0.8 s
	rig.probe.ptsOffset = 3.0;   // the clip starts at 3 s in the file, so a wrong start would show
	rig.probe.rewindSleepMs = 5;
	rig.engine->loadFile(makeMedia("seekloop.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	rig.engine->setLoop(true);
	rig.engine->requestPlayToggle();
	CHECK(waitFor([&] { return rig.probe.shownCount() >= 4; }));

	const size_t before = rig.probe.shownCount();
	rig.engine->requestSeek(0.6);  // frame 30: ten frames, 0.2 s, to the end of the clip
	CHECK(waitFor([&] { return findShown(rig, before, 30) >= 0; }));
	CHECK(near(rig.probe.lastSeekTarget.load(), 3.6, 1e-9));
	CHECK(waitFor([&] { return rig.engine->endCount() >= 1; }, 3.0));
	CHECK(waitFor([&] { return rig.probe.shownCount() >= before + 14; }, 3.0));

	// The trip from the sought frame runs to the end, and the next one starts at the true first frame.
	const int jump = findShown(rig, before, 30);
	if (jump >= 0) {
		const std::vector<int> after = tailFrom(rig.probe.indices(), static_cast<size_t>(jump));
		size_t wrap = 1;
		while (wrap < after.size() && after[wrap] > after[wrap - 1]) {
			++wrap;
		}
		CHECK(wrap < after.size());
		if (wrap < after.size()) {
			CHECK(after[wrap - 1] >= 34);  // the end of the clip
			CHECK(after[wrap] <= 3);       // the start of the clip, not the sought frame again
		}
	}
	CHECK(rig.engine->snapshot().state == TransportState::Playing);
	CHECK(rig.probe.rewinds.load() >= 1);

	// The position counts from the start of the clip all the time, also after the wrap.
	bool inRange = true;
	for (int i = 0; i < 60; ++i) {
		const double position = rig.engine->snapshot().positionSeconds;
		inRange = inRange && position >= 0.0 && position <= 0.8 + 1e-6;
		sleepMs(10);
	}
	CHECK(inRange);
	CHECK(rig.engine->snapshot().state == TransportState::Playing);
}

void testSeekFiresNoEndPulse() {
	currentTest = "seek fires no end pulse";

	// While playing with Loop on, seeks into the middle of a long clip give no pulse.
	{
		Rig rig(Setup(), 500, 50.0);  // 10 s
		rig.engine->loadFile(makeMedia("seekpulse.mp4"));
		CHECK(waitState(*rig.engine, TransportState::Ready));
		rig.engine->setLoop(true);
		rig.engine->requestPlayToggle();
		CHECK(waitFor([&] { return rig.probe.shownCount() >= 4; }));
		for (double target : {5.0, 0.0, 8.0, 0.5}) {
			const size_t before = rig.probe.shownCount();
			rig.engine->requestSeek(target);
			CHECK(waitFor([&] { return findShown(rig, before, static_cast<int>(target * 50.0 + 0.5)) >= 0; }));
			CHECK(rig.engine->endCount() == 0);
		}
		sleepMs(100);
		CHECK(rig.engine->endCount() == 0);
		CHECK(rig.engine->snapshot().state == TransportState::Playing);
	}

	// From the end of a clip, and to the end of a clip, while stopped.
	{
		Rig rig(Setup(), 50, 50.0);  // 1 s
		rig.engine->loadFile(makeMedia("seekpulse2.mp4"));
		CHECK(waitState(*rig.engine, TransportState::Ready));
		rig.engine->requestPlayToggle();
		CHECK(waitState(*rig.engine, TransportState::Ended, 4.0));
		CHECK(rig.engine->endCount() == 1);
		rig.engine->requestSeek(0.0);
		CHECK(waitState(*rig.engine, TransportState::Paused));
		rig.engine->requestSeek(5.0);  // past the end: held to the last frames
		CHECK(waitFor([&] { return rig.probe.lastShown() >= 47; }));
		sleepMs(150);
		CHECK(rig.engine->endCount() == 1);
		CHECK(rig.engine->snapshot().state == TransportState::Paused);

		// Playing the last frames to the end is a real end of the clip, and counts as one.
		rig.engine->requestPlayToggle();
		CHECK(waitFor([&] { return rig.engine->endCount() == 2; }, 3.0));
		CHECK(waitState(*rig.engine, TransportState::Ended));
		sleepMs(100);
		CHECK(rig.engine->endCount() == 2);
	}
}

void testSeekClipStartOffset() {
	currentTest = "seek in a clip that starts late";
	Rig rig(Setup(), 100, 50.0);  // 2 s
	rig.probe.ptsOffset = 1.0;    // the first frame is at 1 s in the file
	rig.engine->loadFile(makeMedia("seekoffset.mp4"));
	CHECK(waitState(*rig.engine, TransportState::Ready));
	CHECK(near(rig.engine->snapshot().positionSeconds, 0.0, 0.001));
	CHECK(near(rig.engine->snapshot().durationSeconds, 2.0, 1e-6));

	// The decoder is asked for file time. The panel is told clip time.
	rig.engine->requestSeek(0.5);
	CHECK(waitFor([&] { return rig.probe.lastShown() == 25; }));
	CHECK(waitState(*rig.engine, TransportState::Paused));
	CHECK(near(rig.probe.lastSeekTarget.load(), 1.5, 1e-9));
	CHECK(near(rig.engine->snapshot().positionSeconds, 0.5, 0.011));

	// Playing goes on in clip time.
	rig.engine->requestPlayToggle();
	CHECK(waitFor([&] {
		const double position = rig.engine->snapshot().positionSeconds;
		return position > 0.55 && position < 1.0;
	}));
	rig.engine->requestPlayToggle();
	CHECK(waitState(*rig.engine, TransportState::Paused));

	// The very start of the clip is file time 1 s.
	const size_t shown = rig.probe.shownCount();
	rig.engine->requestSeek(0.0);
	CHECK(waitFor([&] { return rig.probe.shownCount() == shown + 1 && rig.probe.lastShown() == 0; }));
	CHECK(near(rig.probe.lastSeekTarget.load(), 1.0, 1e-9));
	CHECK(near(rig.engine->snapshot().positionSeconds, 0.0, 0.001));

	// The end of the clip: duration, not duration plus the 1 s the file starts at.
	rig.engine->requestSeek(2.0);
	CHECK(waitFor([&] { return rig.probe.lastShown() >= 97; }));
	CHECK(rig.probe.lastShown() <= 99);
	CHECK(rig.engine->snapshot().positionSeconds > 1.9 && rig.engine->snapshot().positionSeconds < 2.0);
	CHECK(rig.engine->snapshot().state == TransportState::Paused);
	CHECK(rig.engine->snapshot().message.empty());
}

}  // namespace

int main() {
	std::error_code ec;
	gRoot = fs::absolute("build/video_player_engine_test_tmp", ec).string();
	fs::remove_all(gRoot, ec);
	fs::create_directories(gRoot + "/media", ec);
	if (!fs::is_directory(gRoot + "/media", ec)) {
		std::printf("  FAIL: cannot create %s\n", gRoot.c_str());
		return 1;
	}

	testIdleEngine();
	testLoadFile();
	testPlayToEnd();
	testPlayAgainFromEnded();
	testPauseAndResume();
	testStopWhilePlaying();
	testLoopWrap();
	testLoopSwitchedOnLate();
	testSpeed();
	testMissingFile();
	testUnsupportedContainer();
	testDecoderProblems();
	testLinkSorting();
	testYtDlpMissing();
	testYtDlpDownloadAndCache();
	testYtDlpSearchOrder();
	testYtDlpFailures();
	testYtDlpProgressAndButtons();
	testYtDlpStopsWithEngine();
	testSaveAndRestore();
	testClearAndReplace();
	testPlatformWithoutPlayback();
	testNoFrameSlot();
	testSeekWhilePlaying();
	testSeekWhilePaused();
	testSeekFromReady();
	testSeekAfterEnd();
	testSeekCoalesces();
	testSeekToTheEnd();
	testSeekDecoderFailures();
	testSeekWithoutVideo();
	testSeekDroppedByLoad();
	testSeekDuringDownload();
	testSeekThenLoopWrap();
	testSeekFiresNoEndPulse();
	testSeekClipStartOffset();

	std::printf("video_player_engine_test: %d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
