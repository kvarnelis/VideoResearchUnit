#if defined(__APPLE__)

#include "display_recorder.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreAudio/CoreAudioTypes.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>

namespace display_recorder {
namespace {

// Time scale for frame times. 600 divides evenly by 24, 25, 30 and 60 frames per second.
constexpr int kTimeScale = 600;
// How long finish() waits for AVFoundation to close the file.
constexpr int kFinishWaitSeconds = 20;

std::string stdStringFromNSString(NSString* value) {
	if (!value) {
		return std::string();
	}
	const char* utf8 = [value UTF8String];
	return utf8 ? std::string(utf8) : std::string();
}

// Readable text for an NSError. Never empty.
std::string describeError(NSError* error) {
	std::string text = error ? stdStringFromNSString([error localizedDescription]) : std::string();
	if (error) {
		const std::string reason = stdStringFromNSString([error localizedFailureReason]);
		if (!reason.empty()) {
			text += " (" + reason + ")";
		}
	}
	return text.empty() ? std::string("unknown AVFoundation error") : text;
}

// About 0.8 bits per pixel per frame, for pictures of dense one-pixel lines: 37 Mbit/s at 1440 x 1080 and 30 fps. Kept between 2 and 40 Mbit/s.
int bitRateFor(int width, int height, int fps) {
	const double bits = static_cast<double>(width) * static_cast<double>(height) * static_cast<double>(fps) * 0.8;
	return static_cast<int>(std::min(40e6, std::max(2e6, bits)));
}

// RGBA to BGRA for one row; the frame is opaque.
void swizzleRow(const unsigned char* source, unsigned char* dest, int pixels) {
	for (int x = 0; x < pixels; ++x) {
		const unsigned char* in = source + static_cast<std::size_t>(x) * 4;
		unsigned char* out = dest + static_cast<std::size_t>(x) * 4;
		out[0] = in[2];
		out[1] = in[1];
		out[2] = in[0];
		out[3] = 255;
	}
}

// Tags a frame as BT.709, the colour space the movie's output settings declare.
void tagAsRec709(CVPixelBufferRef pixels) {
	CVBufferSetAttachment(pixels, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2,
		kCVAttachmentMode_ShouldPropagate);
	CVBufferSetAttachment(pixels, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2,
		kCVAttachmentMode_ShouldPropagate);
	CVBufferSetAttachment(pixels, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2,
		kCVAttachmentMode_ShouldPropagate);
}

// Releases a CVPixelBuffer on scope exit.
class PixelBufferOwner {
public:
	explicit PixelBufferOwner(CVPixelBufferRef pixels) : pixels_(pixels) {}
	~PixelBufferOwner() {
		if (pixels_) {
			CVPixelBufferRelease(pixels_);
		}
	}
	PixelBufferOwner(const PixelBufferOwner&) = delete;
	PixelBufferOwner& operator=(const PixelBufferOwner&) = delete;

private:
	CVPixelBufferRef pixels_;
};

// Shared between finish() and AVFoundation's completion handler, which may run on another thread.
struct Completion {
	std::mutex mutex;
	std::condition_variable changed;
	bool done = false;
};

// Writes H.264 video into a QuickTime movie (.mov) with AVAssetWriter.
class AvfFrameSink final : public FrameSink {
public:
	AvfFrameSink() = default;
	~AvfFrameSink() override {
		cancel();
	}
	AvfFrameSink(const AvfFrameSink&) = delete;
	AvfFrameSink& operator=(const AvfFrameSink&) = delete;

	bool open(const std::string& path, int width, int height, int fps) override {
		cancel();
		closed = false;
		lastErrorText.clear();
		try {
			@try {
				if (openFile(path, width, height, fps)) {
					return true;
				}
			} @catch (NSException* exception) {
				lastErrorText = "AVFoundation exception: " + stdStringFromNSString([exception reason]);
			}
		} catch (const std::exception& error) {
			lastErrorText = std::string("encoder error: ") + error.what();
		} catch (...) {
			lastErrorText = "encoder error";
		}
		const std::string failure = lastErrorText;
		cancel();
		lastErrorText = failure;
		return false;
	}

