#include "video_player_engine.hpp"

#include "video_external_frame.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#else
extern char** environ;
#endif
#endif

namespace video_player {

namespace {

// A frame this late means the clock ran ahead of the pictures (a stall). The clock waits for the
// picture instead of skipping the video forward.
constexpr double kReanchorLateSeconds = 0.25;
constexpr int kNoticeSeconds = 8;
// A seek that finds no frame (the video track can end before the file's duration) is tried again
// this many times in all, each time further back, before the engine goes back to the start.
constexpr int kSeekAttempts = 5;

std::string trimText(const std::string& text) {
	const char* space = " \t\r\n";
	const std::size_t begin = text.find_first_not_of(space);
	if (begin == std::string::npos) {
		return std::string();
	}
	const std::size_t end = text.find_last_not_of(space);
	return text.substr(begin, end - begin + 1);
}

std::string fileNameOf(const std::string& path) {
	const std::size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool statPath(const std::string& path, bool& isDirectory, long long& size) {
	struct stat info;
	if (::stat(path.c_str(), &info) != 0) {
		return false;
	}
	isDirectory = S_ISDIR(info.st_mode);
	size = static_cast<long long>(info.st_size);
	return true;
}

bool isFile(const std::string& path) {
	bool isDirectory = false;
	long long size = 0;
	return statPath(path, isDirectory, size) && !isDirectory;
}

bool fileHasData(const std::string& path) {
	bool isDirectory = false;
	long long size = 0;
	return statPath(path, isDirectory, size) && !isDirectory && size > 0;
}

void makeDirectory(const std::string& path) {
#ifdef _WIN32
	_mkdir(path.c_str());
#else
	::mkdir(path.c_str(), 0755);
#endif
}

// mkdir -p. True when the folder exists afterwards.
bool makeDirectories(const std::string& path) {
	if (path.empty()) {
		return false;
	}
	std::string partial;
	for (std::size_t i = 0; i <= path.size(); ++i) {
		if (i == path.size() || path[i] == '/' || path[i] == '\\') {
			if (!partial.empty()) {
				makeDirectory(partial);
			}
		}
		if (i < path.size()) {
			partial.push_back(path[i]);
		}
	}
	bool isDirectory = false;
	long long size = 0;
	return statPath(path, isDirectory, size) && isDirectory;
}

// yt-dlp reads % in an output template as a field. The cache path is a fixed name.
std::string escapeOutputTemplate(const std::string& path) {
	std::string out;
	for (char c : path) {
		out.push_back(c);
		if (c == '%') {
			out.push_back('%');
		}
	}
	return out;
}

#ifndef _WIN32
char** currentEnvironment() {
#ifdef __APPLE__
	return *_NSGetEnviron();
#else
	return environ;
#endif
}

// The caller's environment with Homebrew's folders first on PATH. Rack started from Finder has
// a short PATH, and yt-dlp may need the python3 that Homebrew installs next to it.
std::vector<std::string> childEnvironment() {
	std::vector<std::string> env;
	std::string path;
	for (char** entry = currentEnvironment(); entry && *entry; ++entry) {
		const std::string text = *entry;
		if (text.compare(0, 5, "PATH=") == 0) {
			path = text.substr(5);
		} else {
			env.push_back(text);
		}
	}
	if (path.empty()) {
		path = "/usr/bin:/bin:/usr/sbin:/sbin";
	}
	env.push_back("PATH=/opt/homebrew/bin:/usr/local/bin:" + path);
	return env;
}
#endif

}  // namespace

// ---------------------------------------------------------------------------------------------
// UI thread

Engine::Engine(int externalFrameId, const EngineOptions& options)
	: frameId_(externalFrameId), options_(options), customFactory_(static_cast<bool>(options.factory)) {
	if (options_.lookaheadFrames < 2) {
		options_.lookaheadFrames = 2;
	}
	if (options_.maxLongSide < 16) {
		options_.maxLongSide = 16;
	}
	lastSnapshot_ = Clock::now();
	anchorWall_ = lastSnapshot_;
	thread_ = std::thread([this] { run(); });
}

Engine::~Engine() {
	quit_.store(true);
	{
		// The worker checks quit_ while holding this lock before it sleeps, so the wake-up below
		// cannot slip past it.
		std::lock_guard<std::mutex> lock(mutex_);
	}
	cv_.notify_all();
	if (thread_.joinable()) {
		thread_.join();
	}
	clearOutput();
}

void Engine::loadFile(const std::string& path) {
	const std::string text = trimText(path);
	if (text.empty()) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		settings_.source = SourceKind::File;
		settings_.filePath = text;
		settings_.youtubeUrl.clear();
		request_.kind = Request::Kind::File;
		request_.text = text;
	}
	cv_.notify_one();
}

void Engine::loadLink(const std::string& text) {
	const std::string trimmed = trimText(text);
	switch (classifySource(trimmed)) {
		case SourceClass::Empty:
			return;
		case SourceClass::LocalFile:
		case SourceClass::UnsupportedContainer:
			loadFile(trimmed);
			return;
		case SourceClass::YouTube: {
			const std::string url = youtubeWatchUrl(youtubeVideoId(trimmed));
			{
				std::lock_guard<std::mutex> lock(mutex_);
				settings_.source = SourceKind::YouTube;
				settings_.filePath.clear();
				settings_.youtubeUrl = url;
				request_.kind = Request::Kind::YouTube;
				request_.text = url;
			}
			cv_.notify_one();
			return;
		}
		case SourceClass::UnsupportedUrl: {
			{
				std::lock_guard<std::mutex> lock(mutex_);
				notice_ = msgUnsupportedUrl();
				noticePending_ = true;
			}
			cv_.notify_one();
			return;
		}
	}
}

void Engine::clearSource() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		settings_ = Settings();
		request_.kind = Request::Kind::Clear;
		request_.text.clear();
	}
	cv_.notify_one();
}

