#include "crt_beam.hpp"

#include <algorithm>
#include <cmath>

namespace crt {

namespace {
constexpr float kStepPx = 1.f;      // path sampling step: one sample per pixel of beam travel
constexpr int kMaxSteps = 4096;     // a single path never needs more samples than this
// A Round spot is wide, so its path is sampled every two sigma instead of every pixel. Gaussians that
// far apart still sum to an even line (ripple about 1.4 percent), and a wide spot costs far less.
constexpr float kRoundStepSigmas = 2.f;

void splat(EnergyField& field, const BeamSpot& spot, float px, float py, const float rgb[3], int rowLo, int rowHi) {
	const int width = field.width();
	const int height = field.height();
	const float fx = std::floor(px);
	const float fy = std::floor(py);
	const int ix = static_cast<int>(fx);
	const int iy = static_cast<int>(fy);
	const float ax = px - fx;
	const int phase = std::clamp(static_cast<int>((py - fy) * BeamSpot::kPhases + 0.5f), 0, BeamSpot::kPhases);
	const float* w = spot.weights(phase);
	const int top = std::max(rowLo, 0);
	const int bottom = std::min(rowHi, height);
	float* base = field.data();

	for (int t = 0; t < spot.taps(); ++t) {
		const int row = iy + spot.first() + t;
		if (row < top || row >= bottom) {
			continue;
		}
		const float wr = w[t];
		for (int c = 0; c < 2; ++c) {
			const int col = ix + c;
			if (col < 0 || col >= width) {
				continue;
			}
			const float wc = (c == 0 ? 1.f - ax : ax) * wr;
			float* p = base + (static_cast<size_t>(row) * static_cast<size_t>(width) + static_cast<size_t>(col)) * 3u;
			p[0] += wc * rgb[0];
			p[1] += wc * rgb[1];
			p[2] += wc * rgb[2];
		}
	}
}

// The Round spot: the row weights are also used along the columns, with the column phase.
void splatRound(EnergyField& field, const BeamSpot& spot, float px, float py, const float rgb[3], int rowLo, int rowHi) {
	const int width = field.width();
	const int height = field.height();
	const float fx = std::floor(px);
	const float fy = std::floor(py);
	const int ix = static_cast<int>(fx);
	const int iy = static_cast<int>(fy);
	const int phaseX = std::clamp(static_cast<int>((px - fx) * BeamSpot::kPhases + 0.5f), 0, BeamSpot::kPhases);
	const int phaseY = std::clamp(static_cast<int>((py - fy) * BeamSpot::kPhases + 0.5f), 0, BeamSpot::kPhases);
	const float* wx = spot.weights(phaseX);
	const float* wy = spot.weights(phaseY);
	const int taps = spot.taps();
	const int colStart = ix + spot.first();
	// Taps whose column is inside the field: [u0, u1).
	const int u0 = std::max(0, -colStart);
	const int u1 = std::min(taps, width - colStart);
	if (u0 >= u1) {
		return;
	}
	const int top = std::max(rowLo, 0);
	const int bottom = std::min(rowHi, height);
	float* base = field.data();

	for (int t = 0; t < taps; ++t) {
		const int row = iy + spot.first() + t;
		if (row < top || row >= bottom) {
			continue;
		}
		const float wr = wy[t];
		float* p = base + (static_cast<size_t>(row) * static_cast<size_t>(width) + static_cast<size_t>(colStart + u0)) * 3u;
		for (int u = u0; u < u1; ++u) {
			const float w = wr * wx[u];
			p[0] += w * rgb[0];
			p[1] += w * rgb[1];
			p[2] += w * rgb[2];
			p += 3;
		}
	}
}

// 1 - exp(-s) on [0, kMax], tabulated so the per-pixel tone map is a lookup.
struct ToneLut {
	static constexpr int kSteps = 4096;
	static constexpr float kMax = 16.f;
	float value[kSteps + 1];

