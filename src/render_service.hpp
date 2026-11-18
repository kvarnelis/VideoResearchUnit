#pragma once

#include "video_bus.hpp"
#include "video_graph_renderer.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// One plugin-wide place that draws bus pictures (see docs/architecture.md, "Render service").
//
// It owns one VideoGraphRenderer per picture size, made on the first request for that size. Within one tick,
// the first request for a bus at a size renders it; every later request for the same bus and size gets the same
// finished picture, with no render. A request for another bus at that size joins the same pass: the renderer
// advances its stateful nodes (generators, Feedback, Freeze Frame, Scope Trace, the CRT beams) once per tick,
// however many buses are asked for. A lone consumer therefore gets exactly the frames a private renderer gave it.
//
// A tick is one UI frame. What marks a new one is the tick source, one small function that returns a number that
// differs from one UI frame to the next (the plugin installs Rack's frame time). With no source every request
// is a tick of its own, which is what a private renderer did.
//
// Keeping the picture running when nothing is drawn (stage 3). Rack calls draw() only for a widget that is on screen,
// but it calls step() for every widget every UI frame. A consumer therefore REGISTERS from its step() with touch():
// the bus and size it wants. A registration lasts one tick and is renewed by the next step(); a consumer whose module
// is gone simply stops renewing (and also calls forget() when it is destroyed). When a tick ends (the first call of the
// next tick closes it), the service renders every registered bus and size that no frame() call drew in that tick, in
// the size's own pass, so trails, generators and Feedback advance once per tick whether or not anything was drawn.
// frame() then only fetches the tick's picture. A consumer that was drawn gets exactly the frames it got before.
// Cost control: when no frame() call was made in the closing tick, hidden renders run at most 30 times a second.
// touch() also hands back the newest picture of the registered bus that the consumer has not taken yet (a picture
// drawn in a tick, or one rendered here), once, so Video Probe's CV and Display's SHOT and REC need no draw().
// With setHiddenRendering(false) touch() does nothing and the service behaves as in stage 2.
//
// Ownership of a picture, in both modes. A finished picture is a shared, immutable buffer. frame() and touch() hand
// it out in Result::hold and Taken::hold, and `pixels` points into it. The service never writes into a buffer that
// anyone holds: each render goes into a free buffer from a small per-size pool and then swaps the slot's pointer. A
// buffer is on the pool's free list if and only if no one holds it: each picture is a shared_ptr whose deleter puts the
// buffer on the free list, under the pool's mutex, when the last holder lets go (the Result or Taken is destroyed). The
// free-list mutex is also what orders a holder's last read before the next write into the buffer. The pool is shared
// state that every picture keeps alive, so a picture outlives its size, clear() and the service. A steady state
// allocates nothing (the shared_ptr control blocks are pooled with the buffers). A consumer keeps `pixels` valid and unchanged for as long as it keeps the Result or Taken
// it came from, across ticks, across a size being evicted, and across clear(); it must not use `pixels` after that.
// Every call takes one mutex, so any thread may ask; readCopy() copies the picture into the caller's buffer.
//
// The public API (include/vru_bus.h, readFrame) is a second kind of consumer, and readCopy() is its entry. The
// rule for what one API read counts as, precisely:
//   - If a UI consumer (frame()) has already asked for this picture size in the current tick, the API read joins
//     that tick: it gets the tick's shared picture and advances nothing.
//   - Otherwise the API read is a tick of its own for that size's renderer. It starts a new pass there, so every
//     stateful node (generators, Feedback, Freeze Frame, Scope Trace, the beams) advances once per API call, as
//     it did when the API kept a private renderer. Two reads in a row are two advances.
// A UI consumer that asks after an API read in the same tick joins the pass the API read began.
//
// The float plane (the public float read, include/vru_bus.h readFrameF32). A picture is always handed out as bytes:
// that is the picture every consumer gets, whatever the precision setting. When "Full precision between modules" is
// on, the picture the graph made before rounding can be kept as well, in Frame::precise (float, 0..255 units; the bytes
// are exactly that plane rounded half up). The rule, precisely:
//   - A float plane is made for one bus at one size only while precision is on AND a float read of that bus and size
//     was made within the last kFloatKeepTicks ticks. Every other render makes no float plane, adds no memory and
//     no work. A read of bytes never asks for one.
//   - A float read asks before it takes its picture. If it is the read that renders the picture (UI-driven mode: no
//     consumer drew that bus and size earlier in the same tick), the picture has its float plane at once. If the
//     picture was already finished without one (drawn earlier in the tick, or, in thread mode, published by the
//     thread before the ask), the read gets bytes / 255 and the NEXT picture of that bus and size has the plane. So
//     the first float read after a cold start may be byte-derived; a reader that keeps reading gets full precision
//     from its next tick on.
//   - A float read of a picture that has no float plane returns exactly byte / 255. With precision off that is every
//     float read. A picture is never half one and half the other: the plane and the bytes belong to one render.
//   - When no float read has asked for kFloatKeepTicks ticks, the ask lapses: renders stop making the plane, and
//     the planes of the size's free buffers are freed at the next render of that size (buffers held by a consumer
//     keep theirs until they come back to the pool). A size that is no longer used is dropped whole after kIdleTicks.
//
// Render thread (stage 4, off by default). With setThreadMode(true) the service renders on a thread of its own at a
// steady kRenderHz, so the picture has its own clock: a slow graph no longer stalls Rack's interface and a slow
// interface no longer stalls the video. Each tick of that thread renders every registered bus and size in one pass
// per size (the same work closeTick() and the drawn roots do in the UI-driven mode) and publishes the finished
// pictures. frame() and touch() then only look up the newest finished picture; they never render and never wait for a
// render, because they take a second, short mutex (the publish mutex) that is held only to swap pointers. A picture
// the consumer gets stays valid for as long as it keeps the shared pointer in Result or Taken (the same rule as the
// UI-driven mode, and the same buffers: the thread publishes the slot's buffer with no copy). readCopy() copies the
// newest finished picture and never renders on the caller's thread. With the mode off nothing here is used and the
// service behaves exactly as in stage 3.
class RenderService {
public:
	using Frame = VideoFrame;
	using TickSource = uint64_t (*)();
	using TimeSource = double (*)();  // seconds, for the cap on hidden renders; null means the steady clock

