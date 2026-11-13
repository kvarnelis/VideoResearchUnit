#if !defined(__APPLE__)

#include "video_player_decoder.hpp"

#include <memory>
#include <string>

namespace video_player {
namespace {

// Used on platforms without AVFoundation. Every call reports that playback is unavailable.
class StubFrameSource final : public FrameSource {
public:
	bool open(const std::string&, int) override {
		errorText = "macOS only";
		return false;
	}

	bool nextFrame(DecodedFrame&) override {
		errorText = "macOS only";
		return false;
	}

	bool rewind() override {
		errorText = "macOS only";
		return false;
	}

	bool seek(double) override {
		errorText = "macOS only";
		return false;
	}

	double durationSeconds() const override {
		return 0.0;
	}

	int width() const override {
		return 0;
	}

	int height() const override {
		return 0;
	}

	std::string lastError() const override {
		return errorText;
	}

private:
	std::string errorText;
};

}  // namespace

bool platformSupportsPlayback() {
	return false;
}

std::unique_ptr<FrameSource> createFrameSource() {
	return std::make_unique<StubFrameSource>();
}

}  // namespace video_player

#endif  // !defined(__APPLE__)
