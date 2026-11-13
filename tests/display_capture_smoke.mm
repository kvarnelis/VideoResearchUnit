// Smoke test for the Display module's SHOT and REC buttons, with the real encoders. macOS only.
//
// make test builds this with the capture and recorder sources and runs it. It does what a button
// press does: SHOT hands a frame to the screenshot worker and gets a real PNG; REC starts a recording,
// feeds it frames for a second and a half, stops it and gets a real .mov. Both files are then read
// back with the system's own decoders (ImageIO, AVAssetReader) and checked for size, colours,
// orientation and length. It also checks the default folder, the -2 / -3 names and a failing save.
//
// Files go to build/display_capture_smoke_tmp and HOME points there for the whole run, so the real
// home folder is never touched. Each failure prints one line, then the summary
// "display_capture_smoke: N checks, F failed". Exits 0 only when F is 0.

#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include "display_capture.hpp"
#include "display_recorder.hpp"

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
#define CHECK(condition) check((condition), #condition, __LINE__)

bool startsWith(const std::string& text, const std::string& head) {
	return text.compare(0, head.size(), head) == 0;
}

long fileSize(const std::string& path) {
	struct stat st;
	return ::stat(path.c_str(), &st) == 0 ? static_cast<long>(st.st_size) : -1;
}

// ---- Synthetic pictures ------------------------------------------------------------------------

struct Image {
	int width = 0;
	int height = 0;
	std::vector<unsigned char> rgba;

	bool near(int x, int y, int r, int g, int b, int tolerance) const {
		if (x < 0 || y < 0 || x >= width || y >= height)
			return false;
		const unsigned char* p = &rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4];
		return std::abs(p[0] - r) <= tolerance && std::abs(p[1] - g) <= tolerance && std::abs(p[2] - b) <= tolerance;
	}
	// The three quadrants that never change: top right green, bottom left blue, bottom right yellow.
	// A swapped channel, a flipped picture or a mirrored one fails this.
	bool fixedQuadrantsOk(int tolerance) const {
		return near(3 * width / 4, height / 4, 40, 200, 40, tolerance) &&
		       near(width / 4, 3 * height / 4, 40, 40, 200, tolerance) &&
		       near(3 * width / 4, 3 * height / 4, 200, 200, 40, tolerance);
	}
};

// Top-left quadrant is (r, g, b). The other three are fixed.
std::vector<unsigned char> makeFrame(int w, int h, int r, int g, int b) {
	std::vector<unsigned char> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const bool right = x >= w / 2;
			const bool bottom = y >= h / 2;
			int cr = r, cg = g, cb = b;
			if (right && !bottom) {
				cr = 40; cg = 200; cb = 40;
			}
			else if (!right && bottom) {
				cr = 40; cg = 40; cb = 200;
			}
			else if (right && bottom) {
				cr = 200; cg = 200; cb = 40;
			}
			unsigned char* p = &px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4];
			p[0] = static_cast<unsigned char>(cr);
			p[1] = static_cast<unsigned char>(cg);
			p[2] = static_cast<unsigned char>(cb);
			p[3] = 255;
		}
	}
	return px;
}

// ---- Reading the files back with the system decoders ------------------------------------------

bool readPng(const std::string& path, Image& out) {
	bool ok = false;
	@autoreleasepool {
		NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
		CGImageSourceRef source = CGImageSourceCreateWithURL((CFURLRef)url, NULL);
		if (source) {
			CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, NULL);
			CFRelease(source);
			if (image) {
				const std::size_t w = CGImageGetWidth(image);
				const std::size_t h = CGImageGetHeight(image);
				out.width = static_cast<int>(w);
				out.height = static_cast<int>(h);
				out.rgba.assign(w * h * 4, 0);
				// Draw in the image's own colour space, so that no colour conversion changes the values.
				CGColorSpaceRef own = nullptr;
				CGColorSpaceRef space = CGImageGetColorSpace(image);
				if (!space || CGColorSpaceGetModel(space) != kCGColorSpaceModelRGB) {
					own = CGColorSpaceCreateDeviceRGB();
					space = own;
				}
				CGContextRef context = CGBitmapContextCreate(out.rgba.data(), w, h, 8, w * 4, space, kCGImageAlphaPremultipliedLast);
				if (context) {
					CGContextDrawImage(context, CGRectMake(0, 0, static_cast<CGFloat>(w), static_cast<CGFloat>(h)), image);
					CGContextRelease(context);
					ok = true;
				}
				if (own)
					CGColorSpaceRelease(own);
				CGImageRelease(image);
			}
		}
	}
	return ok;
}

struct Movie {
	int width = 0;
	int height = 0;
	double seconds = 0.0;
	int samples = 0;
	bool completed = false;
	bool taggedRec709 = false;
	Image first;
	Image last;
};

