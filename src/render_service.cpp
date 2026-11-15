#include "render_service.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif

// The plugin's one service stops its thread before the library's statics go: at exit, and when the plugin is
// unloaded. Rack has no teardown callback for plugins, so a static destructor, an exit hook and a library
// destructor are what there is; stopThread() is idempotent, so all three may run.
namespace {
std::atomic<RenderService*> liveService {nullptr};

void stopLiveService() {
	RenderService* service = liveService.load();
	if (service) {
		service->stopThread();
	}
}
}

void RenderService::setTickSource(TickSource source) {
	std::lock_guard<std::mutex> lock(mutex);
	tickSource = source;
	haveToken = false;
}

void RenderService::setTimeSource(TimeSource source) {
	std::lock_guard<std::mutex> lock(mutex);
	timeSource = source;
	lastRenderTime = -1.0;
}

void RenderService::setFullPrecision(bool on) {
	// Each renderer reads this at the start of its next pass, so a pass in progress finishes in the format it began in.
	precisionOn.store(on, std::memory_order_relaxed);
}

bool RenderService::fullPrecision() const {
	return precisionOn.load(std::memory_order_relaxed);
}

void RenderService::beginPassOn(VideoGraphRenderer& renderer) const {
	renderer.setFullPrecision(precisionOn.load(std::memory_order_relaxed));
	renderer.beginPass();
}

void RenderService::setHiddenRendering(bool on) {
	std::lock_guard<std::mutex> lock(mutex);
	hiddenOn = on;
	if (!on) {
		registrations.clear();
	}
}

bool RenderService::hiddenRendering() const {
	return hiddenOn.load(std::memory_order_relaxed);
}

