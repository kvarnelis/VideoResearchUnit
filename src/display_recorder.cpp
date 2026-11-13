#include "display_recorder.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace display_recorder {

// State shared between the caller's thread and the worker. Held by shared_ptr, and the worker owns a
// reference, so it stays valid even if the Session was destroyed while the worker is still finishing.
//
// The mailbox is a triple buffer: three frame slots, each owned by exactly one party at a time.
//  - uiIndex: the slot the caller fills. Only the caller reads or changes uiIndex.
//  - midIndex: the newest finished slot, waiting for the worker. Changed only under `mutex`.
//  - workerIndex: the slot the worker reads (the frame on show). Only the worker reads or changes it.
// The caller copies a frame into its slot with no lock held, then swaps uiIndex and midIndex under the
// lock. The worker swaps midIndex and workerIndex under the lock when a fresh frame is waiting. So the
// copy never waits for the worker, and the worker never waits for a copy.
struct SessionShared {
	SessionOptions options;
	int fps = Session::kFps;
	int outWidth = 2;
	int outHeight = 2;

	mutable std::mutex mutex;
	std::condition_variable changed;

	std::vector<unsigned char> slots[3];
	int slotWidth[3] = {0, 0, 0};
	int slotHeight[3] = {0, 0, 0};
	int uiIndex = 0;
	int midIndex = 1;
	int workerIndex = 2;
	bool midFresh = false;  // under mutex: midIndex holds a frame the worker has not taken

	bool stopRequested = false;  // under mutex
	bool finished = false;       // under mutex: the worker has ended
	SessionState state = SessionState::Starting;
	std::string path;
	std::string error;
	SessionStats stats;
	std::uint64_t audioToken = 0;  // claim on the audio ring, taken on the caller's thread
};

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kShutdownWaitMs = 15000;
constexpr std::chrono::milliseconds kMinSubmitGap(14);

void setFailed(SessionShared& s, const std::string& why) {
	{
		std::lock_guard<std::mutex> lock(s.mutex);
		s.state = SessionState::Failed;
		s.error = why.empty() ? std::string("recording failed") : why;
	}
	s.changed.notify_all();
}

void publishStats(SessionShared& s, const SessionStats& stats) {
	std::lock_guard<std::mutex> lock(s.mutex);
	s.stats = stats;
}

// Bilinear resize of an RGBA image (rows packed, top row first).
void scaleBilinear(const unsigned char* src, int srcW, int srcH, std::vector<unsigned char>& dst, int dstW, int dstH) {
	dst.resize(static_cast<std::size_t>(dstW) * static_cast<std::size_t>(dstH) * 4);
	std::vector<int> xLeft(static_cast<std::size_t>(dstW)), xRight(static_cast<std::size_t>(dstW)), xFrac(static_cast<std::size_t>(dstW));
	for (int x = 0; x < dstW; x++) {
		float pos = (static_cast<float>(x) + 0.5f) * static_cast<float>(srcW) / static_cast<float>(dstW) - 0.5f;
		if (pos < 0.f)
			pos = 0.f;
		int left = std::min(static_cast<int>(pos), srcW - 1);
		int right = std::min(left + 1, srcW - 1);
		xLeft[static_cast<std::size_t>(x)] = left * 4;
		xRight[static_cast<std::size_t>(x)] = right * 4;
		xFrac[static_cast<std::size_t>(x)] = std::min(255, static_cast<int>((pos - static_cast<float>(left)) * 256.f));
	}
	const std::size_t srcRow = static_cast<std::size_t>(srcW) * 4;
	for (int y = 0; y < dstH; y++) {
		float pos = (static_cast<float>(y) + 0.5f) * static_cast<float>(srcH) / static_cast<float>(dstH) - 0.5f;
		if (pos < 0.f)
			pos = 0.f;
		int top = std::min(static_cast<int>(pos), srcH - 1);
		int bottom = std::min(top + 1, srcH - 1);
		int yFrac = std::min(255, static_cast<int>((pos - static_cast<float>(top)) * 256.f));
		const unsigned char* rowTop = src + static_cast<std::size_t>(top) * srcRow;
		const unsigned char* rowBottom = src + static_cast<std::size_t>(bottom) * srcRow;
		unsigned char* out = dst.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(dstW) * 4;
		for (int x = 0; x < dstW; x++) {
			const std::size_t i = static_cast<std::size_t>(x);
			const int l = xLeft[i];
			const int r = xRight[i];
			const int fx = xFrac[i];
			for (int c = 0; c < 4; c++) {
				int upper = rowTop[l + c] * (256 - fx) + rowTop[r + c] * fx;
				int lower = rowBottom[l + c] * (256 - fx) + rowBottom[r + c] * fx;
				out[x * 4 + c] = static_cast<unsigned char>((upper * (256 - yFrac) + lower * yFrac) >> 16);
			}
		}
	}
}