void copyBgraToRgba(CVPixelBufferRef pixels, Image& out) {
	CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
	const std::size_t w = CVPixelBufferGetWidth(pixels);
	const std::size_t h = CVPixelBufferGetHeight(pixels);
	const std::size_t stride = CVPixelBufferGetBytesPerRow(pixels);
	const unsigned char* base = static_cast<const unsigned char*>(CVPixelBufferGetBaseAddress(pixels));
	out.width = static_cast<int>(w);
	out.height = static_cast<int>(h);
	out.rgba.assign(w * h * 4, 255);
	if (base) {
		for (std::size_t y = 0; y < h; y++) {
			const unsigned char* src = base + y * stride;
			unsigned char* dst = &out.rgba[y * w * 4];
			for (std::size_t x = 0; x < w; x++) {
				dst[x * 4 + 0] = src[x * 4 + 2];
				dst[x * 4 + 1] = src[x * 4 + 1];
				dst[x * 4 + 2] = src[x * 4 + 0];
			}
		}
	}
	CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
}

bool readMovie(const std::string& path, Movie& movie) {
	bool ok = false;
	@autoreleasepool {
		NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
		AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:@{AVURLAssetPreferPreciseDurationAndTimingKey : @YES}];
		dispatch_semaphore_t loaded = dispatch_semaphore_create(0);
		__block NSArray* loadedTracks = nil;
		[asset loadTracksWithMediaType:AVMediaTypeVideo
		             completionHandler:^(NSArray<AVAssetTrack*>* tracks, NSError* error) {
			(void)error;
			loadedTracks = [tracks retain];
			dispatch_semaphore_signal(loaded);
		}];
		dispatch_semaphore_wait(loaded, DISPATCH_TIME_FOREVER);
		dispatch_release(loaded);
		NSArray* videoTracks = [loadedTracks autorelease];
		if ([videoTracks count] > 0) {
			AVAssetTrack* track = [videoTracks objectAtIndex:0];
			NSArray* descriptions = [track formatDescriptions];
			if ([descriptions count] > 0) {
				CMFormatDescriptionRef description = (CMFormatDescriptionRef)[descriptions objectAtIndex:0];
				auto is709 = [&](CFStringRef key, CFStringRef value) {
					CFTypeRef found = CMFormatDescriptionGetExtension(description, key);
					return found && CFEqual(found, value);
				};
				movie.taggedRec709 = is709(kCMFormatDescriptionExtension_ColorPrimaries, kCMFormatDescriptionColorPrimaries_ITU_R_709_2)
					&& is709(kCMFormatDescriptionExtension_TransferFunction, kCMFormatDescriptionTransferFunction_ITU_R_709_2)
					&& is709(kCMFormatDescriptionExtension_YCbCrMatrix, kCMFormatDescriptionYCbCrMatrix_ITU_R_709_2);
			}
			const CGSize natural = [track naturalSize];
			movie.width = static_cast<int>(natural.width + 0.5);
			movie.height = static_cast<int>(natural.height + 0.5);
			movie.seconds = CMTimeGetSeconds([asset duration]);

			NSError* error = nil;
			AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:&error];
			NSDictionary* settings = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
			AVAssetReaderTrackOutput* output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:settings];
			if (reader && output && [reader canAddOutput:output]) {
				[reader addOutput:output];
				if ([reader startReading]) {
					while (CMSampleBufferRef sample = [output copyNextSampleBuffer]) {
						CVImageBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
						if (pixels) {
							if (movie.samples == 0)
								copyBgraToRgba(pixels, movie.first);
							copyBgraToRgba(pixels, movie.last);
							movie.samples++;
						}
						CFRelease(sample);
					}
					movie.completed = [reader status] == AVAssetReaderStatusCompleted;
					ok = true;
				}
			}
		}
	}
	return ok;
}

// ---- Audio read-back -----------------------------------------------------------------------------

struct AudioInfo {
	int tracks = 0;
	int channels = 0;
	double trackSeconds = 0.0;   // the audio track's own time range
	double decodedSeconds = 0.0; // samples that came out of the decoder / sample rate
	double rms[2] = {0.0, 0.0};
	double hz[2] = {0.0, 0.0};   // from rising zero crossings in the middle of the signal
	double peak[2] = {0.0, 0.0};
	bool formatIsAac = false;
};