double RenderService::now() const {
	const TimeSource source = timeSource.load(std::memory_order_relaxed);
	if (source) {
		return source();
	}
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void RenderService::closeTick() {
	const bool drawn = drawnThisTick;
	drawnThisTick = false;
	if (!hiddenOn) {
		return;
	}
	// The cap: with nothing drawn, render for the registered consumers at most kHiddenMaxHz times a second. A
	// small slack keeps a 60 Hz UI at exactly every other tick despite jitter.
	const double t = now();
	bool run = true;
	if (!drawn && lastRenderTime >= 0.0 && t - lastRenderTime < 1.0 / kHiddenMaxHz - 0.002) {
		run = false;
	}
	bool rendered = false;
	if (run) {
		for (const Registration& reg : registrations) {
			if (reg.lastSeen != serial) {
				continue;
			}
			SizeState& state = stateFor(reg.width, reg.height, false);
			state.lastUsed = serial;
			state.useSeq = ++useCounter;
			Entry& entry = state.entries[static_cast<size_t>(reg.busId)];
			if (state.passTick == serial && entry.pass == state.passCount) {
				continue;  // already drawn (or read) this tick
			}
			if (state.passTick != serial) {
				beginPassOn(state.renderer);
				++state.passCount;
				state.passTick = serial;
			}
			renderEntry(state, entry, reg.busId, reg.width, reg.height);
			rendered = true;
		}
	}
	if (drawn || rendered) {
		lastRenderTime = t;
	}
	// A registration that was not renewed in the tick that just ended is gone.
	registrations.erase(std::remove_if(registrations.begin(), registrations.end(), [this](const Registration& r) { return r.lastSeen != serial; }), registrations.end());
}

void RenderService::advanceTick() {
	bool fresh = true;
	if (tickSource) {
		const uint64_t token = tickSource();
		fresh = !haveToken || token != lastToken;
		haveToken = true;
		lastToken = token;
	}
	if (!fresh) {
		return;
	}
	if (serial > 0) {
		closeTick();
	}
	++serial;
	// Sizes nobody asked for in the last kIdleTicks ticks go.
	for (auto it = sizes.begin(); it != sizes.end();) {
		if (std::max(it->second->lastUsed, it->second->lastApi) + kIdleTicks < serial) {
			it = sizes.erase(it);
		}
		else {
			++it;
		}
	}
}

RenderService::SizeState& RenderService::stateFor(int width, int height, bool forApi) {
	const uint64_t key = sizeKey(width, height);
	auto it = sizes.find(key);
	if (it != sizes.end()) {
		return *it->second;
	}
	// A new size. Over the cap, drop the least recently used size that may go. A UI request spares every size used
	// in the current tick, by anyone. An API request spares only sizes a UI consumer used this tick or the last one,
	// so a client that sweeps sizes recycles its own sizes and never costs the Display its renderer.
	while (sizes.size() >= kMaxSizes) {
		auto oldest = sizes.end();
		for (auto s = sizes.begin(); s != sizes.end(); ++s) {
			const SizeState& st = *s->second;
			const bool spared = forApi ? (st.lastUsed + 1 >= serial) : (st.lastUsed >= serial || st.lastApi >= serial);
			if (!spared && (oldest == sizes.end() || st.useSeq < oldest->second->useSeq)) {
				oldest = s;
			}
		}
		if (oldest == sizes.end()) {
			break;
		}
		sizes.erase(oldest);
	}
	auto made = std::make_unique<SizeState>();
	SizeState& ref = *made;
	sizes[key] = std::move(made);
	return ref;
}

// The buffers of one size. Ownership rule: a Frame is on `freeFrames` if and only if no picture holds it. A picture is a
// shared_ptr<Frame> with a FrameReturn deleter that puts the Frame back here, under `mutex`, when its last holder lets go;
// nothing reads a use count to decide. Taking from and returning to the list under one mutex also gives the happens-before
// edge between a holder's last read and the next render's first write. Every deleter and control block allocator keeps
// the pool alive, so a picture may outlive its SizeState, clear() and the whole service; the last one out frees the pool.
// Control blocks come from `freeBlocks` (all one size), so handing a picture out allocates nothing in steady state.
struct RenderService::FramePool {
	static constexpr size_t kFreeSpares = 3;  // free buffers kept beyond the one in use; more are dropped on return
	std::mutex mutex;
	std::vector<Frame*> freeFrames;
	std::vector<Frame*> allFrames;  // every Frame this pool made that is still alive, free or held
	std::vector<void*> freeBlocks;
	size_t blockSize = 0;

	FramePool() {
		freeFrames.reserve(kFreeSpares + 2);
		allFrames.reserve(16);
		freeBlocks.reserve(16);
	}
	FramePool(const FramePool&) = delete;
	FramePool& operator=(const FramePool&) = delete;
	~FramePool() {
		for (Frame* frame : freeFrames) {
			delete frame;
		}
		for (void* block : freeBlocks) {
			::operator delete(block);
		}
	}

	Frame* take(bool keepPlanes) {
		std::lock_guard<std::mutex> lock(mutex);
		if (!keepPlanes) {
			for (Frame* frame : freeFrames) {
				if (frame->precise.capacity() != 0) {
					std::vector<float>().swap(frame->precise);  // nobody asks for float planes at this size: give the memory back
				}
			}
		}
		if (!freeFrames.empty()) {
			Frame* frame = freeFrames.back();
			freeFrames.pop_back();
			return frame;
		}
		Frame* frame = new Frame();
		allFrames.push_back(frame);
		return frame;
	}
	void give(Frame* frame) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (freeFrames.size() < kFreeSpares + 1) {
				freeFrames.push_back(frame);
				return;
			}
			allFrames.erase(std::find(allFrames.begin(), allFrames.end(), frame));
		}
		delete frame;  // a burst of holders does not leave memory behind
	}
	void* takeBlock(size_t bytes) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (blockSize == 0) {
				blockSize = bytes;
			}
			if (bytes == blockSize && !freeBlocks.empty()) {
				void* block = freeBlocks.back();
				freeBlocks.pop_back();
				return block;
			}
		}
		return ::operator new(bytes);
	}
	void giveBlock(void* block, size_t bytes) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (bytes == blockSize) {
				freeBlocks.push_back(block);
				return;
			}
		}
		::operator delete(block);
	}
	size_t floatBytes() {
		std::lock_guard<std::mutex> lock(mutex);
		size_t total = 0;
		for (const Frame* frame : allFrames) {
			total += frame->precise.capacity() * sizeof(float);
		}
		return total;
	}
};

namespace {
struct FrameReturn {
	std::shared_ptr<RenderService::FramePool> pool;
	void operator()(RenderService::Frame* frame) const {
		pool->give(frame);
	}
};
// The control block allocator: blocks come from, and go back to, the pool (which it keeps alive).
template <class T>
struct BlockAllocator {
	using value_type = T;
	std::shared_ptr<RenderService::FramePool> pool;
	explicit BlockAllocator(std::shared_ptr<RenderService::FramePool> p) : pool(std::move(p)) {}
	template <class U>
	BlockAllocator(const BlockAllocator<U>& other) : pool(other.pool) {}
	T* allocate(size_t n) {
		return static_cast<T*>(pool->takeBlock(n * sizeof(T)));
	}
	void deallocate(T* block, size_t n) {
		pool->giveBlock(block, n * sizeof(T));
	}
	template <class U>
	bool operator==(const BlockAllocator<U>& other) const {
		return pool == other.pool;
	}
	template <class U>
	bool operator!=(const BlockAllocator<U>& other) const {
		return pool != other.pool;
	}
};
}  // namespace

