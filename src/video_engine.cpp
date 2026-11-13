#include "video_engine.hpp"

#include "shapes_field.hpp"

#include <atomic>
#include <thread>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kBar75 = 0.75f;

float clamp01(float value) {
	return std::clamp(value, 0.f, 1.f);
}
}

const std::vector<unsigned char>& PreviewVideoEngine::render(int width, int height, const VideoParams& params) {
	if (width <= 0 || height <= 0) {
		rgba.clear();
		return rgba;
	}

	const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	if (rgba.size() != byteCount) {
		rgba.assign(byteCount, 0);
	}
	if (previous.size() != byteCount || previousWidth != width || previousHeight != height) {
		previous.assign(byteCount, 0);
		previousWidth = width;
		previousHeight = height;
	}

	switch (static_cast<VideoPattern>(params.pattern)) {
	case VideoPattern::Smpte75:
		renderSmpte75(width, height);
		break;
	case VideoPattern::Ebu75:
		renderEbu75(width, height);
		break;
	case VideoPattern::Ebu100:
		renderEbu100(width, height);
		break;
	case VideoPattern::Grayscale:
		renderGrayscale(width, height);
		break;
	case VideoPattern::Grid:
		renderGrid(width, height);
		break;
	case VideoPattern::BarsAndRows:
		renderPair(width, height, params);
		break;
	case VideoPattern::Oscillator:
	default:
		renderOscillator(width, height, params);
		break;
	}

	applyColorControls(width, height, params);
	previous = rgba;
	return rgba;
}

void PreviewVideoEngine::renderOscillator(int width, int height, const VideoParams& params) {
	const float frequency = std::max(0.05f, params.frequency);
	const float feedback = clamp01(params.feedback);
	const float shape = clamp01(params.shape);
	const float angle = params.phase * 0.19f;
	const float ca = std::cos(angle);
	const float sa = std::sin(angle);

	for (int y = 0; y < height; ++y) {
		const float ny = (static_cast<float>(y) / static_cast<float>(height - 1)) * 2.f - 1.f;
		for (int x = 0; x < width; ++x) {
			const float nx = (static_cast<float>(x) / static_cast<float>(width - 1)) * 2.f - 1.f;
			const float u = nx * ca - ny * sa;
			const float v = nx * sa + ny * ca;
			const float radial = std::sqrt(nx * nx + ny * ny);

			const float carrier = std::sin((u * frequency + params.phase) * kPi);
			const float folded = std::abs(fract(v * frequency * 0.5f + params.phase * 0.071f) * 2.f - 1.f);
			const float rings = std::sin((radial * frequency * 2.6f - params.phase * 0.4f) * kPi);
			const float mixed = carrier * (1.f - shape) + (1.f - folded * 2.f) * shape + rings * 0.25f;
			const float luma = smoothstep(-0.75f, 0.85f, mixed);

			float r = 0.f;
			float g = 0.f;
			float b = 0.f;
			hsvToRgb(fract(params.hue + luma * 0.18f + radial * 0.08f), 0.82f, luma, r, g, b);

			const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
			r = r * (1.f - feedback) + (previous[index + 0] / 255.f) * feedback;
			g = g * (1.f - feedback) + (previous[index + 1] / 255.f) * feedback;
			b = b * (1.f - feedback) + (previous[index + 2] / 255.f) * feedback;

			rgba[index + 0] = static_cast<unsigned char>(clamp01(r) * 255.f);
			rgba[index + 1] = static_cast<unsigned char>(clamp01(g) * 255.f);
			rgba[index + 2] = static_cast<unsigned char>(clamp01(b) * 255.f);
			rgba[index + 3] = 255;
		}
	}
}