struct FrameView {
	const unsigned char* data = nullptr;
	int rowBytes = 0;
};

// The frame the worker has on show, in the recording size. A frame of the recording size is used as it
// is, and one a pixel larger is cropped by passing its longer row length. Anything else is scaled once
// and kept in `scaled` until the next fresh frame arrives.
FrameView prepareFrame(SessionShared& s, std::vector<unsigned char>& scaled, bool& scaledValid) {
	const int index = s.workerIndex;
	const int w = s.slotWidth[index];
	const int h = s.slotHeight[index];
	const unsigned char* data = s.slots[index].data();
	FrameView view;
	if (w >= s.outWidth && w - s.outWidth <= 1 && h >= s.outHeight && h - s.outHeight <= 1) {
		view.data = data;
		view.rowBytes = w * 4;
		return view;
	}
	if (!scaledValid) {
		scaleBilinear(data, w, h, scaled, s.outWidth, s.outHeight);
		scaledValid = true;
	}
	view.data = scaled.data();
	view.rowBytes = s.outWidth * 4;
	return view;
}

void runSession(SessionShared& s, std::string& chosenPath) {
	const int fps = s.fps;
	const double period = 1.0 / static_cast<double>(fps);

	std::string path;
	std::string error;
	if (!s.options.choosePath || !s.options.choosePath(path, error)) {
		setFailed(s, error.empty() ? std::string("cannot choose a file name") : error);
		return;
	}
	chosenPath = path;
	{
		std::lock_guard<std::mutex> lock(s.mutex);
		s.path = path;
	}

	std::unique_ptr<FrameSink> sink = s.options.makeSink ? s.options.makeSink() : nullptr;
	if (!sink) {
		setFailed(s, "no video encoder");
		return;
	}
	AudioRing* ring = s.options.audio.get();
	const int audioRate = s.options.audioSampleRate;
	if (ring && audioRate > 0) {
		sink->enableAudio(audioRate);  // false: this sink cannot, and the movie has no sound
	}
	if (!sink->open(path, s.outWidth, s.outHeight, fps)) {
		setFailed(s, sink->lastError());
		return;
	}
	const bool audioOn = ring && audioRate > 0 && sink->hasAudio();
	{
		std::lock_guard<std::mutex> lock(s.mutex);
		if (s.state == SessionState::Starting)
			s.state = SessionState::Recording;
	}

	// The timeline starts now, with the encoder open, so opening it is not counted as recording time.
	// Audio that arrived while the encoder was opening is older than the timeline and is thrown away.
	constexpr int kAudioChunk = 4096;
	std::vector<float> chunk(static_cast<std::size_t>(kAudioChunk) * 2);
	std::vector<float> pend;  // interleaved frames popped from the ring and not yet given to the encoder
	std::size_t pendHead = 0;  // frames at the front of pend already given to the encoder
	std::uint64_t audioPopped = 0;   // frames taken from the ring (plus silence for dropped ones): the master clock
	std::uint64_t audioWritten = 0;  // frames given to the encoder
	std::uint64_t seenDropped = 0;
	if (audioOn) {
		pend.reserve(static_cast<std::size_t>(audioRate) * 2);  // one second; grows only if the encoder stalls
		while (ring->pop(s.audioToken, chunk.data(), kAudioChunk) > 0) {
		}
		seenDropped = ring->dropped();
	}
	const Clock::time_point start = Clock::now();
	auto tickTime = [&](long long tick) {
		return start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(static_cast<double>(tick) * period));
	};
	auto tickAt = [&](Clock::time_point when) -> long long {
		double seconds = std::chrono::duration<double>(when - start).count();
		return seconds <= 0.0 ? 0 : static_cast<long long>(seconds / period + 1e-6);
	};

	std::vector<unsigned char> scaled;
	bool scaledValid = false;
	bool fresh = true;  // the frame on show has not been written yet
	SessionStats stats;
	stats.hasAudio = audioOn;
	long long tick = 0;
	long long lastWritten = -1;

	// Takes the newest frame from the mailbox, if there is one. Call with s.mutex held.
	auto takeNewest = [&]() {
		if (!s.midFresh)
			return;
		std::swap(s.midIndex, s.workerIndex);
		s.midFresh = false;
		scaledValid = false;
		fresh = true;
	};
	// Writes the frame on show at the given tick. false: the encoder failed.
	auto writeFrame = [&](long long at) -> bool {
		FrameView view = prepareFrame(s, scaled, scaledValid);
		if (!sink->append(view.data, view.rowBytes, static_cast<double>(at) * period))
			return false;
		stats.written++;
		if (!fresh)
			stats.repeated++;
		fresh = false;
		lastWritten = at;
		return true;
	};
	// Moves audio from the ring toward the encoder. false: the encoder failed.
	auto pumpAudio = [&]() -> bool {
		if (!audioOn)
			return true;
		for (;;) {
			const std::size_t n = ring->pop(s.audioToken, chunk.data(), kAudioChunk);
			if (n == 0)
				break;
			pend.insert(pend.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n * 2));
			for (std::size_t i = 0; i < n * 2; ++i)
				stats.audioPeak = std::max(stats.audioPeak, std::fabs(chunk[i]));
			audioPopped += n;
		}
		const std::uint64_t dropped = ring->dropped();
		if (dropped > seenDropped) {
			// Frames were lost because the ring was full. Silence of the same length keeps the clock true.
			const std::uint64_t lost = dropped - seenDropped;
			seenDropped = dropped;
			pend.insert(pend.end(), static_cast<std::size_t>(lost) * 2, 0.f);
			audioPopped += lost;
			stats.audioDropped += lost;
		}
		const std::size_t pendFrames = pend.size() / 2;
		while (pendHead < pendFrames && sink->audioReady()) {
			const int count = static_cast<int>(std::min<std::size_t>(pendFrames - pendHead, kAudioChunk));
			const double pts = static_cast<double>(audioWritten) / static_cast<double>(audioRate);
			if (!sink->appendAudio(pend.data() + pendHead * 2, count, pts))
				return false;
			audioWritten += static_cast<std::uint64_t>(count);
			pendHead += static_cast<std::size_t>(count);
		}
		if (pendHead == pendFrames) {
			pend.clear();
			pendHead = 0;
		}
		else if (pendHead > 65536) {
			pend.erase(pend.begin(), pend.begin() + static_cast<std::ptrdiff_t>(pendHead * 2));
			pendHead = 0;
		}
		stats.audioFrames = audioWritten;
		return true;
	};
	// The picture number the audio clock has reached. Picture n belongs at n / fps seconds.
	auto audioTick = [&]() -> long long {
		return static_cast<long long>(audioPopped * static_cast<std::uint64_t>(fps) / static_cast<std::uint64_t>(audioRate));
	};
	auto abandon = [&]() {
		std::string why = sink->lastError();
		sink->cancel();
		publishStats(s, stats);
		setFailed(s, why);
	};

	bool rateChanged = false;
	for (;;) {
		bool stop = false;
		{
			std::unique_lock<std::mutex> lock(s.mutex);
			if (audioOn) {
				// The clock is the audio, so look often and let the samples decide when a picture is due.
				s.changed.wait_for(lock, std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(period * 0.5)),
					[&] { return s.stopRequested; });
			}
			else {
				s.changed.wait_until(lock, tickTime(tick), [&] { return s.stopRequested; });
			}
			stop = s.stopRequested;
			if (!stop)
				takeNewest();
		}
		if (stop)
			break;

		long long now;
		if (audioOn) {
			const float producerRate = ring->sampleRate();
			if (producerRate > 0.f && std::fabs(producerRate - static_cast<float>(audioRate)) > 0.5f) {
				rateChanged = true;  // the engine's rate changed: end the recording here, cleanly
				break;
			}
			if (!pumpAudio()) {
				abandon();
				return;
			}
			now = audioTick();
			if (now < tick)
				continue;  // the audio has not reached the next picture yet (or the engine is paused)
		}
		else {
			// Late: skip the ticks that were missed rather than writing them in a burst.
			now = tickAt(Clock::now());
		}
		if (now > tick) {
			stats.dropped += static_cast<std::uint32_t>(now - tick);
			tick = now;
		}
		if (!sink->readyForFrame()) {
			stats.dropped++;
		}
		else if (!writeFrame(tick)) {
			abandon();
			return;
		}
		tick++;
		publishStats(s, stats);
	}

	// Stop was requested: show the picture as it is now, then close the file.
	{
		std::lock_guard<std::mutex> lock(s.mutex);
		s.state = SessionState::Finishing;
		takeNewest();
	}
	if (audioOn) {
		// Give the encoder every sample that was queued, waiting a little if it is busy.
		const Clock::time_point giveUp = Clock::now() + std::chrono::seconds(5);
		for (;;) {
			if (!pumpAudio()) {
				abandon();
				return;
			}
			if (pendHead >= pend.size() / 2 || Clock::now() > giveUp)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}
	const long long now = audioOn ? audioTick() : tickAt(Clock::now());
	if (now > lastWritten) {
		// The first frame always goes in, at time zero, so the movie never starts with a gap.
		const long long at = lastWritten < 0 ? 0 : now;
		if (lastWritten < 0 || sink->readyForFrame()) {
			if (!writeFrame(at)) {
				abandon();
				return;
			}
		}
		else {
			stats.dropped++;
		}
	}
	publishStats(s, stats);
	if (lastWritten < 0) {
		sink->cancel();
		setFailed(s, "no frames were recorded");
		return;
	}
	double endSeconds = static_cast<double>(std::max(lastWritten, now) + 1) * period;
	if (audioOn) {
		// Never cut sound off: the movie lasts at least as long as the audio, and at least one picture.
		endSeconds = std::max(static_cast<double>(lastWritten + 1) * period,
			static_cast<double>(audioWritten) / static_cast<double>(audioRate));
		stats.stoppedForRateChange = rateChanged;
	}
	publishStats(s, stats);
	if (!sink->finish(endSeconds)) {
		abandon();
		return;
	}
	{
		std::lock_guard<std::mutex> lock(s.mutex);
		s.state = SessionState::Finished;
	}
	s.changed.notify_all();
}