RenderService::SizeState::SizeState() : pool(std::make_shared<FramePool>()) {}
RenderService::SizeState::~SizeState() = default;

std::shared_ptr<RenderService::Frame> RenderService::acquireBuffer(SizeState& state, bool keepPlanes) {
	return std::shared_ptr<Frame>(state.pool->take(keepPlanes), FrameReturn{state.pool}, BlockAllocator<Frame>(state.pool));
}

uint64_t RenderService::floatClock() const {
	return threadOn.load(std::memory_order_acquire) ? tickCount.load(std::memory_order_relaxed) : serial.load(std::memory_order_relaxed);
}

// Callers hold pubMutex. The clock is the service serial in the UI-driven mode (the caller holds `mutex`) and the thread's
// tick count in thread mode.
void RenderService::noteFloatAskLocked(int busId, int width, int height) {
	floatAsks[PubKey(sizeKey(width, height), busId)] = floatClock();
	floatAskCount.store(floatAsks.size(), std::memory_order_release);
}

// True when a float read of this bus (busId 0: any bus) at this size was made within kFloatKeepTicks ticks. Lapsed
// asks are dropped here. Callers hold `mutex` (the render path); the common case, no ask at all, takes no lock.
bool RenderService::floatAskActive(uint64_t sizeKeyValue, int busId) {
	if (floatAskCount.load(std::memory_order_acquire) == 0) {
		return false;
	}
	std::lock_guard<std::mutex> lock(pubMutex);
	const uint64_t clock = floatClock();
	bool active = false;
	for (auto it = floatAsks.begin(); it != floatAsks.end();) {
		if (clock - it->second > kFloatKeepTicks && clock >= it->second) {
			it = floatAsks.erase(it);
			continue;
		}
		if (it->first.first == sizeKeyValue && (busId == 0 || it->first.second == busId)) {
			active = true;
		}
		++it;
	}
	floatAskCount.store(floatAsks.size(), std::memory_order_release);
	return active;
}

size_t RenderService::floatPlaneBytes() const {
	std::lock_guard<std::mutex> lock(mutex);
	size_t total = 0;
	for (const auto& entry : sizes) {
		total += entry.second->pool->floatBytes();
	}
	return total;
}

void RenderService::renderEntry(SizeState& state, Entry& entry, int busId, int width, int height) {
	entry.pass = state.passCount;
	entry.stamp = ++renderCounter;
	// The picture goes into a buffer nobody holds, never into entry.good, which consumers may be reading. On success
	// the slot's pointer moves to the new buffer; a failed render leaves the last good picture to hold.
	const uint64_t key = sizeKey(width, height);
	const bool planes = precisionOn.load(std::memory_order_relaxed) && floatAskActive(key, 0);
	const bool plane = planes && floatAskActive(key, busId);
	std::shared_ptr<Frame> buffer = acquireBuffer(state, planes);
	const VideoGraphRenderer::Status status = state.renderer.renderBusInPass(busId, width, height, *buffer, plane);
	entry.cycle = status == VideoGraphRenderer::Status::Cycle;
	entry.ok = status == VideoGraphRenderer::Status::Ok;
	if (entry.ok) {
		entry.good = std::move(buffer);
		entry.goodWidth = width;
		entry.goodHeight = height;
		entry.signal = true;
	}
	else {
		// Hold the last picture for a cycle, or for a read the writer kept interrupting: the same rule a
		// private renderer follows.
		const bool hold = (status == VideoGraphRenderer::Status::Cycle || (status == VideoGraphRenderer::Status::Empty && state.renderer.wasContended())) &&
			entry.goodWidth == width && entry.goodHeight == height && entry.good && !entry.good->empty();
		entry.signal = hold;
	}
}

RenderService::Result RenderService::frame(int busId, int width, int height) {
	return frame(nullptr, busId, width, height);
}

RenderService::Result RenderService::frame(const void* owner, int busId, int width, int height) {
	if (threadOn.load(std::memory_order_acquire)) {
		return frameThreaded(owner, busId, width, height);
	}
	std::lock_guard<std::mutex> lock(mutex);
	advanceTick();

	Result result;
	result.pixels = &none;
	if (width <= 0 || height <= 0 || busId <= 0 || busId > kVideoBusCount) {
		return result;
	}

	SizeState& state = stateFor(width, height, false);
	state.lastUsed = serial;
	state.useSeq = ++useCounter;
	drawnThisTick = true;
	Entry& entry = state.entries[static_cast<size_t>(busId)];
	if (state.passTick != serial) {
		beginPassOn(state.renderer);
		++state.passCount;
		state.passTick = serial;
	}
	if (entry.pass != state.passCount) {
		renderEntry(state, entry, busId, width, height);
	}
	result.signal = entry.signal;
	result.cycle = entry.cycle;
	if (entry.signal) {
		result.hold = entry.good;
		result.pixels = result.hold.get();
	}
	return result;
}

