// Offline checks for Focus (VRU-24): each control does what its tooltip says, at zero it costs nothing and
// leaves the picture alone, the bus node round-trips, and the proof sheet and timings are produced.
// No Rack needed. Build and run with `make test`.
// Optional argv[1]: directory for the proof PNG (focus_contact_sheet.png). Optional argv[2] "bench": also
// print the milliseconds per frame.

#include "focus_filter.hpp"
#include "video_bus.hpp"
#include "video_engine.hpp"
#include "video_external_frame.hpp"
#include "video_graph_renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

static_assert(std::is_trivially_copyable<VideoBusNode>::value, "a bus node is copied word by word through the seqlock");

namespace {

using Frame = std::vector<unsigned char>;
int failures = 0;

void check(bool ok, const char* what) {
	if (!ok) {
		std::printf("FAIL: %s\n", what);
		++failures;
	}
}

// ---- tiny PNG writer and 5x7 label font (proof sheet only) ----

uint32_t crc32(const unsigned char* data, size_t len, uint32_t crc = 0) {
	crc = ~crc;
	for (size_t i = 0; i < len; ++i) {
		crc ^= data[i];
		for (int k = 0; k < 8; ++k) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}
void putBE32(std::vector<unsigned char>& out, uint32_t v) {
	for (int s = 24; s >= 0; s -= 8) {
		out.push_back(static_cast<unsigned char>(v >> s));
	}
}
void pngChunk(std::vector<unsigned char>& file, const char* type, const std::vector<unsigned char>& data) {
	putBE32(file, static_cast<uint32_t>(data.size()));
	std::vector<unsigned char> body(type, type + 4);
	body.insert(body.end(), data.begin(), data.end());
	file.insert(file.end(), body.begin(), body.end());
	putBE32(file, crc32(body.data(), body.size()));
}
bool writePng(const std::string& path, int width, int height, const Frame& rgba) {
	std::vector<unsigned char> raw;
	for (int y = 0; y < height; ++y) {
		raw.push_back(0);
		raw.insert(raw.end(), rgba.begin() + static_cast<std::ptrdiff_t>(y) * width * 4, rgba.begin() + static_cast<std::ptrdiff_t>(y + 1) * width * 4);
	}
	std::vector<unsigned char> z = {0x78, 0x01};
	size_t pos = 0;
	do {
		const size_t n = std::min<size_t>(65535, raw.size() - pos);
		const bool last = pos + n >= raw.size();
		z.push_back(last ? 1 : 0);
		z.push_back(static_cast<unsigned char>(n & 0xFF));
		z.push_back(static_cast<unsigned char>(n >> 8));
		z.push_back(static_cast<unsigned char>(~n & 0xFF));
		z.push_back(static_cast<unsigned char>((~n >> 8) & 0xFF));
		z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
		pos += n;
	} while (pos < raw.size());
	uint32_t a = 1, b = 0;
	for (unsigned char c : raw) {
		a = (a + c) % 65521u;
		b = (b + a) % 65521u;
	}
	putBE32(z, (b << 16) | a);
	std::vector<unsigned char> file = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	std::vector<unsigned char> ihdr;
	putBE32(ihdr, static_cast<uint32_t>(width));
	putBE32(ihdr, static_cast<uint32_t>(height));
	for (unsigned char c : {8, 6, 0, 0, 0}) {
		ihdr.push_back(c);
	}
	pngChunk(file, "IHDR", ihdr);
	pngChunk(file, "IDAT", z);
	pngChunk(file, "IEND", {});
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
	return static_cast<bool>(out);
}

const char* glyph(char c) {
	switch (c) {
	case 'A': return "01110100011000111111100011000110001";
	case 'B': return "11110100011000111110100011000111110";
	case 'C': return "01110100011000010000100001000101110";
	case 'D': return "11110100011000110001100011000111110";
	case 'E': return "11111100001000011110100001000011111";
	case 'F': return "11111100001000011110100001000010000";
	case 'G': return "01110100011000010111100011000101111";
	case 'H': return "10001100011000111111100011000110001";
	case 'I': return "01110001000010000100001000010001110";
	case 'L': return "10000100001000010000100001000011111";
	case 'M': return "10001110111010110101100011000110001";
	case 'N': return "10001110011010110011100011000110001";
	case 'O': return "01110100011000110001100011000101110";
	case 'P': return "11110100011000111110100001000010000";
	case 'R': return "11110100011000111110101001001010001";
	case 'S': return "01111100001000001110000010000111110";
	case 'T': return "11111001000010000100001000010000100";
	case 'U': return "10001100011000110001100011000101110";
	case 'V': return "10001100011000110001100010101000100";
	case 'W': return "10001100011000110101101011101110001";
	case 'X': return "10001100010101000100010101000110001";
	case 'Y': return "10001100010101000100001000010000100";
	case 'Z': return "11111000010001000100010001000011111";
	case '0': return "01110100011001110101110011000101110";
	case '1': return "00100011000010000100001000010001110";
	case '2': return "01110100010000100010001000100011111";
	case '3': return "11110000010000101110000010000111110";
	case '4': return "00010001100101010010111110001000010";
	case '5': return "11111100001111000001000011000101110";
	case '6': return "00110010001000011110100011000101110";
	case '7': return "11111000010001000100010000100001000";
	case '8': return "01110100011000101110100011000101110";
	case '9': return "01110100011000101111000010001001100";
	case '.': return "00000000000000000000000000110001100";
	case '+': return "00000001000010011111001000010000000";
	case '-': return "00000000000000011111000000000000000";
	default: return nullptr;
	}
}

void drawText(Frame& img, int imgW, int x, int y, const std::string& text, int scale) {
	for (char c : text) {
		if (const char* g = glyph(c)) {
			for (int gy = 0; gy < 7; ++gy) {
				for (int gx = 0; gx < 5; ++gx) {
					if (g[gy * 5 + gx] == '1') {
						for (int sy = 0; sy < scale; ++sy) {
							for (int sx = 0; sx < scale; ++sx) {
								const size_t i = (static_cast<size_t>(y + gy * scale + sy) * imgW + (x + gx * scale + sx)) * 4;
								img[i] = img[i + 1] = img[i + 2] = 255;
								img[i + 3] = 255;
							}
						}
					}
				}
			}
		}
		x += 6 * scale;
	}
}

// ---- test pictures ----

double hash01(int x, int y) {
	uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return static_cast<double>((h ^ (h >> 16)) & 0xFFFFu) / 65535.0;
}

// A bright, textured ball on black: lit sphere, fine stripes and a speckle on top, like a mapped ball.
Frame texturedBall(int w, int h) {
	Frame f(static_cast<size_t>(w) * h * 4, 0);
	const double cx = w * 0.5, cy = h * 0.5, r = h * 0.42;
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const double dx = (x - cx) / r, dy = (y - cy) / r;
			const double d2 = dx * dx + dy * dy;
			double v = 0.0;
			if (d2 < 1.0) {
				const double dz = std::sqrt(1.0 - d2);
				const double shade = std::clamp(0.35 + 0.65 * (-0.4 * dx - 0.5 * dy + 0.77 * dz), 0.0, 1.0);
				const double lon = std::atan2(dx, dz) * 9.0, lat = std::asin(std::clamp(dy, -1.0, 1.0)) * 9.0;
				const double stripes = 0.5 + 0.5 * std::sin(lon * 6.0) * std::sin(lat * 6.0);
				const double speckle = hash01(x / 2, y / 2);
				v = shade * (0.30 + 0.45 * stripes + 0.25 * speckle);
			}
			const size_t i = (static_cast<size_t>(y) * w + x) * 4;
			const unsigned char g = static_cast<unsigned char>(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5);
			f[i] = g;
			f[i + 1] = g;
			f[i + 2] = g;
			f[i + 3] = 255;
		}
	}
	return f;
}