void PreviewVideoEngine::renderSmpte75(int width, int height) {
	const std::vector<Color> topBars = {
		barColor(kBar75, kBar75, kBar75),
		barColor(kBar75, kBar75, 0.f),
		barColor(0.f, kBar75, kBar75),
		barColor(0.f, kBar75, 0.f),
		barColor(kBar75, 0.f, kBar75),
		barColor(kBar75, 0.f, 0.f),
		barColor(0.f, 0.f, kBar75),
	};
	const std::vector<Color> middleBars = {
		barColor(0.f, 0.f, kBar75),
		barColor(0.f, 0.f, 0.f),
		barColor(kBar75, 0.f, kBar75),
		barColor(0.f, 0.f, 0.f),
		barColor(0.f, kBar75, kBar75),
		barColor(0.f, 0.f, 0.f),
		barColor(kBar75, kBar75, kBar75),
	};
	const std::vector<Color> bottomBars = {
		barColor(0.f, 0.f, 0.45f),
		barColor(1.f, 1.f, 1.f),
		barColor(0.32f, 0.f, 0.32f),
		barColor(0.f, 0.f, 0.f),
		barColor(0.02f, 0.02f, 0.02f),
		barColor(0.f, 0.f, 0.f),
		barColor(0.06f, 0.06f, 0.06f),
		barColor(0.f, 0.f, 0.f),
	};

	for (int y = 0; y < height; ++y) {
		const float yNorm = static_cast<float>(y) / static_cast<float>(height);
		for (int x = 0; x < width; ++x) {
			const float xNorm = static_cast<float>(x) / static_cast<float>(width);
			Color color;
			if (yNorm < 2.f / 3.f) {
				color = colorForBar(xNorm, topBars);
			}
			else if (yNorm < 0.79f) {
				color = colorForBar(xNorm, middleBars);
			}
			else {
				color = colorForBar(xNorm, bottomBars);
			}
			setPixel(width, x, y, color);
		}
	}
}

void PreviewVideoEngine::renderEbu75(int width, int height) {
	const std::vector<Color> bars = {
		barColor(1.f, 1.f, 1.f),
		barColor(kBar75, kBar75, 0.f),
		barColor(0.f, kBar75, kBar75),
		barColor(0.f, kBar75, 0.f),
		barColor(kBar75, 0.f, kBar75),
		barColor(kBar75, 0.f, 0.f),
		barColor(0.f, 0.f, kBar75),
	};

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			setPixel(width, x, y, colorForBar(static_cast<float>(x) / static_cast<float>(width), bars));
		}
	}
}

void PreviewVideoEngine::renderEbu100(int width, int height) {
	const std::vector<Color> bars = {
		barColor(1.f, 1.f, 1.f),
		barColor(1.f, 1.f, 0.f),
		barColor(0.f, 1.f, 1.f),
		barColor(0.f, 1.f, 0.f),
		barColor(1.f, 0.f, 1.f),
		barColor(1.f, 0.f, 0.f),
		barColor(0.f, 0.f, 1.f),
		barColor(0.f, 0.f, 0.f),
	};

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			setPixel(width, x, y, colorForBar(static_cast<float>(x) / static_cast<float>(width), bars));
		}
	}
}

void PreviewVideoEngine::renderGrayscale(int width, int height) {
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const float ramp = static_cast<float>(x) / static_cast<float>(std::max(1, width - 1));
			const float step = std::floor(ramp * 10.f) / 9.f;
			const float split = static_cast<float>(height) * 0.55f;
			const float value = y < split ? ramp : clamp01(step);
			setPixel(width, x, y, barColor(value, value, value));
		}
	}
}

// Grid lines are anti-aliased and never thinner than two output pixels, so they survive the
// DeflectionYoke's resampling at any render size. halfWidth is in pixels from the line centre.
namespace {
// Coverage of a line of half-width hw at distance d (pixels) from its centre: 1 inside, falling off over one pixel.
float lineCoverage(float d, float hw) {
	return std::clamp(hw + 0.5f - d, 0.f, 1.f);
}

// Distance from p to the nearest multiple of spacing (p and spacing in pixels).
float distanceToGrid(float p, float spacing) {
	if (spacing <= 0.f) {
		return 1e9f;
	}
	return std::abs(p - std::round(p / spacing) * spacing);
}
}

