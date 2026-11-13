#if defined(__APPLE__)

#include "video_player_decoder.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <string>

namespace video_player {
namespace {

// A seek starts the reader this far before the wanted time, so a frame sitting exactly on it is kept.
constexpr double kSeekSlackSeconds = 1e-4;
// readFrame() drops a frame that is earlier than the wanted time by more than this. The reader starts
// at an earlier key frame, and its first sample is the frame showing at the start of the range, with
// its time set to that start: the slack before the wanted time, give or take one timescale tick. Half
// the slack always drops that sample and always keeps a real frame sitting on the wanted time.
constexpr double kSeekDropMarginSeconds = kSeekSlackSeconds / 2.0;
// Timescale for the reader's start time. Fine enough that rounding never moves it by a visible amount.
constexpr int kSeekTimescale = 90000;

std::string stdStringFromNSString(NSString* value) {
	if (!value) {
		return std::string();
	}
	const char* utf8 = [value UTF8String];
	return utf8 ? std::string(utf8) : std::string();
}

// Readable text for an NSError. Never empty.
std::string describeError(NSError* error) {
	const std::string text = error ? stdStringFromNSString([error localizedDescription]) : std::string();
	return text.empty() ? std::string("unknown AVFoundation error") : text;
}

// Clockwise display rotation (0, 90, 180 or 270) from a track's preferredTransform.
// A matrix that matches none of the quarter turns counts as 0.
int rotationDegrees(const CGAffineTransform& transform) {
	// Tolerance for float noise in the matrix; the quarter-turn values are exact in practice.
	const double eps = 1e-3;
	const bool bIsZero = std::fabs(transform.b) < eps;
	const bool cIsZero = std::fabs(transform.c) < eps;
	if (transform.a > eps && transform.d > eps && bIsZero && cIsZero) {
		return 0;
	}
	if (transform.b > eps && transform.c < -eps) {
		return 90;
	}
	if (transform.a < -eps && transform.d < -eps) {
		return 180;
	}
	if (transform.b < -eps && transform.c > eps) {
		return 270;
	}
	return 0;
}

// Rounds to the nearest pixel count, then down to an even count of at least 2.
int evenPixelCount(double value) {
	const int rounded = std::max(static_cast<int>(std::round(value)), 2);
	return rounded - rounded % 2;
}

// Owns one retained CMSampleBufferRef from copyNextSampleBuffer and releases it on scope exit.
class RetainedSample {
public:
	explicit RetainedSample(CMSampleBufferRef sample) : sample_(sample) {}
	~RetainedSample() {
		if (sample_) {
			CFRelease(sample_);
		}
	}
	RetainedSample(const RetainedSample&) = delete;
	RetainedSample& operator=(const RetainedSample&) = delete;
	CMSampleBufferRef get() const {
		return sample_;
	}

private:
	CMSampleBufferRef sample_;
};

// Read-only lock on a pixel buffer that unlocks on scope exit.
class ReadOnlyPixelLock {
public:
	explicit ReadOnlyPixelLock(CVPixelBufferRef pixels)
		: pixels_(pixels), locked_(CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess) {}
	~ReadOnlyPixelLock() {
		if (locked_) {
			CVPixelBufferUnlockBaseAddress(pixels_, kCVPixelBufferLock_ReadOnly);
		}
	}
	ReadOnlyPixelLock(const ReadOnlyPixelLock&) = delete;
	ReadOnlyPixelLock& operator=(const ReadOnlyPixelLock&) = delete;
	bool locked() const {
		return locked_;
	}

private:
	CVPixelBufferRef pixels_;
	bool locked_;
};

}  // namespace

class AvFrameSource final : public FrameSource {
public:
	AvFrameSource() = default;
	~AvFrameSource() override {
		releaseResources();
	}
	AvFrameSource(const AvFrameSource&) = delete;
	AvFrameSource& operator=(const AvFrameSource&) = delete;