Frame solid(int w, int h, unsigned char r, unsigned char g, unsigned char b) {
	Frame f(static_cast<size_t>(w) * h * 4);
	for (size_t i = 0; i < f.size(); i += 4) {
		f[i] = r;
		f[i + 1] = g;
		f[i + 2] = b;
		f[i + 3] = 255;
	}
	return f;
}

void setPixel(Frame& f, int w, int x, int y, unsigned char r, unsigned char g, unsigned char b) {
	const size_t i = (static_cast<size_t>(y) * w + x) * 4;
	f[i] = r;
	f[i + 1] = g;
	f[i + 2] = b;
	f[i + 3] = 255;
}
unsigned char at(const Frame& f, int w, int x, int y, int c) { return f[(static_cast<size_t>(y) * w + x) * 4 + c]; }

// Bus plumbing: bus 1 is the picture, bus 2 is Focus on it.
struct Rig {
	int frameId = videoExternalFrameAllocate();
	VideoGraphRenderer renderer;
	~Rig() { videoExternalFrameRelease(frameId); }
	void setSource(const Frame& f, int w, int h) {
		videoExternalFramePublish(frameId, f.data(), w, h, 0.0);
		videoBusPublishExternalFrame(1, frameId);
	}
	Frame focus(const FocusParams& p, int w, int h) {
		videoBusPublishFocus(2, 1, p);
		return renderer.renderBus(2, w, h);
	}
	Frame scan(int sourceBus, int w, int h) {
		ScanProcessorParams sp;
		sp.lumaGain = 1.0f;
		sp.vHeight = 0.35f;
		sp.vPos = 0.63f;
		sp.zIntensity = 1.0f;
		sp.lines = 128;
		sp.glow = 0.1f;
		sp.persist = 0.f;
		sp.intensity = 0.9f;
		sp.color = false;
		videoBusPublishScanProcessor(3, sourceBus, sp);
		Frame out;
		for (int pass = 0; pass < 4; ++pass) {
			out = renderer.renderBus(3, w, h);
		}
		return out;
	}
};

