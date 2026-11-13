#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// Soundtrack hand-off for the Video Player. Pure C++17, no Rack dependency.
//
// The worker thread decodes the soundtrack, already at the engine's sample rate, and pushes stereo
// frames into an AudioRing. The audio thread reads them back at the playback speed. The ring is a
// single-producer single-consumer queue built once, so the audio thread never blocks, locks or
// allocates.
//
// Realigning (a seek, a loop wrap, a drift correction) never touches what the audio thread owns.
// The worker calls flush(): it records where the new material starts (the mark) and bumps a
// generation counter. The audio thread sees the new generation, fades out over a few milliseconds
// on what it was playing, jumps its read position to the mark and fades in. Nothing in the ring is
// ever rewritten behind the reader.

namespace video_player {

class AudioRing {
public:
	// capacityFrames is rounded up to a power of two.
	explicit AudioRing(std::size_t capacityFrames) {
		std::size_t capacity = 16;
		while (capacity < capacityFrames) {
			capacity <<= 1;
		}
		mask_ = capacity - 1;
		data_.assign(capacity * 2, 0.f);
	}

	std::size_t capacity() const {
		return mask_ + 1;
	}

	// ---- Producer (worker thread) ----

	// Frames that can be pushed now without overwriting anything the reader may still read.
	std::size_t writable() const {
		const uint64_t used = head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire);
		return used >= capacity() ? 0 : capacity() - static_cast<std::size_t>(used);
	}
	// Pushes `frames` interleaved stereo frames. The caller keeps frames <= writable().
	void push(const float* stereo, std::size_t frames) {
		uint64_t head = head_.load(std::memory_order_relaxed);
		for (std::size_t i = 0; i < frames; ++i) {
			const std::size_t slot = static_cast<std::size_t>(head & mask_) * 2;
			data_[slot] = stereo[2 * i];
			data_[slot + 1] = stereo[2 * i + 1];
			++head;
		}
		head_.store(head, std::memory_order_release);
	}
	// Everything pushed from now on belongs to a new stretch of sound. What is queued already is
	// played out only as far as the fade-out needs.
	void flush() {
		mark_.store(head_.load(std::memory_order_relaxed), std::memory_order_relaxed);
		generation_.fetch_add(1, std::memory_order_release);
	}
	// Frames pushed since the last flush that the reader has not passed yet.
	uint64_t pendingNewFrames() const {
		const uint64_t head = head_.load(std::memory_order_relaxed);
		const uint64_t mark = mark_.load(std::memory_order_relaxed);
		const uint64_t tail = tail_.load(std::memory_order_acquire);
		return head - (tail > mark ? tail : mark);
	}
	// True once the reader has moved to the newest generation and is past the mark.
	bool readerCaughtUp() const {
		return seen_.load(std::memory_order_acquire) == generation_.load(std::memory_order_relaxed) &&
			tail_.load(std::memory_order_acquire) >= mark_.load(std::memory_order_relaxed);
	}
	// Frames between the reader and the writer, counted from the reader's position. Only meaningful
	// when readerCaughtUp().
	uint64_t buffered() const {
		return head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire);
	}
	uint64_t underruns() const {
		return underruns_.load(std::memory_order_relaxed);
	}

	// ---- Consumer (audio thread) ----

	uint64_t head() const {
		return head_.load(std::memory_order_acquire);
	}
	uint64_t tail() const {
		return tail_.load(std::memory_order_relaxed);
	}
	void setTail(uint64_t tail) {
		tail_.store(tail, std::memory_order_release);
	}
	// If the worker flushed since the reader last looked, returns true with the mark to jump to and
	// the generation to acknowledge with acknowledge().
	bool flushPending(uint64_t seenGeneration, uint64_t& mark, uint64_t& generation) const {
		for (;;) {
			const uint64_t first = generation_.load(std::memory_order_acquire);
			if (first == seenGeneration) {
				return false;
			}
			mark = mark_.load(std::memory_order_relaxed);
			const uint64_t second = generation_.load(std::memory_order_acquire);
			if (first == second) {
				generation = first;
				return true;
			}
		}
	}
	void acknowledge(uint64_t generation) {
		seen_.store(generation, std::memory_order_release);
	}
	void countUnderrun() {
		underruns_.fetch_add(1, std::memory_order_relaxed);
	}
	// Both channels of frame `index` (an absolute count, not wrapped).
	void frame(uint64_t index, float& left, float& right) const {
		const std::size_t slot = static_cast<std::size_t>(index & mask_) * 2;
		left = data_[slot];
		right = data_[slot + 1];
	}

