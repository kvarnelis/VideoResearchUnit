#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "render_pool.hpp"

// The picture type the video graph hands from node to node (see docs/architecture.md, "Precision").
//
// A VideoFrame is a std::vector<unsigned char> (RGBA, 8 bits a channel, exactly the old Frame) that can also hold
// the same picture as float32 RGBA in 0..255 units, so an integer level is exact and a conversion is a cast. The
// byte vector is the base class on purpose: every node that still works in 8 bits keeps reading and writing
// frame[i] with no change, and the stores, the ring and the public API keep taking the same std::vector. The float
// plane lives beside it, and `format` says which plane holds the picture. A frame is never both: in F32 format the
// base vector is empty, so code that has not been converted cannot read stale bytes by accident. The graph renderer
// turns a float frame into bytes (ensureU8) before it gives it to a node that has not been converted.
//
// Store rules. A node's maths is float; each node used to end by turning its result into a byte by its own rule
// (see StoreRule). In U8 format the rule is applied exactly as before. In F32 format the value is stored clamped
// to 0..255 and not rounded.
enum class FrameFormat : uint8_t { U8, F32 };

enum class StoreRule : uint8_t {
	Truncate,     // (unsigned char)clamp(v, 0, 255)
	RoundHalfUp,  // (unsigned char)clamp(v + 0.5, 0, 255)
	Lround,       // (unsigned char)clamp(lround(v), 0, 255)
};

// One float (0..255 units) to one byte by `rule`.
template <StoreRule Rule>
inline unsigned char storeByte(float v) {
	if (Rule == StoreRule::Truncate) {
		return static_cast<unsigned char>(std::clamp(v, 0.f, 255.f));
	}
	if (Rule == StoreRule::RoundHalfUp) {
		return static_cast<unsigned char>(std::clamp(v + 0.5f, 0.f, 255.f));
	}
	return static_cast<unsigned char>(std::clamp(static_cast<int>(std::lround(v)), 0, 255));
}

// Whole-plane conversions. Float to byte is round half up, the rule every picture that leaves the graph gets.
inline void floatToBytes(const float* src, unsigned char* dst, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		dst[i] = storeByte<StoreRule::RoundHalfUp>(src[i]);
	}
}
inline void bytesToFloat(const unsigned char* src, float* dst, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		dst[i] = static_cast<float>(src[i]);
	}
}

class VideoFrame : public std::vector<unsigned char> {
public:
	using Bytes = std::vector<unsigned char>;

	VideoFrame() = default;
	explicit VideoFrame(const Bytes& bytes) : Bytes(bytes) {}
	explicit VideoFrame(Bytes&& bytes) : Bytes(std::move(bytes)) {}

	FrameFormat format = FrameFormat::U8;
	std::vector<float> f;  // the picture when format is F32, 0..255 units, RGBA
	// A picture that leaves the graph (a render service slot) is a byte frame. When a float reader has asked for it
	// (include/vru_bus.h, readFrameF32, with "Full precision between modules" on), the slot also keeps the picture
	// before rounding here: RGBA float, 0..255 units, the same count as the bytes, which are exactly this plane
	// rounded half up. Empty otherwise, and always empty in the graph itself. It is NOT part of the picture a node
	// reads: swap() and copyFrom() leave it alone, and nothing but the renderer's root hand-out and the service writes it.
	std::vector<float> precise;

	bool isFloat() const {
		return format == FrameFormat::F32;
	}
	// How many channel values the picture has (width x height x 4), whichever plane holds it.
	size_t count() const {
		return isFloat() ? f.size() : size();
	}
	bool hasPixels() const {
		return count() != 0;
	}
	// One channel value as a float in 0..255 units, whatever the format. (Per-pixel loops should not call this: take
	// the plane pointer once, outside the loop.)
	float at(size_t i) const {
		return isFloat() ? f[i] : static_cast<float>((*this)[i]);
	}

	// Make this a byte frame of `n` values (zeroed when the size changes), or a float frame of `n` values.
	void beginU8(size_t n) {
		format = FrameFormat::U8;
		if (size() != n) {
			Bytes::assign(n, 0);
		}
	}
	void beginF32(size_t n) {
		format = FrameFormat::F32;
		Bytes::clear();
		if (f.size() != n) {
			f.resize(n);
		}
	}