void Engine::restore(const Settings& settings) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		settings_ = Settings();
		request_.kind = Request::Kind::Clear;
		request_.text.clear();
		if (settings.source == SourceKind::File && !trimText(settings.filePath).empty()) {
			settings_.source = SourceKind::File;
			settings_.filePath = trimText(settings.filePath);
			request_.kind = Request::Kind::File;
			request_.text = settings_.filePath;
		} else if (settings.source == SourceKind::YouTube && !trimText(settings.youtubeUrl).empty()) {
			settings_.source = SourceKind::YouTube;
			settings_.youtubeUrl = trimText(settings.youtubeUrl);
			request_.kind = Request::Kind::YouTube;
			request_.text = settings_.youtubeUrl;
		}
	}
	cv_.notify_one();
}

void Engine::requestSeek(double seconds) {
	if (!std::isfinite(seconds)) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		// An earlier target the worker has not acted on yet is replaced: the newest one wins.
		seekSeconds_ = seconds;
		seekPending_ = true;
	}
	cv_.notify_one();
}

Settings Engine::settings() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return settings_;
}

Snapshot Engine::snapshot() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return snapshot_;
}

// ---------------------------------------------------------------------------------------------
// Worker thread: main loop

void Engine::run() {
	while (!quit_.load()) {
		Request request;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (request_.kind != Request::Kind::None) {
				request = std::move(request_);
				request_ = Request();
			}
		}
		if (request.kind != Request::Kind::None) {
			if (request.kind == Request::Kind::Clear) {
				unloadSource();
				publishSnapshot(true);
			} else {
				beginLoad(request.kind == Request::Kind::YouTube, request.text);
			}
			continue;
		}

		const std::chrono::microseconds wait = step();
		if (wait.count() > 0) {
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait_for(lock, wait, [this] {
				return quit_.load() || request_.kind != Request::Kind::None || seekPending_;
			});
		}
	}
}

bool Engine::requestPending() {
	std::lock_guard<std::mutex> lock(mutex_);
	return request_.kind != Request::Kind::None;
}

void Engine::pollNotice() {
	std::string text;
	bool have = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (noticePending_) {
			text = std::move(notice_);
			noticePending_ = false;
			have = true;
		}
	}
	if (!have) {
		return;
	}
	if (!noticeActive_) {
		savedMessage_ = message_;
		savedMessageIsError_ = messageIsError_;
	}
	message_ = text;
	messageIsError_ = true;
	noticeActive_ = true;
	noticeUntil_ = Clock::now() + std::chrono::seconds(kNoticeSeconds);
	publishSnapshot(true);
}