double meanLuma(const Frame& f) {
	double s = 0.0;
	for (size_t i = 0; i < f.size(); i += 4) {
		s += f[i] + f[i + 1] + f[i + 2];
	}
	return s / (3.0 * static_cast<double>(f.size() / 4));
}
// Fine detail: mean absolute second difference along a row, over the green channel.
double fineDetailX(const Frame& f, int w, int h) {
	double s = 0.0;
	long n = 0;
	for (int y = 0; y < h; ++y) {
		for (int x = 1; x < w - 1; ++x) {
			s += std::fabs(at(f, w, x - 1, y, 1) - 2.0 * at(f, w, x, y, 1) + at(f, w, x + 1, y, 1));
			++n;
		}
	}
	return s / static_cast<double>(n);
}
double fineDetailY(const Frame& f, int w, int h) {
	double s = 0.0;
	long n = 0;
	for (int y = 1; y < h - 1; ++y) {
		for (int x = 0; x < w; ++x) {
			s += std::fabs(at(f, w, x, y - 1, 1) - 2.0 * at(f, w, x, y, 1) + at(f, w, x, y + 1, 1));
			++n;
		}
	}
	return s / static_cast<double>(n);
}
int maxDiff(const Frame& a, const Frame& b) {
	int m = 0;
	for (size_t i = 0; i < a.size(); ++i) {
		m = std::max(m, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
	}
	return m;
}

// ---- the checks ----

void testIdentity() {
	const int w = 160, h = 90;
	Rig rig;
	const Frame src = texturedBall(w, h);
	rig.setSource(src, w, h);
	check(maxDiff(rig.focus(FocusParams{}, w, h), src) == 0, "all controls at zero leave the picture untouched");
	FocusParams off;
	off.focus = 0.8f;
	off.smear = 0.8f;
	off.sharp = 0.8f;
	off.converge = 0.8f;
	off.mix = 0.f;
	check(maxDiff(rig.focus(off, w, h), src) == 0, "MIX at zero shows the original exactly");
	FocusParams soft;
	soft.focus = 0.5f;
	const Frame blurred = rig.focus(soft, w, h);
	check(maxDiff(blurred, src) > 20, "FOCUS changes the picture");
	soft.mix = 0.5f;
	const Frame halfWet = rig.focus(soft, w, h);
	bool between = true;
	for (size_t i = 0; i < src.size(); i += 4) {
		const int lo = std::min(src[i + 1], blurred[i + 1]) - 1, hi = std::max(src[i + 1], blurred[i + 1]) + 1;
		between = between && halfWet[i + 1] >= lo && halfWet[i + 1] <= hi;
	}
	check(between, "MIX at half lies between the original and the fully changed picture");
}

void testFocusAndSmear() {
	const int w = 200, h = 100;
	Rig rig;
	// A white block on black, so the softened edge stays above the rounding floor of an 8-bit picture.
	Frame block = solid(w, h, 0, 0, 0);
	for (int y = 40; y < 60; ++y) {
		for (int x = 90; x < 110; ++x) setPixel(block, w, x, y, 255, 255, 255);
	}
	rig.setSource(block, w, h);
	const double sumIn = meanLuma(block) * w * h;

	FocusParams f;
	f.focus = 1.f;
	const Frame blur = rig.focus(f, w, h);
	check(std::fabs(meanLuma(blur) * w * h - sumIn) < sumIn * 0.01, "FOCUS keeps the total brightness");
	// The blur must reach about 6% of the width (12 px) past the block edge.
	int reachX = 0, reachY = 0;
	for (int d = 1; d < 60; ++d) {
		if (at(blur, w, 110 - 1 + d, 50, 1) > 0) reachX = d;
		if (at(blur, w, 100, 60 - 1 + d, 1) > 0) reachY = d;
	}
	std::printf("  focus reach past a block edge: %d px sideways, %d px vertically (6%% of width = %.0f px)\n", reachX, reachY, 0.06 * w);
	check(reachX >= 10 && reachX <= 14, "FOCUS at full reaches about 6% of the width sideways");
	check(std::abs(reachX - reachY) <= 1, "FOCUS spreads equally sideways and up and down");

	FocusParams s;
	s.smear = 1.f;
	const Frame sm = rig.focus(s, w, h);
	bool otherRowsBlack = true;
	for (int y = 0; y < h; ++y) {
		if (y >= 40 && y < 60) continue;
		for (int x = 0; x < w; ++x) otherRowsBlack = otherRowsBlack && at(sm, w, x, y, 1) == 0;
	}
	check(otherRowsBlack, "SMEAR never touches another line: the block stays on its own lines");
	int behind = 0, ahead = 0;
	for (int d = 1; d < 80; ++d) {
		if (at(sm, w, 90 - d, 50, 1) > 0) behind = d;
		if (at(sm, w, 109 + d, 50, 1) > 0) ahead = d;
	}
	std::printf("  smear tail past a block: %d px to the right, %d px to the left (width %d, 15%% = %.0f px)\n", ahead, behind, w, 0.15 * w);
	check(ahead > behind, "SMEAR trails to the right of a bright shape more than to its left");
	check(ahead >= 20 && ahead <= 32, "SMEAR at full trails about 15% of the width");

	// Vertical detail: alternate lines bright and dark. SMEAR must keep every line as it was.
	Frame lines = solid(w, h, 0, 0, 0);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const unsigned char v = (y % 2) ? 220 : 40;
			setPixel(lines, w, x, y, v, v, v);
		}
	}
	rig.setSource(lines, w, h);
	check(maxDiff(rig.focus(s, w, h), lines) <= 1, "SMEAR leaves vertical detail untouched");
	FocusParams fOnly;
	fOnly.focus = 0.3f;
	check(maxDiff(rig.focus(fOnly, w, h), lines) > 60, "FOCUS does blur alternate lines together");

	// Fine detail on the textured ball.
	Rig r2;
	const Frame ball = texturedBall(320, 180);
	r2.setSource(ball, 320, 180);
	FocusParams half;
	half.focus = 0.5f;
	const Frame fb = r2.focus(half, 320, 180);
	FocusParams halfS;
	halfS.smear = 0.5f;
	const Frame sb = r2.focus(halfS, 320, 180);
	const double x0 = fineDetailX(ball, 320, 180), y0 = fineDetailY(ball, 320, 180);
	std::printf("  fine detail along/across the line: source %.2f/%.2f  focus half %.2f/%.2f  smear half %.2f/%.2f\n", x0, y0,
		fineDetailX(fb, 320, 180), fineDetailY(fb, 320, 180), fineDetailX(sb, 320, 180), fineDetailY(sb, 320, 180));
	check(fineDetailX(fb, 320, 180) < x0 * 0.5 && fineDetailY(fb, 320, 180) < y0 * 0.5, "FOCUS removes fine detail in both directions");
	check(fineDetailX(sb, 320, 180) < x0 * 0.5, "SMEAR removes fine detail along the line");
}