bool readAudio(const std::string& path, AudioInfo& info, int decodeRate) {
	bool ok = false;
	@autoreleasepool {
		NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
		AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:@{AVURLAssetPreferPreciseDurationAndTimingKey : @YES}];
		dispatch_semaphore_t loaded = dispatch_semaphore_create(0);
		__block NSArray* loadedTracks = nil;
		[asset loadTracksWithMediaType:AVMediaTypeAudio
		             completionHandler:^(NSArray<AVAssetTrack*>* tracks, NSError* error) {
			(void)error;
			loadedTracks = [tracks retain];
			dispatch_semaphore_signal(loaded);
		}];
		dispatch_semaphore_wait(loaded, DISPATCH_TIME_FOREVER);
		dispatch_release(loaded);
		NSArray* tracks = [loadedTracks autorelease];
		info.tracks = static_cast<int>([tracks count]);
		if (info.tracks == 0)
			return true;  // the answer "no audio" is a valid read
		AVAssetTrack* track = [tracks objectAtIndex:0];
		info.trackSeconds = CMTimeGetSeconds([track timeRange].duration);
		NSArray* descriptions = [track formatDescriptions];
		if ([descriptions count] > 0) {
			CMFormatDescriptionRef description = (CMFormatDescriptionRef)[descriptions objectAtIndex:0];
			const AudioStreamBasicDescription* asbd = CMAudioFormatDescriptionGetStreamBasicDescription(description);
			if (asbd) {
				info.channels = static_cast<int>(asbd->mChannelsPerFrame);
				info.formatIsAac = asbd->mFormatID == kAudioFormatMPEG4AAC;
			}
		}
		NSError* error = nil;
		AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:&error];
		NSDictionary* settings = @{
			AVFormatIDKey : @(kAudioFormatLinearPCM),
			AVLinearPCMBitDepthKey : @32,
			AVLinearPCMIsFloatKey : @YES,
			AVLinearPCMIsBigEndianKey : @NO,
			AVLinearPCMIsNonInterleaved : @NO,
			AVNumberOfChannelsKey : @2,
			AVSampleRateKey : @(decodeRate)
		};
		AVAssetReaderTrackOutput* output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:settings];
		if (!reader || !output || ![reader canAddOutput:output])
			return false;
		[reader addOutput:output];
		if (![reader startReading])
			return false;
		std::vector<float> all;
		while (CMSampleBufferRef sample = [output copyNextSampleBuffer]) {
			CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
			std::size_t length = block ? CMBlockBufferGetDataLength(block) : 0;
			if (length > 0) {
				const std::size_t before = all.size();
				all.resize(before + length / sizeof(float));
				CMBlockBufferCopyDataBytes(block, 0, length, all.data() + before);
			}
			CFRelease(sample);
		}
		const std::size_t frames = all.size() / 2;
		info.decodedSeconds = static_cast<double>(frames) / decodeRate;
		// Measure the middle half, away from the encoder's start-up and the end.
		const std::size_t from = frames / 4, to = frames * 3 / 4;
		for (int c = 0; c < 2 && to > from; c++) {
			double sum = 0.0;
			int rising = 0;
			std::size_t first = 0, last = 0;
			for (std::size_t i = from; i < to; i++) {
				const double v = all[i * 2 + static_cast<std::size_t>(c)];
				sum += v * v;
				info.peak[c] = std::max(info.peak[c], std::fabs(v));
				if (i > from && all[(i - 1) * 2 + static_cast<std::size_t>(c)] < 0.f && v >= 0.0) {
					if (rising == 0)
						first = i;
					last = i;
					rising++;
				}
			}
			info.rms[c] = std::sqrt(sum / static_cast<double>(to - from));
			if (rising > 2)
				info.hz[c] = static_cast<double>(rising - 1) * decodeRate / static_cast<double>(last - first);
		}
		ok = [reader status] == AVAssetReaderStatusCompleted;
	}
	return ok;
}

// A pretend Rack audio thread: pushes a sine on each channel at the real-time pace of `rate`.
// `pause` stops it for a while, like a paused engine.
class ToneProducer {
public:
	ToneProducer(display_recorder::AudioRing& ring, int rate, double hzL, double ampL, double hzR, double ampR)
		: ring_(ring), rate_(rate), hzL_(hzL), ampL_(ampL), hzR_(hzR), ampR_(ampR) {
		thread_ = std::thread([this] { run(); });
	}
	~ToneProducer() {
		stop();
	}
	void stop() {
		quit_ = true;
		if (thread_.joinable())
			thread_.join();
	}
	std::atomic<bool> pause {false};
	std::atomic<int> rateToReport {0};
	long long framesPushed() const {
		return pushed_;
	}

private:
	void run() {
		const double twoPi = 6.283185307179586;
		auto last = std::chrono::steady_clock::now();
		double owed = 0.0;
		while (!quit_) {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			const auto now = std::chrono::steady_clock::now();
			const double dt = std::chrono::duration<double>(now - last).count();
			last = now;
			if (pause)
				continue;
			owed += dt * rate_;
			const int rep = rateToReport.load();
			ring_.setSampleRate(static_cast<float>(rep > 0 ? rep : rate_));
			while (owed >= 1.0) {
				const double t = static_cast<double>(n_) / rate_;
				ring_.push(static_cast<float>(ampL_ * std::sin(twoPi * hzL_ * t)), static_cast<float>(ampR_ * std::sin(twoPi * hzR_ * t)));
				n_++;
				pushed_ = static_cast<long long>(n_);
				owed -= 1.0;
			}
		}
	}
	display_recorder::AudioRing& ring_;
	int rate_;
	double hzL_, ampL_, hzR_, ampR_;
	std::atomic<bool> quit_ {false};
	std::atomic<long long> pushed_ {0};
	long long n_ = 0;
	std::thread thread_;
};