// One pass of the loop. Returns how long to sleep before the next one.
std::chrono::microseconds Engine::step() {
	pollNotice();
	if (noticeActive_ && Clock::now() >= noticeUntil_) {
		noticeActive_ = false;
		message_ = savedMessage_;
		messageIsError_ = savedMessageIsError_;
		savedMessage_.clear();
		publishSnapshot(true);
	}

	handleTransport();
	handleSeek();

	if (state_ == TransportState::Playing) {
		updateClockForSpeed();
		// A wrap or an end can leave more frames due, so look again, with a limit for safety.
		for (int i = 0; i < 64; ++i) {
			present();
			if (!checkEnd()) {
				break;
			}
		}
	}

	bool worked = false;
	if (state_ == TransportState::Ready || state_ == TransportState::Playing ||
		state_ == TransportState::Paused) {
		worked = decodeSome();
	}

	if (state_ == TransportState::Playing) {
		publishSnapshot(false);
	}
	playing_.store(state_ == TransportState::Playing, std::memory_order_relaxed);

	if (worked) {
		return std::chrono::microseconds(0);
	}

	double waitSeconds = 0.010;
	if (state_ == TransportState::NoSource || state_ == TransportState::Error) {
		waitSeconds = 0.050;
	}
	if (state_ == TransportState::Playing) {
		double next = std::numeric_limits<double>::infinity();
		if (!ring_.empty()) {
			next = ring_.front().pts;
		}
		if (!passEnds_.empty()) {
			next = std::min(next, passEnds_.front().endTimeline);
		}
		if (std::isfinite(next)) {
			const double untilNext = (next - mediaNow()) / anchorSpeed_;
			waitSeconds = std::min(waitSeconds, std::max(0.0002, untilNext));
		}
	}
	return std::chrono::microseconds(static_cast<long long>(waitSeconds * 1e6));
}

// ---------------------------------------------------------------------------------------------
// Worker thread: loading

void Engine::beginLoad(bool youtube, const std::string& text) {
	// A seek that is still waiting was aimed at the video that is about to go away.
	dropPendingSeek();
	loadSource(youtube, text);
	// Button presses made while a download was running do not start playback afterwards. Neither
	// does a seek made then, which can only have been aimed at the video before it.
	playSeen_ = playRequests_.load(std::memory_order_relaxed);
	stopSeen_ = stopRequests_.load(std::memory_order_relaxed);
	dropPendingSeek();
}

void Engine::loadSource(bool youtube, const std::string& text) {
	unloadSource();
	state_ = transportStep(state_, TransportEvent::SourceRequested, false).next;

	std::string path = text;
	if (youtube) {
		const std::string id = youtubeVideoId(text);
		sourceName_ = id.empty() ? text : "youtube.com/watch?v=" + id;
		publishSnapshot(true);
		if (id.empty()) {
			fail(msgUnsupportedUrl());
			return;
		}
		path = cachePathForId(options_.cacheDir, id);
		if (!fileHasData(path)) {
			const char* pathEnv = std::getenv("PATH");
			const std::vector<std::string> dirs = options_.useCustomYtDlpDirs
				? options_.ytDlpDirs
				: ytDlpSearchDirs(pathEnv ? pathEnv : "");
			const std::string exe = findYtDlp(dirs);
			if (exe.empty()) {
				fail(msgYtDlpMissing());
				return;
			}
			if (options_.cacheDir.empty() || !makeDirectories(options_.cacheDir)) {
				fail(msgYouTubeDownloadFailed("the download folder could not be created"));
				return;
			}
			std::string error;
			const Download result = download(exe, id, path, error);
			if (result == Download::Cancelled) {
				return;  // a newer request or shutdown; the loop deals with it
			}
			if (result == Download::Failed) {
				fail(error);
				return;
			}
		}
	} else {
		sourceName_ = fileNameOf(text);
		publishSnapshot(true);
		if (!isFile(text)) {
			fail(msgFileNotFound(text));
			return;
		}
		if (classifySource(text) == SourceClass::UnsupportedContainer) {
			fail(msgUnsupportedContainer(fileExtension(text)));
			return;
		}
	}

	std::string error;
	if (!openPath(path, error)) {
		fail(error);
		return;
	}
	resetTimeline();
	if (!decodeOne() || ring_.empty()) {
		fail(msgDecodeFailed(path));
		return;
	}
	state_ = transportStep(state_, TransportEvent::Loaded, false).next;
	pausedMedia_ = 0.0;
	publishFrame(ring_.front().frame);
	ring_.front().published = true;
	presentedLocal_ = ring_.front().localPts;
	message_.clear();
	messageIsError_ = false;
	publishSnapshot(true);
}