	bool readyForFrame() override {
		try {
			@try {
				@autoreleasepool {
					if (!writer || !input || !adaptor) {
						return true;  // append() reports that the sink is not open
					}
					if ([writer status] != AVAssetWriterStatusWriting) {
						return true;  // append() reports the failure
					}
					return [input isReadyForMoreMediaData] ? true : false;
				}
			} @catch (NSException*) {
				return true;
			}
		} catch (...) {
			return true;
		}
	}

	bool append(const unsigned char* rgba, int rowBytes, double ptsSeconds) override {
		lastErrorText.clear();
		try {
			@try {
				return appendFrame(rgba, rowBytes, ptsSeconds);
			} @catch (NSException* exception) {
				lastErrorText = "AVFoundation exception: " + stdStringFromNSString([exception reason]);
			}
		} catch (const std::exception& error) {
			lastErrorText = std::string("encoder error: ") + error.what();
		} catch (...) {
			lastErrorText = "encoder error";
		}
		return false;
	}

	bool finish(double endSeconds) override {
		lastErrorText.clear();
		try {
			@try {
				return finishFile(endSeconds);
			} @catch (NSException* exception) {
				lastErrorText = "AVFoundation exception: " + stdStringFromNSString([exception reason]);
			}
		} catch (const std::exception& error) {
			lastErrorText = std::string("encoder error: ") + error.what();
		} catch (...) {
			lastErrorText = "encoder error";
		}
		return false;
	}

	void cancel() override {
		try {
			@try {
				@autoreleasepool {
					if (writer && !closed && [writer status] == AVAssetWriterStatusWriting) {
						[writer cancelWriting];
					}
				}
			} @catch (NSException*) {
			}
		} catch (...) {
		}
		// A file that was started and never finished is not playable, so it goes.
		if (createdFile && !closed && !outputPath.empty()) {
			::unlink(outputPath.c_str());
		}
		createdFile = false;
		closed = true;
		releaseResources();
	}

	std::string lastError() const override {
		return lastErrorText;
	}

	bool enableAudio(int sampleRate) override {
		if (sampleRate < 8000 || sampleRate > 768000) {
			return false;
		}
		wantedAudioRate = sampleRate;
		return true;
	}

	bool hasAudio() const override {
		return audioInput != nil;
	}

	bool audioReady() override {
		try {
			@try {
				@autoreleasepool {
					if (!writer || !audioInput || [writer status] != AVAssetWriterStatusWriting) {
						return true;  // appendAudio() reports the failure
					}
					return [audioInput isReadyForMoreMediaData] ? true : false;
				}
			} @catch (NSException*) {
				return true;
			}
		} catch (...) {
			return true;
		}
	}

	bool appendAudio(const float* stereo, int frames, double ptsSeconds) override {
		lastErrorText.clear();
		try {
			@try {
				return appendAudioFrames(stereo, frames);
			} @catch (NSException* exception) {
				lastErrorText = "AVFoundation exception: " + stdStringFromNSString([exception reason]);
			}
		} catch (const std::exception& error) {
			lastErrorText = std::string("encoder error: ") + error.what();
		} catch (...) {
			lastErrorText = "encoder error";
		}
		(void)ptsSeconds;
		return false;
	}

private:
	// Output settings for the audio track, or nil when this Mac cannot write any. AAC is what the
	// QuickTime container and every player handle best. AAC wants a rate from 8 to 96 kHz, so a higher
	// engine rate (the encoder converts) is written at 48 kHz. If AAC is refused, 16-bit PCM is used.
	NSDictionary* audioSettingsFor(int sampleRate, bool& isAac) {
		int outRate = sampleRate;
		if (outRate > 96000) {
			outRate = (sampleRate % 44100 == 0) ? 44100 : 48000;
		}
		NSDictionary* aac = @{
			AVFormatIDKey : @(kAudioFormatMPEG4AAC),
			AVNumberOfChannelsKey : @2,
			AVSampleRateKey : @(outRate),
			AVEncoderBitRateKey : @192000
		};
		if ([writer canApplyOutputSettings:aac forMediaType:AVMediaTypeAudio]) {
			isAac = true;
			return aac;
		}
		NSDictionary* pcm = @{
			AVFormatIDKey : @(kAudioFormatLinearPCM),
			AVNumberOfChannelsKey : @2,
			AVSampleRateKey : @(outRate),
			AVLinearPCMBitDepthKey : @16,
			AVLinearPCMIsFloatKey : @NO,
			AVLinearPCMIsBigEndianKey : @NO,
			AVLinearPCMIsNonInterleaved : @NO
		};
		if ([writer canApplyOutputSettings:pcm forMediaType:AVMediaTypeAudio]) {
			isAac = false;
			return pcm;
		}
		return nil;
	}