// ---- The test ----------------------------------------------------------------------------------

void makeDirectory(const std::string& path) {
	@autoreleasepool {
		[[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:path.c_str()]
		                          withIntermediateDirectories:YES
		                                           attributes:nil
		                                                error:nil];
	}
}

void removeTree(const std::string& path) {
	@autoreleasepool {
		[[NSFileManager defaultManager] removeItemAtPath:[NSString stringWithUTF8String:path.c_str()] error:nil];
	}
}

void testScreenshot(const std::string& root, const std::string& defaultFolder, const std::string& stem) {
	using namespace display_capture;

	const int w = 64, h = 48;
	ShotWriter writer;

	// A press with no folder chosen: the default folder, created on this first save.
	CHECK(!directoryExists(defaultFolder));
	{
		ShotJob job;
		job.rgba = makeFrame(w, h, 200, 40, 40);
		job.width = w;
		job.height = h;
		job.stem = stem;
		CHECK(writer.submit(std::move(job)));
	}
	writer.waitIdle();
	ShotResult first;
	CHECK(writer.pollResult(first));
	CHECK(first.ok);
	CHECK(first.error.empty());
	CHECK(first.path == defaultFolder + "/" + stem + ".png");
	CHECK(directoryExists(defaultFolder));
	CHECK(fileSize(first.path) > 100);

	// The same name again: -2, and the first file is untouched.
	const long firstSize = fileSize(first.path);
	{
		ShotJob job;
		job.rgba = makeFrame(w, h, 40, 40, 200);
		job.width = w;
		job.height = h;
		job.stem = stem;
		CHECK(writer.submit(std::move(job)));
	}
	writer.waitIdle();
	ShotResult second;
	CHECK(writer.pollResult(second));
	CHECK(second.ok);
	CHECK(second.path == defaultFolder + "/" + stem + "-2.png");
	CHECK(fileSize(first.path) == firstSize);

	// Read both back with ImageIO: size, orientation and colours.
	Image image;
	CHECK(readPng(first.path, image));
	CHECK(image.width == w && image.height == h);
	CHECK(image.near(w / 4, h / 4, 200, 40, 40, 2));
	CHECK(image.fixedQuadrantsOk(2));
	Image image2;
	CHECK(readPng(second.path, image2));
	CHECK(image2.width == w && image2.height == h);
	CHECK(image2.near(w / 4, h / 4, 40, 40, 200, 2));

	// A chosen folder is used as it is. One that has gone falls back to the default folder.
	const std::string chosen = root + "/chosen";
	makeDirectory(chosen);
	{
		ShotJob job;
		job.rgba = makeFrame(w, h, 200, 40, 40);
		job.width = w;
		job.height = h;
		job.folder = chosen;
		job.stem = stem;
		CHECK(writer.submit(std::move(job)));
	}
	writer.waitIdle();
	ShotResult picked;
	CHECK(writer.pollResult(picked));
	CHECK(picked.ok);
	CHECK(picked.path == chosen + "/" + stem + ".png");
	{
		ShotJob job;
		job.rgba = makeFrame(w, h, 200, 40, 40);
		job.width = w;
		job.height = h;
		job.folder = root + "/gone";
		job.stem = stem;
		CHECK(writer.submit(std::move(job)));
	}
	writer.waitIdle();
	ShotResult fallback;
	CHECK(writer.pollResult(fallback));
	CHECK(fallback.ok);
	CHECK(startsWith(fallback.path, defaultFolder + "/"));
	CHECK(!directoryExists(root + "/gone"));

	// A save that cannot work reports an error and shows nothing: Pictures is a file here.
	const std::string badHome = root + "/badhome";
	makeDirectory(badHome);
	std::FILE* blocker = std::fopen((badHome + "/Pictures").c_str(), "w");
	CHECK(blocker != nullptr);
	if (blocker)
		std::fclose(blocker);
	::setenv("HOME", badHome.c_str(), 1);
	{
		ShotJob job;
		job.rgba = makeFrame(w, h, 200, 40, 40);
		job.width = w;
		job.height = h;
		job.stem = stem;
		CHECK(writer.submit(std::move(job)));
	}
	writer.waitIdle();
	ShotResult failed;
	CHECK(writer.pollResult(failed));
	CHECK(!failed.ok);
	CHECK(!failed.error.empty());
	::setenv("HOME", (root + "/home").c_str(), 1);
}

