#pragma once

#include <cstddef>
#include <vector>

// CRT beam layer. Shared by Scope Trace and DeflectionYoke, and meant for the next raster module
// (Scan Processor style scan-converters). See docs/architecture.md, "CRT beam layer".
//
// Model. A beam writes light as energy. Energy is linear, additive, and kept in float per pixel
// (EnergyField). depositPath spreads the energy it is given evenly along the path, so the caller
// decides how bright a stroke is. A caller that gives every stroke the same energy gets the beam-speed
// law: a stroke that moves fast covers more pixels and gets less energy per pixel, and a slow stroke
// or one that folds back gets more, so brightness per screen area goes as 1/speed (the DeflectionYoke and
// Scan Processor do this). A caller that gives each stroke energy in proportion to its length gets an even
// brightness along the trace instead. Scope Trace blends the two with its DWELL knob (see
// VideoGraphRenderer::depositScopeTrace). Strokes that overlap add. Phosphor persistence multiplies
// the field by a retain factor each frame (EnergyField::decay). Glow is a blur-add over the energy
// (Glow). The tone curve 1 - exp(-k E) turns energy into RGBA8 (toneMap).
//
// Scan timing. Scanning follows NTSC-like timing that does not depend on the render height: the
// line rate is fixed at 15,734 Hz, a field is the visible scanlines plus a vertical blanking
// (flyback) interval of kBlankingLines with the beam off, and the field rate follows from the line
// count. With 240 lines the field is 262.5 lines long, which is 59.94 Hz.

namespace crt {

constexpr double kLineRateHz = 15734.264;  // NTSC horizontal scan rate
constexpr double kBlankingLines = 22.5;    // lines per field with the beam off (262.5 - 240)
constexpr int kMinLines = 120;
constexpr int kMaxLines = 600;
constexpr int kDefaultLines = 240;

// Time for one visible scanline, in seconds.
inline double lineSeconds() {
	return 1.0 / kLineRateHz;
}

// Field period in seconds for `lines` visible scanlines, including the flyback.
inline double fieldSeconds(int lines) {
	return (static_cast<double>(lines) + kBlankingLines) / kLineRateHz;
}

// Linear RGB energy, one float per channel per pixel. One per beam node per renderer.
class EnergyField {
public:
	// Reallocates to width x height and clears to zero.
	void resize(int width, int height);
	void clear();
	// Phosphor persistence: every value is multiplied by retain (0 = forget, 1 = keep).
	void decay(float retain);

	int width() const {
		return width_;
	}
	int height() const {
		return height_;
	}
	bool matches(int width, int height) const {
		return width == width_ && height == height_;
	}
	bool empty() const {
		return rgb_.empty();
	}
	float* data() {
		return rgb_.data();
	}
	const float* data() const {
		return rgb_.data();
	}

private:
	int width_ = 0;
	int height_ = 0;
	std::vector<float> rgb_;  // width * height * 3, interleaved
};

// Profile of the spot. A Row spot is a Gaussian of sigmaPx rows across the stroke and linear (two
// columns) along the pixel grid. That suits the near-horizontal scanlines of the DeflectionYoke and
// Scan Processor, and it leaves a vertical stroke the same width whatever sigmaPx is. A Round spot is the
// same Gaussian along both axes, so a stroke is as wide going up as going across; Scope Trace draws
// strokes in every direction and uses it. sigmaPx 0 gives a plain bilinear splat in either shape.
// Build once per frame and reuse.
class BeamSpot {
public:
	enum class Shape { Row, Round };

	static constexpr int kPhases = 32;           // sub-pixel steps for the weights
	static constexpr int kMaxRowRadius = 4;      // a Row spot reaches this many rows at most
	static constexpr int kMaxRoundRadius = 20;   // a Round spot reaches this many rows and columns at most
	static constexpr int kMaxTaps = 2 * kMaxRoundRadius + 1;

	explicit BeamSpot(float sigmaPx = 0.f, Shape shape = Shape::Row);

	float sigma() const {
		return sigma_;
	}
	Shape shape() const {
		return shape_;
	}
	// Row offset (from the row under the beam) of the first tap, and the number of taps. A Round spot
	// uses the same first tap and tap count for its columns.
	int first() const {
		return first_;
	}
	int taps() const {
		return taps_;
	}
	// Rows a deposit can reach above or below its centre. Callers pad band tests by this.
	int reach() const {
		return reach_;
	}
	const float* weights(int phase) const {
		return weights_[phase];
	}

private:
	float sigma_ = 0.f;
	Shape shape_ = Shape::Row;
	int first_ = 0;
	int taps_ = 2;
	int reach_ = 1;
	float weights_[kPhases + 1][kMaxTaps] {};
};

// Adds a beam path from (x0, y0) to (x1, y1). The path carries total energy `energy` times rgb,
// spread evenly along its length and splatted with `spot`. Only rows in [rowLo, rowHi) are written,
// so callers can split rows across threads with no shared writes. Pixels outside the field are dropped.
void depositPath(EnergyField& field, const BeamSpot& spot, float x0, float y0, float x1, float y1, const float rgb[3], float energy, int rowLo, int rowHi);

// Blur-add glow, as the Scope Trace bloom: energy averaged into a 4x downsample, two box-blur
// passes per axis (a triangular kernel, so a halo has no hard outer edge), and read back bilinearly
// between cell centres, so the glow shows no 4-pixel grid. amount 0 gives a zero field.
class Glow {
public:
	void compute(const EnergyField& energy, float amount);
	// width * height * 3 floats, valid after compute(). Zero until the first compute().
	const float* data() const {
		return field_.empty() ? nullptr : field_.data();
	}

private:
	// A pixel's place on the downsampled grid: the cells either side of it, and the weight of hi.
	struct CellTap {
		int lo;
		int hi;
		float t;
	};

	std::vector<float> small_;
	std::vector<float> temp_;
	std::vector<float> field_;
	std::vector<CellTap> columns_;
};

// Tone-maps energy plus an optional glow field into RGBA8 (alpha 255): out = 1 - exp(-k (E + G)).
// glow may be nullptr. out must hold width * height * 4 bytes.
void toneMap(const EnergyField& energy, const float* glow, float k, unsigned char* rgba);

// One-pole lowpass for one deflection channel: the yoke's inductance. High tones deflect less.
class CoilFilter {
public:
	// Sets the -3 dB cutoff. Cheap, so callers may call it every sample.
	void setCutoff(float hz, float sampleRate);
	float process(float x) {
		state_ += coefficient_ * (x - state_);
		return state_;
	}
	void reset(float value = 0.f) {
		state_ = value;
	}

private:
	float coefficient_ = 1.f;
	float state_ = 0.f;
	float cutoff_ = -1.f;
	float rate_ = -1.f;
};

}  // namespace crt