	bool appendAudioFrames(const float* stereo, int frames) {
		@autoreleasepool {
			if (!writer || !audioInput || closed) {
				lastErrorText = "the audio encoder is not open";
				return false;
			}
			if ([writer status] != AVAssetWriterStatusWriting) {
				lastErrorText = "the movie writer stopped: " + describeError([writer error]);
				return false;
			}
			if (frames <= 0) {
				return true;
			}
			const std::size_t bytes = static_cast<std::size_t>(frames) * 2 * sizeof(float);
			CMBlockBufferRef block = NULL;
			OSStatus status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, NULL, bytes, kCFAllocatorDefault, NULL, 0, bytes,
				kCMBlockBufferAssureMemoryNowFlag, &block);
			if (status != noErr || !block) {
				lastErrorText = "cannot allocate audio";
				return false;
			}
			status = CMBlockBufferReplaceDataBytes(stereo, block, 0, bytes);
			CMSampleBufferRef sample = NULL;
			if (status == noErr) {
				const CMTime pts = CMTimeMake(static_cast<int64_t>(audioWritten), wantedAudioRate);
				status = CMAudioSampleBufferCreateReadyWithPacketDescriptions(kCFAllocatorDefault, block, audioFormat, frames, pts, NULL, &sample);
			}
			CFRelease(block);
			if (status != noErr || !sample) {
				lastErrorText = "cannot make an audio buffer";
				return false;
			}
			const BOOL ok = [audioInput appendSampleBuffer:sample];
			CFRelease(sample);
			if (!ok) {
				lastErrorText = "cannot add audio: " + describeError([writer error]);
				return false;
			}
			audioWritten += static_cast<std::uint64_t>(frames);
			return true;
		}
	}