	bool open(const std::string& path, int maxLongSide) override {
		releaseResources();
		lastErrorText.clear();
		try {
			@try {
				if (openFile(path, maxLongSide)) {
					return true;
				}
			} @catch (NSException* exception) {
				lastErrorText = "AVFoundation exception: " + stdStringFromNSString([exception reason]);
			}
		} catch (const std::exception& error) {
			lastErrorText = std::string("decoder error: ") + error.what();
		} catch (...) {
			lastErrorText = "decoder error";
		}
		releaseResources();
		return false;
	}

	bool nextFrame(DecodedFrame& out) override {
		lastErrorText.clear();
		if (!output) {
			lastErrorText = "decoder is not open";
			return false;
		}
		try {
			@try {
				return readFrame(out);
			} @catch (NSException* exception) {
				lastErrorText = "AVFoundation exception: " + stdStringFromNSString([exception reason]);
			}
		} catch (const std::exception& error) {
			lastErrorText = std::string("decoder error: ") + error.what();
		} catch (...) {
			lastErrorText = "decoder error";
		}
		return false;
	}

	bool rewind() override {
		lastErrorText.clear();
		if (!asset || !track) {
			lastErrorText = "decoder is not open";
			return false;
		}
		try {
			@try {
				return startReading();
			} @catch (NSException* exception) {
				lastErrorText = "AVFoundation exception: " + stdStringFromNSString([exception reason]);
			}
		} catch (const std::exception& error) {
			lastErrorText = std::string("decoder error: ") + error.what();
		} catch (...) {
			lastErrorText = "decoder error";
		}
		releaseReader();
		return false;
	}

	bool seek(double seconds) override {
		lastErrorText.clear();
		if (!asset || !track) {
			lastErrorText = "decoder is not open";
			return false;
		}
		if (!std::isfinite(seconds)) {
			lastErrorText = "bad seek time";
			return false;
		}
		try {
			@try {
				return startReading(true, seconds);
			} @catch (NSException* exception) {
				lastErrorText = "AVFoundation exception: " + stdStringFromNSString([exception reason]);
			}
		} catch (const std::exception& error) {
			lastErrorText = std::string("decoder error: ") + error.what();
		} catch (...) {
			lastErrorText = "decoder error";
		}
		releaseReader();
		return false;
	}

	double durationSeconds() const override {
		return duration_;
	}

	int width() const override {
		return width_;
	}

	int height() const override {
		return height_;
	}

	std::string lastError() const override {
		return lastErrorText;
	}

private:
	// Loads the asset and its first video track, then starts reading. Returns false with lastErrorText set.
	bool openFile(const std::string& path, int maxLongSide) {
		@autoreleasepool {
			NSString* filePath = [NSString stringWithUTF8String:path.c_str()];
			BOOL isDirectory = NO;
			if (!filePath || ![[NSFileManager defaultManager] fileExistsAtPath:filePath isDirectory:&isDirectory] || isDirectory) {
				lastErrorText = "file not found";
				return false;
			}

			asset = [[AVURLAsset alloc] initWithURL:[NSURL fileURLWithPath:filePath] options:nil];
			// The synchronous tracksWithMediaType: is deprecated from macOS 15, so wait on the async loader.
			dispatch_semaphore_t loaded = dispatch_semaphore_create(0);
			__block NSArray* loadedTracks = nil;
			__block NSError* loadError = nil;
			[asset loadTracksWithMediaType:AVMediaTypeVideo
			             completionHandler:^(NSArray<AVAssetTrack*>* tracks, NSError* error) {
				loadedTracks = [tracks retain];
				loadError = [error retain];
				dispatch_semaphore_signal(loaded);
			}];
			dispatch_semaphore_wait(loaded, DISPATCH_TIME_FOREVER);
			dispatch_release(loaded);
			NSArray* videoTracks = [loadedTracks autorelease];
			NSError* loadFailure = [loadError autorelease];
			if (loadFailure) {
				lastErrorText = "cannot read tracks: " + describeError(loadFailure);
				return false;
			}
			if ([videoTracks count] == 0) {
				lastErrorText = "no video track";
				return false;
			}
			track = [[videoTracks objectAtIndex:0] retain];

			const CGSize natural = [track naturalSize];
			const bool usable = std::isfinite(natural.width) && std::isfinite(natural.height) &&
			                    natural.width >= 1.0 && natural.height >= 1.0;
			if (!usable) {
				lastErrorText = "video track has no usable size";
				return false;
			}
			rotation = rotationDegrees([track preferredTransform]);
			const bool quarterTurn = rotation == 90 || rotation == 270;

			// Scale on the display long side and never upscale. The raw (unrotated) size is what AVFoundation outputs.
			const double displayLong = std::max(natural.width, natural.height);
			const double limit = maxLongSide > 0 ? static_cast<double>(maxLongSide) : displayLong;
			const double scale = std::min(1.0, limit / displayLong);
			rawWidth = evenPixelCount(natural.width * scale);
			rawHeight = evenPixelCount(natural.height * scale);
			width_ = quarterTurn ? rawHeight : rawWidth;
			height_ = quarterTurn ? rawWidth : rawHeight;

			const double seconds = CMTimeGetSeconds([asset duration]);
			duration_ = std::isfinite(seconds) ? seconds : 0.0;
			return startReading();
		}
	}