bool Engine::openPath(const std::string& path, std::string& error) {
	if (!customFactory_ && !platformSupportsPlayback()) {
		error = msgNotMacOs();
		return false;
	}
	source_ = customFactory_ ? options_.factory() : createFrameSource();
	if (!source_ || !source_->open(path, options_.maxLongSide)) {
		source_.reset();
		error = msgDecodeFailed(path);
		return false;
	}
	currentPath_ = path;
	durationSeconds_ = source_->durationSeconds();
	width_ = source_->width();
	height_ = source_->height();
	return true;
}

Engine::Download Engine::download(const std::string& exe, const std::string& videoId,
	const std::string& cachePath, std::string& error) {
#ifdef _WIN32
	(void) exe;
	(void) videoId;
	(void) cachePath;
	error = msgNotMacOs();
	return Download::Failed;
#else
	const std::vector<std::string> args = buildYtDlpArgs(exe, videoId, escapeOutputTemplate(cachePath));
	if (args.empty()) {
		error = msgUnsupportedUrl();
		return Download::Failed;
	}

	int fds[2] = {-1, -1};
	if (pipe(fds) != 0) {
		error = msgYouTubeDownloadFailed(std::string("could not start yt-dlp (") + std::strerror(errno) + ")");
		return Download::Failed;
	}
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);
	fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
	posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
	posix_spawnattr_t attributes;
	posix_spawnattr_init(&attributes);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
	// Rack's own file handles stay out of the child.
	posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif

	std::vector<char*> argv;
	for (const std::string& arg : args) {
		argv.push_back(const_cast<char*>(arg.c_str()));
	}
	argv.push_back(nullptr);
	std::vector<std::string> envStrings = childEnvironment();
	std::vector<char*> envp;
	for (std::string& entry : envStrings) {
		envp.push_back(const_cast<char*>(entry.c_str()));
	}
	envp.push_back(nullptr);

	pid_t pid = 0;
	const int spawnResult = posix_spawn(&pid, exe.c_str(), &actions, &attributes, argv.data(), envp.data());
	posix_spawn_file_actions_destroy(&actions);
	posix_spawnattr_destroy(&attributes);
	close(fds[1]);
	if (spawnResult != 0) {
		close(fds[0]);
		error = msgYouTubeDownloadFailed(std::string("could not start yt-dlp (") + std::strerror(spawnResult) + ")");
		return Download::Failed;
	}

	const Clock::time_point started = Clock::now();
	int shownSeconds = -1;
	std::string partial;
	std::string lastLine;
	bool noMp4Format = false;
	int status = 0;
	bool exited = false;
	bool childLost = false;
	bool cancelled = false;

	// Keeps the last line yt-dlp printed. It is the reason shown when a download fails.
	auto takeLine = [&]() {
		const std::string line = trimText(partial);
		partial.clear();
		if (line.empty()) {
			return;
		}
		lastLine = line;
		// yt-dlp's wording when no single file matches the format filter.
		if (line.find("Requested format is not available") != std::string::npos) {
			noMp4Format = true;
		}
	};
	auto drain = [&]() {
		char buffer[512];
		for (;;) {
			const ssize_t count = read(fds[0], buffer, sizeof(buffer));
			if (count <= 0) {
				break;
			}
			for (ssize_t i = 0; i < count; ++i) {
				const char c = buffer[i];
				if (c == '\n' || c == '\r') {
					takeLine();
				} else if (partial.size() < 400) {
					partial.push_back(c);
				}
			}
		}
	};

	for (;;) {
		if (quit_.load() || requestPending()) {
			cancelled = true;
			break;
		}
		drain();
		const pid_t waited = waitpid(pid, &status, WNOHANG);
		if (waited == pid) {
			exited = true;
			break;
		}
		if (waited < 0 && errno != EINTR) {
			// Someone else already collected the child (SIGCHLD ignored). The file tells the result.
			childLost = true;
			break;
		}

		const int seconds = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started).count());
		if (seconds != shownSeconds) {
			shownSeconds = seconds;
			message_ = msgYouTubeDownloading(seconds);
			messageIsError_ = false;
			publishSnapshot(true);
		}

		struct pollfd waitFor;
		waitFor.fd = fds[0];
		waitFor.events = POLLIN;
		waitFor.revents = 0;
		poll(&waitFor, 1, 50);
		if (waitFor.revents & (POLLHUP | POLLERR | POLLNVAL)) {
			usleep(5000);  // output closed, exit status follows within moments
		}
	}

	if (cancelled) {
		kill(pid, SIGTERM);
		bool reaped = false;
		for (int i = 0; i < 100 && !reaped; ++i) {
			const pid_t waited = waitpid(pid, &status, WNOHANG);
			if (waited == pid || (waited < 0 && errno != EINTR)) {
				reaped = true;
			} else {
				usleep(10000);
			}
		}
		if (!reaped) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
		}
		close(fds[0]);
		return Download::Cancelled;
	}

	drain();
	takeLine();  // a last line that had no line ending
	close(fds[0]);
	const bool exitedCleanly = exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
	if ((exitedCleanly || childLost) && fileHasData(cachePath)) {
		return Download::Done;
	}
	// Never leave a file behind that the next attempt would trust.
	std::remove(cachePath.c_str());
	if (noMp4Format) {
		error = msgYouTubeNoMp4();
		return Download::Failed;
	}
	if (lastLine.empty() && exited) {
		if (WIFEXITED(status)) {
			lastLine = "it stopped with code " + std::to_string(WEXITSTATUS(status));
		} else if (WIFSIGNALED(status)) {
			lastLine = "it was stopped by signal " + std::to_string(WTERMSIG(status));
		}
	}
	error = msgYouTubeDownloadFailed(lastLine);
	return Download::Failed;
