#pragma once

// A small set of helper threads that stay alive between pictures, so a node that draws in bands (Shapes) does not
// start and join several threads every frame.
//
// parallelChunks(count, fn) calls fn(c) once for every c in [0, count), spread over the calling thread and the helpers.
// The calling thread always takes part and takes chunks until none are left, so the work gets done even if no helper
// wakes in time. If another thread is already using the helpers, the caller does every chunk itself. Which thread draws
// which chunk never matters to a caller whose chunks write separate memory, so the result equals a plain loop.
// parallelChunks returns only when every chunk has finished and no helper still holds fn.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace render_pool {

// A non-owning reference to a callable taking an int. Unlike std::function it never allocates, which matters because
// pictures are drawn every frame. The callable must outlive the call that receives the reference.
class ChunkFn {
public:
	template <typename F>
	ChunkFn(const F& f) : obj(&f), call([](const void* o, int c) { (*static_cast<const F*>(o))(c); }) {}
	void operator()(int c) const {
		call(obj, c);
	}

private:
	const void* obj;
	void (*call)(const void*, int);
};

class Pool {
public:
	explicit Pool(int helpers) {
		for (int i = 0; i < helpers; ++i) {
			threads.emplace_back([this] { helperMain(); });
		}
	}
	~Pool() {
		{
			std::lock_guard<std::mutex> lock(m);
			quit = true;
		}
		wake.notify_all();
		for (std::thread& t : threads) {
			t.join();
		}
	}
	Pool(const Pool&) = delete;
	Pool& operator=(const Pool&) = delete;

	int helperCount() const {
		return static_cast<int>(threads.size());
	}

	void run(int count, ChunkFn fn) {
		std::unique_lock<std::mutex> job(jobMutex, std::try_to_lock);
		if (!job.owns_lock() || threads.empty() || count < 2) {
			for (int c = 0; c < count; ++c) {
				fn(c);
			}
			return;
		}
		std::atomic<int> next{0};
		{
			std::lock_guard<std::mutex> lock(m);
			current = &fn;
			nextChunk = &next;
			chunkCount = count;
			active = true;
			++generation;
		}
		wake.notify_all();
		takeChunks(fn, next, count);
		std::unique_lock<std::mutex> lock(m);
		active = false;  // a helper that wakes now finds no job
		done.wait(lock, [this] { return helpersInside == 0; });
		current = nullptr;
		nextChunk = nullptr;
	}

private:
	static void takeChunks(ChunkFn fn, std::atomic<int>& next, int count) {
		for (int c = next.fetch_add(1); c < count; c = next.fetch_add(1)) {
			fn(c);
		}
	}

	void helperMain() {
		std::unique_lock<std::mutex> lock(m);
		unsigned long seen = 0;
		for (;;) {
			wake.wait(lock, [this, &seen] { return quit || (active && generation != seen); });
			if (quit) {
				return;
			}
			seen = generation;
			const ChunkFn* fn = current;
			std::atomic<int>* next = nextChunk;
			const int count = chunkCount;
			++helpersInside;
			lock.unlock();
			takeChunks(*fn, *next, count);
			lock.lock();
			if (--helpersInside == 0) {
				done.notify_all();
			}
		}
	}

	std::mutex m;
	std::mutex jobMutex;
	std::condition_variable wake;
	std::condition_variable done;
	std::vector<std::thread> threads;
	const ChunkFn* current = nullptr;
	std::atomic<int>* nextChunk = nullptr;
	int chunkCount = 0;
	unsigned long generation = 0;
	int helpersInside = 0;
	bool active = false;
	bool quit = false;
};

// One pool for the process: hardware threads, at most 8, counting the caller.
inline Pool& shared() {
	static Pool pool(std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8) - 1);
	return pool;
}

template <typename F>
inline void parallelChunks(int count, const F& fn) {
	const ChunkFn ref(fn);
	shared().run(count, ref);
}

// Calls fn(begin, end) over [0, count) in fixed pieces of `piece` items (a multiple of the caller's alignment), run on the
// pool. The pieces do not depend on the thread count, so what each item receives never changes. Small jobs run in one go.
template <typename F>
inline void parallelSpan(size_t count, size_t piece, const F& fn) {
	if (piece == 0 || count <= piece * 2) {
		fn(0, count);
		return;
	}
	const int chunks = static_cast<int>((count + piece - 1) / piece);
	parallelChunks(chunks, [&](int c) {
		const size_t begin = static_cast<size_t>(c) * piece;
		fn(begin, std::min(count, begin + piece));
	});
}

}  // namespace render_pool