RenderService::Taken RenderService::touch(const void* owner, int busId, int width, int height) {
	if (threadOn.load(std::memory_order_acquire)) {
		return touchThreaded(owner, busId, width, height);
	}
	std::lock_guard<std::mutex> lock(mutex);
	Taken taken;
	taken.pixels = &none;
	if (!hiddenOn) {
		return taken;
	}
	advanceTick();
	if (!owner || width <= 0 || height <= 0 || busId <= 0 || busId > kVideoBusCount) {
		return taken;
	}
	Registration* reg = nullptr;
	for (Registration& r : registrations) {
		if (r.owner == owner) {
			reg = &r;
			break;
		}
	}
	if (!reg) {
		registrations.push_back(Registration());
		reg = &registrations.back();
		reg->owner = owner;
	}
	if (reg->busId != busId || reg->width != width || reg->height != height) {
		reg->busId = busId;
		reg->width = width;
		reg->height = height;
		reg->taken = 0;
	}
	reg->lastSeen = serial;
	const auto it = sizes.find(sizeKey(width, height));
	if (it == sizes.end()) {
		return taken;
	}
	const Entry& entry = it->second->entries[static_cast<size_t>(busId)];
	if (entry.stamp != 0 && entry.stamp != reg->taken) {
		reg->taken = entry.stamp;
		taken.fresh = true;
		taken.signal = entry.signal;
		taken.cycle = entry.cycle;
		if (entry.signal) {
			taken.hold = entry.good;
			taken.pixels = taken.hold.get();
		}
	}
	return taken;
}

void RenderService::forget(const void* owner) {
	{
		std::lock_guard<std::mutex> lock(pubMutex);
		threadRegs.erase(std::remove_if(threadRegs.begin(), threadRegs.end(), [owner](const ThreadReg& r) { return r.owner == owner; }), threadRegs.end());
	}
	if (threadOn.load(std::memory_order_acquire)) {
		return;  // thread mode: the UI-driven registrations were cleared when the mode went on, and must not wait for a render
	}
	std::lock_guard<std::mutex> lock(mutex);
	registrations.erase(std::remove_if(registrations.begin(), registrations.end(), [owner](const Registration& r) { return r.owner == owner; }), registrations.end());
}

uint64_t RenderService::renderTotal() const {
	std::lock_guard<std::mutex> lock(mutex);
	return renderCounter;
}

size_t RenderService::registrationCount() const {
	std::lock_guard<std::mutex> lock(mutex);
	return registrations.size();
}

bool RenderService::readCopy(int busId, int width, int height, unsigned char* dst, size_t strideBytes) {
	if (threadOn.load(std::memory_order_acquire)) {
		return readCopyThreaded(busId, width, height, dst, strideBytes);
	}
	std::lock_guard<std::mutex> lock(mutex);
	advanceTick();

	const size_t rowBytes = static_cast<size_t>(width) * 4u;
	if (!dst || width <= 0 || height <= 0 || busId <= 0 || busId > kVideoBusCount || strideBytes < rowBytes) {
		return false;
	}

	SizeState& state = stateFor(width, height, true);
	state.lastApi = serial;
	state.useSeq = ++useCounter;
	Entry& entry = state.entries[static_cast<size_t>(busId)];
	// A UI consumer already drew this size in this tick: share its pass and its pictures. Otherwise this read is
	// a tick of its own for the size, a pass of its own, so the picture advances once per call.
	if (state.lastUsed != serial) {
		beginPassOn(state.renderer);
		++state.passCount;
		state.passTick = serial;
	}
	if (entry.pass != state.passCount) {
		renderEntry(state, entry, busId, width, height);
	}
	if (!entry.ok || entry.cycle || !entry.good || entry.good->size() < rowBytes * static_cast<size_t>(height)) {
		return false;
	}
	for (int y = 0; y < height; ++y) {
		std::memcpy(dst + static_cast<size_t>(y) * strideBytes, entry.good->data() + static_cast<size_t>(y) * rowBytes, rowBytes);
	}
	return true;
}