void PreviewVideoEngine::renderGrid(int width, int height) {
	// Two pixels at 720 lines, scaling up with taller renders.
	const float hw = std::max(1.f, static_cast<float>(height) / 720.f);
	const float majorX = static_cast<float>(width) / 8.f;
	const float majorY = static_cast<float>(height) / 8.f;
	const float minorX = static_cast<float>(width) / 32.f;
	const float minorY = static_cast<float>(height) / 32.f;
	const float cx = static_cast<float>(width) * 0.5f;
	const float cy = static_cast<float>(height) * 0.5f;
	const Color background = barColor(0.03f, 0.035f, 0.045f);
	const Color minorColor = barColor(0.11f, 0.13f, 0.16f);
	const Color majorColor = barColor(0.27f, 0.32f, 0.38f);
	const Color crossColor = barColor(0.86f, 0.92f, 1.f);
	const Color borderColor = barColor(1.f, 1.f, 1.f);

	auto mix = [](const Color& a, const Color& b, float t) {
		return Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
	};

	for (int y = 0; y < height; ++y) {
		const float py = static_cast<float>(y);
		for (int x = 0; x < width; ++x) {
			const float px = static_cast<float>(x);
			Color color = background;

			const float minorCov = std::max(lineCoverage(distanceToGrid(px, minorX), hw), lineCoverage(distanceToGrid(py, minorY), hw));
			const float majorCov = std::max(lineCoverage(distanceToGrid(px, majorX), hw), lineCoverage(distanceToGrid(py, majorY), hw));
			const float crossCov = std::max(lineCoverage(std::abs(px - cx), hw), lineCoverage(std::abs(py - cy), hw));
			const float edgeDist = std::min(std::min(px, static_cast<float>(width - 1) - px), std::min(py, static_cast<float>(height - 1) - py));
			const float borderCov = lineCoverage(edgeDist, hw);

			color = mix(color, minorColor, minorCov);
			color = mix(color, majorColor, majorCov);
			color = mix(color, crossColor, crossCov);
			color = mix(color, borderColor, borderCov);
			setPixel(width, x, y, color);
		}
	}
}

float pairWaveValue(double phase, float wave, double footprint) {
	constexpr double kTwoPi = 6.283185307179586;
	if (!std::isfinite(phase)) {
		return 0.f;
	}
	const double w = std::clamp(footprint, 1e-9, 1.0);
	double p = phase - std::floor(phase);
	if (p >= 1.0) {
		p = 0.0;
	}
	const double s = std::clamp(static_cast<double>(wave), 0.0, 1.0);
	double v;
	if (s < 1.0 / 3.0) {
		// Sine at 0, triangle at 1/3.
		const double sine = 0.5 - 0.5 * std::cos(kTwoPi * p);
		const double triangle = 1.0 - std::abs(2.0 * p - 1.0);
		v = sine + (triangle - sine) * (s * 3.0);
	}
	else if (s < 2.0 / 3.0) {
		// Triangle at 1/3, its peak sliding from the middle of the cycle to the end; a ramp at 2/3.
		const double peak = 0.5 + 0.5 * (s * 3.0 - 1.0);
		v = p < peak ? p / peak : (1.0 - p) / (1.0 - peak);
	}
	else {
		// Ramp at 2/3, steepening about the middle of the cycle until it is a square, high on the second half, at 1.
		const double k = s * 3.0 - 2.0;
		const double gain = std::min(1.0 / std::max(1.0 - k, 1e-9), 1.0 / w);
		v = 0.5 + (p - 0.5) * gain;
	}
	// The wrap edge of a ramp or square, and any other fall, takes at least one footprint.
	v = std::min(v, (1.0 - p) / w);
	return static_cast<float>(std::clamp(v, 0.0, 1.0));
}

