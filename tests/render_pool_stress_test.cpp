// The render service's buffer pool under load (docs/architecture.md, "Render service", ownership). A picture a
// consumer holds must not change, in its bytes or in its float plane, for as long as it is held, and a buffer must go
// back to the pool only when its last holder lets go. The test makes the pool recycle hard at one small size while
// several consumers hold pictures for varying short times, hashing each on taking it and again just before letting
// go; float readers come and go so float planes are attached and dropped; the precision setting and the thread mode
// are flipped under load. It also checks that pictures stay valid and the pool is safe when a size is evicted, the
// service is cleared, or a whole service is destroyed while a consumer still holds a picture.
//
// Build with -fsanitize=thread (make tsan): a buffer reused while a consumer still reads it is a data race report.
// No Rack needed.

#include "vru_bus.h"

#include "render_service.hpp"
#include "video_bus.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <thread>
#include <vector>

extern "C" const VruBusApiV1* vruBusGetApi(uint32_t requestedVersion);

static int failures = 0;
static int checks = 0;
#define CHECK(cond) \
	do { \
		++checks; \
		if (!(cond)) { \
			++failures; \
			std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
		} \
	} while (0)

// Every global allocation is counted, so the steady-state test can say how many a tick makes.
static std::atomic<uint64_t> gAllocs{0};
void* operator new(std::size_t n) {
	gAllocs.fetch_add(1, std::memory_order_relaxed);
	void* p = std::malloc(n ? n : 1);
	if (!p) {
		throw std::bad_alloc();
	}
	return p;
}
void operator delete(void* p) noexcept {
	std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
	std::free(p);
}

static std::atomic<uint64_t> gTick{1};
static uint64_t tickSource() {
	return gTick.load();
}

static const VruBusApiV2* api2 = nullptr;
static const int kW = 48;
static const int kH = 8;
static const int kBus = 3;

static void publishPicture(int n) {
	VideoParams v;
	v.phase = static_cast<float>(n % 997) / 997.f;
	v.frequency = 3.f + static_cast<float>(n % 5);
	v.pattern = 0;
	videoBusPublish(1, v);
	// A pointwise node on top, so that with full precision on the picture is a float one and can keep its plane.
	ColorLabParams lab;
	lab.brightness = 0.25f + static_cast<float>(n % 7) * 0.1f;
	videoBusPublishColorLab(kBus, 1, lab);
}

// One hash over the bytes and the float plane, when there is one.
static uint64_t hashPicture(const RenderService::Frame& f) {
	uint64_t h = 1469598103934665603ull;
	for (unsigned char c : f) {
		h = (h ^ c) * 1099511628211ull;
	}
	const unsigned char* p = reinterpret_cast<const unsigned char*>(f.precise.data());
	const size_t n = f.precise.size() * sizeof(float);
	for (size_t i = 0; i < n; ++i) {
		h = (h ^ p[i]) * 1099511628211ull;
	}
	return h;
}

static void busyFor(int micros) {
	const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(micros);
	while (std::chrono::steady_clock::now() < end) {
	}
}

static void setup(RenderService& svc) {
	svc.setThreadMode(false);
	svc.setFullPrecision(false);
	svc.clear();
	svc.setTickSource(tickSource);
	svc.setHiddenRendering(true);
	for (int bus = 1; bus <= kVideoBusCount; ++bus) videoBusClear(bus);
	publishPicture(0);
}

