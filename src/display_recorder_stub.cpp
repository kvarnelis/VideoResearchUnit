#if !defined(__APPLE__)

#include "display_recorder.hpp"

#include <memory>
#include <string>

namespace display_recorder {
namespace {

// Used on platforms without AVFoundation. Every call reports that recording is unavailable.
class StubFrameSink final : public FrameSink {
public:
	bool open(const std::string&, int, int, int) override {
		errorText = "macOS only";
		return false;
	}

	bool readyForFrame() override {
		return false;
	}

	bool append(const unsigned char*, int, double) override {
		errorText = "macOS only";
		return false;
	}

	bool finish(double) override {
		errorText = "macOS only";
		return false;
	}

	void cancel() override {
	}

	std::string lastError() const override {
		return errorText;
	}

private:
	std::string errorText;
};

}  // namespace

bool platformSupportsRecording() {
	return false;
}

std::unique_ptr<FrameSink> createFrameSink() {
	return std::make_unique<StubFrameSink>();
}

}  // namespace display_recorder

#endif  // !defined(__APPLE__)