void testRecording(const std::string& defaultFolder, const std::string& stem) {
	using namespace display_recorder;

	const int w = 160, h = 120;
	const int frames = 45;  // 1.5 s at 30 frames a second

	SessionOptions options;
	options.fps = Session::kFps;
	options.makeSink = [] { return createFrameSink(); };
	options.choosePath = [stem](std::string& path, std::string& error) {
		return display_capture::chooseNewFilePath("", stem, "mov", path, error);
	};
	options.pathDone = [](const std::string& path) { display_capture::releasePath(path); };

	CHECK(platformSupportsRecording());

	// Frame 0 goes in with the start, then a frame every 1/30 s; the top-left quadrant goes from red to blue.
	auto frameAt = [&](int i) {
		const double t = static_cast<double>(i) / (frames - 1);
		return makeFrame(w, h, static_cast<int>(200 - 160 * t + 0.5), 40, static_cast<int>(40 + 160 * t + 0.5));
	};
	std::vector<unsigned char> frame = frameAt(0);
	Session session(options, frame.data(), w, h);

	const auto start = std::chrono::steady_clock::now();
	for (int i = 1; i < frames; i++) {
		std::this_thread::sleep_until(start + std::chrono::microseconds(1000000 * i / 30));
		frame = frameAt(i);
		session.submit(frame.data(), w, h);
	}
	const auto beforeStop = std::chrono::steady_clock::now();
	session.requestStop();  // returns at once; the file is closed on the worker thread
	const long stopMs = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - beforeStop).count());
	CHECK(stopMs < 100);
	CHECK(session.waitUntilDone(20000));

	CHECK(session.state() == SessionState::Finished);
	CHECK(session.error().empty());
	const std::string path = session.path();
	CHECK(path == defaultFolder + "/" + stem + ".mov");
	CHECK(fileSize(path) > 1000);
	CHECK(session.outputWidth() == w && session.outputHeight() == h);
	CHECK(session.stats().written >= 20);

	Movie movie;
	CHECK(readMovie(path, movie));
	CHECK(movie.completed);
	CHECK(movie.taggedRec709);
	CHECK(movie.width == w && movie.height == h);
	// The picture ran for 1.5 s from when the encoder opened, so the movie is about that long, not
	// sped up and not stretched.
	CHECK(movie.seconds > 0.6 && movie.seconds < 2.4);
	CHECK(movie.samples >= 15 && movie.samples <= 75);
	std::printf("  movie: %dx%d, %.2f s, %d samples, written %u, dropped %u, repeated %u\n", movie.width, movie.height,
		movie.seconds, movie.samples, session.stats().written, session.stats().dropped, session.stats().repeated);
	// Orientation and colours (H.264 is lossy, so a generous tolerance), and the picture really moved
	// on: the first frame is reddish, the last bluish.
	CHECK(movie.first.fixedQuadrantsOk(34));
	CHECK(movie.last.fixedQuadrantsOk(34));
	const Image& a = movie.first;
	const Image& b = movie.last;
	CHECK(a.rgba.size() == static_cast<std::size_t>(w) * h * 4 && b.rgba.size() == a.rgba.size());
	if (a.rgba.size() == static_cast<std::size_t>(w) * h * 4) {
		const unsigned char* pa = &a.rgba[(static_cast<std::size_t>(h / 4) * w + w / 4) * 4];
		const unsigned char* pb = &b.rgba[(static_cast<std::size_t>(h / 4) * w + w / 4) * 4];
		CHECK(pa[0] > pa[2] + 40);
		CHECK(pb[2] > pb[0] + 40);
		CHECK(b.near(w / 4, h / 4, 40, 40, 200, 40));
	}

	// The next recording of the same second gets -2, and two that start together never share a name.
	std::string next, again, error;
	CHECK(display_capture::chooseNewFilePath("", stem, "mov", next, error));
	CHECK(next == defaultFolder + "/" + stem + "-2.mov");
	CHECK(display_capture::chooseNewFilePath("", stem, "mov", again, error));
	CHECK(again == defaultFolder + "/" + stem + "-3.mov");
	display_capture::releasePath(next);
	display_capture::releasePath(again);
}