void testSharp() {
	const int w = 200, h = 40;
	Rig rig;
	Frame edge = solid(w, h, 90, 90, 90);
	for (int y = 0; y < h; ++y) {
		for (int x = 100; x < w; ++x) setPixel(edge, w, x, y, 160, 160, 160);
	}
	rig.setSource(edge, w, h);
	FocusParams p;
	p.sharp = 1.f;
	const Frame out = rig.focus(p, w, h);
	int lo = 255, hi = 0;
	for (int x = 80; x < 120; ++x) {
		lo = std::min<int>(lo, at(out, w, x, 20, 1));
		hi = std::max<int>(hi, at(out, w, x, 20, 1));
	}
	check(lo < 90 - 5, "SHARP puts a dark rim beside a bright edge");
	check(hi > 160 + 5, "SHARP puts a bright rim on a bright edge");
	check(at(out, w, 20, 20, 1) == 90 && at(out, w, 180, 20, 1) == 160, "SHARP leaves flat areas alone");
}

void testConverge() {
	const int w = 200, h = 40;
	Rig rig;
	Frame bar = solid(w, h, 0, 0, 0);
	for (int y = 0; y < h; ++y) {
		for (int x = 98; x < 102; ++x) setPixel(bar, w, x, y, 255, 255, 255);
	}
	rig.setSource(bar, w, h);
	FocusParams p;
	p.converge = 1.f;
	const Frame out = rig.focus(p, w, h);
	auto centreOf = [&](int c) {
		double s = 0, m = 0;
		for (int x = 0; x < w; ++x) {
			s += x * at(out, w, x, 20, c);
			m += at(out, w, x, 20, c);
		}
		return s / m;
	};
	const double shift = focus_filter::convergeShift(1.f, w);
	check(std::fabs((centreOf(0) - centreOf(1)) - shift) < 0.3, "CONVERGE slides red to the right by 3% of the width");
	check(std::fabs((centreOf(1) - centreOf(2)) - shift) < 0.3, "CONVERGE slides blue to the left by the same amount");
	check(std::fabs(centreOf(1) - 99.5) < 0.1, "CONVERGE leaves green where it was");
	p.converge = -1.f;
	const Frame rev = rig.focus(p, w, h);
	double sr = 0, mr = 0;
	for (int x = 0; x < w; ++x) {
		sr += x * at(rev, w, x, 20, 0);
		mr += at(rev, w, x, 20, 0);
	}
	check(sr / mr < 99.5 - shift + 0.3, "a negative CONVERGE slides red the other way");
}