	// Hidden renders (no frame() call in the tick) run at most this often.
	static constexpr double kHiddenMaxHz = 30.0;

	// A request ages a size out when it has not been used for this many ticks, and no more than this many sizes
	// are kept (the least recently used go first; sizes used in the current tick are never dropped).
	static constexpr uint64_t kIdleTicks = 120;
	static constexpr size_t kMaxSizes = 6;

	// The render thread's rate, and how long a registration lasts without being renewed (thread mode only).
	static constexpr double kRenderHz = 60.0;
	static constexpr double kRegistrationSeconds = 2.0;
	// The thread stops itself when no registration has been live for this long.
	static constexpr double kIdleStopSeconds = 3.0;
	// readCopy() waits at most this long for the first picture of a bus and size the thread has not rendered yet.
	static constexpr int kFirstPictureWaitMs = 100;
	// The float plane (see "The float plane" below) is kept for a bus and size for this many ticks after the last float
	// read of it. A tick here is a service tick in the UI-driven mode and a thread tick in thread mode.
	static constexpr uint64_t kFloatKeepTicks = 120;

	// The buffers of one size (render_service.cpp). Opaque here. Shared: every picture handed out keeps its pool alive
	// through its deleter, so a picture may outlive the size, the service and clear().
	struct FramePool;