// Unit checks of the pieces that run on the audio thread.
void testAudioPieces() {
	using namespace display_recorder;

	// Volts to samples: +-5 V is full scale, beyond that is clipped hard, NaN is silence.
	CHECK(std::fabs(voltsToSample(0.f)) < 1e-9f);
	CHECK(std::fabs(voltsToSample(2.5f) - 0.5f) < 1e-6f);
	CHECK(std::fabs(voltsToSample(-5.f) + 1.f) < 1e-6f);
	CHECK(voltsToSample(5.f) == 1.f);
	CHECK(voltsToSample(12.f) == 1.f);
	CHECK(voltsToSample(-40.f) == -1.f);
	CHECK(voltsToSample(std::nanf("")) == 0.f);

	// Routing: both patched, only L, only R, none.
	float l = 0.f, r = 0.f;
	routeAudio(3, 0.25f, -0.5f, l, r);
	CHECK(l == 0.25f && r == -0.5f);
	routeAudio(1, 0.25f, -0.5f, l, r);
	CHECK(l == 0.25f && r == 0.25f);
	routeAudio(2, 0.25f, -0.5f, l, r);
	CHECK(l == -0.5f && r == -0.5f);
	routeAudio(0, 0.25f, -0.5f, l, r);
	CHECK(l == 0.f && r == 0.f);

	// The ring: order, overflow drops and counts and never blocks, a new owner starts empty and the old
	// owner gets nothing.
	AudioRing ring(16);
	CHECK(ring.capacity() == 16);
	const std::uint64_t first = ring.claim();
	for (int i = 0; i < 16; i++)
		CHECK(ring.push(static_cast<float>(i), static_cast<float>(-i)));
	CHECK(!ring.push(99.f, 99.f));
	CHECK(!ring.push(99.f, 99.f));
	CHECK(ring.dropped() == 2);
	float out[64];
	CHECK(ring.pop(first, out, 10) == 10);
	CHECK(out[0] == 0.f && out[1] == 0.f && out[18] == 9.f && out[19] == -9.f);
	CHECK(ring.push(100.f, 101.f));  // room again after the pop
	const std::uint64_t second = ring.claim();
	CHECK(ring.pop(first, out, 10) == 0);
	CHECK(ring.pop(second, out, 10) == 0);  // claim() discarded what was queued
	CHECK(ring.push(7.f, 8.f));
	CHECK(ring.pop(second, out, 10) == 1 && out[0] == 7.f && out[1] == 8.f);
}

struct AudioRun {
	display_recorder::SessionState state = display_recorder::SessionState::Failed;
	std::string path;
	display_recorder::SessionStats stats;
	long long pushed = 0;
	std::string error;
};

// Records `seconds` of picture while a ToneProducer feeds the ring. midway, if given, runs once halfway.
AudioRun recordWithAudio(const std::string& stem, int rate, double seconds, double hzL, double ampL, double hzR, double ampR,
	const std::function<void(ToneProducer&)>& midway) {
	using namespace display_recorder;
	const int w = 96, h = 64;
	auto ring = std::make_shared<AudioRing>();
	SessionOptions options;
	options.fps = Session::kFps;
	options.makeSink = [] { return createFrameSink(); };
	options.choosePath = [stem](std::string& path, std::string& error) {
		return display_capture::chooseNewFilePath("", stem, "mov", path, error);
	};
	options.pathDone = [](const std::string& path) { display_capture::releasePath(path); };
	options.audio = ring;
	options.audioSampleRate = rate;
	std::vector<unsigned char> frame = makeFrame(w, h, 200, 40, 40);
	Session session(options, frame.data(), w, h);
	ToneProducer producer(*ring, rate, hzL, ampL, hzR, ampR);
	const int frames = static_cast<int>(seconds * 30);
	const auto start = std::chrono::steady_clock::now();
	bool didMidway = false;
	for (int i = 1; i < frames; i++) {
		std::this_thread::sleep_until(start + std::chrono::microseconds(1000000 * i / 30));
		frame = makeFrame(w, h, 200 - i, 40, 40 + i);
		session.submit(frame.data(), w, h);
		if (!didMidway && i >= frames / 2 && midway) {
			didMidway = true;
			midway(producer);
		}
		if (session.state() != SessionState::Recording && session.state() != SessionState::Starting)
			break;
	}
	producer.stop();
	session.requestStop();
	CHECK(session.waitUntilDone(20000));
	AudioRun run;
	run.state = session.state();
	run.path = session.path();
	run.stats = session.stats();
	run.pushed = producer.framesPushed();
	run.error = session.error();
	return run;
}