void testBusNode() {
	FocusParams p;
	p.focus = 0.25f;
	p.smear = 0.5f;
	p.sharp = 0.75f;
	p.converge = -0.5f;
	p.mix = 0.6f;
	videoBusPublishFocus(9, 4, p);
	VideoBusNode node;
	check(videoBusReadNode(9, node), "a Focus node can be read back");
	check(node.type == VideoBusNodeType::Focus && node.sourceBusId == 4, "the node says Focus and names its source");
	check(node.focus.focus == 0.25f && node.focus.smear == 0.5f && node.focus.sharp == 0.75f && node.focus.converge == -0.5f && node.focus.mix == 0.6f,
		"the node carries every control");
	videoBusClear(9);
}

// ---- proof sheet and timing ----

void downscale2(const Frame& in, int w, int h, Frame& out) {
	out.assign(static_cast<size_t>(w / 2) * (h / 2) * 4, 255);
	for (int y = 0; y < h / 2; ++y) {
		for (int x = 0; x < w / 2; ++x) {
			for (int c = 0; c < 3; ++c) {
				const int s = at(in, w, 2 * x, 2 * y, c) + at(in, w, 2 * x + 1, 2 * y, c) + at(in, w, 2 * x, 2 * y + 1, c) + at(in, w, 2 * x + 1, 2 * y + 1, c);
				out[(static_cast<size_t>(y) * (w / 2) + x) * 4 + c] = static_cast<unsigned char>(s / 4);
			}
		}
	}
}