namespace {
// Copies a finished 8-bit picture out. The float unit is made here and nowhere else: value / 255, exact for every level.
// A float read takes the picture's float plane when it has one (full precision on, and a float reader had asked; see
// "The float plane" in render_service.hpp), and otherwise the bytes.
void copyPicture(const VideoFrame& picture, int width, int height, const RenderService::ReadRequest& r) {
	const unsigned char* src = picture.data();
	const size_t rowBytes = static_cast<size_t>(width) * 4u;
	if (r.bytes) {
		for (int y = 0; y < height; ++y) {
			std::memcpy(r.bytes + static_cast<size_t>(y) * r.strideBytes, src + static_cast<size_t>(y) * rowBytes, rowBytes);
		}
		return;
	}
	if (picture.precise.size() == picture.size()) {
		for (int y = 0; y < height; ++y) {
			const float* in = picture.precise.data() + static_cast<size_t>(y) * rowBytes;
			float* out = r.floats + static_cast<size_t>(y) * r.strideFloats;
			for (size_t i = 0; i < rowBytes; ++i) {
				out[i] = in[i] / 255.f;
			}
		}
		return;
	}
	for (int y = 0; y < height; ++y) {
		const unsigned char* in = src + static_cast<size_t>(y) * rowBytes;
		float* out = r.floats + static_cast<size_t>(y) * r.strideFloats;
		for (size_t i = 0; i < rowBytes; ++i) {
			out[i] = static_cast<float>(in[i]) / 255.f;
		}
	}
}

bool validRead(const RenderService::ReadRequest& r) {
	if ((r.bytes != nullptr) == (r.floats != nullptr)) {
		return false;
	}
	if (r.width <= 0 || r.height <= 0 || r.busId <= 0 || r.busId > kVideoBusCount) {
		return false;
	}
	const size_t row = static_cast<size_t>(r.width) * 4u;
	return r.bytes ? r.strideBytes >= row : r.strideFloats >= row;
}
}

RenderService::ReadOutcome RenderService::read(const ReadRequest& request) {
	if (threadOn.load(std::memory_order_acquire)) {
		return readThreaded(request);
	}
	ReadOutcome outcome;
	std::lock_guard<std::mutex> lock(mutex);
	advanceTick();
	if (!validRead(request)) {
		return outcome;
	}
	outcome.tick = serial;
	outcome.isNew = request.sinceTick == 0 || serial > request.sinceTick;
	if (!outcome.isNew && !request.copyAlways) {
		outcome.status = videoBusIsActive(request.busId) ? ReadStatus::Unchanged : ReadStatus::NoSignal;
		return outcome;
	}
	outcome.status = ReadStatus::NoSignal;
	if (request.floats) {
		std::lock_guard<std::mutex> pubLock(pubMutex);
		noteFloatAskLocked(request.busId, request.width, request.height);
	}

	SizeState& state = stateFor(request.width, request.height, true);
	state.lastApi = serial;
	state.useSeq = ++useCounter;
	Entry& entry = state.entries[static_cast<size_t>(request.busId)];
	// A read that is not new (copyAlways) reuses the picture of this tick's pass when there is one. Otherwise the
	// rule of readCopy() holds: a pass of its own unless a UI consumer already drew this size in this tick.
	const bool reuse = !outcome.isNew && state.passTick == serial && entry.pass == state.passCount;
	if (!reuse && state.lastUsed != serial) {
		beginPassOn(state.renderer);
		++state.passCount;
		state.passTick = serial;
	}
	if (entry.pass != state.passCount) {
		renderEntry(state, entry, request.busId, request.width, request.height);
	}
	const size_t rowBytes = static_cast<size_t>(request.width) * 4u;
	if (!entry.ok || entry.cycle || !entry.good || entry.good->size() < rowBytes * static_cast<size_t>(request.height)) {
		return outcome;
	}
	copyPicture(*entry.good, request.width, request.height, request);
	outcome.status = ReadStatus::Ok;
	return outcome;
}

uint64_t RenderService::tickSerial() const {
	std::lock_guard<std::mutex> lock(mutex);
	return serial;
}

size_t RenderService::sizeCount() const {
	std::lock_guard<std::mutex> lock(mutex);
	return sizes.size();
}

int RenderService::busRenderCount(int busId, int width, int height) const {
	std::lock_guard<std::mutex> lock(mutex);
	const auto it = sizes.find(sizeKey(width, height));
	return it == sizes.end() ? 0 : it->second->renderer.busRenderCount(busId);
}

bool RenderService::hasSize(int width, int height) const {
	std::lock_guard<std::mutex> lock(mutex);
	return sizes.count(sizeKey(width, height)) != 0;
}

void RenderService::clear() {
	std::lock_guard<std::mutex> lock(mutex);
	sizes.clear();
	registrations.clear();
	std::lock_guard<std::mutex> pubLock(pubMutex);
	threadRegs.clear();
	pubs.clear();
	floatAsks.clear();
	floatAskCount.store(0, std::memory_order_release);
}

// ---------------------------------------------------------------------------------------------------------------
// Render thread (stage 4)

bool RenderService::feedsFromStep() const {
	return hiddenOn.load(std::memory_order_relaxed) || threadOn.load(std::memory_order_relaxed);
}