#endif
}

void Engine::fail(const std::string& message) {
	const std::string keepName = sourceName_;
	unloadSource();
	sourceName_ = keepName;
	enterError(message);
}

void Engine::enterError(const std::string& message) {
	state_ = TransportState::Error;
	message_ = message;
	messageIsError_ = true;
	noticeActive_ = false;
	playing_.store(false, std::memory_order_relaxed);
	publishSnapshot(true);
}

void Engine::unloadSource() {
	flushRing();
	passEnds_.clear();
	pool_.clear();
	source_.reset();
	currentPath_.clear();
	sourceName_.clear();
	durationSeconds_ = 0.0;
	width_ = 0;
	height_ = 0;
	frameDur_ = 1.0 / 30.0;
	pausedMedia_ = 0.0;
	presentedLocal_ = 0.0;
	clipFirstPts_ = 0.0;
	resetTimeline();
	clearOutput();
	state_ = TransportState::NoSource;
	playing_.store(false, std::memory_order_relaxed);
	message_.clear();
	messageIsError_ = false;
	noticeActive_ = false;
	savedMessage_.clear();
}

// ---------------------------------------------------------------------------------------------
// Worker thread: transport

void Engine::handleTransport() {
	// Only the number of presses matters, not their order inside one 10 ms window. Two presses
	// cancel out; any number of stop presses is one stop.
	const uint32_t plays = playRequests_.load(std::memory_order_relaxed);
	const uint32_t stops = stopRequests_.load(std::memory_order_relaxed);
	const uint32_t newPlays = plays - playSeen_;
	const uint32_t newStops = stops - stopSeen_;
	playSeen_ = plays;
	stopSeen_ = stops;
	if (newPlays & 1u) {
		applyEvent(TransportEvent::PlayToggle);
	}
	if (newStops > 0) {
		applyEvent(TransportEvent::Stop);
	}
}