void contactSheet(const std::string& dir) {
	const int w = 640, h = 360;
	Rig rig;
	const Frame src = texturedBall(w, h);
	rig.setSource(src, w, h);
	FocusParams fHalf, sHalf, sharpHigh, conv;
	fHalf.focus = 0.5f;
	sHalf.smear = 0.5f;
	sharpHigh.sharp = 0.9f;
	conv.converge = 0.8f;
	const Frame a = src, b = rig.focus(fHalf, w, h), c = rig.focus(sHalf, w, h);
	const Frame d = rig.focus(sharpHigh, w, h), e = rig.focus(conv, w, h);
	// Second row: a, b and c through the Scan Processor. Bus 2 is the Focus node, so publish each source on its own bus.
	videoBusPublishFocus(4, 1, fHalf);
	videoBusPublishFocus(5, 1, sHalf);
	const Frame sa = rig.scan(1, w, h), sb = rig.scan(4, w, h), sc = rig.scan(5, w, h);

	const int tw = w / 2, th = h / 2, gap = 6, header = 22;
	const int cols = 5;
	const int sheetW = cols * tw + (cols + 1) * gap, sheetH = 2 * (th + header) + 3 * gap;
	Frame sheet(static_cast<size_t>(sheetW) * sheetH * 4, 40);
	for (size_t i = 3; i < sheet.size(); i += 4) sheet[i] = 255;
	auto place = [&](const Frame& img, int col, int row, const std::string& label) {
		Frame small;
		downscale2(img, w, h, small);
		const int ox = gap + col * (tw + gap), oy = gap + row * (th + header + gap) + header;
		for (int y = 0; y < th; ++y) {
			std::memcpy(&sheet[(static_cast<size_t>(oy + y) * sheetW + ox) * 4], &small[static_cast<size_t>(y) * tw * 4], static_cast<size_t>(tw) * 4);
		}
		drawText(sheet, sheetW, ox, oy - header + 4, label, 2);
	};
	place(a, 0, 0, "A SOURCE");
	place(b, 1, 0, "B FOCUS 50");
	place(c, 2, 0, "C SMEAR 50");
	place(d, 3, 0, "D SHARP 90");
	place(e, 4, 0, "E CONVERGE 80");
	place(sa, 0, 1, "A SCAN");
	place(sb, 1, 1, "B FOCUS SCAN");
	place(sc, 2, 1, "C SMEAR SCAN");
	writePng(dir + "/focus_contact_sheet.png", sheetW, sheetH, sheet);
	std::printf("  wrote %s/focus_contact_sheet.png (%dx%d)\n", dir.c_str(), sheetW, sheetH);
	writePng(dir + "/focus_scan_a.png", w, h, sa);
	writePng(dir + "/focus_scan_b.png", w, h, sb);
	writePng(dir + "/focus_scan_c.png", w, h, sc);
}

double msPerFrame(int w, int h, const FocusParams& p, int frames) {
	Rig rig;
	rig.setSource(texturedBall(w, h), w, h);
	videoBusPublishFocus(2, 1, p);
	for (int i = 0; i < 3; ++i) rig.renderer.renderBus(2, w, h);  // warm up and size the scratch buffers
	// The source bus alone, so the cost of the Focus pass is the difference.
	const auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < frames; ++i) rig.renderer.renderBus(2, w, h);
	const auto t1 = std::chrono::steady_clock::now();
	videoBusPublishFocus(2, 1, FocusParams{});
	const auto t2 = std::chrono::steady_clock::now();
	for (int i = 0; i < frames; ++i) rig.renderer.renderBus(2, w, h);
	const auto t3 = std::chrono::steady_clock::now();
	const double withFocus = std::chrono::duration<double, std::milli>(t1 - t0).count() / frames;
	const double zero = std::chrono::duration<double, std::milli>(t3 - t2).count() / frames;
	std::printf("  %dx%d: %.2f ms per frame, %.2f ms with every knob at zero (resample/copy baseline)\n", w, h, withFocus, zero);
	return withFocus;
}

}  // namespace

int main(int argc, char** argv) {
	const std::string dir = argc > 1 ? argv[1] : "";
	const bool bench = argc > 2 && std::string(argv[2]) == "bench";
	testBusNode();
	testIdentity();
	testFocusAndSmear();
	testSharp();
	testConverge();
	if (!dir.empty()) contactSheet(dir);
	if (bench) {
		FocusParams half;
		half.focus = 0.5f;
		half.smear = 0.5f;
		std::printf("FOCUS and SMEAR both at half:\n");
		msPerFrame(640, 360, half, 60);
		msPerFrame(640, 480, half, 60);
		msPerFrame(1440, 1080, half, 20);
		FocusParams all = half;
		all.sharp = 0.5f;
		all.converge = 0.5f;
		std::printf("plus SHARP and CONVERGE at half:\n");
		msPerFrame(640, 360, all, 60);
		msPerFrame(1440, 1080, all, 20);
	}
	if (failures) {
		std::printf("focus_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("focus_test: all checks passed\n");
	return 0;
}