bool RenderService::threadMode() const {
	return threadOn.load(std::memory_order_acquire);
}

void RenderService::setThreadAutoStart(bool on) {
	autoStart.store(on, std::memory_order_release);
}

bool RenderService::threadRunning() const {
	return threadLive.load(std::memory_order_acquire);
}

uint64_t RenderService::threadTickCount() const {
	return tickCount.load(std::memory_order_acquire);
}

uint64_t RenderService::threadStarts() const {
	return startCount.load(std::memory_order_acquire);
}

void RenderService::setThreadMode(bool on) {
	if (on == threadOn.load(std::memory_order_acquire)) {
		return;
	}
	if (on) {
		{
			// The UI-driven registrations belong to the other mode. Anything that renews one after this just ages out.
			std::lock_guard<std::mutex> lock(mutex);
			registrations.clear();
		}
		{
			std::lock_guard<std::mutex> lock(pubMutex);
			floatAsks.clear();  // the asks are counted in another clock now
			floatAskCount.store(0, std::memory_order_release);
		}
		threadOn.store(true, std::memory_order_release);
		// The thread starts when the first consumer registers (ensureThread).
		return;
	}
	threadOn.store(false, std::memory_order_release);
	stopThread();
	std::lock_guard<std::mutex> lock(pubMutex);
	threadRegs.clear();
	pubs.clear();
	floatAsks.clear();  // the asks are counted in another clock now
	floatAskCount.store(0, std::memory_order_release);
}

void RenderService::ensureThread() {
	if (!autoStart.load(std::memory_order_acquire) || threadLive.load(std::memory_order_acquire)) {
		return;
	}
	std::lock_guard<std::mutex> lock(lifeMutex);
	if (!threadOn.load(std::memory_order_acquire)) {
		return;
	}
	if (threadLive.load(std::memory_order_acquire)) {
		return;
	}
	if (worker.joinable()) {
		worker.join();  // a thread that stopped itself when idle; it has finished or is about to
	}
	stopFlag.store(false, std::memory_order_release);
	threadLive.store(true, std::memory_order_release);
	startCount.fetch_add(1, std::memory_order_relaxed);
	worker = std::thread([this]() { threadMain(); });
}

void RenderService::stopThread() {
	std::lock_guard<std::mutex> lock(lifeMutex);
	{
		std::lock_guard<std::mutex> stopLock(stopMutex);
		stopFlag.store(true, std::memory_order_release);
	}
	stopCv.notify_all();
	if (worker.joinable()) {
		worker.join();
	}
	threadLive.store(false, std::memory_order_release);
	pubCv.notify_all();
}

RenderService::~RenderService() {
	stopThread();
	RenderService* me = this;
	liveService.compare_exchange_strong(me, nullptr);
}

void RenderService::threadMain() {
	using Clock = std::chrono::steady_clock;
#ifdef __APPLE__
	// Normal, never real-time: below Rack's audio threads and no higher than its interface.
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
	const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / kRenderHz));
	auto start = Clock::now();
	uint64_t n = 0;
	{
		std::lock_guard<std::mutex> lock(pubMutex);
		lastLive = now();
	}
	while (!stopFlag.load(std::memory_order_acquire)) {
		const double t = now();
		runTick(t);
		bool idle = false;
		{
			std::lock_guard<std::mutex> lock(pubMutex);
			idle = t - lastLive > kIdleStopSeconds;
		}
		if (idle) {
			break;
		}
		// Drift-free: the next wake is start + n * period, not the end of the last tick plus a sleep. A thread that
		// has fallen several periods behind (the machine was busy) restarts its schedule instead of racing to catch up.
		++n;
		auto next = start + period * static_cast<Clock::rep>(n);
		const auto current = Clock::now();
		if (current > next + period * 4) {
			start = current;
			n = 1;
			next = start + period;
		}
		std::unique_lock<std::mutex> lock(stopMutex);
		stopCv.wait_until(lock, next, [this]() { return stopFlag.load(std::memory_order_acquire); });
	}
	threadLive.store(false, std::memory_order_release);
}

void RenderService::runTickForTest() {
	runTick(now());
}

RenderService::ThreadReg& RenderService::registerThreaded(const void* owner, int busId, int width, int height, double t) {
	// pubMutex is held by the caller.
	ThreadReg* reg = nullptr;
	for (ThreadReg& r : threadRegs) {
		if (owner ? r.owner == owner : (!r.owner && r.busId == busId && r.width == width && r.height == height)) {
			reg = &r;
			break;
		}
	}
	if (!reg) {
		threadRegs.emplace_back();
		reg = &threadRegs.back();
		reg->owner = owner;
	}
	if (reg->busId != busId || reg->width != width || reg->height != height) {
		reg->busId = busId;
		reg->width = width;
		reg->height = height;
		reg->taken = 0;
		reg->shown = 0;
	}
	reg->lastSeen = t;
	return *reg;
}