	bool openFile(const std::string& path, int width, int height, int fps) {
		@autoreleasepool {
			NSString* filePath = [NSString stringWithUTF8String:path.c_str()];
			if (!filePath) {
				lastErrorText = "bad file name";
				return false;
			}
			// AVAssetWriter will not replace a file, and this sink must never delete one it did not create.
			if ([[NSFileManager defaultManager] fileExistsAtPath:filePath]) {
				lastErrorText = "the file already exists";
				return false;
			}
			NSError* error = nil;
			writer = [[AVAssetWriter alloc] initWithURL:[NSURL fileURLWithPath:filePath]
			                                   fileType:AVFileTypeQuickTimeMovie
			                                      error:&error];
			if (!writer) {
				lastErrorText = "cannot create the movie: " + describeError(error);
				return false;
			}

			// The codec name is "avc1" (H.264). The literal avoids AVVideoCodecH264 and AVVideoCodecTypeH264,
			// which are deprecated or too new for the oldest macOS this plugin supports.
			NSDictionary* compression = @{
				AVVideoAverageBitRateKey : @(bitRateFor(width, height, fps)),
				AVVideoProfileLevelKey : AVVideoProfileLevelH264HighAutoLevel,
				AVVideoMaxKeyFrameIntervalKey : @(fps * 2),
				AVVideoExpectedSourceFrameRateKey : @(fps)
			};
			NSDictionary* plain = @{
				AVVideoCodecKey : @"avc1",
				AVVideoWidthKey : @(width),
				AVVideoHeightKey : @(height),
				AVVideoCompressionPropertiesKey : compression
			};
			// BT.709 tags, so players decode the colours the way the frames were encoded. The key names
			// are written out because the AVVideoColor... constants need macOS 10.13.
			NSDictionary* tagged = @{
				AVVideoCodecKey : @"avc1",
				AVVideoWidthKey : @(width),
				AVVideoHeightKey : @(height),
				AVVideoCompressionPropertiesKey : compression,
				@"AVVideoColorPropertiesKey" : @{
					@"ColorPrimaries" : @"ITU_R_709_2",
					@"TransferFunction" : @"ITU_R_709_2",
					@"YCbCrMatrix" : @"ITU_R_709_2"
				}
			};
			NSDictionary* settings = plain;
			colourTagged = false;
			if ([writer canApplyOutputSettings:tagged forMediaType:AVMediaTypeVideo]) {
				settings = tagged;
				colourTagged = true;
			} else if (![writer canApplyOutputSettings:plain forMediaType:AVMediaTypeVideo]) {
				lastErrorText = "this Mac cannot encode H.264 video with these settings";
				return false;
			}

			input = [[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:settings] retain];
			if (!input) {
				lastErrorText = "cannot create the video input";
				return false;
			}
			// Frames arrive in real time, so the encoder should keep up rather than build quality.
			input.expectsMediaDataInRealTime = YES;

			NSDictionary* sourceAttributes = @{
				(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
				(id)kCVPixelBufferWidthKey : @(width),
				(id)kCVPixelBufferHeightKey : @(height)
			};
			adaptor = [[AVAssetWriterInputPixelBufferAdaptor alloc] initWithAssetWriterInput:input
			                                                     sourcePixelBufferAttributes:sourceAttributes];
			if (!adaptor || ![writer canAddInput:input]) {
				lastErrorText = "the movie writer does not accept the video input";
				return false;
			}
			[writer addInput:input];

			if (wantedAudioRate > 0) {
				bool aac = false;
				NSDictionary* audioSettings = audioSettingsFor(wantedAudioRate, aac);
				if (audioSettings) {
					AudioStreamBasicDescription format = {};
					format.mSampleRate = wantedAudioRate;
					format.mFormatID = kAudioFormatLinearPCM;
					format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
					format.mBitsPerChannel = 32;
					format.mChannelsPerFrame = 2;
					format.mBytesPerFrame = 8;
					format.mFramesPerPacket = 1;
					format.mBytesPerPacket = 8;
					AudioChannelLayout layout = {};
					layout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
					if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &format, sizeof(layout), &layout, 0, NULL, NULL, &audioFormat) == noErr && audioFormat) {
						AVAssetWriterInput* candidate = [[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
						                                                                    outputSettings:audioSettings
						                                                                  sourceFormatHint:audioFormat] retain];
						if (candidate && [writer canAddInput:candidate]) {
							candidate.expectsMediaDataInRealTime = YES;
							[writer addInput:candidate];
							audioInput = candidate;
						} else {
							[candidate release];
						}
					}
				}
			}

			outputPath = path;
			if (![writer startWriting]) {
				// AVAssetWriter refuses to start when a file is already there, so nothing here is ours to delete.
				lastErrorText = "cannot start writing: " + describeError([writer error]);
				return false;
			}
			createdFile = true;  // startWriting created the file, and it is ours
			[writer startSessionAtSourceTime:kCMTimeZero];
			width_ = width;
			height_ = height;
			return true;
		}
	}