void Engine::applyEvent(TransportEvent event) {
	const TransportState previous = state_;
	const TransportStep next = transportStep(previous, event, loop_.load(std::memory_order_relaxed));
	if (next.next == previous && !next.rewindToFirstFrame) {
		return;
	}
	if (previous == TransportState::Playing) {
		pausedMedia_ = mediaNow();
	}
	state_ = next.next;
	if (next.rewindToFirstFrame && !restartFromBeginning()) {
		return;  // restartFromBeginning left the engine in the Error state
	}
	if (state_ == TransportState::Playing) {
		startClock();
	}
	playing_.store(state_ == TransportState::Playing, std::memory_order_relaxed);
	publishSnapshot(true);
}

void Engine::dropPendingSeek() {
	std::lock_guard<std::mutex> lock(mutex_);
	seekPending_ = false;
}

// Jumps to the newest seek target, if one is waiting.
//
// The state machine in video_player_core has no seek event, so the state is set here. Playing stays
// Playing. Paused stays Paused. Ready and Ended become Paused: Play from Ended would rewind to the
// first frame, and Play from Paused continues from the sought spot, which is what a seek means.
// A seek never reaches the end-of-clip pulse.
void Engine::handleSeek() {
	double target = 0.0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!seekPending_) {
			return;
		}
		// Taken even when it is ignored below. Left set, the wait in run() would wake at once, forever.
		seekPending_ = false;
		target = seekSeconds_;
	}
	const bool hasVideo = state_ == TransportState::Ready || state_ == TransportState::Playing ||
		state_ == TransportState::Paused || state_ == TransportState::Ended;
	if (!hasVideo || !source_ || !(durationSeconds_ > 0.0)) {
		return;
	}
	const bool wasPlaying = state_ == TransportState::Playing;

	// Stay a couple of frames short of the end, where a last frame is certain to exist.
	const double latest = std::max(0.0, durationSeconds_ - 2.0 * frameDur_);
	double at = std::min(std::max(target, 0.0), latest);

	bool landed = false;
	double stepBack = 0.25;
	for (int attempt = 0; attempt < kSeekAttempts; ++attempt) {
		resetTimeline();
		// This pass starts in the middle of the clip. Counting its times from the clip's own first
		// frame keeps the position on screen true, and a loop wrap later returns to the real start.
		haveFirstOfPass_ = true;
		passFirstPts_ = clipFirstPts_;
		if (!source_->seek(clipFirstPts_ + at)) {
			break;
		}
		landed = decodeOne(true) && !ring_.empty();
		if (landed || state_ == TransportState::Error) {
			break;
		}
		// No frame there: the video track can end before the file's duration. Try further back.
		if (at <= 0.0) {
			break;
		}
		at = std::max(0.0, at - stepBack);
		stepBack *= 4.0;
	}
	if (state_ == TransportState::Error) {
		return;  // the decoder reported a real error; decodeOne has shown it
	}
	if (landed) {
		Queued& first = ring_.front();
		publishFrame(first.frame);
		first.published = true;
		presentedLocal_ = first.localPts;
		pausedMedia_ = first.pts;
	} else if (!restartFromBeginning()) {
		return;  // restartFromBeginning left the engine in the Error state
	}

	state_ = wasPlaying ? TransportState::Playing : TransportState::Paused;
	if (wasPlaying) {
		startClock();
	}
	playing_.store(wasPlaying, std::memory_order_relaxed);
	publishSnapshot(true);
}

// Rewinds the decoder, shows the first frame and puts the clock back to zero.
bool Engine::restartFromBeginning() {
	if (!source_) {
		return false;
	}
	resetTimeline();
	pausedMedia_ = 0.0;
	if (!source_->rewind()) {
		enterError(msgDecodeFailed(currentPath_));
		return false;
	}
	if (!decodeOne() || ring_.empty()) {
		if (state_ != TransportState::Error) {
			enterError(msgDecodeFailed(currentPath_));
		}
		return false;
	}
	publishFrame(ring_.front().frame);
	ring_.front().published = true;
	presentedLocal_ = ring_.front().localPts;
	return true;
}

void Engine::startClock() {
	anchorWall_ = Clock::now();
	anchorMedia_ = pausedMedia_;
	anchorSpeed_ = speed_.load(std::memory_order_relaxed);
}

