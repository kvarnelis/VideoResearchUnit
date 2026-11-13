// Offline checks for the Video Player core. Standalone, with no framework and no Rack SDK.
// make test builds this with src/video_player_core.cpp and runs it. Each failure prints one line,
// then the summary "video_player_test: N checks, F failed". Exits 0 only when F is 0.

#include "video_player_core.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

using namespace video_player;

namespace {

int checks = 0;
int failures = 0;

void check(bool ok, const char* what, int line) {
	++checks;
	if (!ok) {
		++failures;
		std::printf("  FAIL line %d: %s\n", line, what);
	}
}

void checkText(const std::string& actual, const std::string& expected, const char* what, int line) {
	++checks;
	if (actual != expected) {
		++failures;
		std::printf("  FAIL line %d: %s is \"%s\", expected \"%s\"\n", line, what, actual.c_str(), expected.c_str());
	}
}

void checkContains(const std::string& text, const std::string& needle, const char* what, int line) {
	++checks;
	if (text.find(needle) == std::string::npos) {
		++failures;
		std::printf("  FAIL line %d: %s does not contain \"%s\": \"%s\"\n", line, what, needle.c_str(), text.c_str());
	}
}

const char* stateName(TransportState state) {
	switch (state) {
	case TransportState::NoSource:
		return "NoSource";
	case TransportState::Loading:
		return "Loading";
	case TransportState::Ready:
		return "Ready";
	case TransportState::Playing:
		return "Playing";
	case TransportState::Paused:
		return "Paused";
	case TransportState::Ended:
		return "Ended";
	case TransportState::Error:
		return "Error";
	}
	return "?";
}

// One transition. The result must match next, rewind and pulse exactly.
void checkStep(const char* label, TransportState state, TransportEvent event, bool loopOn, TransportState next,
	bool rewind, bool pulse, int line) {
	const TransportStep step = transportStep(state, event, loopOn);
	++checks;
	if (step.next != next || step.rewindToFirstFrame != rewind || step.endOfClipPulse != pulse) {
		++failures;
		std::printf("  FAIL line %d: %s gave %s rewind %d pulse %d, expected %s rewind %d pulse %d\n", line, label,
			stateName(step.next), step.rewindToFirstFrame ? 1 : 0, step.endOfClipPulse ? 1 : 0, stateName(next),
			rewind ? 1 : 0, pulse ? 1 : 0);
	}
}

// True when the event leaves the state alone, with no rewind and no pulse.
bool isUnchanged(TransportState state, TransportEvent event, bool loopOn) {
	const TransportStep step = transportStep(state, event, loopOn);
	return step.next == state && !step.rewindToFirstFrame && !step.endOfClipPulse;
}

// Creates an empty file, then sets its permission bits.
bool makeFile(const std::string& path, mode_t mode) {
	{
		std::ofstream out(path);
		if (!out) {
			return false;
		}
	}
	return chmod(path.c_str(), mode) == 0;
}

const TransportState kAllStates[] = {TransportState::NoSource, TransportState::Loading, TransportState::Ready,
	TransportState::Playing, TransportState::Paused, TransportState::Ended, TransportState::Error};

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_TEXT(actual, expected) checkText((actual), (expected), #actual, __LINE__)
#define CHECK_CONTAINS(text, needle) checkContains((text), (needle), #text, __LINE__)
#define CHECK_STEP(state, event, loopOn, next, rewind, pulse) \
	checkStep(#state ", " #event ", loop " #loopOn, (state), (event), (loopOn), (next), (rewind), (pulse), __LINE__)

}  // namespace

int main() {
	// 1. classifySource
	CHECK(classifySource("") == SourceClass::Empty);
	CHECK(classifySource("   ") == SourceClass::Empty);
	CHECK(classifySource("clip.mp4") == SourceClass::LocalFile);
	CHECK(classifySource(" \tclip.mp4\n") == SourceClass::LocalFile);
	CHECK(classifySource("/Users/x/Movies/a.MKV") == SourceClass::UnsupportedContainer);
	CHECK(classifySource("b.WebM") == SourceClass::UnsupportedContainer);
	CHECK(classifySource("https://example.com/v.mp4") == SourceClass::UnsupportedUrl);
	CHECK(classifySource("https://youtu.be/VRUtest0000") == SourceClass::YouTube);
	CHECK(classifySource("youtube.com/watch?v=VRUtest0000") == SourceClass::YouTube);
	CHECK(classifySource("https://www.youtube.com/watch?v=VRUtest0000") == SourceClass::YouTube);
	CHECK(classifySource("music.youtube.com/watch?v=VRUtest0000") == SourceClass::YouTube);
	CHECK(classifySource("https://www.youtube.com/watch?v=short") == SourceClass::UnsupportedUrl);

	// 2. fileExtension
	CHECK_TEXT(fileExtension("a/b.MP4"), "mp4");
	CHECK_TEXT(fileExtension("noext"), "");
	CHECK_TEXT(fileExtension(".hidden"), "");
	CHECK_TEXT(fileExtension("dir.d/file"), "");

	// 3. youtubeVideoId: accepted links
	CHECK_TEXT(youtubeVideoId("https://www.youtube.com/watch?v=VRUtest0000&t=10s"), "VRUtest0000");
	CHECK_TEXT(youtubeVideoId("https://youtu.be/VRUtest0000?si=abc"), "VRUtest0000");
	CHECK_TEXT(youtubeVideoId("https://www.youtube.com/shorts/VRUtest0000"), "VRUtest0000");
	CHECK_TEXT(youtubeVideoId("https://m.youtube.com/embed/VRUtest0000"), "VRUtest0000");
	// 3. youtubeVideoId: rejected links
	CHECK_TEXT(youtubeVideoId("https://evil.example/watch?v=VRUtest0000"), "");
	CHECK_TEXT(youtubeVideoId("https://youtube.com.evil.example/watch?v=VRUtest0000"), "");
	CHECK_TEXT(youtubeVideoId("https://www.youtube.com/watch?v=VRUtest000"), "");
	CHECK_TEXT(youtubeVideoId("https://www.youtube.com/watch?v=VRUtest0000X"), "");
	CHECK_TEXT(youtubeVideoId("https://www.youtube.com/watch?v=VRUtest00!0"), "");
	CHECK_TEXT(youtubeVideoId("https://www.youtube.com/playlist?list=PL123"), "");

	// 4. Watch URL and cache path
	CHECK_TEXT(youtubeWatchUrl("VRUtest0000"), "https://www.youtube.com/watch?v=VRUtest0000");
	CHECK_TEXT(cachePathForId("/cache", "VRUtest0000"), "/cache/VRUtest0000.mp4");
	CHECK_TEXT(cachePathForId("/cache/", "VRUtest0000"), "/cache/VRUtest0000.mp4");
	CHECK_TEXT(cachePathForId("", "VRUtest0000"), "VRUtest0000.mp4");

	// 5. yt-dlp search directories
	const std::vector<std::string> expectedDirs = {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin", "/x"};
	CHECK(ytDlpSearchDirs("/usr/bin:/bin::/x") == expectedDirs);

	// 6. findYtDlp on fixture folders under build/. Only the executable file counts.
	namespace fs = std::filesystem;
	const std::string root = "build/video_player_test_tmp";
	std::error_code ec;
	(void)fs::remove_all(root, ec);
	CHECK(fs::create_directories(root + "/a", ec));
	CHECK(fs::create_directories(root + "/b", ec));
	CHECK(fs::create_directories(root + "/c", ec));
	CHECK(fs::create_directories(root + "/d/yt-dlp", ec));
	CHECK(makeFile(root + "/a/yt-dlp", 0644));
	CHECK(makeFile(root + "/b/yt-dlp", 0755));
	CHECK(makeFile(root + "/c/yt-dlp", 0755));
	const std::string dirA = root + "/a";
	const std::string dirB = root + "/b";
	const std::string dirC = root + "/c";
	const std::string dirD = root + "/d";
	const std::string dirMissing = root + "/missing";
	const std::vector<std::string> aBC = {dirA, dirB, dirC};
	const std::vector<std::string> onlyA = {dirA};
	const std::vector<std::string> cThenB = {dirC, dirB};
	const std::vector<std::string> onlyD = {dirD};
	const std::vector<std::string> onlyMissing = {dirMissing};
	CHECK_TEXT(findYtDlp(aBC), dirB + "/yt-dlp");
	CHECK_TEXT(findYtDlp(onlyA), "");
	CHECK_TEXT(findYtDlp(cThenB), dirC + "/yt-dlp");
	CHECK_TEXT(findYtDlp(onlyD), "");
	CHECK_TEXT(findYtDlp(onlyMissing), "");

	// 7. yt-dlp argument list
	const std::vector<std::string> expectedArgs = {"/x/yt-dlp", "--ignore-config", "--no-playlist",
		"--no-progress", "-f", "best[ext=mp4][vcodec^=avc1][height<=1080]", "-o", "/cache/%(id)s.%(ext)s",
		"https://www.youtube.com/watch?v=VRUtest0000"};
	CHECK(buildYtDlpArgs("/x/yt-dlp", "VRUtest0000", "/cache/%(id)s.%(ext)s") == expectedArgs);
	CHECK(buildYtDlpArgs("/x/yt-dlp", "bad id", "/cache/%(id)s.%(ext)s").empty());

	// 8. transportStep, one check per rule
	for (TransportState s : kAllStates) {
		checkStep("SourceRequested", s, TransportEvent::SourceRequested, false, TransportState::Loading, false, false,
			__LINE__);
	}
	CHECK_STEP(TransportState::Loading, TransportEvent::Loaded, false, TransportState::Ready, false, false);
	CHECK_STEP(TransportState::Loading, TransportEvent::LoadFailed, false, TransportState::Error, false, false);
	for (TransportState s : kAllStates) {
		if (s != TransportState::Loading) {
			CHECK(isUnchanged(s, TransportEvent::Loaded, false));
			CHECK(isUnchanged(s, TransportEvent::LoadFailed, false));
		}
	}
	CHECK_STEP(TransportState::Ready, TransportEvent::PlayToggle, false, TransportState::Playing, false, false);
	CHECK_STEP(TransportState::Paused, TransportEvent::PlayToggle, false, TransportState::Playing, false, false);
	CHECK_STEP(TransportState::Playing, TransportEvent::PlayToggle, false, TransportState::Paused, false, false);
	CHECK_STEP(TransportState::Ended, TransportEvent::PlayToggle, false, TransportState::Playing, true, false);
	CHECK(isUnchanged(TransportState::NoSource, TransportEvent::PlayToggle, false));
	CHECK(isUnchanged(TransportState::Loading, TransportEvent::PlayToggle, false));
	CHECK(isUnchanged(TransportState::Error, TransportEvent::PlayToggle, false));
	CHECK_STEP(TransportState::Ready, TransportEvent::Stop, false, TransportState::Ready, true, false);
	CHECK_STEP(TransportState::Playing, TransportEvent::Stop, false, TransportState::Ready, true, false);
	CHECK_STEP(TransportState::Paused, TransportEvent::Stop, false, TransportState::Ready, true, false);
	CHECK_STEP(TransportState::Ended, TransportEvent::Stop, false, TransportState::Ready, true, false);
	CHECK(isUnchanged(TransportState::NoSource, TransportEvent::Stop, false));
	CHECK(isUnchanged(TransportState::Loading, TransportEvent::Stop, false));
	CHECK(isUnchanged(TransportState::Error, TransportEvent::Stop, false));
	CHECK_STEP(TransportState::Playing, TransportEvent::EndOfClip, false, TransportState::Ended, false, true);
	CHECK_STEP(TransportState::Playing, TransportEvent::EndOfClip, true, TransportState::Playing, true, true);
	for (TransportState s : kAllStates) {
		if (s != TransportState::Playing) {
			CHECK(isUnchanged(s, TransportEvent::EndOfClip, false));
			CHECK(isUnchanged(s, TransportEvent::EndOfClip, true));
		}
	}

	// 9. End-of-clip sequence
	CHECK_STEP(TransportState::Playing, TransportEvent::EndOfClip, false, TransportState::Ended, false, true);
	CHECK(isUnchanged(TransportState::Ended, TransportEvent::EndOfClip, false));
	CHECK_STEP(TransportState::Ended, TransportEvent::PlayToggle, false, TransportState::Playing, true, false);
	CHECK_STEP(TransportState::Playing, TransportEvent::EndOfClip, true, TransportState::Playing, true, true);
	CHECK_STEP(TransportState::Paused, TransportEvent::EndOfClip, false, TransportState::Paused, false, false);

	// 10. Stop
	CHECK_STEP(TransportState::Playing, TransportEvent::Stop, false, TransportState::Ready, true, false);
	CHECK_STEP(TransportState::Paused, TransportEvent::Stop, false, TransportState::Ready, true, false);
	CHECK_STEP(TransportState::Ready, TransportEvent::Stop, false, TransportState::Ready, true, false);
	CHECK_STEP(TransportState::Ended, TransportEvent::Stop, false, TransportState::Ready, true, false);
	CHECK_STEP(TransportState::NoSource, TransportEvent::Stop, false, TransportState::NoSource, false, false);

	// 11. formatClock
	CHECK_TEXT(formatClock(0), "0:00");
	CHECK_TEXT(formatClock(5.9), "0:05");
	CHECK_TEXT(formatClock(61), "1:01");
	CHECK_TEXT(formatClock(3599), "59:59");
	CHECK_TEXT(formatClock(3600), "1:00:00");
	CHECK_TEXT(formatClock(3725.9), "1:02:05");
	CHECK_TEXT(formatClock(-1), "0:00");
	CHECK_TEXT(formatClock(std::numeric_limits<double>::quiet_NaN()), "0:00");
	CHECK_TEXT(formatClock(std::numeric_limits<double>::infinity()), "0:00");

	// 12. Status messages
	CHECK_TEXT(msgFileNotFound("/Users/x/a.mp4"), "File not found: /Users/x/a.mp4");
	CHECK_CONTAINS(msgUnsupportedContainer("mkv"), "MKV");
	CHECK_CONTAINS(msgUnsupportedContainer("mkv"), "WebM");
	CHECK_CONTAINS(msgUnsupportedContainer("mkv"), "MP4");
	CHECK_TEXT(msgUnsupportedUrl(), "Only YouTube links can be used. Paste a youtube.com or youtu.be link.");
	CHECK_CONTAINS(msgYtDlpMissing(), "yt-dlp");
	CHECK_CONTAINS(msgYtDlpMissing(), "not installed");
	CHECK_CONTAINS(msgYtDlpMissing(), "cannot be downloaded");
	CHECK_CONTAINS(msgYtDlpMissing(), "paste the link again");
	CHECK_CONTAINS(msgYtDlpMissing(), "/opt/homebrew/bin");
	CHECK_CONTAINS(msgYtDlpMissing(), "/usr/local/bin");
	CHECK_CONTAINS(msgYtDlpMissing(), "PATH");
	CHECK_TEXT(msgDecodeFailed("/a/b.mp4"),
		"Could not open /a/b.mp4. The file may be damaged or use a format macOS cannot play.");
	CHECK_TEXT(msgNotMacOs(), "Video playback needs macOS.");
	CHECK_TEXT(msgYouTubeDownloading(0), "Downloading from YouTube... 0 s");
	CHECK_TEXT(msgYouTubeDownloading(12), "Downloading from YouTube... 12 s");
	CHECK_TEXT(msgYouTubeDownloading(-5), "Downloading from YouTube... 0 s");
	CHECK_TEXT(msgYouTubeDownloadFailed(""), "YouTube download failed.");
	CHECK_TEXT(msgYouTubeDownloadFailed("  \n"), "YouTube download failed.");
	CHECK_TEXT(msgYouTubeDownloadFailed("  ERROR: Video unavailable \n"),
		"YouTube download failed. yt-dlp said: ERROR: Video unavailable");
	const std::string longReason(500, 'x');
	const std::string longMessage = msgYouTubeDownloadFailed(longReason);
	CHECK_CONTAINS(longMessage, "yt-dlp said: ");
	CHECK_CONTAINS(longMessage, std::string(157, 'x') + "...");
	CHECK(longMessage.find(std::string(158, 'x')) == std::string::npos);
	const std::string exactReason(160, 'y');
	CHECK_CONTAINS(msgYouTubeDownloadFailed(exactReason), exactReason);
	CHECK(msgYouTubeDownloadFailed(exactReason).find("...") == std::string::npos);
	CHECK_CONTAINS(msgYouTubeNoMp4(), "MP4");
	CHECK_CONTAINS(msgYouTubeNoMp4(), "H.264");
	CHECK_CONTAINS(msgYouTubeNoMp4(), "Try another video.");

	// 13. Saved settings
	Settings original;
	original.source = SourceKind::YouTube;
	original.filePath = "/Users/x/Movies/a.mp4";
	original.youtubeUrl = "https://youtu.be/VRUtest0000";
	const KeyValues saved = settingsToKeyValues(original);
	CHECK(saved.size() == 3 && saved.count("source") == 1 && saved.count("filePath") == 1 &&
		saved.count("youtubeUrl") == 1);
	CHECK_TEXT(saved.at("source"), "youtube");
	CHECK_TEXT(saved.at("filePath"), "/Users/x/Movies/a.mp4");
	CHECK_TEXT(saved.at("youtubeUrl"), "https://youtu.be/VRUtest0000");
	const Settings restored = settingsFromKeyValues(saved);
	CHECK(restored.source == original.source && restored.filePath == original.filePath &&
		restored.youtubeUrl == original.youtubeUrl);
	Settings fileOnly;
	fileOnly.source = SourceKind::File;
	fileOnly.filePath = "/Users/x/a.mov";
	CHECK_TEXT(settingsToKeyValues(fileOnly).at("source"), "file");
	CHECK(settingsFromKeyValues(settingsToKeyValues(fileOnly)).source == SourceKind::File);
	CHECK_TEXT(settingsToKeyValues(Settings{}).at("source"), "none");
	CHECK(settingsFromKeyValues(settingsToKeyValues(Settings{})).source == SourceKind::None);
	const Settings missing = settingsFromKeyValues(KeyValues{});
	CHECK(missing.source == SourceKind::None);
	CHECK_TEXT(missing.filePath, "");
	CHECK_TEXT(missing.youtubeUrl, "");
	KeyValues unknown;
	unknown["source"] = "bogus";
	CHECK(settingsFromKeyValues(unknown).source == SourceKind::None);

	std::printf("video_player_test: %d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