	bool appendFrame(const unsigned char* rgba, int rowBytes, double ptsSeconds) {
		@autoreleasepool {
			if (!writer || !input || !adaptor || closed) {
				lastErrorText = "the encoder is not open";
				return false;
			}
			if ([writer status] != AVAssetWriterStatusWriting) {
				lastErrorText = "the movie writer stopped: " + describeError([writer error]);
				return false;
			}
			CVPixelBufferRef pixels = NULL;
			CVPixelBufferPoolRef pool = [adaptor pixelBufferPool];
			CVReturn made = pool ? CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixels) : kCVReturnError;
			if (made != kCVReturnSuccess || !pixels) {
				pixels = NULL;
				made = CVPixelBufferCreate(kCFAllocatorDefault, static_cast<size_t>(width_), static_cast<size_t>(height_),
					kCVPixelFormatType_32BGRA, NULL, &pixels);
				if (made != kCVReturnSuccess || !pixels) {
					lastErrorText = "cannot allocate a video frame";
					return false;
				}
			}
			PixelBufferOwner owner(pixels);
			if (CVPixelBufferLockBaseAddress(pixels, 0) != kCVReturnSuccess) {
				lastErrorText = "cannot lock a video frame";
				return false;
			}
			unsigned char* dest = static_cast<unsigned char*>(CVPixelBufferGetBaseAddress(pixels));
			const std::size_t destStride = CVPixelBufferGetBytesPerRow(pixels);
			if (dest) {
				for (int y = 0; y < height_; ++y) {
					swizzleRow(rgba + static_cast<std::size_t>(y) * static_cast<std::size_t>(rowBytes),
						dest + static_cast<std::size_t>(y) * destStride, width_);
				}
			}
			CVPixelBufferUnlockBaseAddress(pixels, 0);
			if (!dest) {
				lastErrorText = "cannot write a video frame";
				return false;
			}
			if (colourTagged) {
				tagAsRec709(pixels);
			}
			if (![adaptor appendPixelBuffer:pixels withPresentationTime:CMTimeMakeWithSeconds(ptsSeconds, kTimeScale)]) {
				lastErrorText = "cannot add a frame: " + describeError([writer error]);
				return false;
			}
			return true;
		}
	}

	bool finishFile(double endSeconds) {
		@autoreleasepool {
			if (!writer || !input || closed) {
				lastErrorText = "the encoder is not open";
				return false;
			}
			if ([writer status] != AVAssetWriterStatusWriting) {
				lastErrorText = "the movie writer stopped: " + describeError([writer error]);
				return false;
			}
			[input markAsFinished];
			if (audioInput) {
				[audioInput markAsFinished];
			}
			[writer endSessionAtSourceTime:CMTimeMakeWithSeconds(endSeconds, kTimeScale)];
			// The handler runs on another thread. It holds its own reference to the shared state, so it is
			// safe even if this call gives up waiting.
			std::shared_ptr<Completion> completion = std::make_shared<Completion>();
			[writer finishWritingWithCompletionHandler:^{
				std::lock_guard<std::mutex> lock(completion->mutex);
				completion->done = true;
				completion->changed.notify_all();
			}];
			bool done = false;
			{
				std::unique_lock<std::mutex> lock(completion->mutex);
				done = completion->changed.wait_for(lock, std::chrono::seconds(kFinishWaitSeconds),
					[&completion] { return completion->done; });
			}
			if (!done) {
				lastErrorText = "timed out while closing the movie";
				return false;
			}
			if ([writer status] != AVAssetWriterStatusCompleted) {
				lastErrorText = "cannot finish the movie: " + describeError([writer error]);
				return false;
			}
			closed = true;  // complete: cancel() must not delete it
			createdFile = false;
			return true;
		}
	}

	void releaseResources() {
		[audioInput release];
		audioInput = nil;
		if (audioFormat) {
			CFRelease(audioFormat);
			audioFormat = NULL;
		}
		audioWritten = 0;
		[adaptor release];
		adaptor = nil;
		[input release];
		input = nil;
		[writer release];
		writer = nil;
	}

	AVAssetWriter* writer = nil;
	AVAssetWriterInput* input = nil;
	AVAssetWriterInputPixelBufferAdaptor* adaptor = nil;
	AVAssetWriterInput* audioInput = nil;
	CMAudioFormatDescriptionRef audioFormat = NULL;
	int wantedAudioRate = 0;           // 0: no audio track
	std::uint64_t audioWritten = 0;    // audio frames given to the writer so far
	std::string outputPath;
	std::string lastErrorText;
	int width_ = 0;
	int height_ = 0;
	bool colourTagged = false;
	bool createdFile = false;  // a file at outputPath was started by this sink
	bool closed = false;       // finished or cancelled
};

}  // namespace

bool platformSupportsRecording() {
	return true;
}

std::unique_ptr<FrameSink> createFrameSink() {
	return std::make_unique<AvfFrameSink>();
}

}  // namespace display_recorder

#endif  // defined(__APPLE__)