	struct Result {
		const Frame* pixels = nullptr;  // never null; empty when signal is false; valid while `hold` is kept
		bool signal = false;
		bool cycle = false;
		// Thread mode only. fresh: a picture this consumer has not been given by frame() before (always true in the
		// UI-driven mode). pending: the thread has not finished a picture for this bus and size yet (the consumer
		// keeps showing what it showed; this is not "no video").
		bool fresh = true;
		bool pending = false;
		std::shared_ptr<const Frame> hold;  // owns `pixels`: keep it as long as the pixels are read
	};

	void setTickSource(TickSource source);
	void setTimeSource(TimeSource source);
	// Switch for the whole behaviour above (default on). Off, touch() ignores its callers.
	void setHiddenRendering(bool on);
	bool hiddenRendering() const;
	// Full precision between modules (default off; docs/architecture.md, "Precision"). On, the converted nodes hand
	// float pictures to each other and a picture is rounded to 8 bits (half up) once per root, where it leaves the
	// graph: every picture this service hands out, and readCopy(), is still 8-bit. It may be flipped at any time,
	// with the render thread running; it takes effect at the start of each renderer's next pass.
	void setFullPrecision(bool on);
	bool fullPrecision() const;

	struct Taken {
		const Frame* pixels = nullptr;  // never null; empty unless fresh && signal
		bool fresh = false;  // a picture this consumer has not seen: take it now
		bool signal = false;
		bool cycle = false;
		std::shared_ptr<const Frame> hold;  // owns `pixels`: keep it as long as the pixels are read
	};
	// Registers `owner` (any pointer that is the same every call) for busId at width x height for this tick, from the UI
	// thread's step(). Returns the newest unseen picture of that bus and size. The pixels stay valid and unchanged while the Taken is kept.
	Taken touch(const void* owner, int busId, int width, int height);
	// Drops the registration of `owner` (call when the widget is destroyed).
	void forget(const void* owner);

	// The picture of `busId` at width x height for the current tick (a UI consumer: Display, Syphon Out, Probe).
	Result frame(int busId, int width, int height);
	// The same for a consumer that wants to know whether the picture is new to it (thread mode: Result::fresh).
	Result frame(const void* owner, int busId, int width, int height);

	// ---- Render thread (stage 4) ----
	// The setting "Render video on its own thread". Off is the default. Turning it off stops and joins the thread
	// before the call returns, so the UI-driven path owns every renderer again. Safe in both directions while running.
	void setThreadMode(bool on);
	bool threadMode() const;
	// True when consumers are fed by touch() from step(): thread mode, or the hidden-rendering setting.
	bool feedsFromStep() const;
	// Tests only: with false the thread is never started and ticks are driven by runTickForTest().
	void setThreadAutoStart(bool on);
	// One tick of the thread loop, run synchronously with the clock from setTimeSource (steady clock when none).
	void runTickForTest();
	bool threadRunning() const;
	// Stops and joins the thread (idempotent). The destructor and the plugin unload hook call it.
	void stopThread();
	uint64_t threadTickCount() const;  // ticks run by the thread loop body, real or synchronous
	uint64_t threadStarts() const;  // how many times a thread was started
	RenderService();
	~RenderService();

	// The public API's read (see the rule above). Copies width x height RGBA rows into `dst` (strideBytes apart) and
	// returns true, or returns false and leaves `dst` alone when the bus has no picture or sits in a cycle. Unlike
	// frame() it never holds a last picture for a cycle or a contended read. Safe from any thread.
	bool readCopy(int busId, int width, int height, unsigned char* dst, size_t strideBytes);

	// The public API's version 2 reads (include/vru_bus.h, VruBusApiV2). One call covers the 8-bit and the float read:
	// exactly one of `bytes` and `floats` is set. It takes the same mutex path as readCopy() and joins the same passes
	// by the same rule. Besides the picture it reports the service's tick serial, an opaque counter that never goes
	// down, and with `sinceTick` set it skips the render and the copy when the tick has not moved (Unchanged), unless
	// `copyAlways` is set; then it copies the finished picture of the current pass if there is one.
	struct ReadRequest {
		int busId = 0;
		int width = 0;
		int height = 0;
		unsigned char* bytes = nullptr;
		size_t strideBytes = 0;
		float* floats = nullptr;
		size_t strideFloats = 0;  // in floats
		uint64_t sinceTick = 0;  // 0: no earlier read, the picture is always new
		bool copyAlways = false;
	};
	enum class ReadStatus { Ok, NoSignal, Unchanged, BadArgument };
	struct ReadOutcome {
		ReadStatus status = ReadStatus::BadArgument;
		uint64_t tick = 0;
		bool isNew = false;
	};
	ReadOutcome read(const ReadRequest& request);