	ToneLut() {
		for (int i = 0; i <= kSteps; ++i) {
			value[i] = 1.f - std::exp(-kMax * static_cast<float>(i) / static_cast<float>(kSteps));
		}
	}
	float at(float s) const {
		if (s <= 0.f) {
			return 0.f;
		}
		if (s >= kMax) {
			return 1.f;
		}
		const float f = s * (static_cast<float>(kSteps) / kMax);
		const int i = static_cast<int>(f);
		const float frac = f - static_cast<float>(i);
		return value[i] + (value[i + 1] - value[i]) * frac;
	}
};

const ToneLut& toneLut() {
	static const ToneLut lut;
	return lut;
}
}  // namespace

// ---- EnergyField ----

void EnergyField::resize(int width, int height) {
	width_ = std::max(0, width);
	height_ = std::max(0, height);
	rgb_.assign(static_cast<size_t>(width_) * static_cast<size_t>(height_) * 3u, 0.f);
}

void EnergyField::clear() {
	std::fill(rgb_.begin(), rgb_.end(), 0.f);
}

void EnergyField::decay(float retain) {
	for (float& v : rgb_) {
		v *= retain;
	}
}

// ---- BeamSpot ----

BeamSpot::BeamSpot(float sigmaPx, Shape shape) : sigma_(std::max(0.f, sigmaPx)), shape_(shape) {
	if (sigma_ <= 0.f) {
		// Linear: rows iy and iy + 1, weights (1 - f) and f.
		first_ = 0;
		taps_ = 2;
		reach_ = 1;
		for (int p = 0; p <= kPhases; ++p) {
			const float f = static_cast<float>(p) / static_cast<float>(kPhases);
			weights_[p][0] = 1.f - f;
			weights_[p][1] = f;
		}
		return;
	}
	// Gaussian across the stroke. A Row spot is cut at about two sigma (at least one row either side);
	// a Round spot at two and a half, because it is wide enough for the cut to show.
	const bool round = shape == Shape::Round;
	const int radius = round ? std::clamp(static_cast<int>(std::ceil(2.5f * sigma_)), 1, kMaxRoundRadius)
	                         : std::clamp(static_cast<int>(std::ceil(2.f * sigma_)), 1, kMaxRowRadius);
	first_ = -radius;
	taps_ = 2 * radius + 1;
	reach_ = radius;
	for (int p = 0; p <= kPhases; ++p) {
		const float f = static_cast<float>(p) / static_cast<float>(kPhases);
		float sum = 0.f;
		for (int j = 0; j < taps_; ++j) {
			const float d = static_cast<float>(first_ + j) - f;  // row offset from the beam position
			const float w = std::exp(-(d * d) / (2.f * sigma_ * sigma_));
			weights_[p][j] = w;
			sum += w;
		}
		// Normalised, so a deposit adds its full energy whatever the spot size.
		for (int j = 0; j < taps_; ++j) {
			weights_[p][j] /= sum;
		}
	}
}

// ---- Path deposit ----

void depositPath(EnergyField& field, const BeamSpot& spot, float x0, float y0, float x1, float y1, const float rgb[3], float energy, int rowLo, int rowHi) {
	if (field.empty() || !(energy > 0.f) || !(rgb[0] > 0.f || rgb[1] > 0.f || rgb[2] > 0.f)) {
		return;
	}
	const float dx = x1 - x0;
	const float dy = y1 - y0;
	const float length = std::sqrt(dx * dx + dy * dy);
	const bool round = spot.shape() == BeamSpot::Shape::Round;
	const float stepPx = round ? std::max(kStepPx, kRoundStepSigmas * spot.sigma()) : kStepPx;
	const int steps = std::clamp(static_cast<int>(std::ceil(length / stepPx)), 1, kMaxSteps);
	const float each = energy / static_cast<float>(steps);
	const float weighted[3] = {rgb[0] * each, rgb[1] * each, rgb[2] * each};
	for (int s = 0; s < steps; ++s) {
		const float t = (static_cast<float>(s) + 0.5f) / static_cast<float>(steps);
		if (round) {
			splatRound(field, spot, x0 + dx * t, y0 + dy * t, weighted, rowLo, rowHi);
		}
		else {
			splat(field, spot, x0 + dx * t, y0 + dy * t, weighted, rowLo, rowHi);
		}
	}
}

// ---- Glow ----

void Glow::compute(const EnergyField& energy, float amount) {
	const int width = energy.width();
	const int height = energy.height();
	if (amount <= 0.0001f || energy.empty()) {
		field_.clear();
		return;
	}
	constexpr int kDown = 4;
	constexpr int kRadius = 2;
	const int sw = std::max(1, (width + kDown - 1) / kDown);
	const int sh = std::max(1, (height + kDown - 1) / kDown);
	const size_t smallCount = static_cast<size_t>(sw) * static_cast<size_t>(sh) * 3u;
	small_.assign(smallCount, 0.f);
	temp_.assign(smallCount, 0.f);
	field_.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 3u, 0.f);