void testRecordingWithAudio(const std::string& stem) {
	using namespace display_recorder;
	const double frameSeconds = 1.0 / 30.0;

	// Two different tones, about 2 s: 440 Hz at 2.5 V on L, 1000 Hz at 1.25 V on R.
	{
		AudioRun run = recordWithAudio(stem + "-tones", 48000, 2.0, 440.0, 0.5, 1000.0, 0.25, nullptr);
		CHECK(run.state == SessionState::Finished);
		CHECK(run.stats.hasAudio);
		CHECK(run.stats.audioDropped == 0);
		Movie movie;
		CHECK(readMovie(run.path, movie));
		CHECK(movie.completed);
		AudioInfo audio;
		CHECK(readAudio(run.path, audio, 48000));
		std::printf("  audio: %d track(s), %d channels, aac %d, track %.3f s, decoded %.3f s, movie %.3f s, pushed %.3f s\n", audio.tracks,
			audio.channels, audio.formatIsAac ? 1 : 0, audio.trackSeconds, audio.decodedSeconds, movie.seconds, run.pushed / 48000.0);
		std::printf("  L %.1f Hz rms %.3f peak %.3f, R %.1f Hz rms %.3f peak %.3f\n", audio.hz[0], audio.rms[0], audio.peak[0], audio.hz[1],
			audio.rms[1], audio.peak[1]);
		CHECK(audio.tracks == 1);
		CHECK(audio.channels == 2);
		CHECK(audio.formatIsAac);
		// The audio is as long as the sound that was fed in, and the movie as long as the audio, to one picture.
		CHECK(std::fabs(audio.decodedSeconds - run.stats.audioFrames / 48000.0) < frameSeconds + 0.03);
		CHECK(std::fabs(audio.decodedSeconds - run.pushed / 48000.0) < frameSeconds + 0.1);
		CHECK(std::fabs(movie.seconds - audio.decodedSeconds) < frameSeconds + 0.03);
		CHECK(audio.decodedSeconds > 1.7 && audio.decodedSeconds < 2.4);
		// The channels differ the way they were fed (AAC is lossy: loose tolerances).
		CHECK(std::fabs(audio.hz[0] - 440.0) < 12.0);
		CHECK(std::fabs(audio.hz[1] - 1000.0) < 25.0);
		CHECK(audio.rms[0] > 0.30 && audio.rms[0] < 0.40);   // 0.5 / sqrt 2 = 0.354
		CHECK(audio.rms[1] > 0.14 && audio.rms[1] < 0.21);   // 0.25 / sqrt 2 = 0.177
		CHECK(audio.rms[0] > audio.rms[1] * 1.6);
	}

	// A 44.1 kHz engine is written as it is, and a 192 kHz engine is converted (AAC tops out at 96 kHz).
	{
		AudioRun run = recordWithAudio(stem + "-441", 44100, 1.0, 300.0, 0.4, 300.0, 0.4, nullptr);
		CHECK(run.state == SessionState::Finished);
		AudioInfo audio;
		CHECK(readAudio(run.path, audio, 44100));
		CHECK(audio.tracks == 1 && audio.channels == 2);
		CHECK(std::fabs(audio.decodedSeconds - run.pushed / 44100.0) < frameSeconds + 0.1);
		CHECK(std::fabs(audio.hz[0] - 300.0) < 10.0);
	}
	{
		AudioRun run = recordWithAudio(stem + "-192", 192000, 1.0, 500.0, 0.4, 500.0, 0.4, nullptr);
		CHECK(run.state == SessionState::Finished);
		AudioInfo audio;
		CHECK(readAudio(run.path, audio, 48000));
		CHECK(audio.tracks == 1 && audio.channels == 2);
		CHECK(std::fabs(audio.decodedSeconds - run.pushed / 192000.0) < frameSeconds + 0.1);
		CHECK(std::fabs(audio.hz[0] - 500.0) < 15.0);
	}

	// High engine rates (Rack at 88.2/96/176.4/192/352.8 kHz): L and R must come out at the right pitch and level, never silent.
	{
		const int rates[] = {88200, 96000, 176400, 352800};
		for (int rate : rates) {
			AudioRun run = recordWithAudio(stem + "-hi" + std::to_string(rate), rate, 1.5, 440.0, 0.5, 1000.0, 0.25, nullptr);
			CHECK(run.state == SessionState::Finished);
			CHECK(run.stats.hasAudio);
			CHECK(run.stats.audioPeak > 0.4f && run.stats.audioPeak < 0.55f);
			AudioInfo audio;
			const int decodeRate = (rate % 44100 == 0) ? 44100 : 48000;
			CHECK(readAudio(run.path, audio, decodeRate));
			std::printf("  hi-rate %d: L %.1f Hz rms %.3f, R %.1f Hz rms %.3f, %.3f s (pushed %.3f s)\n", rate, audio.hz[0], audio.rms[0],
				audio.hz[1], audio.rms[1], audio.decodedSeconds, run.pushed / static_cast<double>(rate));
			CHECK(audio.tracks == 1 && audio.channels == 2);
			CHECK(std::fabs(audio.decodedSeconds - run.pushed / static_cast<double>(rate)) < frameSeconds + 0.1);
			CHECK(std::fabs(audio.hz[0] - 440.0) < 12.0);
			CHECK(std::fabs(audio.hz[1] - 1000.0) < 25.0);
			CHECK(audio.rms[0] > 0.30 && audio.rms[0] < 0.40);
			CHECK(audio.rms[1] > 0.14 && audio.rms[1] < 0.21);
		}
	}

	// A silent source (cables patched, nothing playing) is recorded as digital silence and reported as such.
	{
		AudioRun run = recordWithAudio(stem + "-silent", 176400, 1.0, 440.0, 0.0, 440.0, 0.0, nullptr);
		CHECK(run.state == SessionState::Finished);
		CHECK(run.stats.hasAudio && run.stats.audioPeak == 0.f);
		AudioInfo audio;
		CHECK(readAudio(run.path, audio, 44100));
		CHECK(audio.peak[0] < 0.001 && audio.peak[1] < 0.001);
	}

	// A paused engine (no samples for 0.6 s) pauses the movie: nothing crashes, and picture and sound stay together.
	{
		AudioRun run = recordWithAudio(stem + "-pause", 48000, 2.0, 440.0, 0.5, 440.0, 0.5, [](ToneProducer& producer) {
			producer.pause = true;
			std::this_thread::sleep_for(std::chrono::milliseconds(600));
			producer.pause = false;
		});
		CHECK(run.state == SessionState::Finished);
		Movie movie;
		CHECK(readMovie(run.path, movie));
		AudioInfo audio;
		CHECK(readAudio(run.path, audio, 48000));
		CHECK(audio.tracks == 1);
		CHECK(std::fabs(movie.seconds - audio.decodedSeconds) < frameSeconds + 0.05);
		CHECK(audio.decodedSeconds < 2.3);
	}

	// The engine sample rate changing mid-recording ends the recording cleanly; the file is complete up to there.
	{
		AudioRun run = recordWithAudio(stem + "-rate", 48000, 2.0, 440.0, 0.5, 440.0, 0.5, [](ToneProducer& producer) {
			producer.rateToReport = 44100;
		});
		CHECK(run.state == SessionState::Finished);
		CHECK(run.stats.stoppedForRateChange);
		Movie movie;
		CHECK(readMovie(run.path, movie));
		CHECK(movie.completed);
		AudioInfo audio;
		CHECK(readAudio(run.path, audio, 48000));
		CHECK(audio.tracks == 1);
		CHECK(audio.decodedSeconds > 0.5 && audio.decodedSeconds < 1.5);
	}

	// Nothing patched: no audio ring, no audio track, and the movie is the same as before audio existed.
	{
		const int w = 96, h = 64;
		SessionOptions options;
		options.fps = Session::kFps;
		options.makeSink = [] { return createFrameSink(); };
		options.choosePath = [stem](std::string& path, std::string& error) {
			return display_capture::chooseNewFilePath("", stem + "-silent", "mov", path, error);
		};
		options.pathDone = [](const std::string& path) { display_capture::releasePath(path); };
		std::vector<unsigned char> frame = makeFrame(w, h, 200, 40, 40);
		Session session(options, frame.data(), w, h);
		const auto start = std::chrono::steady_clock::now();
		for (int i = 1; i < 60; i++) {
			std::this_thread::sleep_until(start + std::chrono::microseconds(1000000 * i / 30));
			frame = makeFrame(w, h, 200 - i, 40, 40 + i);
			session.submit(frame.data(), w, h);
		}
		session.requestStop();
		CHECK(session.waitUntilDone(20000));
		CHECK(session.state() == SessionState::Finished);
		CHECK(!session.stats().hasAudio);
		AudioInfo audio;
		CHECK(readAudio(session.path(), audio, 48000));
		CHECK(audio.tracks == 0);
		Movie movie;
		CHECK(readMovie(session.path(), movie));
		CHECK(movie.completed && movie.samples > 10);
		CHECK(movie.seconds > 1.5 && movie.seconds < 2.5);
	}
}

}  // namespace