double Engine::mediaNow() const {
	const double elapsed = std::chrono::duration<double>(Clock::now() - anchorWall_).count();
	return anchorMedia_ + elapsed * anchorSpeed_;
}

void Engine::updateClockForSpeed() {
	const double speed = speed_.load(std::memory_order_relaxed);
	if (speed != anchorSpeed_) {
		anchorMedia_ = mediaNow();
		anchorWall_ = Clock::now();
		anchorSpeed_ = speed;
	}
}

// ---------------------------------------------------------------------------------------------
// Worker thread: decoding and presenting

bool Engine::decodeSome() {
	if (!source_ || static_cast<int>(ring_.size()) >= options_.lookaheadFrames) {
		return false;
	}
	return decodeOne();
}

// Decodes one frame, or notes the end of the clip, or wraps the decoder. True when it did any of
// them. With allowEmpty, a decoder that has no frame at all in this pass (a seek past its last
// frame) is not the end of a clip: nothing is noted, no error is raised, and the result is false.
bool Engine::decodeOne(bool allowEmpty) {
	if (!source_) {
		return false;
	}
	if (sourceAtEnd_) {
		// Loop on: rewind at once. The frames already queued keep the picture moving meanwhile.
		return loop_.load(std::memory_order_relaxed) ? wrapDecoder() : false;
	}

	DecodedFrame frame = acquireFrame();
	if (source_->nextFrame(frame)) {
		if (!haveFirstOfPass_) {
			// A trip through the whole clip begins here. (A seek sets haveFirstOfPass_ itself.)
			passFirstPts_ = frame.presentationSeconds;
			clipFirstPts_ = passFirstPts_;
			haveFirstOfPass_ = true;
		}
		double local = frame.presentationSeconds - passFirstPts_;
		if (passFrames_ > 0) {
			if (local <= prevLocalPts_) {
				local = prevLocalPts_ + frameDur_;  // times must climb
			}
			const double delta = local - prevLocalPts_;
			if (delta > 0.001 && delta < 1.0) {
				frameDur_ = delta;
			}
		}
		prevLocalPts_ = local;
		++passFrames_;

		Queued queued;
		queued.pts = decodePassOffset_ + local;
		queued.localPts = local;
		queued.pass = decodePass_;
		queued.frame = std::move(frame);
		ring_.push_back(std::move(queued));
		return true;
	}

	recycle(std::move(frame));
	if (!source_->lastError().empty()) {
		enterError(msgDecodeFailed(currentPath_));
		return false;
	}
	if (allowEmpty && passFrames_ == 0) {
		return false;
	}
	onDecodeEnd();
	return true;
}

// The decoder reached the end of the clip. Records where this trip through the clip ends on the
// timeline: one frame after the last frame.
void Engine::onDecodeEnd() {
	if (passFrames_ == 0) {
		enterError(msgDecodeFailed(currentPath_));
		return;
	}
	PassEnd end;
	end.pass = decodePass_;
	end.startTimeline = decodePassOffset_;
	end.endTimeline = decodePassOffset_ + prevLocalPts_ + frameDur_;
	passEnds_.push_back(end);
	lastPassEnd_ = end.endTimeline;
	sourceAtEnd_ = true;
}

// Starts the next trip through the clip. Its frames continue the timeline where the last one ended.
bool Engine::wrapDecoder() {
	if (!source_ || !source_->rewind()) {
		enterError(msgDecodeFailed(currentPath_));
		return false;
	}
	++decodePass_;
	decodePassOffset_ = lastPassEnd_;
	haveFirstOfPass_ = false;
	passFrames_ = 0;
	prevLocalPts_ = 0.0;
	sourceAtEnd_ = false;
	return true;
}

void Engine::resetTimeline() {
	flushRing();
	passEnds_.clear();
	decodePass_ = 0;
	decodePassOffset_ = 0.0;
	lastPassEnd_ = 0.0;
	sourceAtEnd_ = false;
	haveFirstOfPass_ = false;
	passFirstPts_ = 0.0;
	prevLocalPts_ = 0.0;
	passFrames_ = 0;
	presentedLocal_ = 0.0;
}