	// Replaces any earlier reader with a new one on the same asset and track, then starts it.
	// By default the reader covers the whole clip. With fromTime it starts at startSeconds instead,
	// and readFrame() skips whatever the reader still hands over from before that time.
	bool startReading(bool fromTime = false, double startSeconds = 0.0) {
		@autoreleasepool {
			releaseReader();
			NSError* error = nil;
			reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
			if (!reader) {
				lastErrorText = "cannot open video: " + describeError(error);
				releaseReader();
				return false;
			}
			NSDictionary* settings = @{
				(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
				(id)kCVPixelBufferWidthKey : @(rawWidth),
				(id)kCVPixelBufferHeightKey : @(rawHeight)
			};
			output = [[AVAssetReaderTrackOutput alloc] initWithTrack:track outputSettings:settings];
			if (!output || ![reader canAddOutput:output]) {
				lastErrorText = "cannot decode video track";
				releaseReader();
				return false;
			}
			[reader addOutput:output];
			double dropBefore = -std::numeric_limits<double>::infinity();
			if (fromTime) {
				// The range can only be set before reading starts. It runs to the end of the clip.
				dropBefore = startSeconds - kSeekDropMarginSeconds;
				const CMTime from = CMTimeMakeWithSeconds(std::max(0.0, startSeconds - kSeekSlackSeconds), kSeekTimescale);
				[reader setTimeRange:CMTimeRangeMake(from, kCMTimePositiveInfinity)];
			}
			if (![reader startReading]) {
				lastErrorText = "cannot start reading: " + describeError([reader error]);
				releaseReader();
				return false;
			}
			dropBeforeSeconds_ = dropBefore;
			return true;
		}
	}

	// Reads the next sample into out. Returns false at the end of the clip (no error text) or on error.
	// After a seek, samples from before the wanted time are dropped here without copying their pixels:
	// the reader may start at an earlier key frame and hand those over first.
	bool readFrame(DecodedFrame& out) {
		for (;;) {
			@autoreleasepool {
				RetainedSample sample([output copyNextSampleBuffer]);
				if (!sample.get()) {
					if ([reader status] == AVAssetReaderStatusFailed) {
						lastErrorText = "read failed: " + describeError([reader error]);
					}
					return false;
				}
				if (std::isfinite(dropBeforeSeconds_)) {
					const double seconds = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample.get()));
					if (std::isfinite(seconds) && seconds < dropBeforeSeconds_) {
						continue;
					}
					// The first sample that counts ends the skipping for this reader.
					dropBeforeSeconds_ = -std::numeric_limits<double>::infinity();
				}
				return convertFrame(sample.get(), out);
			}
		}
	}