// The main stress: consumers, a recycling driver, float readers and a controller, all at once.
static void poolStress() {
	std::printf("render pool: held pictures never change under recycling, float readers and flips\n");
	RenderService& svc = renderService();
	setup(svc);
	svc.setThreadAutoStart(true);

	std::atomic<bool> stop{false};
	std::atomic<long> holds{0}, changed{0}, planeHolds{0}, floatReads{0}, flips{0}, modeFlips{0}, driven{0};

	// The driver: a new picture and a new tick as fast as it can go, at the same one small size, so buffers recycle hard.
	std::thread driver([&] {
		int owner = 0;
		int n = 0;
		while (!stop.load()) {
			++n;
			publishPicture(n);
			++gTick;
			const RenderService::Result r = svc.frame(&owner, kBus, kW, kH);
			if (r.signal) {
				++driven;
			}
			// Fast enough to recycle hard, slow enough that a float read's ask (kFloatKeepTicks ticks) is still live.
			std::this_thread::sleep_for(std::chrono::microseconds(25));
		}
	});
	// Consumers: take a picture (frame() or touch()), hash it, hold it for a varying short time, hash again, let go.
	std::vector<std::thread> consumers;
	for (int c = 0; c < 4; ++c) {
		consumers.emplace_back([&, c] {
			int owner = 0;
			uint32_t seed = 12345u + static_cast<uint32_t>(c) * 7919u;
			long n = 0;
			while (!stop.load()) {
				++n;
				seed = seed * 1664525u + 1013904223u;
				RenderService::Result r;
				RenderService::Taken t;
				const RenderService::Frame* pixels = nullptr;
				if ((n + c) % 3 == 0) {
					t = svc.touch(&owner, kBus, kW, kH);
					if (t.fresh && t.signal) {
						pixels = t.pixels;
					}
				}
				else {
					r = svc.frame(&owner, kBus, kW, kH);
					if (r.signal) {
						pixels = r.pixels;
					}
				}
				if (pixels && !pixels->empty()) {
					const uint64_t h1 = hashPicture(*pixels);
					busyFor(static_cast<int>((seed >> 16) % 400));
					if (hashPicture(*pixels) != h1) {
						++changed;
					}
					if (!pixels->precise.empty()) {
						++planeHolds;
					}
					++holds;
				}
				// Every so often keep one across a few more calls (a slow consumer), checked again at the end.
				if (n % 17 == 0) {
					RenderService::Result keep = svc.frame(&owner, kBus, kW, kH);
					if (keep.signal && !keep.pixels->empty()) {
						const uint64_t k1 = hashPicture(*keep.pixels);
						for (int j = 0; j < 4; ++j) {
							RenderService::Result other = svc.frame(&owner, kBus, kW, kH);
							(void)other;
						}
						if (hashPicture(*keep.pixels) != k1) {
							++changed;
						}
						++holds;
					}
				}
			}
		});
	}
	// Float readers that come and go, so planes are attached and dropped.
	std::vector<std::thread> readers;
	for (int c = 0; c < 2; ++c) {
		readers.emplace_back([&, c] {
			std::vector<float> out(static_cast<size_t>(kW) * kH * 4u);
			long n = 0;
			while (!stop.load()) {
				++n;
				if (((n / (300 + c * 150)) % 2) == 1) {
					std::this_thread::sleep_for(std::chrono::microseconds(200));
					continue;
				}
				if (api2->readFrameF32(kBus, kW, kH, out.data(), static_cast<size_t>(kW) * 4u) == VRU_BUS_OK) {
					++floatReads;
				}
				std::this_thread::sleep_for(std::chrono::microseconds(50));
			}
		});
	}
	// The controller: precision flipped often, the render thread started and stopped now and then.
	std::thread controller([&] {
		bool on = false;
		int step = 0;
		while (!stop.load()) {
			on = !on;
			svc.setFullPrecision(on);
			++flips;
			++step;
			if (step % 40 == 0) {
				svc.setThreadMode(true);
				std::this_thread::sleep_for(std::chrono::milliseconds(60));
				svc.setThreadMode(false);
				++modeFlips;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(2500));
	stop.store(true);
	controller.join();
	for (auto& t : readers) t.join();
	for (auto& t : consumers) t.join();
	driver.join();
	svc.setThreadMode(false);
	svc.setFullPrecision(false);
	std::printf("  %ld holds (%ld with a float plane), %ld driven ticks, %ld float reads, %ld flips, %ld mode flips, %ld changed\n", holds.load(),
		planeHolds.load(), driven.load(), floatReads.load(), flips.load(), modeFlips.load(), changed.load());
	CHECK(holds.load() > 500);
	CHECK(driven.load() > 2000);
	CHECK(floatReads.load() > 50);
	CHECK(planeHolds.load() > 0);
	CHECK(modeFlips.load() > 0);
	CHECK(changed.load() == 0);
	for (int bus = 1; bus <= kVideoBusCount; ++bus) videoBusClear(bus);
	svc.setTickSource(nullptr);
	svc.clear();
}

// Pictures held across eviction of their size, across clear(), and across the service being destroyed.
static void lifetimes() {
	std::printf("render pool: pictures outlive eviction, clear() and the service\n");
	{
		RenderService svc;
		svc.setTickSource(tickSource);
		svc.setHiddenRendering(true);
		svc.setFullPrecision(true);
		publishPicture(5);
		++gTick;
		(void)svc.frame(kBus, kW, kH);

		// A held picture across eviction by sweeping sizes.
		++gTick;
		publishPicture(6);
		RenderService::Result keep = svc.frame(kBus, kW, kH);
		CHECK(keep.signal);
		const uint64_t hk = hashPicture(*keep.pixels);
		for (int n = 0; n < 12; ++n) {
			++gTick;
			publishPicture(7 + n);
			(void)svc.frame(kBus, 16 + 2 * n, 8);
		}
		CHECK(!svc.hasSize(kW, kH));
		CHECK(hashPicture(*keep.pixels) == hk);

		// Across clear().
		++gTick;
		publishPicture(30);
		RenderService::Result a = svc.frame(kBus, 24, 8);
		const uint64_t ha = hashPicture(*a.pixels);
		svc.clear();
		++gTick;
		publishPicture(31);
		RenderService::Result b = svc.frame(kBus, 24, 8);  // a new renderer for the same size: it must not reuse a's buffer
		CHECK(a.pixels != b.pixels);
		CHECK(hashPicture(*a.pixels) == ha);

		// Across the whole service going away: the pictures are the last owners of their buffers.
		++gTick;
		publishPicture(32);
		RenderService::Result c = svc.frame(kBus, kW, kH);
		const uint64_t hc = hashPicture(*c.pixels);
		keep.hold.reset();  // one of them lets go before the service dies
		svc.clear();
		keep = RenderService::Result();
		RenderService::Result survivor;
		uint64_t hd = 0;
		{
			RenderService doomed;
			doomed.setTickSource(tickSource);
			++gTick;
			publishPicture(33);
			survivor = doomed.frame(kBus, kW, kH);
			CHECK(survivor.signal);
			hd = hashPicture(*survivor.pixels);
		}
		CHECK(survivor.signal && !survivor.pixels->empty());
		CHECK(hashPicture(*survivor.pixels) == hd);
		CHECK(hashPicture(*c.pixels) == hc);
		CHECK(hashPicture(*a.pixels) == ha);
		(void)b;
	}
	// The service the pictures came from is gone; they were released after it (b, c, a at the end of the block above).
	for (int bus = 1; bus <= kVideoBusCount; ++bus) videoBusClear(bus);
}

// Steady state: a few distinct buffers, and no allocation per tick (none beyond what the renderer itself makes).
static void steadyState() {
	std::printf("render pool: bounded buffers and no allocation per tick in steady state\n");
	RenderService svc;
	svc.setTickSource(tickSource);
	svc.setHiddenRendering(true);
	for (int pass = 0; pass < 2; ++pass) {
		svc.setFullPrecision(pass == 1);
		std::vector<const void*> seen;
		seen.reserve(64);  // so the counted loop makes no allocation of its own
		RenderService::Result previous;
		std::vector<float> fout(static_cast<size_t>(kW) * kH * 4u);
		for (int n = 0; n < 40; ++n) {  // warm up
			++gTick;
			publishPicture(n);
			previous = svc.frame(kBus, kW, kH);
		}
		const uint64_t before = gAllocs.load();
		for (int n = 0; n < 300; ++n) {
			++gTick;
			publishPicture(100 + n);
			const RenderService::Result r = svc.frame(kBus, kW, kH);
			CHECK(r.signal);
			if (std::find(seen.begin(), seen.end(), static_cast<const void*>(r.pixels)) == seen.end() && seen.size() < 64) {
				seen.push_back(r.pixels);
			}
			previous = r;
		}
		const uint64_t allocs = gAllocs.load() - before;
		std::printf("  precision %s: %zu distinct buffers over 300 ticks, %llu allocations\n", pass == 1 ? "on" : "off", seen.size(),
			static_cast<unsigned long long>(allocs));
		CHECK(seen.size() <= 6);
		CHECK(allocs == 0);
	}
	CHECK(true);
}

int main() {
	api2 = reinterpret_cast<const VruBusApiV2*>(vruBusGetApi(VRU_BUS_API_VERSION_2));
	CHECK(api2 != nullptr);
	if (!api2) {
		return 1;
	}
	steadyState();
	lifetimes();
	poolStress();
	std::printf("render_pool_stress_test: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