int main() {
	@autoreleasepool {
		// Everything happens under build/display_capture_smoke_tmp, with HOME pointing into it.
		NSString* cwd = [[NSFileManager defaultManager] currentDirectoryPath];
		const std::string root = std::string([cwd UTF8String]) + "/build/display_capture_smoke_tmp";
		removeTree(root);
		makeDirectory(root + "/home");
		::setenv("HOME", (root + "/home").c_str(), 1);

		using namespace display_capture;
		const std::string defaultFolder = root + "/home/Pictures/Video Research Unit";
		CHECK(homeDirectory() == root + "/home");
		CHECK(defaultCaptureFolder() == defaultFolder);
		CHECK(resolveCaptureFolder("") == defaultFolder);
		CHECK(resolveCaptureFolder(root + "/not/there") == defaultFolder);

		// vru-YYYY-MM-DD_HH-MM-SS
		const std::string stem = timestampStem(std::time(nullptr));
		CHECK(stem.size() == 23);
		CHECK(startsWith(stem, "vru-20"));
		CHECK(stem[8] == '-' && stem[11] == '-' && stem[14] == '_' && stem[17] == '-' && stem[20] == '-');
		CHECK(timestampStem(1794000000) != timestampStem(1794000001));

		testScreenshot(root, defaultFolder, stem);
		testRecording(defaultFolder, stem);
		testAudioPieces();
		testRecordingWithAudio(stem);

		// Keep the files when something failed, so they can be looked at.
		if (failures == 0 && !std::getenv("VRU_KEEP_SMOKE_FILES"))
			removeTree(root);
	}
	std::printf("display_capture_smoke: %d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
