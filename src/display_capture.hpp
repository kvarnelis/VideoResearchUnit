#pragma once

// Rack-free helpers for the Display module's SHOT button: where files go, what they are called, how
// a PNG is made and written, and the worker thread that does it, so a button press never waits for
// the disk. Nothing here shows a dialog. The REC button's recorder is in display_recorder.hpp and
// shares the folder and file name helpers below.

#include <condition_variable>
#include <cstddef>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace display_capture {

// ---- Folders and names -------------------------------------------------------------------------

// The user's home folder, or "" when it cannot be found.
std::string homeDirectory();

// <home>/Pictures/Video Research Unit, or "" when there is no home folder. This is where captures go when no
// folder was chosen or the chosen one has gone. It is deliberately not Desktop, Documents or
// Downloads: macOS asks the user for permission before an app writes there.
std::string defaultCaptureFolder();

bool directoryExists(const std::string& path);

// The folder to write into: `chosen` when it is an existing directory, otherwise the default.
// Touches the disk (one stat), so call it from a worker thread, not from a button press.
std::string resolveCaptureFolder(const std::string& chosen);

// "/Users/me/Pictures/x" becomes "~/Pictures/x". Other paths are returned unchanged.
std::string shortenHome(const std::string& path);

// shortenHome(), cut to about maxChars for a menu: the end of the path is kept, with "..." in front.
std::string shortenForMenu(const std::string& path, std::size_t maxChars = 42);

// "vru-YYYY-MM-DD_HH-MM-SS" in local time. The caller adds ".png" or ".mov".
std::string timestampStem(std::time_t when);

// Replaces the folder creation used by ensureFolder(). The Display module installs
// rack::system::createDirectories here. The function returns false and fills `error` on failure.
// May be called while workers run, so set it once at start-up.
using CreateFolderFn = std::function<bool(const std::string& path, std::string& error)>;
void setCreateFolderHook(CreateFolderFn hook);

// Creates the folder and any missing parents (it does nothing when it exists). false: `error` says why.
bool ensureFolder(const std::string& path, std::string& error);

// ---- The folder the user picked last ------------------------------------------------------------

// New Display modules start with this folder. The plugin saves it in Rack's settings, so it lasts
// across patches and restarts. "" means the default folder.
std::string lastPickedFolder();
void setLastPickedFolder(const std::string& path);

// ---- Writing files without overwriting ---------------------------------------------------------

// Writes data to folder/stem.ext, or folder/stem-2.ext, -3, ... when the name is taken. `ext` is
// given without the dot ("png"). The file is created exclusively, so an existing file is never
// replaced or truncated. `writtenPath` is the file created. On a failed write the partial file is
// removed. false: `error` says why.
bool writeNewFile(const std::string& folder, const std::string& stem, const std::string& ext,
	const unsigned char* data, std::size_t size, std::string& writtenPath, std::string& error);

// For writers that create the file themselves (the video encoder refuses an existing file): returns a
// free folder/stem.ext, or -2, -3, ..., and keeps it reserved in this process until releasePath().
// Two recorders started in the same second therefore never pick the same name. "" when no free name
// was found.
std::string reserveUniquePath(const std::string& folder, const std::string& stem, const std::string& ext);
void releasePath(const std::string& path);

// Everything a recording needs before it can start, in one call for a worker thread: the folder to use
// (`chosenFolder` when it exists, else the default), created if missing, and a reserved free
// folder/stem.ext in it. Call releasePath() with the result when the recording is over. false: `error`
// says why (no home folder, cannot create the folder, no free name).
bool chooseNewFilePath(const std::string& chosenFolder, const std::string& stem, const std::string& ext,
	std::string& path, std::string& error);

// ---- PNG ---------------------------------------------------------------------------------------

// Encodes width*height RGBA pixels (top row first, alpha ignored) as an 8-bit RGB PNG. Defined in
// display_png.cpp, the only file that includes stb_image_write.
bool encodePng(const unsigned char* rgba, int width, int height, std::vector<unsigned char>& out, std::string& error);

// ---- Screenshot worker -------------------------------------------------------------------------

struct ShotJob {
	std::vector<unsigned char> rgba;  // width * height * 4, top row first. Moved in, freed after encoding.
	int width = 0;
	int height = 0;
	std::string folder;               // the folder the user chose; "" or a missing folder means the default
	std::string stem;                 // file name without extension
};

struct ShotResult {
	bool ok = false;
	std::string path;   // the file written
	std::string error;  // why it failed
};

// Encodes and writes screenshots on its own thread, one at a time. The thread starts on the first
// submit(). The destructor writes whatever is still queued, then stops the thread.
class ShotWriter {
public:
	static constexpr std::size_t kMaxQueued = 3;

	ShotWriter() = default;
	~ShotWriter();
	ShotWriter(const ShotWriter&) = delete;
	ShotWriter& operator=(const ShotWriter&) = delete;

	// true when submit() would take a job. Cheap, so check it before copying a frame.
	bool canAccept();
	// Queues a job and returns at once. false when the queue is full (the job is left untouched).
	bool submit(ShotJob&& job);
	// Takes the oldest finished result, if any. Never blocks.
	bool pollResult(ShotResult& result);
	// Blocks until the queue is empty and the worker is idle. For tests and shutdown.
	void waitIdle();

private:
	void run();

	std::mutex mutex;
	std::condition_variable wake;
	std::condition_variable idle;
	std::deque<ShotJob> jobs;
	std::deque<ShotResult> results;
	bool busy = false;
	bool stopping = false;
	std::thread worker;
};

}  // namespace display_capture