	// Debug and test accessors.
	// Bytes of float plane memory the pool buffers hold at this moment (every size, free and held buffers).
	size_t floatPlaneBytes() const;
	uint64_t renderTotal() const;  // every bus render the service has made (drawn, hidden and API)
	size_t registrationCount() const;
	uint64_t tickSerial() const;
	size_t sizeCount() const;
	// How many times `busId` was rendered in this size's latest pass (0 or 1 for the tick's own buses).
	int busRenderCount(int busId, int width, int height) const;
	// Frees every renderer.
	void clear();
	// Whether a renderer for this size is kept.
	bool hasSize(int width, int height) const;

private:
	struct Entry {
		std::shared_ptr<const Frame> good;  // the last picture that rendered (also the hold picture); never written again
		int goodWidth = 0;
		int goodHeight = 0;
		uint64_t pass = 0;  // the size's pass this entry was rendered in; 0 means never
		bool signal = false;  // a picture to show (a fresh one, or the held one)
		bool ok = false;  // the render in `pass` succeeded (no hold), which is all the API accepts
		bool cycle = false;
		uint64_t stamp = 0;  // renderTotal when this entry last rendered; 0 means never
	};
	struct Registration {
		const void* owner = nullptr;
		int busId = 0;
		int width = 0;
		int height = 0;
		uint64_t lastSeen = 0;  // the tick of the last touch()
		uint64_t taken = 0;  // the entry stamp this consumer last took
	};
	struct SizeState {
		SizeState();
		~SizeState();
		VideoGraphRenderer renderer;
		std::array<Entry, kVideoBusCount + 1> entries;
		std::shared_ptr<FramePool> pool;
		uint64_t passTick = 0;  // the tick the current pass began in
		uint64_t passCount = 0;  // passes begun at this size; an entry is current when its pass equals it
		uint64_t lastUsed = 0;  // the last tick a UI consumer asked for this size
		uint64_t lastApi = 0;  // the last tick an API read asked for this size
		uint64_t useSeq = 0;  // order of last use, for the least recently used rule
	};

	static uint64_t sizeKey(int width, int height) {
		return (static_cast<uint64_t>(static_cast<uint32_t>(width)) << 32) | static_cast<uint32_t>(height);
	}
	void advanceTick();
	void closeTick();  // renders the registered buses nobody drew in tick `serial`; runs as the tick rolls over
	double now() const;
	SizeState& stateFor(int width, int height, bool forApi);
	// A buffer no one holds, wrapped so that its last holder letting go puts it back on the pool's free list.
	static std::shared_ptr<Frame> acquireBuffer(SizeState& state, bool keepPlanes);
	// The float plane ask book (see "The float plane"). Under pubMutex; floatAskCount lets a render skip the lock when
	// there is no ask at all.
	uint64_t floatClock() const;
	void noteFloatAskLocked(int busId, int width, int height);
	bool floatAskActive(uint64_t sizeKeyValue, int busId);  // busId 0: any bus of the size
	void renderEntry(SizeState& state, Entry& entry, int busId, int width, int height);