void workerMain(std::shared_ptr<SessionShared> shared) {
	SessionShared& s = *shared;
	std::string chosenPath;
	try {
		runSession(s, chosenPath);
	}
	catch (const std::exception& e) {
		setFailed(s, std::string("recorder error: ") + e.what());
	}
	catch (...) {
		setFailed(s, "recorder error");
	}
	try {
		if (!chosenPath.empty() && s.options.pathDone)
			s.options.pathDone(chosenPath);
	}
	catch (...) {
	}
	{
		std::lock_guard<std::mutex> lock(s.mutex);
		s.finished = true;
	}
	s.changed.notify_all();
}

}  // namespace

Session::Session(SessionOptions options, const unsigned char* firstRgba, int width, int height)
	: shared(std::make_shared<SessionShared>()) {
	SessionShared& s = *shared;
	s.options = std::move(options);
	s.fps = s.options.fps > 0 ? s.options.fps : static_cast<int>(kFps);
	if (!firstRgba || width < 2 || height < 2) {
		s.state = SessionState::Failed;
		s.error = "the picture is too small to record";
		s.finished = true;
		return;
	}
	if (s.options.audio && s.options.audioSampleRate > 0) {
		s.audioToken = s.options.audio->claim();
	}
	s.outWidth = width & ~1;
	s.outHeight = height & ~1;
	// The first frame goes straight into the worker's slot, so it is there when the worker starts.
	const std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
	s.slots[s.workerIndex].assign(firstRgba, firstRgba + bytes);
	s.slotWidth[s.workerIndex] = width;
	s.slotHeight[s.workerIndex] = height;
	try {
		std::shared_ptr<SessionShared> keep = shared;
		worker = std::thread([keep]() { workerMain(keep); });
	}
	catch (...) {
		s.state = SessionState::Failed;
		s.error = "cannot start the recorder thread";
		s.finished = true;
	}
}