// Shows the newest frame that is due and drops the older ones. Frames of the next trip through
// the clip wait until the end of this one has been dealt with in checkEnd().
void Engine::present() {
	if (ring_.empty()) {
		return;
	}
	const double now = mediaNow();
	Queued latest;
	bool got = false;
	while (!ring_.empty()) {
		Queued& front = ring_.front();
		if (!passEnds_.empty() && front.pass > passEnds_.front().pass) {
			break;
		}
		if (front.pts > now) {
			break;
		}
		if (got) {
			recycle(std::move(latest.frame));
		}
		latest = std::move(front);
		ring_.pop_front();
		got = true;
	}
	if (!got) {
		return;
	}
	if (now - latest.pts > kReanchorLateSeconds) {
		anchorMedia_ = latest.pts;
		anchorWall_ = Clock::now();
	}
	if (!latest.published) {
		publishFrame(latest.frame);
	}
	presentedLocal_ = latest.localPts;
	recycle(std::move(latest.frame));
}

// True when a trip through the clip just ended and playback goes on with the next one.
bool Engine::checkEnd() {
	if (passEnds_.empty()) {
		return false;
	}
	const PassEnd end = passEnds_.front();
	if (mediaNow() < end.endTimeline) {
		return false;
	}
	passEnds_.pop_front();

	const TransportStep next = transportStep(
		TransportState::Playing, TransportEvent::EndOfClip, loop_.load(std::memory_order_relaxed));
	if (next.endOfClipPulse) {
		endCount_.fetch_add(1, std::memory_order_relaxed);
	}
	state_ = next.next;
	if (state_ == TransportState::Ended) {
		// The last frame stays on screen. Anything decoded beyond it is thrown away.
		flushRing();
		passEnds_.clear();
		pausedMedia_ = end.endTimeline;
		presentedLocal_ = end.endTimeline - end.startTimeline;
		playing_.store(false, std::memory_order_relaxed);
		publishSnapshot(true);
		return false;
	}
	if (sourceAtEnd_ && passEnds_.empty()) {
		// Loop was switched on after the decoder had already stopped at the end. Rewind now.
		wrapDecoder();
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// Worker thread: frames and snapshots

DecodedFrame Engine::acquireFrame() {
	if (!pool_.empty()) {
		DecodedFrame frame = std::move(pool_.back());
		pool_.pop_back();
		return frame;
	}
	return DecodedFrame();
}

void Engine::recycle(DecodedFrame&& frame) {
	if (pool_.size() < static_cast<std::size_t>(options_.lookaheadFrames) + 3) {
		pool_.push_back(std::move(frame));
	}
}

void Engine::flushRing() {
	while (!ring_.empty()) {
		recycle(std::move(ring_.front().frame));
		ring_.pop_front();
	}
}

void Engine::publishFrame(const DecodedFrame& frame) {
	if (frame.width <= 0 || frame.height <= 0 ||
		frame.rgba.size() < static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 4u) {
		return;
	}
	width_ = frame.width;
	height_ = frame.height;
	if (frameId_ > 0) {
		videoExternalFramePublish(frameId_, frame.rgba.data(), frame.width, frame.height, frame.presentationSeconds);
		hasFrame_.store(true, std::memory_order_relaxed);
	}
	if (options_.onFramePublished) {
		options_.onFramePublished(frame);
	}
}

void Engine::clearOutput() {
	hasFrame_.store(false, std::memory_order_relaxed);
	if (frameId_ > 0) {
		videoExternalFrameClear(frameId_);
	}
}

void Engine::publishSnapshot(bool force) {
	const Clock::time_point now = Clock::now();
	if (!force && now - lastSnapshot_ < std::chrono::milliseconds(50)) {
		return;
	}
	lastSnapshot_ = now;
	Snapshot snapshot;
	snapshot.state = state_;
	snapshot.sourceName = sourceName_;
	snapshot.message = message_;
	snapshot.messageIsError = messageIsError_;
	snapshot.positionSeconds = presentedLocal_;
	snapshot.durationSeconds = durationSeconds_;
	snapshot.width = width_;
	snapshot.height = height_;
	std::lock_guard<std::mutex> lock(mutex_);
	snapshot_ = std::move(snapshot);
}

}  // namespace video_player
