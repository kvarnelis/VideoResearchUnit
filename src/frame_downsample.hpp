#pragma once

// A smaller copy of a finished picture, for a consumer that reads a picture at a small size while another consumer
// has the same bus drawn larger (src/render_service.cpp, runTick). Drawing the graph a second time at the small size
// costs a whole pass; averaging the large picture down costs a fraction of a millisecond.

#include "render_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace frame_downsample {

// Box average of `src` (sw x sh, RGBA bytes) into `dst` (dw x dh, RGBA bytes), for dw <= sw and dh <= sh.
// Output pixel (x, y) is the mean of source columns [x sw / dw, (x + 1) sw / dw) and rows [y sh / dh, (y + 1) sh / dh),
// each channel rounded to the nearest level. Every source pixel goes into exactly one output pixel, so a flat picture
// stays flat and the mean brightness is kept. With dw == sw and dh == sh it is a copy.
inline void box(const unsigned char* src, int sw, int sh, unsigned char* dst, int dw, int dh) {
	if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || dw > sw || dh > sh) {
		return;
	}
	std::vector<int> columns(static_cast<size_t>(dw) + 1u);
	for (int x = 0; x <= dw; ++x) {
		columns[static_cast<size_t>(x)] = static_cast<int>(static_cast<int64_t>(x) * sw / dw);
	}
	const int* cols = columns.data();
	render_pool::parallelSpan(static_cast<size_t>(dh), 8u, [=](size_t rowBegin, size_t rowEnd) {
		std::vector<uint32_t> sums(static_cast<size_t>(dw) * 4u);
		for (size_t y = rowBegin; y < rowEnd; ++y) {
			const int y0 = static_cast<int>(static_cast<int64_t>(y) * sh / dh);
			const int y1 = static_cast<int>(static_cast<int64_t>(y + 1u) * sh / dh);
			std::fill(sums.begin(), sums.end(), 0u);
			for (int sy = y0; sy < y1; ++sy) {
				const unsigned char* row = src + static_cast<size_t>(sy) * static_cast<size_t>(sw) * 4u;
				uint32_t* sum = sums.data();
				for (int x = 0; x < dw; ++x) {
					uint32_t r = 0, g = 0, b = 0, a = 0;
					for (int sx = cols[x]; sx < cols[x + 1]; ++sx) {
						const unsigned char* px = row + static_cast<size_t>(sx) * 4u;
						r += px[0];
						g += px[1];
						b += px[2];
						a += px[3];
					}
					sum[0] += r;
					sum[1] += g;
					sum[2] += b;
					sum[3] += a;
					sum += 4;
				}
			}
			unsigned char* out = dst + y * static_cast<size_t>(dw) * 4u;
			const uint32_t* sum = sums.data();
			for (int x = 0; x < dw; ++x) {
				const uint32_t count = static_cast<uint32_t>((y1 - y0) * (cols[x + 1] - cols[x]));
				for (int c = 0; c < 4; ++c) {
					out[c] = static_cast<unsigned char>((sum[c] + count / 2u) / count);
				}
				out += 4;
				sum += 4;
			}
		}
	});
}

// Whether a W x H picture has the shape of a w x h one, to within half a percent (a Display's width is its rows
// times 4 / 3 rounded, so it is rarely exact).
inline bool sameShape(int W, int H, int w, int h) {
	const int64_t a = static_cast<int64_t>(W) * h;
	const int64_t b = static_cast<int64_t>(H) * w;
	const int64_t d = a > b ? a - b : b - a;
	return d * 200 <= a;
}

}  // namespace frame_downsample
