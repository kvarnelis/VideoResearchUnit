#include "video_player_core.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace video_player {

namespace {

bool isTrimChar(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string trimmed(const std::string& text) {
	std::size_t begin = 0;
	std::size_t end = text.size();
	while (begin < end && isTrimChar(text[begin])) {
		++begin;
	}
	while (end > begin && isTrimChar(text[end - 1])) {
		--end;
	}
	return text.substr(begin, end - begin);
}

char lowerAscii(char c) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string lowerCase(std::string text) {
	for (char& c : text) {
		c = lowerAscii(c);
	}
	return text;
}

// ASCII case-insensitive prefix test. The prefix must already be lower-case.
bool startsWithIgnoreCase(const std::string& text, const std::string& prefix) {
	return text.size() >= prefix.size() && lowerCase(text.substr(0, prefix.size())) == prefix;
}

bool startsWith(const std::string& text, const std::string& prefix) {
	return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// Splits on `sep` and keeps only the non-empty pieces.
std::vector<std::string> splitNonEmpty(const std::string& text, char sep) {
	std::vector<std::string> pieces;
	std::size_t begin = 0;
	while (begin <= text.size()) {
		std::size_t end = text.find(sep, begin);
		if (end == std::string::npos) {
			end = text.size();
		}
		if (end > begin) {
			pieces.push_back(text.substr(begin, end - begin));
		}
		begin = end + 1;
	}
	return pieces;
}

// The text from `start` up to the next '/' or the end. `start` must not pass the end of `text`.
std::string segmentFrom(const std::string& text, std::size_t start) {
	const std::size_t end = text.find('/', start);
	return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// Value of the first pair named `key` in a query string, or "" when no pair has that name.
std::string queryValue(const std::string& query, const std::string& key) {
	for (const std::string& pair : splitNonEmpty(query, '&')) {
		const std::size_t eq = pair.find('=');
		if (pair.substr(0, eq) == key) {
			return eq == std::string::npos ? std::string() : pair.substr(eq + 1);
		}
	}
	return std::string();
}

bool isVideoId(const std::string& id) {
	if (id.size() != 11) {
		return false;
	}
	for (char c : id) {
		const bool alphanumeric = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (!alphanumeric && c != '_' && c != '-') {
			return false;
		}
	}
	return true;
}

std::string valueOr(const KeyValues& values, const std::string& key) {
	const auto found = values.find(key);
	return found == values.end() ? std::string() : found->second;
}

}  // namespace

SourceClass classifySource(const std::string& input) {
	const std::string text = trimmed(input);
	if (text.empty()) {
		return SourceClass::Empty;
	}
	const bool urlLike = text.find("://") != std::string::npos ||
		startsWithIgnoreCase(text, "youtu.be/") || startsWithIgnoreCase(text, "youtube.com/") ||
		startsWithIgnoreCase(text, "www.youtube.com/") || startsWithIgnoreCase(text, "m.youtube.com/") ||
		startsWithIgnoreCase(text, "music.youtube.com/");
	if (urlLike) {
		return youtubeVideoId(text).empty() ? SourceClass::UnsupportedUrl : SourceClass::YouTube;
	}
	const std::string extension = fileExtension(text);
	if (extension == "mkv" || extension == "webm") {
		return SourceClass::UnsupportedContainer;
	}
	return SourceClass::LocalFile;
}

std::string fileExtension(const std::string& path) {
	const std::size_t slash = path.find_last_of('/');
	const std::size_t nameStart = slash == std::string::npos ? 0 : slash + 1;
	const std::size_t dot = path.find_last_of('.');
	// A dot in a folder name, or as the first character of the file name (".hidden"), is no extension.
	if (dot == std::string::npos || dot <= nameStart) {
		return std::string();
	}
	return lowerCase(path.substr(dot + 1));
}

std::string youtubeVideoId(const std::string& url) {
	std::string rest = url;
	if (startsWithIgnoreCase(rest, "https://")) {
		rest.erase(0, 8);
	} else if (startsWithIgnoreCase(rest, "http://")) {
		rest.erase(0, 7);
	}

	const std::size_t hostEnd = rest.find_first_of("/?#");
	const std::string host = lowerCase(hostEnd == std::string::npos ? rest : rest.substr(0, hostEnd));
	const bool knownHost = host == "youtube.com" || host == "www.youtube.com" || host == "m.youtube.com" ||
		host == "music.youtube.com" || host == "youtu.be";
	if (!knownHost) {
		return std::string();
	}

	const std::string afterHost = hostEnd == std::string::npos ? std::string() : rest.substr(hostEnd);
	const std::size_t pathEnd = afterHost.find_first_of("?#");
	const std::string path = afterHost.substr(0, pathEnd);
	std::string query;
	if (pathEnd != std::string::npos && afterHost[pathEnd] == '?') {
		const std::size_t queryEnd = afterHost.find('#', pathEnd + 1);
		query = afterHost.substr(pathEnd + 1,
			queryEnd == std::string::npos ? std::string::npos : queryEnd - pathEnd - 1);
	}

	std::string candidate;
	if (host == "youtu.be") {
		candidate = path.empty() ? std::string() : segmentFrom(path, 1);
	} else if (startsWith(path, "/watch")) {
		candidate = queryValue(query, "v");
	} else {
		const char* const pathPrefixes[] = {"/shorts/", "/embed/", "/live/", "/v/"};
		for (const char* prefix : pathPrefixes) {
			if (startsWith(path, prefix)) {
				candidate = segmentFrom(path, std::string(prefix).size());
				break;
			}
		}
	}
	return isVideoId(candidate) ? candidate : std::string();
}

std::string youtubeWatchUrl(const std::string& videoId) {
	return "https://www.youtube.com/watch?v=" + videoId;
}

std::string cachePathForId(const std::string& cacheDir, const std::string& videoId) {
	if (cacheDir.empty()) {
		return videoId + ".mp4";
	}
	if (cacheDir.back() == '/') {
		return cacheDir + videoId + ".mp4";
	}
	return cacheDir + "/" + videoId + ".mp4";
}

std::vector<std::string> ytDlpSearchDirs(const std::string& pathEnv) {
	std::vector<std::string> dirs = {"/opt/homebrew/bin", "/usr/local/bin"};
	for (const std::string& dir : splitNonEmpty(pathEnv, ':')) {
		dirs.push_back(dir);
	}
	return dirs;
}

std::string findYtDlp(const std::vector<std::string>& dirs) {
	for (const std::string& dir : dirs) {
		const std::string candidate = dir + "/yt-dlp";
		struct stat info;
		if (stat(candidate.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
			access(candidate.c_str(), X_OK) == 0) {
			return candidate;
		}
	}
	return std::string();
}

std::vector<std::string> buildYtDlpArgs(const std::string& ytDlpPath, const std::string& videoId,
	const std::string& outputTemplate) {
	if (!isVideoId(videoId)) {
		return std::vector<std::string>();
	}
	return {ytDlpPath,
		"--ignore-config",
		"--no-playlist",
		"--no-progress",
		"-f",
		"best[ext=mp4][vcodec^=avc1][height<=1080]",
		"-o",
		outputTemplate,
		youtubeWatchUrl(videoId)};
}

TransportStep transportStep(TransportState state, TransportEvent event, bool loopOn) {
	const TransportStep unchanged{state, false, false};
	switch (event) {
	case TransportEvent::SourceRequested:
		return {TransportState::Loading, false, false};
	case TransportEvent::Loaded:
		return state == TransportState::Loading ? TransportStep{TransportState::Ready, false, false} : unchanged;
	case TransportEvent::LoadFailed:
		return state == TransportState::Loading ? TransportStep{TransportState::Error, false, false} : unchanged;
	case TransportEvent::PlayToggle:
		switch (state) {
		case TransportState::Ready:
		case TransportState::Paused:
			return {TransportState::Playing, false, false};
		case TransportState::Playing:
			return {TransportState::Paused, false, false};
		case TransportState::Ended:
			return {TransportState::Playing, true, false};
		default:
			return unchanged;
		}
	case TransportEvent::Stop:
		switch (state) {
		case TransportState::Ready:
		case TransportState::Playing:
		case TransportState::Paused:
		case TransportState::Ended:
			return {TransportState::Ready, true, false};
		default:
			return unchanged;
		}
	case TransportEvent::EndOfClip:
		if (state == TransportState::Playing) {
			return loopOn ? TransportStep{TransportState::Playing, true, true}
				: TransportStep{TransportState::Ended, false, true};
		}
		return unchanged;
	}
	return unchanged;
}

std::string formatClock(double seconds) {
	if (!std::isfinite(seconds) || seconds < 0.0) {
		return "0:00";
	}
	// fmod is exact for doubles, so this holds for any finite input without 64-bit overflow.
	const double whole = std::floor(seconds);
	const double withinHour = std::fmod(whole, 3600.0);
	const double hours = (whole - withinHour) / 3600.0;
	const int minutes = static_cast<int>(withinHour) / 60;
	const int secs = static_cast<int>(withinHour) % 60;
	char buffer[400];
	if (hours >= 1.0) {
		std::snprintf(buffer, sizeof(buffer), "%.0f:%02d:%02d", hours, minutes, secs);
	} else {
		std::snprintf(buffer, sizeof(buffer), "%d:%02d", minutes, secs);
	}
	return buffer;
}

std::string msgFileNotFound(const std::string& path) {
	return "File not found: " + path;
}

std::string msgUnsupportedContainer(const std::string& /*extension*/) {
	return "MKV and WebM files can't be played here. Convert the file to MP4 or MOV.";
}

std::string msgUnsupportedUrl() {
	return "Only YouTube links can be used. Paste a youtube.com or youtu.be link.";
}

std::string msgYtDlpMissing() {
	return "yt-dlp is not installed, so YouTube links cannot be downloaded. "
		"Install yt-dlp, then paste the link again. "
		"Video Research Unit looks for it in /opt/homebrew/bin, /usr/local/bin and your PATH.";
}

std::string msgDecodeFailed(const std::string& path) {
	return "Could not open " + path + ". The file may be damaged or use a format macOS cannot play.";
}

std::string msgNotMacOs() {
	return "Video playback needs macOS.";
}

std::string msgYouTubeDownloading(int seconds) {
	return "Downloading from YouTube... " + std::to_string(seconds < 0 ? 0 : seconds) + " s";
}

std::string msgYouTubeDownloadFailed(const std::string& detail) {
	std::string reason = trimmed(detail);
	constexpr std::size_t maxReason = 160;
	if (reason.size() > maxReason) {
		reason.resize(maxReason - 3);
		reason += "...";
	}
	std::string message = "YouTube download failed.";
	if (!reason.empty()) {
		message += " yt-dlp said: " + reason;
	}
	return message;
}

std::string msgYouTubeNoMp4() {
	return "YouTube has no single-file MP4 (H.264) version of this video, so it cannot be played here. "
		   "Try another video.";
}

KeyValues settingsToKeyValues(const Settings& settings) {
	std::string source = "none";
	if (settings.source == SourceKind::File) {
		source = "file";
	} else if (settings.source == SourceKind::YouTube) {
		source = "youtube";
	}
	KeyValues values;
	values["source"] = source;
	values["filePath"] = settings.filePath;
	values["youtubeUrl"] = settings.youtubeUrl;
	return values;
}

Settings settingsFromKeyValues(const KeyValues& values) {
	Settings settings;
	const std::string source = valueOr(values, "source");
	if (source == "file") {
		settings.source = SourceKind::File;
	} else if (source == "youtube") {
		settings.source = SourceKind::YouTube;
	}
	settings.filePath = valueOr(values, "filePath");
	settings.youtubeUrl = valueOr(values, "youtubeUrl");
	return settings;
}

}  // namespace video_player
