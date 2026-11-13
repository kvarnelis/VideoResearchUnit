#pragma once

#include <array>
#include <cstddef>
#include <mutex>

// Hands out ids 1..N to module instances. An id is held until release() and
// is never handed to a second owner while it is claimed. Claim and release run
// on the UI/construction thread only, never on the audio thread; the one call
// the audio thread may make is tryClaim(), which never waits.
template <size_t N>
class VideoSlotAllocator {
public:
	// Claims preferredId when it is valid and free, otherwise the lowest free id.
	// Returns 0 when every id is in use.
	int claim(int preferredId = 0) {
		std::lock_guard<std::mutex> lock(mutex);
		if (preferredId >= 1 && preferredId <= static_cast<int>(N) && !used[static_cast<size_t>(preferredId - 1)]) {
			used[static_cast<size_t>(preferredId - 1)] = true;
			return preferredId;
		}
		return claimLowest();
	}

	// claim() for a caller that must not wait, such as a module retrying from process() after its
	// constructor found the pool full. Returns 0 at once when every id is in use or another thread
	// holds the lock; the caller asks again later.
	int tryClaim() {
		std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
		if (!lock.owns_lock()) {
			return 0;
		}
		return claimLowest();
	}

	void release(int id) {
		if (id < 1 || id > static_cast<int>(N)) {
			return;
		}
		std::lock_guard<std::mutex> lock(mutex);
		used[static_cast<size_t>(id - 1)] = false;
	}

private:
	// The lowest free id, marked used; 0 when none is free. The caller holds the lock.
	int claimLowest() {
		for (size_t i = 0; i < N; ++i) {
			if (!used[i]) {
				used[i] = true;
				return static_cast<int>(i) + 1;
			}
		}
		return 0;
	}

	std::mutex mutex;
	std::array<bool, N> used {};
};