void PreviewVideoEngine::renderPair(int width, int height, const VideoParams& params) {
	const float feedback = clamp01(params.feedback);
	const float spread = clamp01(params.spread);
	const int mix = std::clamp(params.mix, 0, 3);
	const float waveA = clamp01(params.shape);
	const float waveB = clamp01(params.waveB);
	const double bend = std::clamp(static_cast<double>(params.bend), -8.0, 8.0);
	const double cyclesA = std::clamp(static_cast<double>(params.cyclesA), kPairMinCycles, kPairMaxCycles);
	const double cyclesB = std::clamp(static_cast<double>(params.cyclesB), kPairMinCycles, kPairMaxCycles);
	const double cplA = PairClock::cyclesPerLine(cyclesA, params.barsA);
	const double cplB = PairClock::cyclesPerLine(cyclesB, params.barsB);
	const double stepB = cplB / width;  // B's phase across one pixel (A's also takes the bend)

	// Phase at the start of a scanline. Locked bars begin every line at zero; locked rows run on down the
	// field; a free oscillator starts from the phase latched at the top of the field, and on bars the
	// second, interlaced field starts half a line later.
	auto lineStart = [&](bool locked, bool bars, double cpl, int line, double latched) {
		if (locked) {
			return bars ? 0.0 : PairClock::wrap(cpl * line);
		}
		const double half = bars && params.fieldOdd ? 0.5 : 0.0;
		return PairClock::wrap(latched + cpl * (half + line));
	};
	// One oscillator: the wave, pulled to mid level as a pixel comes to span half a cycle or more,
	// where bars finer than the picture can show would only alias.
	auto oscillator = [&](double phase, float wave, double footprint) {
		const float value = pairWaveValue(phase, wave, footprint);
		const float fade = 1.f - smoothstep(0.3f, 0.5f, static_cast<float>(footprint));
		return 0.5f + (value - 0.5f) * fade;
	};

	const double uBefore = -0.5 / width;
	for (int y = 0; y < height; ++y) {
		const int line = std::min(kPairLines - 1, static_cast<int>((y + 0.5) * kPairLines / height));
		const double startA = lineStart(params.lockA, params.barsA, cplA, line, params.latchedA);
		const double startB = lineStart(params.lockB, params.barsB, cplB, line, params.latchedB);
		const float bBefore = oscillator(startB + cplB * uBefore, waveB, stepB);
		double phaseBefore = startA + cplA * uBefore + bend * bBefore;
		for (int x = 0; x < width; ++x) {
			const double u = (x + 0.5) / width;
			const float b = oscillator(startB + cplB * u, waveB, stepB);
			const double phaseA = startA + cplA * u + bend * b;
			const float a = oscillator(phaseA, waveA, std::abs(phaseA - phaseBefore));
			phaseBefore = phaseA;

			float level = a;
			if (mix == 1) {
				level = 0.5f * (a + b);
			}
			else if (mix == 2) {
				level = 0.5f + 0.5f * (a - b);
			}
			else if (mix == 3) {
				level = a * b;
			}
			const float share = mix == 0 ? 0.f : b / (a + b + 1e-4f);
			float r = 0.f;
			float g = 0.f;
			float blue = 0.f;
			hsvToRgb(fract(params.hue + spread * share), 1.f, level, r, g, blue);

			const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
			r = r * (1.f - feedback) + (previous[index + 0] / 255.f) * feedback;
			g = g * (1.f - feedback) + (previous[index + 1] / 255.f) * feedback;
			blue = blue * (1.f - feedback) + (previous[index + 2] / 255.f) * feedback;
			rgba[index + 0] = static_cast<unsigned char>(clamp01(r) * 255.f);
			rgba[index + 1] = static_cast<unsigned char>(clamp01(g) * 255.f);
			rgba[index + 2] = static_cast<unsigned char>(clamp01(blue) * 255.f);
			rgba[index + 3] = 255;
		}
	}
}

