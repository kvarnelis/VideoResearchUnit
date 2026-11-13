#pragma once

#include <map>
#include <string>
#include <vector>

// Video Player core. Pure C++17 with no Rack or Apple dependency, so the offline checks in
// tests/video_player_test.cpp run under `make test`. Covers source sorting, the yt-dlp lookup
// and argument list, the transport state machine, status messages and saved settings.
//
// The core never runs yt-dlp. YouTube downloads take their video ID from youtubeVideoId(), so
// raw user text never reaches the yt-dlp argument list.

namespace video_player {

enum class SourceClass { Empty, LocalFile, YouTube, UnsupportedContainer, UnsupportedUrl };

// Sorts pasted or saved source text. Spaces, tabs and newlines are trimmed from both ends first.
SourceClass classifySource(const std::string& input);

// Lower-cased extension after the last '.' in the final path component, or "" when there is none.
std::string fileExtension(const std::string& path);

// The 11-character video ID of an accepted YouTube link, or "" for anything else.
std::string youtubeVideoId(const std::string& url);

std::string youtubeWatchUrl(const std::string& videoId);

// Cache file for a video ID: dir + "/" + id + ".mp4", with no doubled slash when dir ends in '/'.
std::string cachePathForId(const std::string& cacheDir, const std::string& videoId);

// Directories to search for yt-dlp, in order: Homebrew, /usr/local/bin, then the PATH entries.
std::vector<std::string> ytDlpSearchDirs(const std::string& pathEnv);

// The first dir + "/yt-dlp" that is a regular file and executable, or "" when none is.
std::string findYtDlp(const std::vector<std::string>& dirs);

// The exact yt-dlp command line. videoId must come from youtubeVideoId(); an ID that is not
// 11 valid characters gives an empty vector.
std::vector<std::string> buildYtDlpArgs(const std::string& ytDlpPath, const std::string& videoId,
	const std::string& outputTemplate);

enum class TransportState { NoSource, Loading, Ready, Playing, Paused, Ended, Error };
enum class TransportEvent { SourceRequested, Loaded, LoadFailed, PlayToggle, Stop, EndOfClip };

struct TransportStep {
	TransportState next;
	bool rewindToFirstFrame;
	bool endOfClipPulse;
};

// Pure transition for the playback controls. loopOn only matters for EndOfClip while Playing.
TransportStep transportStep(TransportState state, TransportEvent event, bool loopOn);

// "m:ss", or "h:mm:ss" from one hour up. Non-finite and negative input gives "0:00".
std::string formatClock(double seconds);

// Status messages for the panel, in plain words.
std::string msgFileNotFound(const std::string& path);
std::string msgUnsupportedContainer(const std::string& extension);
std::string msgUnsupportedUrl();
std::string msgYtDlpMissing();
std::string msgDecodeFailed(const std::string& path);
std::string msgNotMacOs();
// "Downloading from YouTube... 12 s". Seconds below zero count as zero.
std::string msgYouTubeDownloading(int seconds);
// "YouTube download failed." and, when yt-dlp left a reason, " yt-dlp said: <reason>". The reason
// is trimmed and cut to 160 characters so the panel can show it.
std::string msgYouTubeDownloadFailed(const std::string& detail);
// Shown when yt-dlp reports that the video has no single-file H.264 MP4 version to download.
std::string msgYouTubeNoMp4();

enum class SourceKind { None, File, YouTube };

struct Settings {
	SourceKind source = SourceKind::None;
	std::string filePath;
	std::string youtubeUrl;
};

using KeyValues = std::map<std::string, std::string>;

// Saved-settings keys: "source" (value "none", "file" or "youtube"), "filePath" and "youtubeUrl".
KeyValues settingsToKeyValues(const Settings& settings);

// Missing keys give defaults. An unknown source value gives SourceKind::None.
Settings settingsFromKeyValues(const KeyValues& values);

}  // namespace video_player