	// Copies the BGRA sample into out as RGBA, applying the track rotation while copying.
	bool convertFrame(CMSampleBufferRef sample, DecodedFrame& out) {
		CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
		if (!pixels) {
			lastErrorText = "frame has no image";
			return false;
		}
		if (CVPixelBufferGetPixelFormatType(pixels) != kCVPixelFormatType_32BGRA ||
		    static_cast<int>(CVPixelBufferGetWidth(pixels)) != rawWidth ||
		    static_cast<int>(CVPixelBufferGetHeight(pixels)) != rawHeight) {
			lastErrorText = "frame size or format differs from the request";
			return false;
		}

		ReadOnlyPixelLock pixelLock(pixels);
		const unsigned char* source = static_cast<const unsigned char*>(CVPixelBufferGetBaseAddress(pixels));
		if (!pixelLock.locked() || !source) {
			lastErrorText = "cannot read frame pixels";
			return false;
		}
		const size_t stride = CVPixelBufferGetBytesPerRow(pixels);

		const int outWidth = width_;
		const int outHeight = height_;
		out.width = outWidth;
		out.height = outHeight;
		out.rgba.resize(static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight) * 4);
		unsigned char* dest = out.rgba.data();
		for (int oy = 0; oy < outHeight; ++oy) {
			for (int ox = 0; ox < outWidth; ++ox) {
				// Map each display pixel back to its raw pixel for the track rotation.
				int rx = ox;
				int ry = oy;
				switch (rotation) {
					case 90:
						rx = oy;
						ry = rawHeight - 1 - ox;
						break;
					case 180:
						rx = rawWidth - 1 - ox;
						ry = rawHeight - 1 - oy;
						break;
					case 270:
						rx = rawWidth - 1 - oy;
						ry = ox;
						break;
					default:
						break;
				}
				const unsigned char* in = source + static_cast<size_t>(ry) * stride + static_cast<size_t>(rx) * 4;
				unsigned char* px = dest + (static_cast<size_t>(oy) * static_cast<size_t>(outWidth) + static_cast<size_t>(ox)) * 4;
				px[0] = in[2];  // R from BGRA
				px[1] = in[1];  // G
				px[2] = in[0];  // B
				px[3] = 255;
			}
		}

		const double seconds = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample));
		out.presentationSeconds = std::isfinite(seconds) ? seconds : 0.0;
		return true;
	}

	void releaseReader() {
		dropBeforeSeconds_ = -std::numeric_limits<double>::infinity();
		if (output) {
			[output release];
			output = nil;
		}
		if (reader) {
			if ([reader status] == AVAssetReaderStatusReading) {
				[reader cancelReading];
			}
			[reader release];
			reader = nil;
		}
	}

	void releaseResources() {
		releaseReader();
		if (track) {
			[track release];
			track = nil;
		}
		if (asset) {
			[asset release];
			asset = nil;
		}
		rotation = 0;
		rawWidth = 0;
		rawHeight = 0;
		width_ = 0;
		height_ = 0;
		duration_ = 0.0;
	}

	AVURLAsset* asset = nil;
	AVAssetTrack* track = nil;
	AVAssetReader* reader = nil;
	AVAssetReaderTrackOutput* output = nil;
	// After a seek: samples earlier than this are skipped. -infinity when the reader covers the whole clip.
	double dropBeforeSeconds_ = -std::numeric_limits<double>::infinity();
	int rotation = 0;  // degrees clockwise: 0, 90, 180 or 270
	int rawWidth = 0;  // decoded (unrotated) size requested from AVFoundation
	int rawHeight = 0;
	int width_ = 0;    // display size
	int height_ = 0;
	double duration_ = 0.0;
	std::string lastErrorText;
};

bool platformSupportsPlayback() {
	return true;
}

std::unique_ptr<FrameSource> createFrameSource() {
	return std::make_unique<AvFrameSource>();
}

}  // namespace video_player

#endif  // defined(__APPLE__)