	// ---- thread mode internals ----
	// Two mutexes. `mutex` (below) is the render mutex: it covers every renderer and is held for a whole render, by the
	// thread or by the UI-driven path, never by both modes at once. `pubMutex` covers registrations and published
	// pictures and is held only for lookups and pointer swaps. Lock order: mutex, then pubMutex. A consumer in thread
	// mode takes pubMutex only.
	struct ThreadReg {
		const void* owner = nullptr;  // null: anonymous, keyed by bus and size (frame() without owner, the API)
		int busId = 0;
		int width = 0;
		int height = 0;
		double lastSeen = 0.0;
		uint64_t taken = 0;  // the stamp touch() last gave this consumer
		uint64_t shown = 0;  // the stamp frame() last gave this consumer
	};
	struct Pub {
		std::shared_ptr<const Frame> frame;  // null when there is no picture to show
		uint64_t stamp = 0;  // 0: nothing finished yet
		bool signal = false;
		bool cycle = false;
		bool ok = false;
		uint64_t tick = 0;  // the service's tick serial when this picture was rendered
	};
	using PubKey = std::pair<uint64_t, int>;  // size key, bus id
	struct Job {
		int busId;
		int width;
		int height;
	};
	void runTick(double t);
	void threadMain();
	void ensureThread();
	ThreadReg& registerThreaded(const void* owner, int busId, int width, int height, double t);
	Result frameThreaded(const void* owner, int busId, int width, int height);
	Taken touchThreaded(const void* owner, int busId, int width, int height);
	bool readCopyThreaded(int busId, int width, int height, unsigned char* dst, size_t strideBytes);
	ReadOutcome readThreaded(const ReadRequest& request);

	std::atomic<bool> threadOn {false};
	std::atomic<bool> autoStart {true};
	std::atomic<bool> threadLive {false};  // a thread was started and has not finished
	std::atomic<bool> stopFlag {false};
	// Phase lock to the UI. A UI draw that takes a new picture (frameThreaded / touchThreaded) counts one here and wakes the
	// thread, which then starts the next tick while the UI still has most of a frame to wait, so the picture is ready before
	// the next draw. Without it the 60 Hz tick clock and the UI's frame clock free-run against each other, and a draw
	// sometimes finds no new picture (held frame) while another finds two (one skipped).
	std::atomic<uint64_t> demandSeq {0};
	std::atomic<int64_t> demandNs {0};  // steady-clock time of the last such draw, ns; 0 before the first
	void noteDemand();
	std::atomic<uint64_t> tickCount {0};
	std::atomic<uint64_t> startCount {0};
	std::mutex lifeMutex;  // guards `worker` (start and join)
	std::thread worker;
	std::mutex stopMutex;
	std::condition_variable stopCv;
	mutable std::mutex pubMutex;
	std::condition_variable pubCv;
	std::vector<ThreadReg> threadRegs;
	std::map<PubKey, Pub> pubs;
	uint64_t pubCounter = 0;
	double lastLive = 0.0;  // the last time a registration was live (steady seconds or the injected clock)

	mutable std::mutex mutex;
	TickSource tickSource = nullptr;
	bool haveToken = false;
	uint64_t lastToken = 0;
	// Our own tick number, counted up from 1 whenever the token changes. Written under `mutex`; atomic because floatClock()
	// reads it from a thread-mode caller that raced a switch back to the UI-driven mode, without `mutex`.
	std::atomic<uint64_t> serial {0};
	uint64_t useCounter = 0;
	std::map<uint64_t, std::unique_ptr<SizeState>> sizes;
	Frame none;
	std::atomic<TimeSource> timeSource {nullptr};
	std::atomic<bool> hiddenOn {true};
	std::atomic<bool> precisionOn {false};
	std::map<PubKey, uint64_t> floatAsks;  // (size key, bus) -> floatClock() of the last float read; under pubMutex
	std::atomic<size_t> floatAskCount {0};
	void beginPassOn(VideoGraphRenderer& renderer) const;
	bool drawnThisTick = false;  // a frame() call was made in the current tick
	double lastRenderTime = -1.0;  // when a tick last rendered (drawn or hidden), for the cap
	uint64_t renderCounter = 0;
	std::vector<Registration> registrations;
};

// The plugin's one service.
RenderService& renderService();