	// Hide the byte vector's assign/swap so that a frame can never keep a stale format after one.
	template <class It>
	void assign(It first, It last) {
		Bytes::assign(first, last);
		format = FrameFormat::U8;
	}
	void assign(size_t n, unsigned char value) {
		Bytes::assign(n, value);
		format = FrameFormat::U8;
	}
	void swap(VideoFrame& other) {
		Bytes::swap(other);
		f.swap(other.f);
		std::swap(format, other.format);
	}
	void swap(Bytes& other) {
		Bytes::swap(other);
		format = FrameFormat::U8;
	}
	// A copy of the picture in whichever format it is in (the memo and the shared taps use this).
	void copyFrom(const VideoFrame& other) {
		format = other.format;
		if (other.isFloat()) {
			Bytes::clear();
			if (f.size() == other.f.size() && f.size() >= kParallelCopyFloats) {
				// A frame being refilled at its size: the pages are already mapped, so the copy is memory bandwidth and
				// the helpers share it. The bytes written are the same.
				float* dst = f.data();
				const float* src = other.f.data();
				render_pool::parallelSpan(f.size(), kParallelCopyFloats / 4, [dst, src](size_t begin, size_t end) {
					std::memcpy(dst + begin, src + begin, (end - begin) * sizeof(float));
				});
			}
			else {
				f.assign(other.f.begin(), other.f.end());
			}
		}
		else if (size() == other.size() && size() >= kParallelCopyBytes) {
			// The same for a byte picture of 2 MiB or more (960 x 720 and up); a smaller one is quicker copied in one go.
			unsigned char* dst = data();
			const unsigned char* src = other.data();
			render_pool::parallelSpan(size(), kParallelCopyBytes / 4, [dst, src](size_t begin, size_t end) {
				std::memcpy(dst + begin, src + begin, end - begin);
			});
		}
		else {
			Bytes::assign(other.begin(), other.end());
		}
	}
	static constexpr size_t kParallelCopyFloats = 1u << 20;
	static constexpr size_t kParallelCopyBytes = 1u << 21;

	// To a byte frame, in place, round half up. A byte frame is left alone.
	void ensureU8() {
		if (!isFloat()) {
			return;
		}
		Bytes::resize(f.size());
		floatToBytes(f.data(), Bytes::data(), f.size());
		format = FrameFormat::U8;
	}
	// To a float frame, in place. A float frame is left alone.
	void ensureF32() {
		if (isFloat()) {
			return;
		}
		f.resize(size());
		bytesToFloat(Bytes::data(), f.data(), size());
		Bytes::clear();
		format = FrameFormat::F32;
	}
	// The picture as bytes in `dst`, whichever format this is in, without changing this frame.
	void copyBytesTo(Bytes& dst) const {
		if (isFloat()) {
			dst.resize(f.size());
			floatToBytes(f.data(), dst.data(), f.size());
		}
		else {
			dst.assign(begin(), end());
		}
	}
};

// Per-pixel readers and writers. A node's loop is written once as a generic lambda over a reader and a writer, and
// the format is chosen outside the pixel loop (withSource, withDest), so the byte path compiles to the same loop it
// always was.
struct ByteSource {
	const unsigned char* p;
	float operator[](size_t i) const {
		return p[i];
	}
};
struct FloatSource {
	const float* p;
	float operator[](size_t i) const {
		return p[i];
	}
};
// A reader that checks the format at every read. Only for nodes that read several pictures, where taking every
// combination of formats as its own loop is not worth the code; the all-bytes case still gets ByteSource.
struct MixedSource {
	const unsigned char* u;
	const float* f;
	float operator[](size_t i) const {
		return f != nullptr ? f[i] : static_cast<float>(u[i]);
	}
};
inline MixedSource mixedSourceOf(const VideoFrame& frame) {
	return frame.isFloat() ? MixedSource{nullptr, frame.f.data()} : MixedSource{frame.data(), nullptr};
}

template <StoreRule Rule>
struct ByteDest {
	unsigned char* p;
	void put(size_t i, float v) const {
		p[i] = storeByte<Rule>(v);
	}
	// `x` already carries the half level added by round-half-up (the caller wrote `v * 255.f + 0.5f`, which the
	// compiler fuses into one multiply-add; adding the half inside a helper would round once more and could move a
	// pixel by one level). The byte is x cut off.
	void putBiased(size_t i, float x) const {
		p[i] = storeByte<StoreRule::Truncate>(x);
	}
};
struct FloatDest {
	float* p;
	void put(size_t i, float v) const {
		p[i] = std::clamp(v, 0.f, 255.f);
	}
	void putBiased(size_t i, float x) const {
		p[i] = std::clamp(x - 0.5f, 0.f, 255.f);
	}
};

template <class Fn>
inline void withSource(const VideoFrame& frame, Fn&& fn) {
	if (frame.isFloat()) {
		fn(FloatSource{frame.f.data()});
	}
	else {
		fn(ByteSource{frame.data()});
	}
}
// `out` must already have been begun (beginU8 or beginF32); the writer follows its format.
template <StoreRule Rule, class Fn>
inline void withDest(VideoFrame& out, Fn&& fn) {
	if (out.isFloat()) {
		fn(FloatDest{out.f.data()});
	}
	else {
		fn(ByteDest<Rule>{out.data()});
	}
}