Session::~Session() {
	if (!shared)
		return;
	requestStop();
	if (worker.joinable()) {
		if (waitUntilDone(kShutdownWaitMs))
			worker.join();
		else
			worker.detach();  // the encoder is stuck; the worker ends on its own and releases its state
	}
}

void Session::submit(const unsigned char* rgba, int width, int height) {
	if (!rgba || width <= 0 || height <= 0)
		return;
	const Clock::time_point now = Clock::now();
	if (now - lastSubmit < kMinSubmitGap)
		return;
	if (!active())
		return;
	lastSubmit = now;
	SessionShared& s = *shared;
	const int index = s.uiIndex;
	const std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
	s.slots[index].assign(rgba, rgba + bytes);
	s.slotWidth[index] = width;
	s.slotHeight[index] = height;
	std::lock_guard<std::mutex> lock(s.mutex);
	std::swap(s.uiIndex, s.midIndex);
	s.midFresh = true;
}

void Session::requestStop() {
	{
		std::lock_guard<std::mutex> lock(shared->mutex);
		shared->stopRequested = true;
	}
	shared->changed.notify_all();
}

bool Session::waitUntilDone(int timeoutMs) {
	std::unique_lock<std::mutex> lock(shared->mutex);
	return shared->changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return shared->finished; });
}

SessionState Session::state() const {
	std::lock_guard<std::mutex> lock(shared->mutex);
	return shared->state;
}

bool Session::active() const {
	std::lock_guard<std::mutex> lock(shared->mutex);
	return !shared->stopRequested && (shared->state == SessionState::Starting || shared->state == SessionState::Recording);
}

std::string Session::path() const {
	std::lock_guard<std::mutex> lock(shared->mutex);
	return shared->path;
}

std::string Session::error() const {
	std::lock_guard<std::mutex> lock(shared->mutex);
	return shared->error;
}

SessionStats Session::stats() const {
	std::lock_guard<std::mutex> lock(shared->mutex);
	return shared->stats;
}

int Session::outputWidth() const {
	return shared->outWidth;
}

int Session::outputHeight() const {
	return shared->outHeight;
}

}  // namespace display_recorder