void PreviewVideoEngine::applyColorControls(int width, int height, const VideoParams& params) {
	const float saturation = std::max(0.f, params.saturation);
	const float brightness = std::max(0.f, params.brightness);
	const float contrast = std::max(0.f, params.contrast);
	const bool neutral = std::abs(saturation - 1.f) < 0.0001f && std::abs(brightness - 1.f) < 0.0001f && std::abs(contrast - 1.f) < 0.0001f;
	if (neutral) {
		return;
	}

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
			float r = rgba[index + 0] / 255.f;
			float g = rgba[index + 1] / 255.f;
			float b = rgba[index + 2] / 255.f;
			const float luma = r * 0.2126f + g * 0.7152f + b * 0.0722f;

			r = luma + (r - luma) * saturation;
			g = luma + (g - luma) * saturation;
			b = luma + (b - luma) * saturation;

			r = (r - 0.5f) * contrast + 0.5f;
			g = (g - 0.5f) * contrast + 0.5f;
			b = (b - 0.5f) * contrast + 0.5f;

			rgba[index + 0] = static_cast<unsigned char>(clamp01(r * brightness) * 255.f);
			rgba[index + 1] = static_cast<unsigned char>(clamp01(g * brightness) * 255.f);
			rgba[index + 2] = static_cast<unsigned char>(clamp01(b * brightness) * 255.f);
		}
	}
}

void PreviewVideoEngine::setPixel(int width, int x, int y, Color color) {
	const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
	rgba[index + 0] = static_cast<unsigned char>(clamp01(color.r) * 255.f);
	rgba[index + 1] = static_cast<unsigned char>(clamp01(color.g) * 255.f);
	rgba[index + 2] = static_cast<unsigned char>(clamp01(color.b) * 255.f);
	rgba[index + 3] = 255;
}

float PreviewVideoEngine::fract(float value) {
	return value - std::floor(value);
}

float PreviewVideoEngine::smoothstep(float edge0, float edge1, float value) {
	const float t = clamp01((value - edge0) / (edge1 - edge0));
	return t * t * (3.f - 2.f * t);
}

PreviewVideoEngine::Color PreviewVideoEngine::barColor(float r, float g, float b) {
	return Color {clamp01(r), clamp01(g), clamp01(b)};
}

PreviewVideoEngine::Color PreviewVideoEngine::colorForBar(float xNorm, const std::vector<Color>& bars) {
	if (bars.empty()) {
		return {};
	}
	const int index = std::clamp(static_cast<int>(xNorm * static_cast<float>(bars.size())), 0, static_cast<int>(bars.size()) - 1);
	return bars[index];
}

void PreviewVideoEngine::hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
	h = fract(h) * 6.f;
	const int sector = static_cast<int>(std::floor(h));
	const float f = h - static_cast<float>(sector);
	const float p = v * (1.f - s);
	const float q = v * (1.f - s * f);
	const float t = v * (1.f - s * (1.f - f));

	switch (sector % 6) {
	case 0:
		r = v; g = t; b = p;
		break;
	case 1:
		r = q; g = v; b = p;
		break;
	case 2:
		r = p; g = v; b = t;
		break;
	case 3:
		r = p; g = q; b = v;
		break;
	case 4:
		r = t; g = p; b = v;
		break;
	default:
		r = v; g = p; b = q;
		break;
	}
}