private:
	std::vector<float> data_;
	std::size_t mask_ = 0;
	std::atomic<uint64_t> head_{0};
	std::atomic<uint64_t> tail_{0};
	std::atomic<uint64_t> mark_{0};
	std::atomic<uint64_t> generation_{0};
	std::atomic<uint64_t> seen_{0};
	std::atomic<uint64_t> underruns_{0};
};

// The audio thread's side: reads the ring at a variable speed with linear interpolation (so 2x is
// faster and higher, like tape) and ramps the level so that starting, stopping and jumping never
// click. Owned and called only by the audio thread.
class AudioReader {
public:
	// Output in -1..1 for each channel. Silence while not `active`, at speed <= 0, or when the ring
	// has nothing to give.
	void tick(AudioRing& ring, bool active, float speed, float sampleRate, float& left, float& right) {
		left = 0.f;
		right = 0.f;
		const float rate = sampleRate >= 1000.f ? sampleRate : 48000.f;
		const float step = 1.f / (kFadeSeconds * rate);
		const bool want = active && speed > 0.f;

		uint64_t tail = ring.tail();
		uint64_t limit = ring.head();
		float target = want ? 1.f : 0.f;

		uint64_t mark = 0;
		uint64_t generation = 0;
		const bool jump = ring.flushPending(seenGeneration_, mark, generation);
		if (jump) {
			target = 0.f;
			limit = mark;  // play out the old sound only for the length of the fade
		}
		if (gain_ < target) {
			gain_ = gain_ + step < target ? gain_ + step : target;
		}
		else if (gain_ > target) {
			gain_ = gain_ - step > target ? gain_ - step : target;
		}
		if (jump && gain_ <= 0.f) {
			if (mark > tail) {
				tail = mark;
				ring.setTail(tail);
			}
			frac_ = 0.f;
			seenGeneration_ = generation;
			ring.acknowledge(generation);
			underrunReported_ = false;
			return;
		}
		if (gain_ <= 0.f && target <= 0.f) {
			return;
		}
		if (limit > tail) {
			// Running out of sound: ease the level down over the last few milliseconds, so the cut to
			// silence (an underrun, or the end of a soundtrack) does not click.
			const float fadeFrames = kFadeSeconds * rate * (speed > 1.f ? speed : 1.f);
			const float cap = static_cast<float>(limit - tail) / fadeFrames;
			if (gain_ > cap) {
				gain_ = cap;
			}
		}
		if (limit < tail + 2) {
			// Nothing to read: silence, and come back in with a fade.
			gain_ = 0.f;
			if (!underrunReported_ && want) {
				ring.countUnderrun();
				underrunReported_ = true;
			}
			return;
		}
		underrunReported_ = false;

		float aL = 0.f;
		float aR = 0.f;
		float bL = 0.f;
		float bR = 0.f;
		ring.frame(tail, aL, aR);
		ring.frame(tail + 1, bL, bR);
		left = (aL + (bL - aL) * frac_) * gain_;
		right = (aR + (bR - aR) * frac_) * gain_;

		frac_ += speed > 0.f ? speed : 0.f;
		const uint64_t whole = static_cast<uint64_t>(frac_);
		frac_ -= static_cast<float>(whole);
		if (whole > 0) {
			ring.setTail(tail + whole);
		}
	}

	static constexpr float kFadeSeconds = 0.004f;

private:
	float gain_ = 0.f;
	float frac_ = 0.f;
	uint64_t seenGeneration_ = 0;
	bool underrunReported_ = false;
};

}  // namespace video_player