namespace {
bool validRequest(int busId, int width, int height) {
	return width > 0 && height > 0 && busId > 0 && busId <= kVideoBusCount;
}
}

RenderService::Result RenderService::frameThreaded(const void* owner, int busId, int width, int height) {
	Result result;
	result.pixels = &none;
	if (!validRequest(busId, width, height)) {
		return result;
	}
	const double t = now();
	{
		std::lock_guard<std::mutex> lock(pubMutex);
		ThreadReg& reg = registerThreaded(owner, busId, width, height, t);
		const auto it = pubs.find(PubKey(sizeKey(width, height), busId));
		if (it == pubs.end() || it->second.stamp == 0) {
			result.pending = true;
			result.fresh = false;
		}
		else {
			const Pub& pub = it->second;
			result.fresh = pub.stamp != reg.shown;
			reg.shown = pub.stamp;
			result.signal = pub.signal;
			result.cycle = pub.cycle;
			result.hold = pub.frame;
		}
	}
	if (result.hold) {
		result.pixels = result.hold.get();
	}
	ensureThread();
	return result;
}

RenderService::Taken RenderService::touchThreaded(const void* owner, int busId, int width, int height) {
	Taken taken;
	taken.pixels = &none;
	if (!owner || !validRequest(busId, width, height)) {
		return taken;
	}
	const double t = now();
	{
		std::lock_guard<std::mutex> lock(pubMutex);
		ThreadReg& reg = registerThreaded(owner, busId, width, height, t);
		const auto it = pubs.find(PubKey(sizeKey(width, height), busId));
		if (it != pubs.end() && it->second.stamp != 0 && it->second.stamp != reg.taken) {
			const Pub& pub = it->second;
			reg.taken = pub.stamp;
			taken.fresh = true;
			taken.signal = pub.signal;
			taken.cycle = pub.cycle;
			taken.hold = pub.frame;
		}
	}
	if (taken.hold) {
		taken.pixels = taken.hold.get();
	}
	ensureThread();
	return taken;
}

bool RenderService::readCopyThreaded(int busId, int width, int height, unsigned char* dst, size_t strideBytes) {
	const size_t rowBytes = static_cast<size_t>(width) * 4u;
	if (!dst || !validRequest(busId, width, height) || strideBytes < rowBytes) {
		return false;
	}
	const double t = now();
	std::shared_ptr<const Frame> picture;
	bool ok = false;
	bool cycle = false;
	{
		std::unique_lock<std::mutex> lock(pubMutex);
		registerThreaded(nullptr, busId, width, height, t);
		const PubKey key(sizeKey(width, height), busId);
		auto finished = [&]() {
			const auto it = pubs.find(key);
			return it != pubs.end() && it->second.stamp != 0;
		};
		if (!finished() && autoStart.load(std::memory_order_acquire)) {
			lock.unlock();
			ensureThread();
			lock.lock();
			if (threadLive.load(std::memory_order_acquire)) {
				pubCv.wait_for(lock, std::chrono::milliseconds(kFirstPictureWaitMs), finished);
			}
		}
		const auto it = pubs.find(key);
		if (it == pubs.end() || it->second.stamp == 0) {
			return false;
		}
		picture = it->second.frame;
		ok = it->second.ok;
		cycle = it->second.cycle;
	}
	if (!ok || cycle || !picture || picture->size() < rowBytes * static_cast<size_t>(height)) {
		return false;
	}
	for (int y = 0; y < height; ++y) {
		std::memcpy(dst + static_cast<size_t>(y) * strideBytes, picture->data() + static_cast<size_t>(y) * rowBytes, rowBytes);
	}
	return true;
}