void renderRamps(std::vector<unsigned char>& rgba, int width, int height, const RampsParams& params) {
	if (width <= 0 || height <= 0) {
		rgba.clear();
		return;
	}
	rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
	const RampsParams p = params;
	const float cx = (width - 1) * 0.5f;
	const float cy = (height - 1) * 0.5f;
	const float radius = std::min(width, height) * 0.5f;
	auto fract = [](float v) { return v - std::floor(v); };
	auto layer = [&](int axis, float freq, float phase, float wave, int x, int y) {
		const float dx = x - cx;
		const float dy = y - cy;
		float u = static_cast<float>(x) / width;
		if (axis == 1) u = static_cast<float>(y) / height;
		else if (axis == 2) u = std::sqrt(dx * dx + dy * dy) / radius;
		else if (axis == 3) u = fract(std::atan2(dy, dx) / (2.f * kPi));
		const float pos = fract(u * freq + phase);
		const float tri = 1.f - std::abs(2.f * pos - 1.f);
		const float shapes[4] = {pos, tri, 0.5f - 0.5f * std::cos(2.f * kPi * pos), clamp01((tri - 0.5f) * 4.f + 0.5f)};
		const float morph = clamp01(wave) * 3.f;
		const int segment = std::min(2, static_cast<int>(morph));
		const float t = morph - segment;
		return shapes[segment] + (shapes[segment + 1] - shapes[segment]) * t;
	};
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const float a = layer(p.axisA, p.freqA, p.phaseA, p.waveA, x, y);
			const float b = layer(p.axisB, p.freqB, p.phaseB, p.waveB, x, y);
			float v = a;
			if (p.mix == 1) v = 0.5f * (a + b);
			else if (p.mix == 2) v = 0.5f + 0.5f * (a - b);
			else if (p.mix == 3) v = a * b;
			const unsigned char grey = static_cast<unsigned char>(std::lround(255.f * clamp01(v)));
			const size_t i = (static_cast<size_t>(y) * width + x) * 4u;
			rgba[i] = rgba[i + 1] = rgba[i + 2] = grey;
			rgba[i + 3] = 255;
		}
	}
}

void renderShapes(std::vector<unsigned char>& rgba, int width, int height, const ShapesParams& params) {
	if (width <= 0 || height <= 0) {
		rgba.clear();
		return;
	}
	rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
	const shapes_field::Scene scene = shapes_field::makeScene(params, width, height);
	const float aspectWorld = static_cast<float>(width) / static_cast<float>(height);
	auto renderRows = [&](int firstRow, int endRow) {
		for (int j = firstRow; j < endRow; ++j) {
			const float wy = (0.5f - (static_cast<float>(j) + 0.5f) / static_cast<float>(height)) * 2.f;
			unsigned char* row = rgba.data() + static_cast<size_t>(j) * static_cast<size_t>(width) * 4u;
			for (int i = 0; i < width; ++i) {
				const float wx = ((static_cast<float>(i) + 0.5f) / static_cast<float>(width) - 0.5f) * 2.f * aspectWorld;
				const float v = shapes_field::shade(scene, wx, wy);
				const unsigned char grey = static_cast<unsigned char>(std::lround(255.f * v));
				unsigned char* px = row + static_cast<size_t>(i) * 4u;
				px[0] = px[1] = px[2] = grey;
				px[3] = 255;
			}
		}
	};
	const int pixels = width * height;
	int threads = static_cast<int>(std::thread::hardware_concurrency());
	threads = std::clamp(threads, 1, 8);
	constexpr int kChunkRows = 8;
	const int chunks = (height + kChunkRows - 1) / kChunkRows;
	if (pixels < 24000 || threads == 1 || chunks < 2) {
		renderRows(0, height);
		return;
	}
	// Workers take bands of rows in turn, so the busy middle of the picture is shared out evenly.
	std::atomic<int> nextChunk{0};
	auto worker = [&]() {
		for (;;) {
			const int c = nextChunk.fetch_add(1);
			if (c >= chunks) {
				return;
			}
			renderRows(c * kChunkRows, std::min(height, (c + 1) * kChunkRows));
		}
	};
	std::vector<std::thread> pool;
	pool.reserve(static_cast<size_t>(threads - 1));
	for (int t = 1; t < threads; ++t) {
		pool.emplace_back(worker);
	}
	worker();
	for (std::thread& th : pool) {
		th.join();
	}
}