	const float* src = energy.data();
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const size_t s = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3u;
			const size_t d = (static_cast<size_t>(y / kDown) * static_cast<size_t>(sw) + static_cast<size_t>(x / kDown)) * 3u;
			small_[d + 0] += src[s + 0];
			small_[d + 1] += src[s + 1];
			small_[d + 2] += src[s + 2];
		}
	}
	// A cell is the average of the pixels it covers. The last row or column can cover fewer than
	// kDown, so each cell divides by its own count.
	for (int y = 0; y < sh; ++y) {
		const int rows = std::min(kDown, height - y * kDown);
		for (int x = 0; x < sw; ++x) {
			const int cols = std::min(kDown, width - x * kDown);
			const float norm = 1.f / static_cast<float>(rows * cols);
			const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(sw) + static_cast<size_t>(x)) * 3u;
			small_[i + 0] *= norm;
			small_[i + 1] *= norm;
			small_[i + 2] *= norm;
		}
	}

	// Box blur, horizontal then vertical, twice over. Two passes per axis make a triangular kernel,
	// which fades out with no hard outer edge. One pass would leave a square halo around each stroke.
	const float boxNorm = 1.f / static_cast<float>(2 * kRadius + 1);
	for (int pass = 0; pass < 2; ++pass) {
		for (int y = 0; y < sh; ++y) {
			for (int x = 0; x < sw; ++x) {
				for (int c = 0; c < 3; ++c) {
					float sum = 0.f;
					for (int k = -kRadius; k <= kRadius; ++k) {
						const int xx = std::clamp(x + k, 0, sw - 1);
						sum += small_[(static_cast<size_t>(y) * sw + xx) * 3u + c];
					}
					temp_[(static_cast<size_t>(y) * sw + x) * 3u + c] = sum * boxNorm;
				}
			}
		}
		for (int y = 0; y < sh; ++y) {
			for (int x = 0; x < sw; ++x) {
				for (int c = 0; c < 3; ++c) {
					float sum = 0.f;
					for (int k = -kRadius; k <= kRadius; ++k) {
						const int yy = std::clamp(y + k, 0, sh - 1);
						sum += temp_[(static_cast<size_t>(yy) * sw + x) * 3u + c];
					}
					small_[(static_cast<size_t>(y) * sw + x) * 3u + c] = sum * boxNorm;
				}
			}
		}
	}

	// Read back bilinearly between cell centres, so the glow has no 4-pixel steps. Pixels past the
	// outermost cell centres hold the edge value.
	const auto tapFor = [](int p, int cells) {
		const float f = std::clamp((static_cast<float>(p) + 0.5f) / static_cast<float>(kDown) - 0.5f, 0.f, static_cast<float>(cells - 1));
		const int lo = static_cast<int>(f);
		return CellTap {lo, std::min(lo + 1, cells - 1), f - static_cast<float>(lo)};
	};
	columns_.resize(static_cast<size_t>(width));
	for (int x = 0; x < width; ++x) {
		columns_[static_cast<size_t>(x)] = tapFor(x, sw);
	}
	const float gain = amount * 4.f;
	const size_t rowFloats = static_cast<size_t>(sw) * 3u;
	float* out = field_.data();
	for (int y = 0; y < height; ++y) {
		// temp_ is free after the blur. It holds this row, interpolated between two rows of cells.
		const CellTap row = tapFor(y, sh);
		const float* lo = small_.data() + static_cast<size_t>(row.lo) * rowFloats;
		const float* hi = small_.data() + static_cast<size_t>(row.hi) * rowFloats;
		for (size_t i = 0; i < rowFloats; ++i) {
			temp_[i] = lo[i] + (hi[i] - lo[i]) * row.t;
		}
		for (int x = 0; x < width; ++x) {
			const CellTap& col = columns_[static_cast<size_t>(x)];
			const float* a = temp_.data() + static_cast<size_t>(col.lo) * 3u;
			const float* b = temp_.data() + static_cast<size_t>(col.hi) * 3u;
			for (int c = 0; c < 3; ++c) {
				*out++ = (a[c] + (b[c] - a[c]) * col.t) * gain;
			}
		}
	}
}

// ---- Tone map ----

void toneMap(const EnergyField& energy, const float* glow, float k, unsigned char* rgba) {
	const ToneLut& lut = toneLut();
	const size_t pixels = static_cast<size_t>(energy.width()) * static_cast<size_t>(energy.height());
	const float* e = energy.data();
	for (size_t p = 0; p < pixels; ++p) {
		for (size_t c = 0; c < 3u; ++c) {
			float value = e[p * 3u + c];
			if (glow) {
				value += glow[p * 3u + c];
			}
			const float lit = lut.at(k * value);
			rgba[p * 4u + c] = static_cast<unsigned char>(std::clamp(lit * 255.f + 0.5f, 0.f, 255.f));
		}
		rgba[p * 4u + 3] = 255;
	}
}

// ---- CoilFilter ----

void CoilFilter::setCutoff(float hz, float sampleRate) {
	if (hz == cutoff_ && sampleRate == rate_) {
		return;
	}
	cutoff_ = hz;
	rate_ = sampleRate;
	if (!(hz > 0.f) || !(sampleRate > 0.f)) {
		coefficient_ = 1.f;
		return;
	}
	const float a = 1.f - std::exp(-2.f * 3.14159265358979323846f * hz / sampleRate);
	coefficient_ = std::clamp(a, 0.f, 1.f);
}

}  // namespace crt
