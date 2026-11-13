#include "display_capture.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace display_capture {
namespace {

constexpr int kMaxNameTries = 1000;
constexpr std::size_t kMaxResults = 16;

std::string errnoText(int code) {
	return std::error_code(code, std::generic_category()).message();
}

bool isSeparator(char c) {
#if defined(_WIN32)
	return c == '/' || c == '\\';
#else
	return c == '/';
#endif
}

std::string joinPath(const std::string& folder, const std::string& name) {
	if (folder.empty())
		return name;
	std::string out = folder;
	if (!isSeparator(out.back()))
		out += '/';
	return out + name;
}

// folder/stem.ext for n = 1, folder/stem-2.ext for n = 2, and so on.
std::string numberedName(const std::string& stem, const std::string& ext, int n) {
	std::string name = stem;
	if (n > 1)
		name += "-" + std::to_string(n);
	if (!ext.empty()) {
		if (ext[0] != '.')
			name += '.';
		name += ext;
	}
	return name;
}

#if defined(_WIN32)
std::wstring widen(const std::string& s) {
	if (s.empty())
		return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	if (n <= 0)
		return std::wstring();
	std::wstring w(static_cast<std::size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
	w.resize(static_cast<std::size_t>(n) - 1);
	return w;
}
#endif

bool pathExists(const std::string& path) {
#if defined(_WIN32)
	return GetFileAttributesW(widen(path).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
	struct stat st;
	return ::lstat(path.c_str(), &st) == 0;
#endif
}

// Opens a new file for writing and fails with EEXIST when the name is taken.
int openNewFile(const std::string& path) {
#if defined(_WIN32)
	return _wopen(widen(path).c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
	return ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
#endif
}

// Returns the number of bytes written, or -1 with errno set.
long writeSome(int fd, const unsigned char* data, std::size_t size) {
#if defined(_WIN32)
	return _write(fd, data, static_cast<unsigned>(std::min<std::size_t>(size, 1u << 30)));
#else
	return static_cast<long>(::write(fd, data, size));
#endif
}

int closeFile(int fd) {
#if defined(_WIN32)
	return _close(fd);
#else
	return ::close(fd);
#endif
}

void removeFile(const std::string& path) {
#if defined(_WIN32)
	_wunlink(widen(path).c_str());
#else
	::unlink(path.c_str());
#endif
}

int makeOneDirectory(const std::string& path) {
#if defined(_WIN32)
	return _wmkdir(widen(path).c_str());
#else
	return ::mkdir(path.c_str(), 0755);
#endif
}

// mkdir -p. The built-in fallback for ensureFolder(); the plugin installs Rack's createDirectories.
bool makeDirectories(const std::string& path, std::string& error) {
	if (path.empty()) {
		error = "empty folder name";
		return false;
	}
	std::size_t pos = 0;
	while (pos < path.size()) {
		std::size_t next = pos;
		while (next < path.size() && !isSeparator(path[next]))
			next++;
		if (next > 0) {
			std::string prefix = path.substr(0, next);
			bool driveRoot = prefix.size() == 2 && prefix[1] == ':';
			if (!driveRoot && !directoryExists(prefix)) {
				if (makeOneDirectory(prefix) != 0 && errno != EEXIST) {
					error = errnoText(errno);
					return false;
				}
			}
		}
		pos = next + 1;
	}
	if (!directoryExists(path)) {
		error = "not a folder";
		return false;
	}
	return true;
}

std::mutex& hookMutex() {
	static std::mutex m;
	return m;
}

CreateFolderFn& hookStorage() {
	static CreateFolderFn fn;
	return fn;
}

std::mutex& pickedMutex() {
	static std::mutex m;
	return m;
}

std::string& pickedStorage() {
	static std::string path;
	return path;
}

std::mutex& reservedMutex() {
	static std::mutex m;
	return m;
}

std::set<std::string>& reservedStorage() {
	static std::set<std::string> paths;
	return paths;
}

// Picks the folder a capture goes into (the chosen one, or the default) and makes sure it exists.
bool prepareFolder(const std::string& chosen, std::string& folder, std::string& error) {
	folder = resolveCaptureFolder(chosen);
	if (folder.empty()) {
		error = "no folder to save into";
		return false;
	}
	std::string folderError;
	if (!ensureFolder(folder, folderError)) {
		error = "cannot create " + folder + ": " + folderError;
		return false;
	}
	return true;
}

// One screenshot, start to finish. Runs on the ShotWriter thread.
ShotResult processJob(ShotJob& job) {
	ShotResult result;
	std::string folder;
	if (!prepareFolder(job.folder, folder, result.error))
		return result;
	std::vector<unsigned char> png;
	if (!encodePng(job.rgba.data(), job.width, job.height, png, result.error))
		return result;
	// The pixels are no longer needed; free the large buffer before the (possibly slow) write.
	std::vector<unsigned char>().swap(job.rgba);
	std::string writeError;
	if (!writeNewFile(folder, job.stem, "png", png.data(), png.size(), result.path, writeError)) {
		result.error = "cannot write in " + folder + ": " + writeError;
		return result;
	}
	result.ok = true;
	return result;
}

}  // namespace

// ---- Folders and names -------------------------------------------------------------------------

std::string homeDirectory() {
#if defined(_WIN32)
	const char* profile = std::getenv("USERPROFILE");
	if (profile && *profile)
		return profile;
	const char* drive = std::getenv("HOMEDRIVE");
	const char* path = std::getenv("HOMEPATH");
	if (drive && path && *drive && *path)
		return std::string(drive) + path;
	return std::string();
#else
	const char* home = std::getenv("HOME");
	if (home && *home)
		return home;
	struct passwd entry;
	struct passwd* found = nullptr;
	char buffer[4096];
	if (getpwuid_r(getuid(), &entry, buffer, sizeof buffer, &found) == 0 && found && found->pw_dir && *found->pw_dir)
		return found->pw_dir;
	return std::string();
#endif
}

std::string defaultCaptureFolder() {
	std::string home = homeDirectory();
	if (home.empty())
		return std::string();
	return joinPath(home, "Pictures/Video Research Unit");
}

bool directoryExists(const std::string& path) {
	if (path.empty())
		return false;
#if defined(_WIN32)
	DWORD attributes = GetFileAttributesW(widen(path).c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
	struct stat st;
	return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

std::string resolveCaptureFolder(const std::string& chosen) {
	if (!chosen.empty() && directoryExists(chosen))
		return chosen;
	return defaultCaptureFolder();
}

std::string shortenHome(const std::string& path) {
	std::string home = homeDirectory();
	while (home.size() > 1 && isSeparator(home.back()))
		home.pop_back();
	if (home.empty() || home.size() == 1)
		return path;
	if (path == home)
		return "~";
	if (path.size() > home.size() && path.compare(0, home.size(), home) == 0 && isSeparator(path[home.size()]))
		return "~" + path.substr(home.size());
	return path;
}

std::string shortenForMenu(const std::string& path, std::size_t maxChars) {
	std::string s = shortenHome(path);
	auto isContinuation = [](char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; };
	std::size_t chars = 0;
	for (char c : s) {
		if (!isContinuation(c))
			chars++;
	}
	if (chars <= maxChars)
		return s;
	if (maxChars < 4)
		maxChars = 4;
	// Keep the last (maxChars - 3) characters, found by walking back over whole UTF-8 characters.
	const std::size_t keep = maxChars - 3;
	std::size_t seen = 0;
	std::size_t start = s.size();
	while (start > 0 && seen < keep) {
		start--;
		if (!isContinuation(s[start]))
			seen++;
	}
	// Start on a whole folder name when there is a separator to start on.
	std::size_t separator = start;
	while (separator < s.size() && !isSeparator(s[separator]))
		separator++;
	if (separator < s.size())
		start = separator;
	return "..." + s.substr(start);
}

std::string timestampStem(std::time_t when) {
	struct tm parts = {};
#if defined(_WIN32)
	localtime_s(&parts, &when);
#else
	localtime_r(&when, &parts);
#endif
	char buffer[64];
	std::snprintf(buffer, sizeof buffer, "vru-%04d-%02d-%02d_%02d-%02d-%02d", parts.tm_year + 1900,
		parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
	return buffer;
}

void setCreateFolderHook(CreateFolderFn hook) {
	std::lock_guard<std::mutex> lock(hookMutex());
	hookStorage() = std::move(hook);
}

bool ensureFolder(const std::string& path, std::string& error) {
	if (path.empty()) {
		error = "empty folder name";
		return false;
	}
	if (directoryExists(path))
		return true;
	CreateFolderFn hook;
	{
		std::lock_guard<std::mutex> lock(hookMutex());
		hook = hookStorage();
	}
	if (hook)
		return hook(path, error);
	return makeDirectories(path, error);
}

// ---- The folder the user picked last ------------------------------------------------------------

std::string lastPickedFolder() {
	std::lock_guard<std::mutex> lock(pickedMutex());
	return pickedStorage();
}

void setLastPickedFolder(const std::string& path) {
	std::lock_guard<std::mutex> lock(pickedMutex());
	pickedStorage() = path;
}

// ---- Writing files without overwriting ---------------------------------------------------------

bool writeNewFile(const std::string& folder, const std::string& stem, const std::string& ext,
	const unsigned char* data, std::size_t size, std::string& writtenPath, std::string& error) {
	writtenPath.clear();
	if (folder.empty()) {
		error = "empty folder name";
		return false;
	}
	for (int n = 1; n <= kMaxNameTries; n++) {
		std::string path = joinPath(folder, numberedName(stem, ext, n));
		int fd = openNewFile(path);
		if (fd < 0) {
			if (errno == EEXIST)
				continue;
			error = errnoText(errno);
			return false;
		}
		int failure = 0;
		const unsigned char* cursor = data;
		std::size_t left = size;
		while (left > 0) {
			long wrote = writeSome(fd, cursor, left);
			if (wrote < 0) {
				if (errno == EINTR)
					continue;
				failure = errno;
				break;
			}
			cursor += wrote;
			left -= static_cast<std::size_t>(wrote);
		}
		// A write error can also show up only when the file is closed (network folders).
		if (closeFile(fd) != 0 && failure == 0)
			failure = errno;
		if (failure != 0) {
			removeFile(path);
			error = errnoText(failure);
			return false;
		}
		writtenPath = path;
		return true;
	}
	error = "no free file name";
	return false;
}

std::string reserveUniquePath(const std::string& folder, const std::string& stem, const std::string& ext) {
	if (folder.empty())
		return std::string();
	std::lock_guard<std::mutex> lock(reservedMutex());
	std::set<std::string>& reserved = reservedStorage();
	for (int n = 1; n <= kMaxNameTries; n++) {
		std::string path = joinPath(folder, numberedName(stem, ext, n));
		if (reserved.count(path) != 0 || pathExists(path))
			continue;
		reserved.insert(path);
		return path;
	}
	return std::string();
}

void releasePath(const std::string& path) {
	std::lock_guard<std::mutex> lock(reservedMutex());
	reservedStorage().erase(path);
}

bool chooseNewFilePath(const std::string& chosenFolder, const std::string& stem, const std::string& ext,
	std::string& path, std::string& error) {
	path.clear();
	std::string folder;
	if (!prepareFolder(chosenFolder, folder, error))
		return false;
	path = reserveUniquePath(folder, stem, ext);
	if (path.empty()) {
		error = "no free file name in " + folder;
		return false;
	}
	return true;
}

// ---- Screenshot worker -------------------------------------------------------------------------

ShotWriter::~ShotWriter() {
	{
		std::lock_guard<std::mutex> lock(mutex);
		stopping = true;
	}
	wake.notify_all();
	if (worker.joinable())
		worker.join();
}

bool ShotWriter::canAccept() {
	std::lock_guard<std::mutex> lock(mutex);
	return !stopping && jobs.size() < kMaxQueued;
}

bool ShotWriter::submit(ShotJob&& job) {
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (stopping || jobs.size() >= kMaxQueued)
			return false;
		if (!worker.joinable()) {
			try {
				worker = std::thread(&ShotWriter::run, this);
			}
			catch (...) {
				return false;
			}
		}
		jobs.push_back(std::move(job));
	}
	wake.notify_one();
	return true;
}

bool ShotWriter::pollResult(ShotResult& result) {
	std::lock_guard<std::mutex> lock(mutex);
	if (results.empty())
		return false;
	result = std::move(results.front());
	results.pop_front();
	return true;
}

void ShotWriter::waitIdle() {
	std::unique_lock<std::mutex> lock(mutex);
	idle.wait(lock, [this] { return jobs.empty() && !busy; });
}

void ShotWriter::run() {
	for (;;) {
		ShotJob job;
		{
			std::unique_lock<std::mutex> lock(mutex);
			wake.wait(lock, [this] { return !jobs.empty() || stopping; });
			if (jobs.empty())
				return;  // stopping, and everything queued has been written
			job = std::move(jobs.front());
			jobs.pop_front();
			busy = true;
		}
		ShotResult result = processJob(job);
		{
			std::lock_guard<std::mutex> lock(mutex);
			results.push_back(std::move(result));
			while (results.size() > kMaxResults)
				results.pop_front();
			busy = false;
		}
		idle.notify_all();
	}
}

}  // namespace display_capture