RenderService::ReadOutcome RenderService::readThreaded(const ReadRequest& request) {
	ReadOutcome outcome;
	if (!validRead(request)) {
		return outcome;
	}
	const size_t rowBytes = static_cast<size_t>(request.width) * 4u;
	const double t = now();
	std::shared_ptr<const Frame> picture;
	bool ok = false;
	bool cycle = false;
	{
		std::unique_lock<std::mutex> lock(pubMutex);
		registerThreaded(nullptr, request.busId, request.width, request.height, t);
		if (request.floats) {
			noteFloatAskLocked(request.busId, request.width, request.height);
		}
		const PubKey key(sizeKey(request.width, request.height), request.busId);
		auto finished = [&]() {
			const auto it = pubs.find(key);
			return it != pubs.end() && it->second.stamp != 0;
		};
		if (!finished() && autoStart.load(std::memory_order_acquire)) {
			lock.unlock();
			ensureThread();
			lock.lock();
			if (threadLive.load(std::memory_order_acquire)) {
				pubCv.wait_for(lock, std::chrono::milliseconds(kFirstPictureWaitMs), finished);
			}
		}
		const auto it = pubs.find(key);
		outcome.status = ReadStatus::NoSignal;
		if (it == pubs.end() || it->second.stamp == 0) {
			return outcome;
		}
		outcome.tick = it->second.tick;
		outcome.isNew = request.sinceTick == 0 || it->second.tick > request.sinceTick;
		if (!outcome.isNew && !request.copyAlways) {
			outcome.status = it->second.ok && !it->second.cycle ? ReadStatus::Unchanged : ReadStatus::NoSignal;
			return outcome;
		}
		picture = it->second.frame;
		ok = it->second.ok;
		cycle = it->second.cycle;
	}
	if (!ok || cycle || !picture || picture->size() < rowBytes * static_cast<size_t>(request.height)) {
		return outcome;
	}
	copyPicture(*picture, request.width, request.height, request);
	outcome.status = ReadStatus::Ok;
	return outcome;
}

// One tick of the thread loop: every registered bus and size is rendered once, in one pass per size, and the finished
// pictures are published. Runs on the render thread, or synchronously from runTickForTest().
void RenderService::runTick(double t) {
	std::vector<Job> jobs;
	{
		std::lock_guard<std::mutex> lock(pubMutex);
		threadRegs.erase(std::remove_if(threadRegs.begin(), threadRegs.end(), [t](const ThreadReg& r) { return t - r.lastSeen > kRegistrationSeconds; }), threadRegs.end());
		for (const ThreadReg& r : threadRegs) {
			bool seen = false;
			for (const Job& j : jobs) {
				if (j.busId == r.busId && j.width == r.width && j.height == r.height) {
					seen = true;
					break;
				}
			}
			if (!seen) {
				jobs.push_back(Job {r.busId, r.width, r.height});
			}
		}
		// A picture nobody is registered for any more is dropped.
		for (auto it = pubs.begin(); it != pubs.end();) {
			bool wanted = false;
			for (const Job& j : jobs) {
				if (it->first == PubKey(sizeKey(j.width, j.height), j.busId)) {
					wanted = true;
					break;
				}
			}
			it = wanted ? std::next(it) : pubs.erase(it);
		}
		if (!jobs.empty()) {
			lastLive = t;
		}
	}
	tickCount.fetch_add(1, std::memory_order_relaxed);
	if (jobs.empty()) {
		return;
	}

	std::lock_guard<std::mutex> lock(mutex);
	++serial;
	for (auto it = sizes.begin(); it != sizes.end();) {
		if (std::max(it->second->lastUsed, it->second->lastApi) + kIdleTicks < serial) {
			it = sizes.erase(it);
		}
		else {
			++it;
		}
	}
	// Group the jobs by size, in the order the sizes first appear.
	std::vector<uint64_t> order;
	for (const Job& j : jobs) {
		const uint64_t key = sizeKey(j.width, j.height);
		if (std::find(order.begin(), order.end(), key) == order.end()) {
			order.push_back(key);
		}
	}
	for (const uint64_t key : order) {
		SizeState* state = nullptr;
		bool begun = false;
		for (const Job& j : jobs) {
			if (sizeKey(j.width, j.height) != key) {
				continue;
			}
			if (!state) {
				state = &stateFor(j.width, j.height, false);
				state->lastUsed = serial;
				state->useSeq = ++useCounter;
			}
			if (!begun) {
				beginPassOn(state->renderer);
				++state->passCount;
				state->passTick = serial;
				begun = true;
			}
			Entry& entry = state->entries[static_cast<size_t>(j.busId)];
			renderEntry(*state, entry, j.busId, j.width, j.height);

			// Publish: the slot's buffer is shared as it is, with no copy. It is never written again, and goes back to
			// the size's pool when the slot, this publication and every consumer have let go of it.
			{
				std::lock_guard<std::mutex> pubLock(pubMutex);
				Pub& pub = pubs[PubKey(key, j.busId)];
				pub.frame = entry.signal ? entry.good : nullptr;
				pub.signal = entry.signal;
				pub.cycle = entry.cycle;
				pub.ok = entry.ok;
				pub.stamp = ++pubCounter;
				pub.tick = serial;
			}
			pubCv.notify_all();
		}
	}
}

RenderService::RenderService() {}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((destructor)) static void renderServiceUnload() {
	stopLiveService();
}
#endif

RenderService& renderService() {
	static RenderService service;
	static const bool hooked = [&]() {
		liveService.store(&service);
		std::atexit(stopLiveService);
		return true;
	}();
	(void)hooked;
	return service;
}
