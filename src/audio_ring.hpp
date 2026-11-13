#pragma once

// A lock-free ring of stereo audio frames from one producer thread to one consumer thread.
//
// Rack-free, so the Display recorder and its tests can use it on their own. The producer is Rack's
// audio thread (Display::process()). The consumer is the recorder's worker thread.
//
// The producer never locks, allocates or waits: push() writes two floats and moves one counter, or,
// when the ring is full, drops the frame and counts it. The storage is allocated once, in the
// constructor, on whatever thread builds the ring (the UI thread).
//
// Several consumers may exist over the life of one ring (a recording that is still closing, then a
// new one). claim() hands the ring to a new consumer and discards what is queued; a consumer that
// was replaced gets nothing from pop() afterwards.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace display_recorder {

class AudioRing {
public:
	// capacityFrames is rounded up to a power of two.
	explicit AudioRing(std::size_t capacityFrames = kDefaultCapacityFrames) {
		std::size_t size = 16;
		while (size < capacityFrames) {
			size <<= 1;
		}
		capacity_ = size;
		mask_ = size - 1;
		data_.reset(new float[size * 2]());
	}
	AudioRing(const AudioRing&) = delete;
	AudioRing& operator=(const AudioRing&) = delete;

	// About 2.7 seconds at 48 kHz: 1 MB.
	static constexpr std::size_t kDefaultCapacityFrames = 1u << 17;

	// ---- Producer thread only. Real-time safe: no lock, no allocation, no system call. ---------

	// false: the ring was full and the frame was dropped and counted.
	bool push(float left, float right) {
		const std::uint64_t h = head_.load(std::memory_order_relaxed);
		const std::uint64_t t = tail_.load(std::memory_order_acquire);
		if (h - t >= capacity_) {
			dropped_.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		float* slot = data_.get() + (h & mask_) * 2;
		slot[0] = left;
		slot[1] = right;
		head_.store(h + 1, std::memory_order_release);
		return true;
	}
	// The producer's sample rate. The consumer compares it with the rate the recording started at.
	void setSampleRate(float rate) {
		rate_.store(rate, std::memory_order_relaxed);
	}

	// ---- Consumer side ---------------------------------------------------------------------------

	// Makes the caller the only consumer and discards what is queued. Returns the caller's token for pop().
	// Call from one thread at a time (the UI thread).
	std::uint64_t claim() {
		const std::uint64_t token = owner_.fetch_add(1, std::memory_order_acq_rel) + 1;
		tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
		return token;
	}
	// Copies up to maxFrames interleaved frames into out and removes them. Returns how many. Returns 0 when
	// the token is no longer the owner.
	std::size_t pop(std::uint64_t token, float* out, std::size_t maxFrames) {
		if (owner_.load(std::memory_order_acquire) != token) {
			return 0;
		}
		std::uint64_t t = tail_.load(std::memory_order_acquire);
		const std::uint64_t h = head_.load(std::memory_order_acquire);
		std::size_t n = static_cast<std::size_t>(h - t);
		if (n > maxFrames) {
			n = maxFrames;
		}
		if (n == 0) {
			return 0;
		}
		for (std::size_t i = 0; i < n; ++i) {
			const float* slot = data_.get() + ((t + i) & mask_) * 2;
			out[i * 2] = slot[0];
			out[i * 2 + 1] = slot[1];
		}
		// If a new owner claimed the ring while we copied, the tail has moved and this copy is stale.
		if (!tail_.compare_exchange_strong(t, t + n, std::memory_order_acq_rel)) {
			return 0;
		}
		return n;
	}
	// Frames dropped because the ring was full, since the ring was built.
	std::uint64_t dropped() const {
		return dropped_.load(std::memory_order_relaxed);
	}
	float sampleRate() const {
		return rate_.load(std::memory_order_relaxed);
	}
	std::size_t capacity() const {
		return capacity_;
	}

private:
	std::size_t capacity_ = 0;
	std::size_t mask_ = 0;
	std::unique_ptr<float[]> data_;
	alignas(64) std::atomic<std::uint64_t> head_ {0};
	alignas(64) std::atomic<std::uint64_t> tail_ {0};
	alignas(64) std::atomic<std::uint64_t> dropped_ {0};
	std::atomic<std::uint64_t> owner_ {0};
	std::atomic<float> rate_ {0.f};
};

// The recorder's fixed rule for turning Rack volts into a sample: +-5 V is full scale, anything beyond
// is clipped hard at +-1.0, and a NaN becomes silence.
inline float voltsToSample(float volts) {
	if (!(volts == volts)) {
		return 0.f;
	}
	const float v = volts * 0.2f;
	return v > 1.f ? 1.f : (v < -1.f ? -1.f : v);
}

// Which channels carry a cable decides what the two recorded channels hear.
// bit 0: L patched, bit 1: R patched.
inline void routeAudio(unsigned patchedMask, float l, float r, float& outL, float& outR) {
	if ((patchedMask & 3u) == 3u) {
		outL = l;
		outR = r;
	}
	else if (patchedMask & 1u) {
		outL = outR = l;
	}
	else if (patchedMask & 2u) {
		outL = outR = r;
	}
	else {
		outL = outR = 0.f;
	}
}

}  // namespace display_recorder
