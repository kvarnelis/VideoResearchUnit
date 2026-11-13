// Offline checks for Shapes (VRU-21): the distance picture, every shape and mode on the panel, the control map,
// the bus node and the proof pictures. No Rack needed. Build and run with `make test`.
// Optional argv[1]: directory for the proof PNGs (the same folder the bus tests use; files are named shapes_*.png).

#include "shapes_field.hpp"
#include "shapes_math.hpp"
#include "video_bus.hpp"
#include "video_engine.hpp"
#include "video_graph_renderer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <vector>

static_assert(std::is_trivially_copyable<VideoBusNode>::value, "a bus node is copied word by word through the seqlock");

namespace {

namespace sf = shapes_field;
using Frame = std::vector<unsigned char>;

constexpr double kPi = 3.14159265358979323846;
constexpr int kW = 256;
constexpr int kH = 144;
constexpr double kAspectWorld = static_cast<double>(kW) / kH;
constexpr double kPixel = 2.0 / kH;  // world size of one pixel at the standard test size

int failures = 0;
int checks = 0;
std::string gProofDir = "build/bus_test_proofs";

void check(bool ok, const std::string& what) {
	++checks;
	if (!ok) {
		++failures;
		std::printf("  FAIL: %s\n", what.c_str());
	}
}

void checkNear(double got, double want, double tolerance, const std::string& what) {
	++checks;
	if (!(std::fabs(got - want) <= tolerance)) {
		++failures;
		std::printf("  FAIL: %s (got %.6g, want %.6g, tolerance %.3g)\n", what.c_str(), got, want, tolerance);
	}
}

void checkAtMost(double got, double limit, const std::string& what) {
	++checks;
	if (!(got <= limit)) {
		++failures;
		std::printf("  FAIL: %s (got %.6g, limit %.6g)\n", what.c_str(), got, limit);
	}
}

// A label with numbers in it, for checks inside loops.
std::string label(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string label(const char* format, ...) {
	char buffer[256];
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof buffer, format, args);
	va_end(args);
	return buffer;
}

// ---- frames ----

Frame render(const ShapesParams& p, int w = kW, int h = kH) {
	Frame f;
	renderShapes(f, w, h, p);
	return f;
}

int grey(const Frame& f, int w, int x, int y) {
	return f[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u];
}

// World position (y up, 1.0 is half the picture height) of the middle of pixel (i, j).
double worldX(int i, int w, int h) {
	return ((i + 0.5) / w - 0.5) * 2.0 * w / h;
}
double worldY(int j, int h) {
	return (0.5 - (j + 0.5) / h) * 2.0;
}

// True when every pixel has equal red, green and blue and a full alpha.
bool isOpaqueGrey(const Frame& f) {
	for (size_t i = 0; i + 3 < f.size(); i += 4) {
		if (f[i] != f[i + 1] || f[i] != f[i + 2] || f[i + 3] != 255) {
			return false;
		}
	}
	return !f.empty();
}

int maxDiff(const Frame& a, const Frame& b) {
	if (a.size() != b.size() || a.empty()) {
		return 255;
	}
	int worst = 0;
	for (size_t i = 0; i < a.size(); ++i) {
		worst = std::max(worst, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
	}
	return worst;
}

// What a grey frame holds, measured as areas of white: the sum of the grey is the area of a MATTE picture to a
// fraction of a pixel. Pixels at or above mid grey are inside the shape (grey 128 means the distance is not above 0).
struct Measure {
	double area = 0.0;  // world units squared, from the grey sum
	double cx = 0.0;    // centre of the grey, world
	double cy = 0.0;
	double ixx = 0.0;   // second moments about that centre
	double iyy = 0.0;
	double minX = 1e9;  // extent of the pixels at or above mid grey
	double maxX = -1e9;
	double minY = 1e9;
	double maxY = -1e9;
	int inside = 0;
};

Measure measure(const Frame& f, int w, int h) {
	Measure m;
	const double pix = 2.0 / h;
	double sum = 0.0, sx = 0.0, sy = 0.0;
	for (int j = 0; j < h; ++j) {
		for (int i = 0; i < w; ++i) {
			const int g = grey(f, w, i, j);
			const double v = g / 255.0;
			const double wx = worldX(i, w, h), wy = worldY(j, h);
			sum += v;
			sx += v * wx;
			sy += v * wy;
			if (g >= 128) {
				++m.inside;
				m.minX = std::min(m.minX, wx);
				m.maxX = std::max(m.maxX, wx);
				m.minY = std::min(m.minY, wy);
				m.maxY = std::max(m.maxY, wy);
			}
		}
	}
	m.area = sum * pix * pix;
	if (sum > 0.0) {
		m.cx = sx / sum;
		m.cy = sy / sum;
		double xx = 0.0, yy = 0.0;
		for (int j = 0; j < h; ++j) {
			for (int i = 0; i < w; ++i) {
				const double v = grey(f, w, i, j) / 255.0;
				const double dx = worldX(i, w, h) - m.cx, dy = worldY(j, h) - m.cy;
				xx += v * dx * dx;
				yy += v * dy * dy;
			}
		}
		m.ixx = xx / sum;
		m.iyy = yy / sum;
	}
	return m;
}

// Separate shapes in the picture: groups of touching pixels at or above mid grey.
int countBlobs(const Frame& f, int w, int h, int minPixels = 4) {
	std::vector<unsigned char> seen(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
	std::vector<int> stack;
	int blobs = 0;
	for (int start = 0; start < w * h; ++start) {
		if (seen[static_cast<size_t>(start)] || grey(f, w, start % w, start / w) < 128) {
			continue;
		}
		int size = 0;
		seen[static_cast<size_t>(start)] = 1;
		stack.assign(1, start);
		while (!stack.empty()) {
			const int at = stack.back();
			stack.pop_back();
			++size;
			const int x = at % w, y = at / w;
			const int nx[4] = {x - 1, x + 1, x, x};
			const int ny[4] = {y, y, y - 1, y + 1};
			for (int k = 0; k < 4; ++k) {
				if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) {
					continue;
				}
				const int to = ny[k] * w + nx[k];
				if (!seen[static_cast<size_t>(to)] && grey(f, w, nx[k], ny[k]) >= 128) {
					seen[static_cast<size_t>(to)] = 1;
					stack.push_back(to);
				}
			}
		}
		if (size >= minPixels) {
			++blobs;
		}
	}
	return blobs;
}

// The same pixel loop as renderShapes, on one thread, for comparing the threaded renderer with the kernel.
Frame referenceRender(const ShapesParams& p, int w, int h) {
	const sf::Scene scene = sf::makeScene(p, w, h);
	const float aspectWorld = static_cast<float>(w) / static_cast<float>(h);
	Frame out(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
	for (int j = 0; j < h; ++j) {
		const float wy = (0.5f - (static_cast<float>(j) + 0.5f) / static_cast<float>(h)) * 2.f;
		for (int i = 0; i < w; ++i) {
			const float wx = ((static_cast<float>(i) + 0.5f) / static_cast<float>(w) - 0.5f) * 2.f * aspectWorld;
			const unsigned char g = static_cast<unsigned char>(std::lround(255.f * sf::shade(scene, wx, wy)));
			unsigned char* px = out.data() + (static_cast<size_t>(j) * static_cast<size_t>(w) + static_cast<size_t>(i)) * 4u;
			px[0] = px[1] = px[2] = g;
			px[3] = 255;
		}
	}
	return out;
}

// ---- minimal PNG writer (stored deflate blocks, no compression) and proof sheets ----

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
	out.push_back(static_cast<unsigned char>(v >> 24));
	out.push_back(static_cast<unsigned char>(v >> 16));
	out.push_back(static_cast<unsigned char>(v >> 8));
	out.push_back(static_cast<unsigned char>(v));
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
	ihdr.push_back(8);
	ihdr.push_back(6);
	ihdr.push_back(0);
	ihdr.push_back(0);
	ihdr.push_back(0);
	pngChunk(file, "IHDR", ihdr);
	pngChunk(file, "IDAT", z);
	pngChunk(file, "IEND", {});
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
	return static_cast<bool>(out);
}

// Lays equal-sized frames out in a grid with a white gutter and writes them as one PNG in the proof folder.
void saveSheet(const std::string& name, const std::vector<Frame>& panels, int cols, int w = kW, int h = kH) {
	const int gutter = 4;
	const int rows = (static_cast<int>(panels.size()) + cols - 1) / cols;
	const int ow = cols * w + (cols - 1) * gutter;
	const int oh = rows * h + (rows - 1) * gutter;
	Frame out(static_cast<size_t>(ow) * static_cast<size_t>(oh) * 4u, 255);
	for (size_t p = 0; p < panels.size(); ++p) {
		const int x0 = (static_cast<int>(p) % cols) * (w + gutter);
		const int y0 = (static_cast<int>(p) / cols) * (h + gutter);
		for (int y = 0; y < h; ++y) {
			for (int x = 0; x < w; ++x) {
				for (int c = 0; c < 4; ++c) {
					out[(static_cast<size_t>(y0 + y) * static_cast<size_t>(ow) + static_cast<size_t>(x0 + x)) * 4u + static_cast<size_t>(c)] =
						panels[p][(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u + static_cast<size_t>(c)];
				}
			}
		}
	}
	const std::string path = gProofDir + "/" + name;
	check(writePng(path, ow, oh, out), "proof written: " + path);
	std::printf("  proof %s (%d panels)\n", path.c_str(), static_cast<int>(panels.size()));
}

// ---- references that use none of the kernel's code ----

double polygonArea(int m, double apothem) {
	return m * apothem * apothem * std::tan(kPi / m);
}

// Area of an m-sided shape whose corners are rounded by ROUND: the polygon shrunk by 1 - ROUND, grown by a disc.
double roundedArea(int m, double apothem, double round) {
	return apothem * apothem * (m * std::tan(kPi / m) * (1.0 - round * round) + kPi * round * round);
}

// Area of the star that POINT draws: 2m triangles from the middle, between a point at the middle of a side and a corner.
double starArea(int m, double apothem, double point) {
	const double beta = kPi / m;
	const double corner = apothem / std::cos(beta);
	const double rho = apothem + (static_cast<double>(sf::kInnerRatio) * corner - apothem) * point;
	return m * rho * corner * std::sin(beta);
}

// Signed distance to a closed polygon (negative inside), by testing every edge.
double polygonSignedDistance(const std::vector<double>& vx, const std::vector<double>& vy, double px, double py) {
	const size_t n = vx.size();
	double best = 1e30;
	bool inside = false;
	for (size_t i = 0; i < n; ++i) {
		const size_t j = (i + 1) % n;
		const double ex = vx[j] - vx[i], ey = vy[j] - vy[i];
		const double wx = px - vx[i], wy = py - vy[i];
		const double l2 = ex * ex + ey * ey;
		const double t = l2 > 0.0 ? std::clamp((wx * ex + wy * ey) / l2, 0.0, 1.0) : 0.0;
		const double dx = wx - t * ex, dy = wy - t * ey;
		best = std::min(best, dx * dx + dy * dy);
		const bool c1 = py >= vy[i], c2 = py < vy[j], c3 = ex * wy > ey * wx;
		if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) {
			inside = !inside;
		}
	}
	return inside ? -std::sqrt(best) : std::sqrt(best);
}

// The star that ROUND grows into the shape: its corners and the middles of its sides, pulled in towards the
// middle by 1 - ROUND. The shape is every point within r of it.
struct Core {
	std::vector<double> x;
	std::vector<double> y;
	double r = 0.0;
	double corner = 0.0;
};

Core coreStar(int m, double apothem, double point, double round) {
	Core c;
	const double beta = kPi / m;
	c.corner = apothem / std::cos(beta);
	const double rho = apothem + (static_cast<double>(sf::kInnerRatio) * c.corner - apothem) * point;
	const double vox = c.corner * std::cos(beta), voy = c.corner * std::sin(beta);
	const double len = std::hypot(vox - rho, voy);
	const double keep = 1.0 - round;
	c.r = round * rho * voy / len;  // ROUND times the distance of the edge lines from the middle
	for (int k = 0; k < m; ++k) {
		const double phi = -0.5 * kPi + 2.0 * kPi * k / m;
		c.x.push_back(keep * rho * std::cos(phi));
		c.y.push_back(keep * rho * std::sin(phi));
		c.x.push_back(keep * c.corner * std::cos(phi + beta));
		c.y.push_back(keep * c.corner * std::sin(phi + beta));
	}
	return c;
}

// The outline of the shape, stretched by sx across and sy up, as a closed polyline. A shape with sharp corners
// is its own vertices; a rounded one is found by walking out along `rays` rays from the middle to where the
// distance to the core reaches r.
struct Outline {
	std::vector<double> x;
	std::vector<double> y;
};

Outline referenceOutline(int m, double apothem, double point, double round, double sx, double sy, int rays) {
	const Core c = coreStar(m, apothem, point, round);
	Outline o;
	if (round <= 0.0) {
		o.x = c.x;
		o.y = c.y;
	}
	else {
		for (int j = 0; j < rays; ++j) {
			const double th = 2.0 * kPi * j / rays;
			const double dx = std::cos(th), dy = std::sin(th);
			double lo = 0.0, hi = 1.5 * c.corner + 1.0;
			for (int it = 0; it < 44; ++it) {
				const double mid = 0.5 * (lo + hi);
				if (polygonSignedDistance(c.x, c.y, mid * dx, mid * dy) - c.r <= 0.0) {
					lo = mid;
				}
				else {
					hi = mid;
				}
			}
			o.x.push_back(lo * dx);
			o.y.push_back(lo * dy);
		}
	}
	for (size_t i = 0; i < o.x.size(); ++i) {
		o.x[i] *= sx;
		o.y[i] *= sy;
	}
	return o;
}

// The disc a circle draws: grey from the distance to its edge.
double discGrey(double wx, double wy, double cx, double cy, double radius, double leff) {
	return std::clamp(0.5 - (std::hypot(wx - cx, wy - cy) - radius) / (2.0 * leff), 0.0, 1.0);
}

struct Rng {
	std::mt19937 gen;
	explicit Rng(unsigned seed) : gen(seed) {}
	float u(float a, float b) {
		return std::uniform_real_distribution<float>(a, b)(gen);
	}
};

// ---- the standard shapes the tests draw ----

// Plain polygon or star at the middle: SIDES, SIZE and a soft ramp, everything else off.
ShapesParams shape(float sides, float size, float spread = 0.1f) {
	ShapesParams p;
	p.sides = sides;
	p.size = size;
	p.spread = spread;
	return p;
}

ShapesParams matte(ShapesParams p) {
	p.matte = 1;
	return p;
}

// ---- the bus ----

constexpr int kBusA = 1;
constexpr int kBusB = 2;
constexpr int kBusC = 3;
constexpr int kBusD = 4;

bool sameParams(const ShapesParams& a, const ShapesParams& b) {
	return a.sides == b.sides && a.round == b.round && a.point == b.point && a.size == b.size && a.x == b.x && a.y == b.y &&
	       a.aspect == b.aspect && a.turn == b.turn && a.repeat == b.repeat && a.copies == b.copies && a.ring == b.ring &&
	       a.melt == b.melt && a.spread == b.spread && a.matte == b.matte;
}

void testBus() {
	std::printf("Shapes on the video bus\n");
	ShapesParams p;
	p.sides = 5.5f;
	p.round = 0.3f;
	p.point = 0.2f;
	p.size = 0.4f;
	p.x = 0.1f;
	p.y = -0.05f;
	p.aspect = 1.5f;
	p.turn = 0.08f;
	p.repeat = 2;
	p.copies = 3;
	p.ring = 0.5f;
	p.melt = 0.4f;
	p.spread = 0.2f;
	p.matte = 0;
	videoBusPublishShapes(kBusA, p);
	VideoBusNode node;
	check(videoBusReadNode(kBusA, node), "a published Shapes node can be read back");
	check(node.type == VideoBusNodeType::Shapes, "its type is Shapes");
	check(node.sourceBusId == 0, "a source names no source bus");
	check(sameParams(node.shapes, p), "every parameter crosses the bus unchanged");

	VideoGraphRenderer renderer;
	const Frame first = renderer.renderBus(kBusA, kW, kH);
	check(first.size() == static_cast<size_t>(kW) * kH * 4u, "the bus renderer returns a full frame");
	check(maxDiff(first, render(p)) == 0, "the bus renderer draws exactly what renderShapes draws");

	ShapesParams q = p;
	q.sides = 8.f;
	q.copies = 4;
	q.round = 0.f;
	videoBusPublishShapes(kBusA, q);
	const Frame second = renderer.renderBus(kBusA, kW, kH);
	check(maxDiff(second, render(q)) == 0, "a node published later is the next picture");
	check(maxDiff(second, first) > 32, "and the two pictures are different");

	const Frame small = renderer.renderBus(kBusA, 64, 36);
	check(small.size() == 64u * 36u * 4u && maxDiff(small, render(q, 64, 36)) == 0, "the renderer follows the size it is asked for");

	// A source can feed a processor with no bus of its own to read.
	ShapesParams plain = matte(shape(4.f, 0.5f));
	videoBusPublishShapes(kBusA, plain);
	KeyerParams kp;
	videoBusPublishKeyer(kBusB, kBusA, kp);
	const Frame keyed = renderer.renderBus(kBusB, kW, kH);
	check(!keyed.empty() && keyed.size() == static_cast<size_t>(kW) * kH * 4u, "a Keyer reads Shapes as its source");

	videoBusClear(kBusB);
	videoBusClear(kBusA);
	check(!videoBusReadNode(kBusA, node), "a cleared bus is empty");
	check(renderer.renderBus(kBusA, kW, kH).empty(), "an empty bus renders nothing");
}

// ---- the control map ----

void testControlMap() {
	std::printf("Shapes control map\n");
	using namespace shapes_math;

	ShapesControls c;
	ShapesParams p = shapesParams(c);
	checkNear(p.sides, 4.0, 1e-6, "SIDES starts at 4");
	checkNear(p.round, 0.0, 1e-6, "ROUND starts at 0");
	checkNear(p.point, 0.0, 1e-6, "POINT starts at 0");
	checkNear(p.size, 2.5 * 0.45 * 0.45, 1e-6, "SIZE 0.45 is 2.5 k squared half heights");
	checkNear(p.x, 0.0, 1e-6, "X starts at 0");
	checkNear(p.y, 0.0, 1e-6, "Y starts at 0");
	checkNear(p.aspect, 1.0, 1e-6, "ASPECT starts square");
	checkNear(p.turn, 0.0, 1e-6, "TURN starts at 0");
	check(p.repeat == 1 && p.copies == 1, "REPEAT and COPIES start at 1");
	checkNear(p.ring, 1.2 * 0.4, 1e-6, "RING 0.4 is 0.48 half heights");
	checkNear(p.melt, 0.0, 1e-6, "MELT starts at 0");
	checkNear(p.spread, 0.004 * std::pow(500.0, 0.55), 1e-5, "SPREAD 0.55 is 0.004 times 500 to the 0.55");
	check(p.matte == 0, "MATTE starts off");

	// CV scales.
	c = ShapesControls{};
	c.sidesCv = 1.f;
	checkNear(shapesParams(c).sides, 4.9, 1e-5, "SIDES CV is 0.9 sides per volt");
	c.sidesCv = 10.f;
	checkNear(shapesParams(c).sides, 12.0, 1e-6, "10 V on SIDES stops at 12");
	c.sidesCv = -10.f;
	checkNear(shapesParams(c).sides, 3.0, 1e-6, "minus 10 V on SIDES stops at 3");
	c = ShapesControls{};
	c.round = 0.2f;
	c.roundCv = 5.f;
	checkNear(shapesParams(c).round, 0.7, 1e-5, "ROUND CV is 0.1 per volt");
	c.roundCv = 10.f;
	checkNear(shapesParams(c).round, 1.0, 1e-6, "ROUND stops at 1");
	c.roundCv = -10.f;
	checkNear(shapesParams(c).round, 0.0, 1e-6, "ROUND stops at 0");
	c = ShapesControls{};
	c.point = 0.1f;
	c.pointCv = 3.f;
	checkNear(shapesParams(c).point, 0.4, 1e-5, "POINT CV is 0.1 per volt");
	c = ShapesControls{};
	c.melt = 0.1f;
	c.meltCv = 4.f;
	checkNear(shapesParams(c).melt, 0.5, 1e-5, "MELT CV is 0.1 per volt");

	c = ShapesControls{};
	c.size = 0.25f;
	c.sizeCv = 2.f;
	checkNear(shapesParams(c).size, 2.5 * 0.45 * 0.45, 1e-5, "SIZE CV is 0.1 of the knob per volt");
	c.sizeCv = -10.f;
	checkNear(shapesParams(c).size, 0.0, 1e-6, "SIZE stops at 0");
	c.sizeCv = 10.f;
	checkNear(shapesParams(c).size, 2.5, 1e-6, "SIZE stops at 2.5 half heights");

	c = ShapesControls{};
	c.x = 0.1f;
	c.xCv = 2.f;
	checkNear(shapesParams(c).x, 0.3, 1e-5, "X CV is 0.1 of the frame per volt");
	c.xCv = 10.f;
	checkNear(shapesParams(c).x, 1.0, 1e-6, "X stops at a whole frame");
	c.xCv = -10.f;
	checkNear(shapesParams(c).x, -0.9, 1e-5, "X goes the other way");
	c = ShapesControls{};
	c.y = -0.1f;
	c.yCv = 3.f;
	checkNear(shapesParams(c).y, 0.2, 1e-5, "Y CV is 0.1 of the frame per volt");

	c = ShapesControls{};
	c.aspectCv = 2.5f;
	checkNear(shapesParams(c).aspect, 2.0, 1e-5, "ASPECT 2.5 V is a ratio of 2");
	c.aspectCv = 5.f;
	checkNear(shapesParams(c).aspect, 4.0, 1e-5, "ASPECT 5 V is a ratio of 4");
	c.aspectCv = -5.f;
	checkNear(shapesParams(c).aspect, 0.25, 1e-6, "ASPECT minus 5 V is a ratio of a quarter");
	c.aspectCv = 10.f;
	checkNear(shapesParams(c).aspect, 4.0, 1e-5, "ASPECT stops at 4");
	c = ShapesControls{};
	c.aspect = 1.f;
	checkNear(shapesParams(c).aspect, 4.0, 1e-5, "ASPECT knob right is 4");
	c.aspect = -1.f;
	checkNear(shapesParams(c).aspect, 0.25, 1e-6, "ASPECT knob left is a quarter");

	c = ShapesControls{};
	c.turn = 0.4f;
	c.turnCv = 2.f;
	checkNear(shapesParams(c).turn, -0.4, 1e-5, "TURN wraps: 0.4 turn and 2 V (0.2 turn) is 0.6, which is minus 0.4");
	c = ShapesControls{};
	double worstTurn = 0.0;
	for (int step = 0; step <= 20; ++step) {
		c.turnCv = 0.5f * static_cast<float>(step);
		const double got = shapesParams(c).turn;
		const double want = 0.1 * c.turnCv;
		worstTurn = std::max(worstTurn, std::fabs(std::cos(2.0 * kPi * got) - std::cos(2.0 * kPi * want)) + std::fabs(std::sin(2.0 * kPi * got) - std::sin(2.0 * kPi * want)));
		check(std::fabs(got) <= 0.5 + 1e-6, label("TURN stays within half a turn at %.1f V", c.turnCv));
	}
	checkAtMost(worstTurn, 1e-4, "a 0 to 10 V ramp on TURN spins the shape round once with no seam");

	c = ShapesControls{};
	c.spread = 0.f;
	checkNear(shapesParams(c).spread, 0.004, 1e-7, "SPREAD knob left is 0.004");
	c.spread = 1.f;
	checkNear(shapesParams(c).spread, 2.0, 1e-5, "SPREAD knob right is 2");
	c = ShapesControls{};
	c.spread = 0.f;
	c.spreadCv = 5.f;
	checkNear(shapesParams(c).spread, 0.004 * std::sqrt(500.0), 1e-5, "SPREAD CV is 0.1 of the knob per volt");

	c = ShapesControls{};
	c.repeat = 2.4f;
	c.copies = 2.6f;
	check(shapesParams(c).repeat == 2 && shapesParams(c).copies == 3, "REPEAT and COPIES round to whole numbers");
	c.repeat = 0.f;
	c.copies = -4.f;
	check(shapesParams(c).repeat == 1 && shapesParams(c).copies == 1, "REPEAT and COPIES never go under 1");
	c.repeat = 20.f;
	c.copies = 99.f;
	check(shapesParams(c).repeat == 8 && shapesParams(c).copies == 8, "REPEAT and COPIES never go over 8");
	c = ShapesControls{};
	c.ring = 1.f;
	checkNear(shapesParams(c).ring, 1.2, 1e-6, "RING knob right is 1.2 half heights");
	c.ring = 3.f;
	checkNear(shapesParams(c).ring, 1.2, 1e-6, "RING stops at 1.2");
	c.ring = -1.f;
	checkNear(shapesParams(c).ring, 0.0, 1e-6, "RING stops at 0");
	c = ShapesControls{};
	c.matte = 0.49f;
	check(shapesParams(c).matte == 0, "MATTE under half is off");
	c.matte = 0.51f;
	check(shapesParams(c).matte == 1, "MATTE over half is on");

	// Round trips between a knob and the value it makes.
	double worstRound = 0.0;
	for (float k : {0.f, 0.1f, 0.35f, 0.5f, 0.8f, 1.f}) {
		worstRound = std::max(worstRound, static_cast<double>(std::fabs(knobFromSize(sizeFromKnob(k)) - k)));
		worstRound = std::max(worstRound, static_cast<double>(std::fabs(knobFromSpread(spreadFromKnob(k)) - k)));
	}
	for (float k : {-1.f, -0.4f, 0.f, 0.6f, 1.f}) {
		worstRound = std::max(worstRound, static_cast<double>(std::fabs(knobFromAspect(aspectFromKnob(k)) - k)));
	}
	checkAtMost(worstRound, 1e-5, "the knob to value maps are undone by their inverses");

	// Whatever arrives on the cables, the result is a picture the renderer can draw.
	auto inRange = [](const ShapesParams& q) {
		return std::isfinite(q.sides) && q.sides >= 3.f && q.sides <= 12.f && q.round >= 0.f && q.round <= 1.f && q.point >= 0.f &&
		       q.point <= 1.f && std::isfinite(q.size) && q.size >= 0.f && q.size <= 2.5f && std::fabs(q.x) <= 1.f && std::fabs(q.y) <= 1.f &&
		       q.aspect >= 0.25f && q.aspect <= 4.f && std::fabs(q.turn) <= 0.5f && q.repeat >= 1 && q.repeat <= 8 && q.copies >= 1 &&
		       q.copies <= 8 && q.ring >= 0.f && q.ring <= 1.2f && q.melt >= 0.f && q.melt <= 1.f && q.spread >= 0.004f && q.spread <= 2.f;
	};
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	for (float poison : {nan, inf, -inf, 1e30f, -1e30f}) {
		ShapesControls w;
		float* fields[] = {&w.sides, &w.round, &w.point, &w.size, &w.x, &w.y, &w.aspect, &w.turn, &w.repeat, &w.copies, &w.ring, &w.melt,
		                   &w.spread, &w.matte, &w.sidesCv, &w.roundCv, &w.pointCv, &w.sizeCv, &w.xCv, &w.yCv, &w.aspectCv, &w.turnCv,
		                   &w.meltCv, &w.spreadCv};
		for (float* f : fields) {
			*f = poison;
		}
		check(inRange(shapesParams(w)), label("every control at %g gives parameters in range", static_cast<double>(poison)));
		// One cable at a time.
		for (float* f : fields) {
			ShapesControls one;
			float* own[] = {&one.sides, &one.round, &one.point, &one.size, &one.x, &one.y, &one.aspect, &one.turn, &one.repeat, &one.copies,
			                &one.ring, &one.melt, &one.spread, &one.matte, &one.sidesCv, &one.roundCv, &one.pointCv, &one.sizeCv, &one.xCv,
			                &one.yCv, &one.aspectCv, &one.turnCv, &one.meltCv, &one.spreadCv};
			*own[f - fields[0]] = poison;
			check(inRange(shapesParams(one)), label("control %d at %g gives parameters in range", static_cast<int>(f - fields[0]), static_cast<double>(poison)));
		}
	}
	// A cable that is not a number counts as 0 V, so the knobs rule.
	ShapesControls knobs;
	knobs.sides = 7.f;
	knobs.size = 0.3f;
	knobs.x = 0.2f;
	knobs.turn = 0.1f;
	ShapesControls withNan = knobs;
	withNan.sidesCv = withNan.roundCv = withNan.pointCv = withNan.sizeCv = withNan.xCv = withNan.yCv = withNan.aspectCv = withNan.turnCv =
		withNan.meltCv = withNan.spreadCv = nan;
	check(sameParams(shapesParams(withNan), shapesParams(knobs)), "a CV that is not a number counts as 0 V");
	ShapesControls nanKnobs;
	nanKnobs.sides = nanKnobs.round = nanKnobs.point = nanKnobs.size = nanKnobs.x = nanKnobs.y = nanKnobs.aspect = nanKnobs.turn =
		nanKnobs.repeat = nanKnobs.copies = nanKnobs.ring = nanKnobs.melt = nanKnobs.spread = nanKnobs.matte = nan;
	check(sameParams(shapesParams(nanKnobs), shapesParams(ShapesControls{})), "a knob that is not a number falls back to its default");

	// Wide random controls still make a parameter set that draws.
	Rng rng(5);
	int bad = 0;
	for (int i = 0; i < 400; ++i) {
		ShapesControls w;
		w.sides = rng.u(-5, 20);
		w.round = rng.u(-2, 3);
		w.point = rng.u(-2, 3);
		w.size = rng.u(-1, 2);
		w.x = rng.u(-2, 2);
		w.y = rng.u(-2, 2);
		w.aspect = rng.u(-3, 3);
		w.turn = rng.u(-3, 3);
		w.repeat = rng.u(-2, 12);
		w.copies = rng.u(-2, 12);
		w.ring = rng.u(-1, 2);
		w.melt = rng.u(-1, 2);
		w.spread = rng.u(-1, 2);
		w.matte = rng.u(0, 1);
		w.sidesCv = rng.u(-12, 12);
		w.roundCv = rng.u(-12, 12);
		w.pointCv = rng.u(-12, 12);
		w.sizeCv = rng.u(-12, 12);
		w.xCv = rng.u(-12, 12);
		w.yCv = rng.u(-12, 12);
		w.aspectCv = rng.u(-12, 12);
		w.turnCv = rng.u(-12, 12);
		w.meltCv = rng.u(-12, 12);
		w.spreadCv = rng.u(-12, 12);
		if (!inRange(shapesParams(w))) {
			++bad;
		}
	}
	check(bad == 0, "400 random controls all make parameters in range");
}

// ---- the grey picture ----

void testGreyPicture() {
	std::printf("Shapes grey picture: brightness is the distance from the edge\n");

	// A circle has a distance anyone can write down. Every pixel is checked against it.
	for (float spread : {0.05f, 0.2f, 0.6f}) {
		ShapesParams p = shape(6.f, 0.5f, spread);
		p.round = 1.f;
		p.x = 0.1f;
		p.y = -0.15f;
		const Frame f = render(p);
		const double cx = 0.1 * 2.0 * kAspectWorld, cy = -0.15 * 2.0;
		int worst = 0;
		for (int j = 0; j < kH; ++j) {
			for (int i = 0; i < kW; ++i) {
				const double want = 255.0 * discGrey(worldX(i, kW, kH), worldY(j, kH), cx, cy, 0.5, std::max<double>(spread, kPixel));
				worst = std::max(worst, std::abs(grey(f, kW, i, j) - static_cast<int>(std::lround(want))));
			}
		}
		check(worst <= 1, label("circle, SPREAD %.2f: every pixel is the distance formula to within one level (worst %d)", static_cast<double>(spread), worst));
		check(isOpaqueGrey(f), label("circle, SPREAD %.2f: the picture is opaque grey", static_cast<double>(spread)));
	}

	// Mid grey on the edge, a linear ramp SPREAD wide to each side, then white inside and black outside.
	{
		const sf::Scene sc = sf::makeScene(shape(4.f, 0.5f, 0.2f), kW, kH);
		checkNear(sf::shade(sc, 0.f, -0.5f), 0.5, 1e-4, "the flat bottom edge is mid grey");
		checkNear(sf::shade(sc, 0.5f, 0.f), 0.5, 1e-4, "the right edge is mid grey");
		checkNear(sf::shade(sc, -0.5f, 0.f), 0.5, 1e-4, "the left edge is mid grey");
		checkNear(sf::shade(sc, 0.f, 0.5f), 0.5, 1e-4, "the top edge is mid grey");
		checkNear(sf::shade(sc, 0.f, -0.6f), 0.25, 1e-4, "a tenth outside is a quarter grey (half the ramp)");
		checkNear(sf::shade(sc, 0.f, -0.4f), 0.75, 1e-4, "a tenth inside is three quarters grey");
		checkNear(sf::shade(sc, 0.f, -0.7f), 0.0, 1e-5, "SPREAD outside the edge is black");
		checkNear(sf::shade(sc, 0.f, -0.3f), 1.0, 1e-5, "SPREAD inside the edge is white");
		checkNear(sf::shade(sc, 1.5f, 0.9f), 0.0, 1e-6, "far outside is black");
		checkNear(sf::shade(sc, 0.f, 0.f), 1.0, 1e-6, "deep inside is white");
	}

	// Brighter inside, darker outside: the middle row only ever rises towards the shape and falls away from it.
	{
		const Frame f = render(shape(4.f, 0.5f, 0.2f));
		const int j = kH / 2;
		bool rising = true, falling = true;
		for (int i = 1; i <= kW / 2; ++i) {
			rising = rising && grey(f, kW, i, j) >= grey(f, kW, i - 1, j);
		}
		for (int i = kW / 2 + 1; i < kW; ++i) {
			falling = falling && grey(f, kW, i, j) <= grey(f, kW, i - 1, j);
		}
		check(rising && falling, "grey rises towards the shape and falls away on the other side");
		check(grey(f, kW, 2, j) == 0 && grey(f, kW, kW - 3, j) == 0, "far outside is black in the picture");
		check(grey(f, kW, kW / 2, j) == 255, "the middle of the shape is white");
		// The edges of the square sit 36.5 pixels either side of the middle column.
		check(grey(f, kW, kW / 2 - 30, j) > 127 && grey(f, kW, kW / 2 + 30, j) > 127, "just inside the edge is brighter than mid grey");
		check(grey(f, kW, kW / 2 - 43, j) < 128 && grey(f, kW, kW / 2 + 43, j) < 128, "just outside the edge is darker than mid grey");
	}

	// SPREAD sets how fast the grey falls away: the ramp is 2 SPREAD wide at each edge.
	for (float spread : {0.05f, 0.1f, 0.2f, 0.4f}) {
		const Frame f = render(shape(4.f, 0.5f, spread));
		int partial = 0;
		for (int i = 0; i < kW; ++i) {
			const int g = grey(f, kW, i, kH / 2);
			partial += (g > 0 && g < 255) ? 1 : 0;
		}
		checkNear(partial, 4.0 * spread / kPixel, 3.0, label("SPREAD %.2f: the ramp crosses about %.0f pixels over the two edges (got %d)", static_cast<double>(spread), 4.0 * spread / kPixel, partial));
	}
	{
		// Under a pixel the ramp is held to a pixel, so an edge never aliases.
		const Frame f = render(shape(4.f, 0.5f, 0.0001f));
		int partial = 0;
		for (int i = 0; i < kW; ++i) {
			const int g = grey(f, kW, i, kH / 2);
			partial += (g > 0 && g < 255) ? 1 : 0;
		}
		check(partial <= 6, "a SPREAD under a pixel is held to a pixel");
		check(maxDiff(f, render(shape(4.f, 0.5f, 0.001f))) == 0, "every SPREAD under a pixel draws the same");
	}

	// Slanted edges follow the same rule: the distance across an edge, not along an axis.
	{
		const double spread = 0.15;
		const sf::Scene sc = sf::makeScene(shape(3.f, 0.4f, static_cast<float>(spread)), kW, kH);
		// The right-hand edge of the triangle has its normal 30 degrees above the x axis.
		const double nx = std::cos(kPi / 6.0), ny = std::sin(kPi / 6.0);
		for (double t : {-0.1, -0.05, 0.0, 0.05, 0.1}) {
			const double px = (0.4 + t) * nx, py = (0.4 + t) * ny;
			checkNear(sf::shade(sc, static_cast<float>(px), static_cast<float>(py)), std::clamp(0.5 - t / (2.0 * spread), 0.0, 1.0), 1e-3,
			          label("triangle, %.2f outside the slanted edge, grey follows the perpendicular distance", t));
		}
	}
}

// ---- MATTE ----

void testMatte() {
	std::printf("Shapes MATTE: plain white on black\n");
	// Against the exact distance to a hexagon and to a star: white inside, black outside, one pixel of ramp on the edge.
	for (int kind = 0; kind < 2; ++kind) {
		ShapesParams p = matte(kind == 0 ? shape(6.f, 0.5f, 0.3f) : shape(5.f, 0.5f, 0.3f));
		if (kind == 1) {
			p.point = 1.f;
		}
		const Frame f = render(p);
		const Core core = coreStar(static_cast<int>(p.sides), p.size, p.point, 0.0);
		int worst = 0, inside = 0, outside = 0, ramp = 0;
		for (int j = 0; j < kH; ++j) {
			for (int i = 0; i < kW; ++i) {
				const double d = polygonSignedDistance(core.x, core.y, worldX(i, kW, kH), worldY(j, kH));
				const double want = 255.0 * std::clamp(0.5 - d / kPixel, 0.0, 1.0);
				worst = std::max(worst, std::abs(grey(f, kW, i, j) - static_cast<int>(std::lround(want))));
				inside += (d < -kPixel && grey(f, kW, i, j) == 255) ? 1 : 0;
				outside += (d > kPixel && grey(f, kW, i, j) == 0) ? 1 : 0;
				ramp += (grey(f, kW, i, j) > 0 && grey(f, kW, i, j) < 255) ? 1 : 0;
			}
		}
		check(worst <= 1, label("%s: every pixel is the one-pixel matte ramp on the exact distance (worst %d)", kind == 0 ? "hexagon" : "star", worst));
		check(inside > 1000 && outside > 10000, label("%s: white inside, black outside", kind == 0 ? "hexagon" : "star"));
		checkAtMost(ramp, 2.5 * (kind == 0 ? 6 : 10) * 0.6 / kPixel, label("%s: only the edge pixels are grey (%d)", kind == 0 ? "hexagon" : "star", ramp));
		check(isOpaqueGrey(f), label("%s: the matte is opaque grey", kind == 0 ? "hexagon" : "star"));
	}
	// SPREAD does nothing to a matte.
	check(maxDiff(render(matte(shape(5.f, 0.4f, 0.004f))), render(matte(shape(5.f, 0.4f, 2.f)))) == 0, "MATTE ignores SPREAD");
	// The matte is the grey picture's mid-grey contour.
	{
		const Frame g = render(shape(7.f, 0.45f, 0.25f));
		const Frame m = render(matte(shape(7.f, 0.45f, 0.25f)));
		int differ = 0;
		for (size_t i = 0; i < g.size(); i += 4) {
			differ += ((g[i] >= 128) != (m[i] >= 128)) ? 1 : 0;
		}
		check(differ <= 3, label("MATTE is the grey picture cut at mid grey (%d pixels differ)", differ));
	}
	// Plain white on black needs nothing else: with SIZE 0 there is no shape at all.
	{
		const Frame f = render(matte(shape(5.f, 0.f)));
		bool black = true;
		for (size_t i = 0; i < f.size(); i += 4) {
			black = black && f[i] == 0;
		}
		check(black, "SIZE 0 with MATTE is a black picture");
	}
}

// ---- the zoo: a handful of scenes that between them use every control ----

std::vector<ShapesParams> sceneZoo() {
	std::vector<ShapesParams> zoo;
	zoo.push_back(ShapesParams{});
	{
		ShapesParams p = shape(6.f, 0.45f, 0.3f);
		p.round = 0.4f;
		zoo.push_back(p);
	}
	{
		ShapesParams p = shape(5.f, 0.5f, 0.2f);
		p.point = 1.f;
		p.round = 0.2f;
		zoo.push_back(p);
	}
	zoo.push_back(shape(7.5f, 0.4f, 0.15f));
	{
		ShapesParams p = shape(5.f, 0.35f, 0.1f);
		p.aspect = 2.5f;
		p.round = 0.3f;
		zoo.push_back(p);
	}
	{
		ShapesParams p = shape(6.f, 0.6f, 0.12f);
		p.repeat = 3;
		zoo.push_back(p);
	}
	{
		ShapesParams p = shape(4.f, 0.2f, 0.08f);
		p.copies = 5;
		p.ring = 0.5f;
		zoo.push_back(p);
	}
	{
		ShapesParams p = shape(12.f, 0.25f, 0.12f);
		p.copies = 6;
		p.ring = 0.55f;
		p.melt = 0.7f;
		p.round = 1.f;
		zoo.push_back(p);
	}
	{
		ShapesParams p = shape(5.f, 0.5f, 0.1f);
		p.repeat = 4;
		p.copies = 3;
		p.ring = 0.4f;
		p.melt = 0.5f;
		zoo.push_back(p);
	}
	zoo.push_back(matte(shape(8.f, 0.4f)));
	{
		ShapesParams p = shape(9.3f, 0.3f, 0.2f);
		p.turn = 0.13f;
		p.x = 0.2f;
		p.y = -0.1f;
		p.aspect = 0.5f;
		zoo.push_back(p);
	}
	{
		ShapesParams p = shape(11.5f, 0.3f, 0.9f);
		p.round = 0.5f;
		p.point = 0.5f;
		p.aspect = 3.f;
		p.turn = 0.3f;
		p.repeat = 5;
		p.copies = 7;
		p.ring = 0.9f;
		p.melt = 0.9f;
		zoo.push_back(p);
	}
	return zoo;
}

// ---- the same picture however it is drawn ----

void testDeterminism() {
	std::printf("Shapes: the same picture however many threads draw it\n");
	const std::vector<ShapesParams> zoo = sceneZoo();
	const int sizes[][2] = {{33, 17}, {64, 36}, {191, 125}, {256, 144}, {333, 187}};
	int worst = 0;
	int repeatsDiffer = 0;
	for (const ShapesParams& p : zoo) {
		for (const auto& s : sizes) {
			const Frame a = render(p, s[0], s[1]);
			const Frame b = render(p, s[0], s[1]);
			repeatsDiffer += maxDiff(a, b) != 0 ? 1 : 0;
			worst = std::max(worst, maxDiff(a, referenceRender(p, s[0], s[1])));
		}
	}
	check(repeatsDiffer == 0, "drawing the same scene twice gives the same bytes");
	check(worst == 0, label("the threaded renderer matches a single pass of the kernel at every size (worst %d)", worst));

	Frame f = render(ShapesParams{});
	renderShapes(f, 0, 100, ShapesParams{});
	check(f.empty(), "a width of 0 draws nothing");
	renderShapes(f, 100, -3, ShapesParams{});
	check(f.empty(), "a negative height draws nothing");
	renderShapes(f, 1, 1, ShapesParams{});
	check(f.size() == 4u && f[3] == 255, "a one pixel picture is one opaque pixel");
	renderShapes(f, 2, 2, ShapesParams{});
	check(f.size() == 16u, "a two by two picture is four pixels");
	// A portrait picture: the height is the unit, so the frame is narrower than it is tall.
	{
		ShapesParams p = matte(shape(4.f, 0.4f));
		const Frame tall = render(p, 90, 160);
		const Measure m = measure(tall, 90, 160);
		checkNear(m.area, 4.0 * 0.4 * 0.4, 0.01, "a portrait picture keeps the height as the unit");
		const Frame wide = render(p, 320, 120);
		checkNear(measure(wide, 320, 120).area, 4.0 * 0.4 * 0.4, 0.01, "a wide picture keeps the height as the unit");
	}
}

// ---- shapes: SIDES, fractional SIDES, ROUND and POINT ----

constexpr int kBigW = 512;  // large pictures for measuring areas
constexpr int kBigH = 288;
constexpr double kBigPixel = 2.0 / kBigH;
constexpr int kSheetW = 192;  // the size of one panel on a proof sheet
constexpr int kSheetH = 108;

Measure matteMeasure(ShapesParams p) {
	p.matte = 1;
	return measure(render(p, kBigW, kBigH), kBigW, kBigH);
}

Frame sheetPanel(const ShapesParams& p) {
	return render(p, kSheetW, kSheetH);
}

float asF(int v) {
	return static_cast<float>(v);
}

void testPolygons() {
	std::printf("Shapes SIDES: whole numbers 3 to 12 draw regular polygons with a flat bottom\n");
	const double a = 0.45;
	double worst = 0.0;
	std::vector<Frame> sheet;
	for (int m = 3; m <= 12; ++m) {
		const ShapesParams p = shape(asF(m), static_cast<float>(a), 0.12f);
		const Measure me = matteMeasure(p);
		const double want = polygonArea(m, a);
		worst = std::max(worst, std::fabs(me.area - want) / want);
		checkNear(me.area, want, 0.004 * want, label("%d sides: area %.4f is that of a regular polygon", m, want));
		checkNear(me.cx, 0.0, 0.004, label("%d sides: centred across", m));
		checkNear(me.cy, 0.0, 0.004, label("%d sides: centred up", m));
		const double top = (m % 2 == 0) ? a : a / std::cos(kPi / m);
		checkNear(me.minY, -a, 1.5 * kBigPixel, label("%d sides: the bottom is a flat edge at minus the apothem", m));
		checkNear(me.maxY, top, 1.5 * kBigPixel, label("%d sides: the top is %s", m, (m % 2 == 0) ? "a flat edge" : "a corner"));
		const sf::Scene sc = sf::makeScene(p, kW, kH);
		const double half = a * std::tan(kPi / m);
		for (double t : {-0.8, 0.0, 0.8}) {
			checkNear(sf::shade(sc, static_cast<float>(t * half), static_cast<float>(-a)), 0.5, 2e-3,
			          label("%d sides: the bottom edge is mid grey along its length (%.1f of the half side)", m, t));
		}
		sheet.push_back(sheetPanel(p));
	}
	std::printf("  worst polygon area error %.3f%%\n", 100.0 * worst);
	saveSheet("shapes_sides.png", sheet, 5, kSheetW, kSheetH);
}

void testFractionalSides() {
	std::printf("Shapes SIDES between whole numbers: one polygon blends into the next\n");
	const float a = 0.4f;
	check(maxDiff(render(shape(5.f, a)), render(shape(5.0004f, a))) == 0, "SIDES a hair over 5 draws the pentagon");
	check(maxDiff(render(shape(6.f, a)), render(shape(5.9996f, a))) == 0, "SIDES a hair under 6 draws the hexagon");
	check(maxDiff(render(shape(12.f, a)), render(shape(13.f, a))) == 0, "SIDES over 12 stays a 12-gon");
	check(maxDiff(render(shape(3.f, a)), render(shape(2.f, a))) == 0, "SIDES under 3 stays a triangle");

	// Every pixel of a blend lies between the two polygons it blends (to a level of rounding).
	for (int m : {3, 4, 5, 7, 11}) {
		const Frame lo = render(shape(asF(m), a, 0.2f)), hi = render(shape(asF(m + 1), a, 0.2f));
		for (float f : {0.25f, 0.5f, 0.75f}) {
			const Frame mid = render(shape(asF(m) + f, a, 0.2f));
			bool between = true;
			for (size_t i = 0; i < mid.size(); i += 4) {
				between = between && mid[i] >= std::min(lo[i], hi[i]) - 1 && mid[i] <= std::max(lo[i], hi[i]) + 1;
			}
			check(between, label("%d.%02d sides lies between the %d-gon and the %d-gon at every pixel", m, static_cast<int>(f * 100.f), m, m + 1));
		}
	}
	// A slow turn of SIDES changes the picture slowly: no jumps anywhere along the way. The blend is
	// (1 - t) x one polygon's distance + t x the next one's, and the two polygons share an apothem, so a corner of
	// the m-gon is the farthest any point of the picture can be from the (m+1)-gon: R - a. A step of 0.05 in t
	// can therefore move the distance by at most 0.05 (R - a), which the ramp turns into grey levels.
	double worst = 0.0, worstOverBound = 0.0;
	const double leff = 0.15;
	for (int m : {3, 6, 9, 11}) {
		const double bound = 255.0 * 0.05 * (a / std::cos(kPi / m) - a) / (2.0 * leff);
		double worstHere = 0.0;
		Frame previous = render(shape(asF(m), a, static_cast<float>(leff)));
		for (int step = 1; step <= 20; ++step) {
			const Frame now = render(shape(asF(m) + 0.05f * asF(step), a, static_cast<float>(leff)));
			worstHere = std::max(worstHere, static_cast<double>(maxDiff(previous, now)));
			previous = now;
		}
		worst = std::max(worst, worstHere);
		worstOverBound = std::max(worstOverBound, worstHere - bound);
		checkAtMost(worstHere, bound + 2.0, label("%d to %d sides, moved 0.05 at a time: no pixel changes by more than the corners moving 0.05 of the way (%.1f levels)", m, m + 1, bound));
	}
	std::printf("  largest change in a pixel for 0.05 of a side: %.0f levels (%.1f over the geometric bound at worst)\n", worst, worstOverBound);

	// The blend of two sizes of polygon lies between their areas.
	double areaLo = matteMeasure(shape(5.f, a)).area, areaHi = matteMeasure(shape(6.f, a)).area;
	const double areaMid = matteMeasure(shape(5.5f, a)).area;
	check(areaMid > std::min(areaLo, areaHi) && areaMid < std::max(areaLo, areaHi), "5.5 sides has an area between the pentagon and the hexagon");

	std::vector<Frame> sheet;
	for (float s : {4.f, 4.25f, 4.5f, 4.75f, 5.f, 5.5f}) {
		sheet.push_back(sheetPanel(shape(s, 0.4f, 0.12f)));
	}
	saveSheet("shapes_fractional_sides.png", sheet, 3, kSheetW, kSheetH);
}

void testRoundAndPoint() {
	std::printf("Shapes ROUND and POINT: corners soften to a circle, sides pull in to a star\n");
	const double a = 0.4;

	// ROUND: the polygon shrunk by 1 - ROUND and grown by a disc, whose area is known exactly.
	double worstRound = 0.0;
	for (int m : {3, 4, 5, 6, 8, 12}) {
		double previous = 1e9;
		for (double r : {0.0, 0.25, 0.5, 0.75, 1.0}) {
			ShapesParams p = shape(asF(m), static_cast<float>(a), 0.12f);
			p.round = static_cast<float>(r);
			const double got = matteMeasure(p).area;
			const double want = roundedArea(m, a, r);
			worstRound = std::max(worstRound, std::fabs(got - want) / want);
			checkNear(got, want, 0.004 * want, label("%d sides, ROUND %.2f: area %.4f matches the shrink-and-grow formula", m, r, want));
			check(got < previous, label("%d sides, ROUND %.2f: area is less than with sharper corners", m, r));
			previous = got;
		}
	}
	std::printf("  worst rounded area error %.3f%%\n", 100.0 * worstRound);

	// ROUND 1 is a circle of the apothem's radius whatever the number of sides.
	{
		ShapesParams p = shape(3.f, static_cast<float>(a), 0.1f);
		p.round = 1.f;
		const Frame reference = render(p);
		int worst = 0;
		for (int m = 3; m <= 12; ++m) {
			p.sides = asF(m);
			worst = std::max(worst, maxDiff(render(p), reference));
		}
		check(worst <= 1, label("ROUND 1 draws the same circle for every number of sides (worst %d)", worst));
	}

	// A rounded shape stays inside the sharp one and is never smaller than the circle in it.
	{
		bool nested = true;
		for (int m : {4, 5, 8}) {
			ShapesParams p = shape(asF(m), static_cast<float>(a), 0.1f);
			const Frame sharp = render(p);
			p.round = 0.5f;
			const Frame soft = render(p);
			p.round = 1.f;
			const Frame circle = render(p);
			for (size_t i = 0; i < sharp.size(); i += 4) {
				nested = nested && soft[i] <= sharp[i] + 1 && soft[i] + 1 >= circle[i];
			}
		}
		check(nested, "ROUND 0.5 lies between the polygon and its inscribed circle at every pixel");
	}

	// POINT: the middle of each side moves in, the corners stay where they are.
	double worstPoint = 0.0;
	for (int m = 3; m <= 12; ++m) {
		double previous = 1e9;
		for (double pt : {0.0, 0.25, 0.5, 1.0}) {
			ShapesParams p = shape(asF(m), static_cast<float>(a), 0.12f);
			p.point = static_cast<float>(pt);
			const Measure me = matteMeasure(p);
			const double want = pt == 0.0 ? polygonArea(m, a) : starArea(m, a, pt);
			worstPoint = std::max(worstPoint, std::fabs(me.area - want) / want);
			checkNear(me.area, want, 0.006 * want, label("%d sides, POINT %.2f: area %.4f matches the star", m, pt, want));
			check(me.area < previous, label("%d sides, POINT %.2f: area is less than with a shallower star", m, pt));
			previous = me.area;
			// The picture only shows a corner once a pixel centre fits inside it, so the sharper the corner, the
			// further down the picture's edge sits: a row, plus the tangent of the corner's half angle over a pixel.
			const Core star = coreStar(m, a, pt, 0.0);
			const double ax = star.x[0] - star.x[1], ay = star.y[0] - star.y[1];
			const double bx = star.x[2] - star.x[1], by = star.y[2] - star.y[1];
			const double halfAngle = 0.5 * std::acos((ax * bx + ay * by) / (std::hypot(ax, ay) * std::hypot(bx, by)));
			const double reach = kBigPixel * (2.0 + 1.0 / std::tan(halfAngle));
			const double top = (m % 2 == 0) ? a : a / std::cos(kPi / m);
			checkNear(me.maxY, top, reach, label("%d sides, POINT %.2f: the corners stay where they were (top)", m, pt));
			checkNear(me.minY, -a, reach, label("%d sides, POINT %.2f: the corners stay where they were (bottom)", m, pt));
			// And exactly: from the bottom right corner the distance runs out along the corner's axis one for one,
			// and a point just inside the corner is inside.
			const sf::Scene sc = sf::makeScene(p, kBigW, kBigH);
			const double cornerX = a * std::tan(kPi / m), cornerY = -a;
			const double ux = cornerX / std::hypot(cornerX, cornerY), uy = cornerY / std::hypot(cornerX, cornerY);
			const float outside = sf::sdShape(sc.shape, static_cast<float>(cornerX + 0.02 * ux), static_cast<float>(cornerY + 0.02 * uy));
			const float within = sf::sdShape(sc.shape, static_cast<float>(cornerX - 0.02 * ux), static_cast<float>(cornerY - 0.02 * uy));
			checkNear(outside, 0.02, 2e-4, label("%d sides, POINT %.2f: 0.02 out from the corner along its axis is 0.02 from the shape", m, pt));
			check(within < 0.f, label("%d sides, POINT %.2f: 0.02 in from the corner along its axis is inside", m, pt));
		}
	}
	std::printf("  worst star area error %.3f%%\n", 100.0 * worstPoint);
	{
		// At the middle of the bottom side the picture goes from inside the polygon to outside the star.
		const sf::Scene flat = sf::makeScene(matte(shape(6.f, static_cast<float>(a))), kW, kH);
		ShapesParams starP = matte(shape(6.f, static_cast<float>(a)));
		starP.point = 1.f;
		const sf::Scene star = sf::makeScene(starP, kW, kH);
		const float y = static_cast<float>(-0.5 * (a + 0.38 * a / std::cos(kPi / 6.0)));
		check(sf::shade(flat, 0.f, y) > 0.99f, "hexagon, POINT 0: the middle of the bottom side is inside");
		check(sf::shade(star, 0.f, y) < 0.01f, "hexagon, POINT 1: the middle of the bottom side has been pulled in");
	}

	// A rounded star: ROUND grows the tips into discs and leaves the notches sharp, as a disc rolled round the
	// outside of the star does. Inside the ramp the grey is checked against the distance to the true outline, walked
	// out ray by ray, so the notches are measured as well as the tips.
	{
		double worst = 0.0;
		for (int m : {3, 5, 8}) {
			for (double r : {0.3, 0.6, 1.0}) {
				ShapesParams p = shape(asF(m), static_cast<float>(a), 0.1f);
				p.point = 0.7f;
				p.round = static_cast<float>(r);
				const Outline outline = referenceOutline(m, a, 0.7, r, 1.0, 1.0, 2880);
				const sf::Scene sc = sf::makeScene(p, kSheetW, kSheetH);
				const double leff = 0.1;
				for (int j = 0; j < kSheetH; j += 2) {
					for (int i = 0; i < kSheetW; i += 2) {
						const double wx = worldX(i, kSheetW, kSheetH), wy = worldY(j, kSheetH);
						const double ref = polygonSignedDistance(outline.x, outline.y, wx, wy);
						if (std::fabs(ref) > 0.9 * leff) {
							continue;
						}
						const double got = (0.5 - sf::shade(sc, static_cast<float>(wx), static_cast<float>(wy))) * 2.0 * leff;
						worst = std::max(worst, std::fabs(got - ref));
					}
				}
			}
		}
		checkAtMost(worst / kBigPixel, 0.15, "rounded stars: the grey matches the distance to the true outline to a fraction of a pixel");
		std::printf("  rounded star distance error %.4f of a pixel at 512x288\n", worst / kBigPixel);
	}

	// Proof sheets.
	std::vector<Frame> roundSheet, pointSheet;
	for (float r : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
		ShapesParams p = shape(5.f, 0.42f, 0.1f);
		p.round = r;
		roundSheet.push_back(sheetPanel(p));
	}
	for (float r : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
		ShapesParams p = shape(5.f, 0.42f, 0.1f);
		p.point = 0.8f;
		p.round = r;
		roundSheet.push_back(sheetPanel(p));
	}
	saveSheet("shapes_round.png", roundSheet, 5, kSheetW, kSheetH);
	for (float pt : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
		ShapesParams p = shape(5.f, 0.42f, 0.1f);
		p.point = pt;
		pointSheet.push_back(sheetPanel(p));
	}
	for (float pt : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
		ShapesParams p = shape(8.f, 0.42f, 0.1f);
		p.point = pt;
		pointSheet.push_back(sheetPanel(p));
	}
	saveSheet("shapes_point.png", pointSheet, 5, kSheetW, kSheetH);
}

// ---- placement: SIZE, X, Y, ASPECT and TURN ----

void testPlacement() {
	std::printf("Shapes SIZE, X, Y, ASPECT, TURN: where the shape sits\n");
	const double A = kBigW / static_cast<double>(kBigH);  // world half-width of the big picture

	// SIZE is the apothem in half picture heights: areas go with its square.
	{
		double previous = 0.0;
		for (double size : {0.1, 0.2, 0.4, 0.8}) {
			const Measure me = matteMeasure(shape(4.f, static_cast<float>(size)));
			checkNear(me.area, 4.0 * size * size, 0.004 * 4.0 * size * size + 2e-5, label("SIZE %.1f: a square of half-side %.1f has area %.4f", size, size, 4.0 * size * size));
			check(me.area > previous, label("SIZE %.1f is bigger than the one before", size));
			previous = me.area;
		}
	}
	// A shape of size 0 is gone (a matte of nothing), and a shape the size of the picture fills it.
	{
		const Frame none = render(matte(shape(5.f, 0.f)));
		bool black = true;
		for (size_t i = 0; i < none.size(); i += 4) {
			black = black && none[i] == 0;
		}
		check(black, "SIZE 0 with MATTE draws nothing");
		const Frame all = render(shape(4.f, 2.5f, 0.1f));
		bool white = true;
		for (size_t i = 0; i < all.size(); i += 4) {
			white = white && all[i] == 255;
		}
		check(white, "SIZE 2.5 fills the whole picture");
		// With a soft ramp, SIZE 0 is a point: the cone of distance to it, 0.5 at the middle.
		const sf::Scene cone = sf::makeScene(shape(5.f, 0.f, 0.3f), kW, kH);
		checkNear(sf::shade(cone, 0.f, 0.f), 0.5, 1e-3, "SIZE 0 with a soft ramp is mid grey at its one point");
		checkNear(sf::shade(cone, 0.3f, 0.f), 0.0, 1e-3, "and black a SPREAD away");
	}

	// X and Y are fractions of the picture: 0.5 is the edge.
	for (double fx : {-0.4, -0.15, 0.0, 0.25}) {
		for (double fy : {-0.3, 0.0, 0.2}) {
			ShapesParams p = shape(6.f, 0.2f);
			p.x = static_cast<float>(fx);
			p.y = static_cast<float>(fy);
			const Measure me = matteMeasure(p);
			checkNear(me.cx, fx * 2.0 * A, 0.004, label("X %.2f: the shape's centre is at %.2f of the picture width", fx, fx));
			checkNear(me.cy, fy * 2.0, 0.004, label("Y %.2f: the shape's centre is at %.2f of the picture height", fy, fy));
			checkNear(me.area, polygonArea(6, 0.2), 0.004 * polygonArea(6, 0.2), label("X %.2f Y %.2f: moving the shape does not change it", fx, fy));
		}
	}
	{
		// Half off the picture still draws the half that shows: X 0.5 puts the centre on the right-hand edge.
		ShapesParams p = shape(4.f, 0.3f);
		p.x = 0.5f;
		const Measure me = matteMeasure(p);
		checkNear(me.area, 0.5 * 4.0 * 0.09, 0.004, "a square centred on the right-hand edge shows half of itself");
		checkNear(me.maxX, A, 2.0 * kBigPixel, "and reaches the edge");
	}

	// TURN: a quarter turn of a square is a square, an eighth is a diamond, and positive is counter-clockwise.
	{
		const double a = 0.3;
		const Measure r0 = matteMeasure(shape(4.f, static_cast<float>(a)));
		checkNear(r0.maxX, a, 1.5 * kBigPixel, "TURN 0: the square's right edge is at the apothem");
		ShapesParams p = shape(4.f, static_cast<float>(a));
		p.turn = 0.125f;
		const Measure d = matteMeasure(p);
		checkNear(d.maxX, a * std::sqrt(2.0), 2.0 * kBigPixel, "TURN 0.125: the square is a diamond, corners at the apothem times root 2 (right)");
		checkNear(d.maxY, a * std::sqrt(2.0), 2.0 * kBigPixel, "TURN 0.125: the diamond's top corner");
		checkNear(d.area, 4.0 * a * a, 0.004 * 4.0 * a * a, "TURN 0.125: a turned shape has the same area");
		p.turn = 0.25f;
		const Measure q = matteMeasure(p);
		checkNear(q.maxX, a, 1.5 * kBigPixel, "TURN 0.25: a square again");
		checkNear(q.maxY, a, 1.5 * kBigPixel, "TURN 0.25: a square again (top)");
		// The triangle's corner points up at TURN 0 and a positive TURN carries it to the left.
		ShapesParams t = shape(3.f, static_cast<float>(a));
		const Measure t0 = matteMeasure(t);
		checkNear(t0.maxY, 2.0 * a, 1.5 * kBigPixel, "triangle, TURN 0: corner up");
		t.turn = 0.25f;
		const Measure tl = matteMeasure(t);
		checkNear(tl.minX, -2.0 * a, 1.5 * kBigPixel, "triangle, TURN +0.25: corner to the left (counter-clockwise)");
		checkNear(tl.maxX, a, 1.5 * kBigPixel, "triangle, TURN +0.25: the flat edge on the right");
		t.turn = -0.25f;
		const Measure tr = matteMeasure(t);
		checkNear(tr.maxX, 2.0 * a, 1.5 * kBigPixel, "triangle, TURN -0.25: corner to the right");
		t.turn = 0.5f;
		const Measure td = matteMeasure(t);
		checkNear(td.minY, -2.0 * a, 1.5 * kBigPixel, "triangle, TURN 0.5: corner down");
		// Three-fold symmetry: a third of a turn is the same picture.
		ShapesParams t3 = shape(3.f, static_cast<float>(a), 0.15f);
		const Frame base = render(t3);
		t3.turn = 1.f / 3.f;
		check(maxDiff(base, render(t3)) <= 2, "triangle, TURN 1/3: the same picture");
		t3.turn = -1.f / 3.f;
		check(maxDiff(base, render(t3)) <= 2, "triangle, TURN -1/3: the same picture");
		// A whole turn is no turn at all (TURN wraps).
		ShapesParams w = shape(5.f, 0.3f, 0.15f);
		const Frame w0 = render(w);
		w.turn = 1.f;
		check(maxDiff(w0, render(w)) <= 1, "TURN 1 is TURN 0");
		w.turn = 0.3f;
		const Frame w3 = render(w);
		w.turn = 1.3f;
		check(maxDiff(w3, render(w)) <= 1, "TURN 1.3 is TURN 0.3");
		w.turn = -0.7f;
		check(maxDiff(w3, render(w)) <= 1, "TURN -0.7 is TURN 0.3");
	}

	// ASPECT is width over height, area kept, and TURN carries the stretch round with the shape.
	{
		const double a = 0.3;
		for (double ratio : {0.25, 0.5, 2.0, 4.0}) {
			ShapesParams p = shape(4.f, static_cast<float>(a));
			p.aspect = static_cast<float>(ratio);
			const Measure me = matteMeasure(p);
			const double sx = std::sqrt(ratio), sy = 1.0 / sx;
			checkNear(me.area, 4.0 * a * a, 0.006 * 4.0 * a * a, label("ASPECT %.2f: the area is kept", ratio));
			checkNear(me.maxX, a * sx, 2.0 * kBigPixel, label("ASPECT %.2f: half width is the apothem times root of the ratio", ratio));
			checkNear(me.maxY, a * sy, 2.0 * kBigPixel, label("ASPECT %.2f: half height is the apothem over root of the ratio", ratio));
			p.turn = 0.25f;
			const Measure turned = matteMeasure(p);
			checkNear(turned.maxX, a * sy, 2.0 * kBigPixel, label("ASPECT %.2f, TURN 0.25: the stretch turns with the shape (width)", ratio));
			checkNear(turned.maxY, a * sx, 2.0 * kBigPixel, label("ASPECT %.2f, TURN 0.25: the stretch turns with the shape (height)", ratio));
		}
		// Stretching a round shape gives an ellipse: the second moments of an ellipse with these half-axes.
		ShapesParams e = matte(shape(8.f, 0.3f));
		e.round = 1.f;
		e.aspect = 2.25f;
		const Measure me = measure(render(e, kBigW, kBigH), kBigW, kBigH);
		const double rx = 0.3 * 1.5, ry = 0.3 / 1.5;
		checkNear(me.area, kPi * rx * ry, 0.004 * kPi * rx * ry, "a stretched circle is an ellipse of the same area");
		checkNear(me.ixx, rx * rx / 4.0, 0.01 * rx * rx / 4.0, "ellipse: second moment across");
		checkNear(me.iyy, ry * ry / 4.0, 0.01 * ry * ry / 4.0, "ellipse: second moment up");
	}
}

// Controls the way the module sets them: a knob and a voltage, through the same map the panel uses.
shapes_math::ShapesControls controls() {
	return shapes_math::ShapesControls{};
}

void testCvDriven() {
	std::printf("Shapes under CV: size, position and softness at three settings each\n");
	using namespace shapes_math;
	std::vector<Frame> sheet;
	const double A = kBigW / static_cast<double>(kBigH);

	// SIZE: the SIZE jack moves the knob by 0.1 per volt.
	{
		double previous = 0.0;
		for (float volts : {-2.5f, 0.f, 1.5f}) {
			ShapesControls c = controls();
			c.sides = 6.f;
			c.sizeCv = volts;
			ShapesParams p = shapesParams(c);
			p.spread = 0.08f;
			const double want = polygonArea(6, p.size);
			const double got = matteMeasure(p).area;
			checkNear(got, want, 0.005 * want, label("SIZE jack at %+.1f V: the hexagon's area follows its size %.3f", static_cast<double>(volts), static_cast<double>(p.size)));
			check(got > previous, label("SIZE jack at %+.1f V is bigger than at the lower voltage", static_cast<double>(volts)));
			previous = got;
			sheet.push_back(sheetPanel(p));
		}
	}
	// X and Y: 1 V moves the shape a tenth of the picture.
	for (float volts : {-3.f, 0.f, 3.f}) {
		ShapesControls c = controls();
		c.size = 0.3f;
		c.xCv = volts;
		c.yCv = -volts * 0.5f;
		ShapesParams p = shapesParams(c);
		p.spread = 0.08f;
		const Measure me = matteMeasure(p);
		checkNear(me.cx, 0.1 * static_cast<double>(volts) * 2.0 * A, 0.006, label("X jack at %+.0f V: the shape is %+.0f tenths of the picture across", static_cast<double>(volts), static_cast<double>(volts)));
		checkNear(me.cy, -0.05 * static_cast<double>(volts) * 2.0, 0.006, label("Y jack at %+.1f V: the shape is that far up", static_cast<double>(-volts * 0.5f)));
		sheet.push_back(sheetPanel(p));
	}
	// ASPECT and TURN.
	for (float volts : {-4.f, 0.f, 4.f}) {
		ShapesControls c = controls();
		c.sides = 5.f;
		c.size = 0.3f;
		c.aspectCv = volts;
		c.turnCv = volts * 0.5f;
		ShapesParams p = shapesParams(c);
		p.spread = 0.08f;
		checkNear(p.aspect, std::pow(4.0, 0.2 * static_cast<double>(volts)), 1e-4, label("ASPECT jack at %+.0f V gives the ratio 4 to the %+.1f", static_cast<double>(volts), 0.2 * static_cast<double>(volts)));
		sheet.push_back(sheetPanel(p));
	}
	// SPREAD: the edge ramp, measured across the middle of a square.
	{
		double previousWidth = 0.0;
		for (float volts : {-3.f, 0.f, 3.f}) {
			ShapesControls c = controls();
			c.size = 0.3f;
			c.spreadCv = volts;
			const ShapesParams p = shapesParams(c);
			const Frame f = render(p);
			int partial = 0;
			for (int i = 0; i < kW; ++i) {
				const int g = grey(f, kW, i, kH / 2);
				partial += (g > 0 && g < 255) ? 1 : 0;
			}
			const double want = std::min(4.0 * static_cast<double>(p.spread) / kPixel, static_cast<double>(kW));
			if (volts < 1.f) {
				checkNear(partial, want, 3.0, label("SPREAD jack at %+.0f V: the ramp is %.1f pixels over the two edges (SPREAD %.3f)", static_cast<double>(volts), want, static_cast<double>(p.spread)));
			}
			check(partial > previousWidth, label("SPREAD jack at %+.0f V: a wider ramp than at the lower voltage", static_cast<double>(volts)));
			previousWidth = partial;
			sheet.push_back(sheetPanel(p));
		}
	}
	saveSheet("shapes_cv.png", sheet, 3, kSheetW, kSheetH);
}

// ---- REPEAT, COPIES and MELT ----

void testRepeat() {
	std::printf("Shapes REPEAT: the shape tiles across the screen in a grid\n");
	const float size = 0.45f;
	// Counted from where the lattice puts the cells: pitch 2/n, with an offset of 1/n when n is even.
	struct Row {
		int n;
		int blobs;
	};
	const Row rows[] = {{1, 1}, {2, 8}, {3, 15}, {5, 45}};
	for (const Row& row : rows) {
		ShapesParams p = matte(shape(4.f, size));
		p.repeat = row.n;
		const Frame f = render(p);
		check(countBlobs(f, kW, kH) == row.blobs, label("REPEAT %d: %d separate shapes (got %d)", row.n, row.blobs, countBlobs(f, kW, kH)));
	}
	// Each shape is the same size as the first: the total area is blobs x one square.
	{
		ShapesParams p = matte(shape(4.f, size));
		p.repeat = 3;
		const double one = 4.0 * (size / 3.0) * (size / 3.0);
		const Measure me = measure(render(p, kBigW, kBigH), kBigW, kBigH);
		checkNear(me.area, 15.0 * one, 0.005 * 15.0 * one, "REPEAT 3: fifteen squares one third the size");
	}
	// An odd REPEAT has a shape at the middle of the picture, an even one has a gap.
	for (int n = 1; n <= 8; ++n) {
		ShapesParams p = matte(shape(4.f, 0.3f));
		p.repeat = n;
		const Frame f = render(p);
		const bool centre = grey(f, kW, kW / 2, kH / 2) == 255;
		check(centre == (n % 2 == 1), label("REPEAT %d: %s at the middle of the picture", n, (n % 2 == 1) ? "a shape" : "a gap"));
	}
	// The picture repeats exactly every H/n pixels along both axes.
	for (int n : {2, 3, 4, 6}) {
		ShapesParams p = shape(6.f, 0.5f, 0.1f);
		p.repeat = n;
		p.round = 0.3f;
		const int pitch = 288 / n;  // pixels at 512x288
		const Frame f = render(p, kBigW, kBigH);
		int worstX = 0, worstY = 0;
		for (int j = 0; j < kBigH; j += 3) {
			for (int i = 0; i + pitch < kBigW; i += 3) {
				worstX = std::max(worstX, std::abs(grey(f, kBigW, i, j) - grey(f, kBigW, i + pitch, j)));
			}
		}
		for (int j = 0; j + pitch < kBigH; j += 3) {
			for (int i = 0; i < kBigW; i += 3) {
				worstY = std::max(worstY, std::abs(grey(f, kBigW, i, j) - grey(f, kBigW, i, j + pitch)));
			}
		}
		check(worstX <= 2 && worstY <= 2, label("REPEAT %d: the grid repeats every %d pixels both ways (worst %d across, %d down)", n, pitch, worstX, worstY));
	}
	// X and Y move the whole grid, and a move of one cell is the same grid.
	{
		ShapesParams p = shape(6.f, 0.5f, 0.1f);
		p.repeat = 4;
		const Frame base = render(p, kBigW, kBigH);
		// One cell is 2/4 world = 0.25 of the picture height: Y 0.25 and X 0.25 * 288/512 of the width.
		p.y = 0.25f;
		check(maxDiff(base, render(p, kBigW, kBigH)) <= 2, "REPEAT 4: Y moved one cell is the same grid");
		p.y = 0.f;
		p.x = static_cast<float>(0.25 * kBigH / kBigW);
		check(maxDiff(base, render(p, kBigW, kBigH)) <= 2, "REPEAT 4: X moved one cell is the same grid");
		p.x = 0.5f * static_cast<float>(0.25 * kBigH / kBigW);
		check(maxDiff(base, render(p, kBigW, kBigH)) > 40, "REPEAT 4: X moved half a cell is a different grid");
	}
	// SPREAD is in picture units, so the halos of a fine grid overlap and the picture rises towards white.
	{
		auto meanGrey = [](int n, float spread) {
			ShapesParams p = shape(4.f, 0.3f, spread);
			p.repeat = n;
			const Frame f = render(p);
			double sum = 0.0;
			for (size_t i = 0; i < f.size(); i += 4) {
				sum += f[i];
			}
			return sum / (static_cast<double>(f.size()) / 4.0);
		};
		check(meanGrey(1, 0.5f) < meanGrey(8, 0.5f), "REPEAT 8 with a wide SPREAD is brighter than REPEAT 1: the halos overlap");
	}

	std::vector<Frame> sheet;
	for (int n = 1; n <= 8; ++n) {
		ShapesParams p = shape(5.f, 0.5f, 0.12f);
		p.repeat = n;
		sheet.push_back(sheetPanel(p));
	}
	saveSheet("shapes_repeat.png", sheet, 4, kSheetW, kSheetH);
}

void testCopies() {
	std::printf("Shapes COPIES: up to eight in a ring\n");
	const sf::Scene dummy = sf::makeScene(ShapesParams{}, kW, kH);
	check(dummy.copies == 1, "the default scene has one copy");

	for (int c = 1; c <= 8; ++c) {
		ShapesParams p = matte(shape(5.f, 0.12f));
		p.copies = c;
		p.ring = 0.5f;
		const Frame f = render(p);
		check(countBlobs(f, kW, kH) == c, label("COPIES %d: %d shapes (got %d)", c, c, countBlobs(f, kW, kH)));
		const Measure me = measure(render(p, kBigW, kBigH), kBigW, kBigH);
		const double one = polygonArea(5, 0.12);
		checkNear(me.area, c * one, 0.006 * c * one, label("COPIES %d: %d shapes of the same size", c, c));
		if (c >= 2) {
			checkNear(me.cx, 0.0, 0.004, label("COPIES %d: the ring is centred across", c));
			checkNear(me.cy, 0.0, 0.004, label("COPIES %d: the ring is centred up", c));
		}
		if (c >= 3) {
			checkNear(me.ixx / me.iyy, 1.0, 0.01, label("COPIES %d: the ring is as wide as it is tall", c));
		}
	}
	// One copy ignores RING; a ring of more copies grows with it.
	{
		ShapesParams a = matte(shape(5.f, 0.2f));
		ShapesParams b = a;
		a.ring = 0.1f;
		b.ring = 1.0f;
		check(maxDiff(render(a), render(b)) == 0, "one copy sits at the middle whatever RING says");
		a.copies = b.copies = 4;
		check(maxDiff(render(a), render(b)) > 100, "four copies move out as RING grows");
		a.ring = 0.f;
		const ShapesParams one = matte(shape(5.f, 0.2f));
		// A ring of radius 0 stacks every copy on the middle, each turned a quarter further: the union of four turned pentagons.
		check(render(a)[4 * (kW / 2 + kW * (kH / 2))] == 255, "COPIES with RING 0 stack at the middle");
		check(matteMeasure(a).area > matteMeasure(one).area * 1.15, "four stacked copies cover more than one shape does");
	}
	// The ring starts at the top, and the copies turn with it.
	{
		ShapesParams p = matte(shape(4.f, 0.1f));
		p.copies = 3;
		p.ring = 0.5f;
		const Frame f = render(p);
		auto at = [&](double wx, double wy) {
			const int i = static_cast<int>(std::lround((wx / (2.0 * kAspectWorld) + 0.5) * kW - 0.5));
			const int j = static_cast<int>(std::lround((0.5 - wy / 2.0) * kH - 0.5));
			return grey(f, kW, i, j);
		};
		check(at(0.0, 0.5) == 255, "COPIES 3: the first copy is at the top of the ring");
		check(at(0.5 * std::cos(kPi / 6.0 + 0.0), -0.25) == 255 && at(-0.5 * std::cos(kPi / 6.0), -0.25) == 255, "COPIES 3: the others are a third of a turn along");
		check(at(0.0, 0.0) == 0, "COPIES 3: nothing at the middle");
		// A positive TURN carries the whole ring counter-clockwise, so the top copy moves to the left.
		p.turn = 0.25f;
		const Frame g = render(p);
		auto atG = [&](double wx, double wy) {
			const int i = static_cast<int>(std::lround((wx / (2.0 * kAspectWorld) + 0.5) * kW - 0.5));
			const int j = static_cast<int>(std::lround((0.5 - wy / 2.0) * kH - 0.5));
			return grey(g, kW, i, j);
		};
		check(atG(-0.5, 0.0) == 255 && atG(0.0, 0.5) == 0, "COPIES 3, TURN +0.25: the ring turned a quarter, counter-clockwise");
	}
	// The field has the symmetry of the ring: turned by a n-th of a turn about the middle it is the same.
	{
		double worst = 0.0;
		Rng rng(7);
		for (int c = 2; c <= 8; ++c) {
			ShapesParams p = shape(5.f, 0.15f, 0.2f);
			p.round = 0.2f;
			p.copies = c;
			p.ring = 0.45f;
			p.turn = 0.07f;
			p.melt = 0.3f;
			const sf::Scene sc = sf::makeScene(p, kW, kH);
			const double th = 2.0 * kPi / c;
			for (int k = 0; k < 400; ++k) {
				const double x = rng.u(-1.2f, 1.2f), y = rng.u(-0.9f, 0.9f);
				const double rx = x * std::cos(th) - y * std::sin(th), ry = x * std::sin(th) + y * std::cos(th);
				worst = std::max(worst, static_cast<double>(std::fabs(sf::shade(sc, static_cast<float>(x), static_cast<float>(y)) -
				                                                          sf::shade(sc, static_cast<float>(rx), static_cast<float>(ry)))));
			}
		}
		checkAtMost(worst, 0.003, "COPIES 2 to 8: the ring looks the same turned by one copy's share (to a grey level)");
		std::printf("  ring symmetry error %.5f of full scale\n", worst);
	}
	// A whole copy's share of TURN is the same picture.
	{
		for (int c : {3, 4, 6}) {
			ShapesParams p = shape(5.f, 0.15f, 0.2f);
			p.copies = c;
			p.ring = 0.5f;
			const Frame base = render(p);
			p.turn = 1.f / asF(c);
			check(maxDiff(base, render(p)) <= 2, label("COPIES %d: TURN of 1/%d is the same ring", c, c));
		}
	}

	std::vector<Frame> sheet;
	for (int c = 1; c <= 8; ++c) {
		ShapesParams p = shape(5.f, 0.2f, 0.1f);
		p.copies = c;
		p.ring = 0.45f;
		sheet.push_back(sheetPanel(p));
	}
	saveSheet("shapes_copies.png", sheet, 4, kSheetW, kSheetH);
}

void testMelt() {
	std::printf("Shapes MELT: neighbours fuse like drops\n");
	// Two discs a ring apart: the gap between them fuses when the soft union reaches across it.
	// d at the midpoint is g/2 - k ln 2 with k = MELT x 0.28; inside when g < 2 k ln 2.
	const float radius = 0.2f, ring = 0.3f;  // gap g = 2 ring - 2 radius = 0.2
	auto twoDiscs = [&](float melt) {
		ShapesParams p = shape(8.f, radius, 0.1f);
		p.round = 1.f;
		p.copies = 2;
		p.ring = ring;
		p.melt = melt;
		return p;
	};
	const double gap = 2.0 * (ring - radius);
	const double meltFuse = gap / (2.0 * std::log(2.0) * 0.28);
	std::printf("  the two discs fuse for MELT above %.4f\n", meltFuse);
	for (float melt : {0.f, 0.2f, 0.45f, 0.6f, 0.8f, 1.f}) {
		const sf::Scene sc = sf::makeScene(twoDiscs(melt), kW, kH);
		const double k = melt * 0.28;
		const double d = melt == 0.f ? gap / 2.0 : gap / 2.0 - k * std::log(2.0);
		const double want = std::clamp(0.5 - d / (2.0 * 0.1), 0.0, 1.0);
		checkNear(sf::shade(sc, 0.f, 0.f), want, 0.006, label("MELT %.2f: the midpoint between the discs is at distance g/2 - k ln 2 (%.4f)", static_cast<double>(melt), d));
		ShapesParams m = matte(twoDiscs(melt));
		const int blobs = countBlobs(render(m, kBigW, kBigH), kBigW, kBigH);
		check(blobs == (static_cast<double>(melt) > meltFuse + 0.005 ? 1 : 2), label("MELT %.2f: the matte is %d shape(s) (got %d)", static_cast<double>(melt), static_cast<double>(melt) > meltFuse + 0.005 ? 1 : 2, blobs));
	}
	// The more MELT, the more is drawn.
	{
		double previous = 0.0;
		for (float melt : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
			const double area = matteMeasure(twoDiscs(melt)).area;
			check(area > previous, label("MELT %.2f: more area than at the lower setting", static_cast<double>(melt)));
			previous = area;
		}
		checkNear(matteMeasure(twoDiscs(0.f)).area, 2.0 * kPi * radius * radius, 0.004 * 2.0 * kPi * radius * radius, "MELT 0: two discs of their own area");
	}
	// Melting does not depend on the order the copies are drawn: turning the ring by a share swaps them.
	{
		ShapesParams p = twoDiscs(0.6f);
		const Frame a = render(p);
		p.turn = 0.5f;
		check(maxDiff(a, render(p)) <= 1, "MELT is symmetric: swapping the two discs changes nothing");
	}
	// A grid of discs that nearly touch fuses into one piece with MELT, and breaks apart without it.
	// REPEAT 2 puts the centres at 0.5 and 1.5 across and at 0.5 up and down: eight discs of radius 0.45 show, a gap 0.1 apart.
	{
		ShapesParams p = matte(shape(6.f, 0.9f));
		p.round = 1.f;
		p.repeat = 2;
		const int apart = countBlobs(render(p, kBigW, kBigH), kBigW, kBigH);
		p.melt = 1.f;
		const int together = countBlobs(render(p, kBigW, kBigH), kBigW, kBigH);
		check(apart == 8, label("REPEAT 2 discs, MELT 0: eight drops (got %d)", apart));
		check(together == 1, label("REPEAT 2 discs, MELT 1: one piece (got %d)", together));
	}
	// And a ring of eight copies joins up into a necklace.
	{
		ShapesParams p = matte(shape(8.f, 0.11f));
		p.round = 1.f;
		p.copies = 8;
		p.ring = 0.45f;
		const int apart = countBlobs(render(p), kW, kH);
		p.melt = 1.f;
		const int together = countBlobs(render(p), kW, kH);
		check(apart == 8 && together == 1, label("COPIES 8: eight drops without MELT (%d), one necklace with it (%d)", apart, together));
	}

	std::vector<Frame> sheet;
	for (float melt : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
		ShapesParams p = shape(8.f, 0.13f, 0.1f);
		p.round = 1.f;
		p.copies = 6;
		p.ring = 0.45f;
		p.melt = melt;
		sheet.push_back(sheetPanel(p));
	}
	for (float melt : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
		// A grid of discs with a gap of about a quarter of a half cell between them: they fuse into a net with MELT.
		ShapesParams p = shape(8.f, 0.88f, 0.05f);
		p.round = 1.f;
		p.repeat = 3;
		p.melt = melt;
		sheet.push_back(sheetPanel(p));
	}
	saveSheet("shapes_melt.png", sheet, 5, kSheetW, kSheetH);
}

// ---- the field: seams, culling, exact distances, smoothness and extremes ----

// Pixel centres in world units, worked out the way renderShapes works them out.
float pixelX(int i, int w, int h) {
	const float aspectWorld = static_cast<float>(w) / static_cast<float>(h);
	return ((static_cast<float>(i) + 0.5f) / static_cast<float>(w) - 0.5f) * 2.f * aspectWorld;
}
float pixelY(int j, int h) {
	return (0.5f - (static_cast<float>(j) + 0.5f) / static_cast<float>(h)) * 2.f;
}

// A scene that uses every control, picked from a seeded generator so a failure can be run again.
ShapesParams randomScene(Rng& rng, int it) {
	ShapesParams p;
	p.sides = 3.f + std::floor(rng.u(0.f, 10.f));
	if (it % 3 == 0) {
		p.sides += rng.u(0.f, 1.f);
	}
	p.round = (it % 2) ? rng.u(0.f, 1.f) : 0.f;
	p.point = (it % 4 == 1) ? rng.u(0.f, 1.f) : 0.f;
	p.size = rng.u(0.1f, (it % 7 == 0) ? 3.f : 1.1f);
	p.aspect = (it % 5 == 0) ? rng.u(0.3f, 3.f) : 1.f;
	p.turn = rng.u(-0.5f, 0.5f);
	p.repeat = (it % 4 == 3) ? 1 : 1 + (it % 8);
	p.copies = (it % 3 == 0) ? 1 : 1 + (it % 8);
	p.ring = rng.u(0.f, 1.2f);
	p.melt = (it % 2) ? rng.u(0.f, 1.f) : 0.f;
	p.spread = std::exp(rng.u(std::log(0.004f), std::log(2.f)));
	p.matte = (it % 11 == 0) ? 1 : 0;
	p.x = rng.u(-0.5f, 0.5f);
	p.y = rng.u(-0.5f, 0.5f);
	return p;
}

// One pixel of the field the slow way: every cell in a wide window and every copy, no culling and no early exit.
float bruteShade(const sf::Scene& S, float wx, float wy, int reach) {
	const float i0 = std::floor((wx - S.originX) / S.pitch + 0.5f);
	const float j0 = std::floor((wy - S.originY) / S.pitch + 0.5f);
	const int lo = S.tiled ? -reach : 0;
	const int hi = S.tiled ? reach : 0;
	sf::Melt melt;
	const bool soft = S.km > 0.f;
	for (int dj = lo; dj <= hi; ++dj) {
		for (int di = lo; di <= hi; ++di) {
			const float ox = S.originX + (S.tiled ? (i0 + static_cast<float>(di)) * S.pitch : 0.f);
			const float oy = S.originY + (S.tiled ? (j0 + static_cast<float>(dj)) * S.pitch : 0.f);
			const float ux = (wx - ox) / S.s;
			const float uy = (wy - oy) / S.s;
			for (int k = 0; k < S.copies; ++k) {
				const size_t c = static_cast<size_t>(k);
				const float dx = ux - S.cx[c];
				const float dy = uy - S.cy[c];
				const float qx = S.rc[c] * dx + S.rs[c] * dy;
				const float qy = -S.rs[c] * dx + S.rc[c] * dy;
				const float d = S.stretched ? sf::sdShapeStretched(S.shape, qx, qy) : sf::sdShape(S.shape, qx, qy);
				if (soft) {
					sf::meltAdd(melt, d, S.invKm);
				}
				else {
					melt.nearest = std::min(melt.nearest, d);
				}
			}
		}
	}
	const float field = soft ? sf::meltValue(melt, S.km) : melt.nearest;
	return std::clamp(0.5f - field * S.invTwoLc, 0.f, 1.f);
}

void testSeams() {
	std::printf("Shapes REPEAT, COPIES and MELT leave no seams between cells\n");
	Rng rng(31);
	const int w = 640, h = 360;
	const double eps = 1e-5;
	double worstSharp = 0.0, worstMelt = 0.0;
	int sharpScenes = 0, meltScenes = 0;
	for (int it = 0; it < 160; ++it) {
		ShapesParams p = randomScene(rng, it);
		p.repeat = 2 + (it % 7);
		p.x = rng.u(-0.3f, 0.3f);
		p.y = rng.u(-0.3f, 0.3f);
		p.size = rng.u(0.15f, 1.0f);
		p.ring = rng.u(0.05f, 0.9f);
		p.spread = rng.u(0.01f, 1.0f) / static_cast<float>(p.repeat);
		p.matte = 0;
		const sf::Scene sc = sf::makeScene(p, w, h);
		// What a step of 2 eps may change the grey by in a field that is 1-Lipschitz.
		const double slope = 2.0 * eps / (2.0 * std::max(static_cast<double>(p.spread), static_cast<double>(sc.pixel)));
		double worst = 0.0;
		for (int a = 0; a < 60; ++a) {
			const float xb = sc.originX + (static_cast<float>(std::floor(rng.u(-3.f, 3.f))) + 0.5f) * sc.pitch;
			const float y = rng.u(-1.f, 1.f);
			worst = std::max(worst, std::fabs(static_cast<double>(sf::shade(sc, xb - static_cast<float>(eps), y)) -
			                                   static_cast<double>(sf::shade(sc, xb + static_cast<float>(eps), y))) - slope);
			const float yb = sc.originY + (static_cast<float>(std::floor(rng.u(-3.f, 3.f))) + 0.5f) * sc.pitch;
			const float x = rng.u(-1.7f, 1.7f);
			worst = std::max(worst, std::fabs(static_cast<double>(sf::shade(sc, x, yb - static_cast<float>(eps))) -
			                                   static_cast<double>(sf::shade(sc, x, yb + static_cast<float>(eps)))) - slope);
		}
		if (p.melt > 0.f) {
			worstMelt = std::max(worstMelt, worst);
			++meltScenes;
		}
		else {
			worstSharp = std::max(worstSharp, worst);
			++sharpScenes;
		}
	}
	std::printf("  worst jump across a cell boundary, beyond what the ramp allows: %.5f without MELT (%d scenes), %.5f with MELT (%d scenes)\n",
	            worstSharp, sharpScenes, worstMelt, meltScenes);
	checkAtMost(worstSharp, 0.0015, "REPEAT and COPIES without MELT: no seam at a cell boundary");
	checkAtMost(worstMelt, 0.006, "REPEAT and COPIES with MELT: any seam at a cell boundary is under 1.5 grey levels");
}

void testCullingAgainstBruteForce() {
	std::printf("Shapes field: the culled kernel against a brute-force sum over every cell and copy\n");
	Rng rng(11);
	const int w = 96, h = 54;
	double worst = 0.0;
	long long over = 0, total = 0;
	int worstScene = -1;
	for (int it = 0; it < 90; ++it) {
		const ShapesParams p = randomScene(rng, it);
		const sf::Scene sc = sf::makeScene(p, w, h);
		for (int j = 0; j < h; ++j) {
			for (int i = 0; i < w; ++i) {
				const float wx = pixelX(i, w, h), wy = pixelY(j, h);
				const double diff = std::fabs(static_cast<double>(sf::shade(sc, wx, wy)) - static_cast<double>(bruteShade(sc, wx, wy, 7))) * 255.0;
				if (diff > worst) {
					worst = diff;
					worstScene = it;
				}
				if (diff > 1.5) {
					++over;
				}
				++total;
			}
		}
	}
	std::printf("  worst difference %.2f grey levels (scene %d), %lld of %lld pixels over 1.5\n", worst, worstScene, over, total);
	checkAtMost(worst, 2.0, "90 scenes using every control: the culled kernel is within 2 grey levels of the brute-force sum");
	checkAtMost(static_cast<double>(over), 0.0, "and never more than 1.5 levels from it");
}

// ---- the field table: the fast picture against the kernel it replaced ----

// The kernel as it stood before the field table, copied here and kept here: fold into the home cell, then take every
// tile in order and every copy that can reach the pixel, with the same culling and the same early exit. The table
// is held to what this draws.
float oldShade(const sf::Scene& S, float wx, float wy) {
	float ux = wx - S.originX;
	float uy = wy - S.originY;
	if (S.tiled) {
		const float fx = ux / S.pitch;
		const float fy = uy / S.pitch;
		ux = (fx - std::floor(fx + 0.5f)) * 2.f;
		uy = (fy - std::floor(fy + 0.5f)) * 2.f;
	}
	sf::Melt melt;
	const bool soft = S.km > 0.f;
	for (int t = 0; t < S.tileCount; ++t) {
		const float px = ux - 2.f * static_cast<float>(S.tileI[static_cast<size_t>(t)]);
		const float py = uy - 2.f * static_cast<float>(S.tileJ[static_cast<size_t>(t)]);
		if (S.tiled) {
			const float cutTile = std::max(std::min(melt.nearest, S.lc) + S.tail, 0.f);
			const float reach = S.tileReach + cutTile;
			if (px * px + py * py >= reach * reach) {
				continue;
			}
		}
		for (int k = 0; k < S.copies; ++k) {
			const float dx = px - S.cx[static_cast<size_t>(k)];
			const float dy = py - S.cy[static_cast<size_t>(k)];
			const float r2 = dx * dx + dy * dy;
			if (r2 < S.deepRadius2) {
				return 1.f;
			}
			const float cut = std::max(std::min(melt.nearest, S.lc) + S.tail, 0.f);
			const float limit = S.cullOffset + cut;
			if (r2 >= limit * limit) {
				continue;
			}
			const float rc = S.rc[static_cast<size_t>(k)];
			const float rs = S.rs[static_cast<size_t>(k)];
			const float qx = rc * dx + rs * dy;
			const float qy = -rs * dx + rc * dy;
			float d;
			if (S.stretched) {
				const float bx = std::max(std::fabs(qx) - S.boxX, 0.f);
				const float by = std::max(std::fabs(qy) - S.boxY, 0.f);
				if (bx * bx + by * by > cut * cut) {
					continue;
				}
				d = sf::sdShapeStretched(S.shape, qx, qy);
			}
			else {
				d = sf::sdShape(S.shape, qx, qy);
			}
			if (soft) {
				sf::meltAdd(melt, d, S.invKm);
			}
			else {
				melt.nearest = std::min(melt.nearest, d);
			}
		}
	}
	const float field = soft ? sf::meltValue(melt, S.km) : melt.nearest;
	return std::clamp(0.5f - field * S.invTwoLc, 0.f, 1.f);
}

// A whole picture in the bytes renderShapes writes, one pixel at a time through `grey`. A few threads share the rows;
// every pixel is worked out on its own, so that cannot change it.
template <class Grey>
Frame renderRows(int w, int h, Grey grey) {
	Frame out(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
	std::atomic<int> nextRow{0};
	auto worker = [&]() {
		for (int j = nextRow.fetch_add(1); j < h; j = nextRow.fetch_add(1)) {
			const float wy = pixelY(j, h);
			for (int i = 0; i < w; ++i) {
				const unsigned char g = static_cast<unsigned char>(std::lround(255.f * grey(pixelX(i, w, h), wy)));
				unsigned char* px = out.data() + (static_cast<size_t>(j) * static_cast<size_t>(w) + static_cast<size_t>(i)) * 4u;
				px[0] = px[1] = px[2] = g;
				px[3] = 255;
			}
		}
	};
	const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
	std::vector<std::thread> pool;
	for (int t = 1; t < threads; ++t) {
		pool.emplace_back(worker);
	}
	worker();
	for (std::thread& th : pool) {
		th.join();
	}
	return out;
}

// The old kernel over a whole picture.
Frame oldRender(const sf::Scene& sc, int w, int h) {
	return renderRows(w, h, [&](float wx, float wy) { return oldShade(sc, wx, wy); });
}

// The scene as it stands, table and all, over a whole picture.
Frame sceneRender(const sf::Scene& sc, int w, int h) {
	return renderRows(w, h, [&](float wx, float wy) { return sf::shade(sc, wx, wy); });
}

constexpr int kControlCount = 14;

// One control at the low or the high end of its range: the extremes the engine clamps to, or (panel) the ends the
// knob can reach.
void setEnd(ShapesParams& p, int control, bool high, bool panel) {
	switch (control) {
		case 0: p.sides = high ? 12.f : 3.f; break;
		case 1: p.round = high ? 1.f : 0.f; break;
		case 2: p.point = high ? 1.f : 0.f; break;
		case 3: p.size = high ? (panel ? 2.5f : 4.f) : 0.f; break;
		case 4: p.x = high ? (panel ? 0.5f : 2.f) : (panel ? -0.5f : -2.f); break;
		case 5: p.y = high ? (panel ? 0.5f : 2.f) : (panel ? -0.5f : -2.f); break;
		case 6: p.aspect = high ? 4.f : 0.25f; break;
		case 7: p.turn = high ? 0.5f : -0.5f; break;
		case 8: p.repeat = high ? 8 : 1; break;
		case 9: p.copies = high ? 8 : 1; break;
		case 10: p.ring = high ? (panel ? 1.2f : 2.f) : 0.f; break;
		case 11: p.melt = high ? 1.f : 0.f; break;
		case 12: p.spread = high ? (panel ? 2.f : 4.f) : (panel ? 0.004f : 0.001f); break;
		default: p.matte = high ? 1 : 0; break;
	}
}

struct TableCase {
	ShapesParams p;
	int w;
	int h;
};

// The scenes the table is checked on: the busiest combination at every spread, then each control alone at both ends
// of its range from two busy starting points, then every control at an end at once, then a seeded spread of ordinary
// scenes. Pictures of at least 30000 pixels, so that a busy scene is tabulated.
std::vector<TableCase> tableCases() {
	std::vector<TableCase> v;
	ShapesParams worst = shape(7.5f, 0.3f, 2.f);
	worst.round = 0.5f;
	worst.repeat = 8;
	worst.copies = 8;
	worst.melt = 0.5f;
	worst.aspect = 2.f;
	for (float spread : {2.f, 0.001f, 0.02f, 0.12f, 0.5f, 4.f}) {
		for (float melt : {0.5f, 1.f}) {
			TableCase c{worst, 480, 270};
			c.p.spread = spread;
			c.p.melt = melt;
			v.push_back(c);
		}
	}
	ShapesParams other = shape(5.f, 0.4f, 0.05f);
	other.round = 0.2f;
	other.point = 0.3f;
	other.repeat = 4;
	other.copies = 5;
	other.ring = 0.6f;
	other.melt = 0.4f;
	other.turn = 0.1f;
	other.x = 0.1f;
	other.y = -0.2f;
	ShapesParams tight = worst;
	tight.spread = 0.12f;
	for (const ShapesParams& base : {tight, other}) {
		for (int control = 0; control < kControlCount; ++control) {
			for (bool high : {false, true}) {
				TableCase c{base, 240, 135};
				setEnd(c.p, control, high, false);
				v.push_back(c);
			}
		}
	}
	Rng corners(77);
	for (int it = 0; it < 48; ++it) {
		TableCase c{shape(4.f, 0.5f), 240, 135};
		for (int control = 0; control < kControlCount; ++control) {
			setEnd(c.p, control, corners.u(0.f, 1.f) < 0.5f, corners.u(0.f, 1.f) < 0.5f);
		}
		v.push_back(c);
	}
	Rng rng(2718);
	for (int it = 0; it < 120; ++it) {
		v.push_back({randomScene(rng, it), 240, 135});
	}
	return v;
}

// Scenes that are not busy enough to be tabulated by themselves, where the table is built anyway: a spread biased
// toward the places a table is easiest to fool (thin ramps, a little MELT, sharp points, stretched shapes, small
// and large shapes, lone copies and crowded ones).
std::vector<TableCase> forcedCases() {
	std::vector<TableCase> v;
	Rng rng(4242);
	auto pick = [&](int n) { return std::min(n - 1, static_cast<int>(rng.u(0.f, static_cast<float>(n)))); };
	for (int it = 0; it < 150; ++it) {
		ShapesParams p;
		p.repeat = 2 + pick(7);
		p.copies = 1 + pick(8);
		p.sides = pick(2) ? static_cast<float>(3 + pick(10)) : rng.u(3.f, 12.f);
		p.size = std::exp(rng.u(std::log(0.08f), std::log(1.5f)));
		const int r = pick(10);
		p.round = r < 4 ? 0.f : (r < 7 ? rng.u(0.f, 1.f) : 1.f);
		p.point = pick(10) < 6 ? 0.f : rng.u(0.f, 1.f);
		p.aspect = pick(10) < 3 ? 1.f : std::exp(rng.u(std::log(0.25f), std::log(4.f)));
		p.ring = rng.u(0.f, 1.3f);
		const int m = pick(20);
		p.melt = m < 7 ? 0.f : (m < 12 ? std::exp(rng.u(std::log(0.005f), std::log(0.12f))) : (m < 16 ? rng.u(0.1f, 0.5f) : 1.f));
		p.spread = std::exp(rng.u(std::log(0.001f), std::log(0.4f)));
		p.matte = pick(10) < 2 ? 1 : 0;
		p.turn = rng.u(0.f, 1.f);
		p.x = rng.u(-1.f, 1.f);
		p.y = rng.u(-1.f, 1.f);
		v.push_back({p, 240, 135});
	}
	return v;
}

void testTableAgainstOldKernel() {
	std::printf("Shapes table: the fast picture against the kernel it replaced, pixel by pixel\n");
	const std::vector<TableCase> cases = tableCases();
	long long pixels = 0, offByOne = 0;
	int worstDiff = 0, worstCase = -1, engaged = 0, threadsDiffer = 0;
	for (size_t n = 0; n < cases.size(); ++n) {
		const TableCase& c = cases[n];
		const sf::Scene sc = sf::makeScene(c.p, c.w, c.h);
		const Frame fast = render(c.p, c.w, c.h);
		const Frame old = oldRender(sc, c.w, c.h);
		engaged += sc.table ? 1 : 0;
		for (size_t i = 0; i < fast.size(); i += 4) {
			const int diff = std::abs(static_cast<int>(fast[i]) - static_cast<int>(old[i]));
			offByOne += diff == 1 ? 1 : 0;
			if (diff > worstDiff) {
				worstDiff = diff;
				worstCase = static_cast<int>(n);
			}
		}
		pixels += static_cast<long long>(c.w) * c.h;
		// The picture does not depend on how many threads draw it, tabulated or not.
		if (n < 12 || (n % 9 == 0 && sc.table)) {
			threadsDiffer += maxDiff(fast, referenceRender(c.p, c.w, c.h)) != 0 ? 1 : 0;
		}
	}
	std::printf("  %zu scenes (%d tabulated), %lld pixels compared, %lld differ by one grey level, worst difference %d (scene %d)\n",
	            cases.size(), engaged, pixels, offByOne, worstDiff, worstCase);
	check(cases.size() >= 200, "at least 200 scenes are compared");
	check(engaged >= 60, label("the table is built for many of them (%d)", engaged));
	check(sf::makeScene(cases[0].p, cases[0].w, cases[0].h).table != nullptr, "the busiest scene (REPEAT 8, COPIES 8, ASPECT 2, ROUND, MELT) is tabulated");
	check(worstDiff <= 1, label("every pixel is within one grey level of the old kernel (worst %d)", worstDiff));
	check(threadsDiffer == 0, "and the same bytes come from one thread and from several");

	// The same with the table forced on for scenes that would not get one.
	const std::vector<TableCase> forced = forcedCases();
	long long forcedPixels = 0, forcedOffByOne = 0;
	int forcedWorst = 0, forcedWorstCase = -1, forcedBuilt = 0;
	for (size_t n = 0; n < forced.size(); ++n) {
		const TableCase& c = forced[n];
		sf::Scene sc = sf::makeScene(c.p, c.w, c.h);
		if (!sc.table) {
			// Half of them get the budget a frame of this size would, half all the nodes they could use, so that no
			// square is left "exact" just because the budget ran out.
			const long long px = static_cast<long long>(c.w) * c.h;
			const long long budget = n % 2 ? sf::kTableMaxEvals : std::clamp<long long>(px / sf::kTableBudgetDiv, 600, sf::kTableMaxEvals);
			sc.table = sf::buildFieldTable(sc, static_cast<int>(budget), 3000.f);
		}
		forcedBuilt += sc.table && !sc.table->leaf.empty() ? 1 : 0;
		const Frame fast = sceneRender(sc, c.w, c.h);
		const Frame old = oldRender(sc, c.w, c.h);
		for (size_t i = 0; i < fast.size(); i += 4) {
			const int diff = std::abs(static_cast<int>(fast[i]) - static_cast<int>(old[i]));
			forcedOffByOne += diff == 1 ? 1 : 0;
			if (diff > forcedWorst) {
				forcedWorst = diff;
				forcedWorstCase = static_cast<int>(n);
			}
		}
		forcedPixels += static_cast<long long>(c.w) * c.h;
	}
	std::printf("  %zu more scenes with the table forced on (%d built), %lld pixels compared, %lld differ by one grey level, worst difference %d (scene %d)\n",
	            forced.size(), forcedBuilt, forcedPixels, forcedOffByOne, forcedWorst, forcedWorstCase);
	check(forced.size() >= 100 && forcedBuilt == static_cast<int>(forced.size()), "every one of them gets a table");
	check(forcedWorst <= 1, label("and each stays within one grey level of the old kernel (worst %d)", forcedWorst));
}

// The scenes the speed work is aimed at, against the old kernel pixel by pixel: REPEAT off with COPIES and MELT at
// every spread, SPREAD raised with MELT where the picture repeats, and a seeded spread of random scenes. Every
// fourth is also drawn on one thread, which must give the same bytes.
void testBusyScenesAgainstOldKernel() {
	std::printf("Shapes speed work: REPEAT off with COPIES and MELT, SPREAD up with MELT, random scenes, against the old kernel\n");
	std::vector<TableCase> v;
	ShapesParams heavy = shape(7.5f, 0.3f, 2.f);
	heavy.round = 0.5f;
	heavy.copies = 8;
	heavy.aspect = 2.f;
	for (float aspect : {2.f, 1.f}) {
		for (float melt : {0.f, 0.1f, 0.5f, 1.f}) {
			for (float spread : {0.02f, 0.12f, 0.5f, 2.f, 4.f}) {
				for (int copies : {1, 3, 8}) {
					TableCase c{heavy, 160, 90};
					c.p.aspect = aspect;
					c.p.repeat = 1;
					c.p.melt = melt;
					c.p.spread = spread;
					c.p.copies = copies;
					v.push_back(c);
				}
			}
		}
	}
	const size_t repeatOff = v.size();
	for (float aspect : {2.f, 1.f}) {
		for (int repeat : {2, 8}) {
			for (float melt : {0.5f, 1.f}) {
				for (float spread : {0.5f, 2.f, 4.f}) {
					TableCase c{heavy, 240, 135};
					c.p.aspect = aspect;
					c.p.repeat = repeat;
					c.p.melt = melt;
					c.p.spread = spread;
					v.push_back(c);
				}
			}
		}
	}
	const size_t spreadUp = v.size() - repeatOff;
	Rng rng(9090);
	for (int it = 0; it < 60; ++it) {
		TableCase c{randomScene(rng, it), 160, 90};
		if (it % 2 == 0) {
			c.p.repeat = 1;
			c.p.copies = 2 + it % 7;
			c.p.melt = rng.u(0.05f, 1.f);
		}
		v.push_back(c);
	}
	long long pixels = 0, offByOne = 0;
	int worstDiff = 0, worstCase = -1, threadsDiffer = 0, tabulated = 0;
	for (size_t n = 0; n < v.size(); ++n) {
		const TableCase& c = v[n];
		const sf::Scene sc = sf::makeScene(c.p, c.w, c.h);
		tabulated += sc.table ? 1 : 0;
		const Frame fast = render(c.p, c.w, c.h);
		const Frame old = oldRender(sc, c.w, c.h);
		for (size_t i = 0; i < fast.size(); i += 4) {
			const int diff = std::abs(static_cast<int>(fast[i]) - static_cast<int>(old[i]));
			offByOne += diff == 1 ? 1 : 0;
			if (diff > worstDiff) {
				worstDiff = diff;
				worstCase = static_cast<int>(n);
			}
		}
		pixels += static_cast<long long>(c.w) * c.h;
		if (n % 4 == 0) {
			threadsDiffer += maxDiff(fast, referenceRender(c.p, c.w, c.h)) != 0 ? 1 : 0;
		}
	}
	std::printf("  %zu scenes (%zu with REPEAT off, %zu with SPREAD up and MELT, %d tabulated), %lld pixels compared, %lld differ by one grey level, worst difference %d (scene %d)\n",
	            v.size(), repeatOff, spreadUp, tabulated, pixels, offByOne, worstDiff, worstCase);
	check(worstDiff <= 1, label("every pixel is within one grey level of the old kernel (worst %d)", worstDiff));
	check(threadsDiffer == 0, "and the same bytes come from one thread and from several");
}

// REPEAT off: a busy scene now builds its table over the frame rather than over one lattice cell, and every table
// reads its busy cells from curved surfaces, each checked at 25 points. Against the old kernel pixel by pixel: REPEAT
// off at the smallest size that builds a frame table and at 960x540, the frame table forced on at full budget for
// random scenes in wide, tall and square frames (the squares outside the frame are left to the per-pixel kernel),
// and thin ramps with MELT where the picture repeats, which the curved surfaces now tabulate. Every tabulated scene
// with REPEAT off, and every third of the others, is also drawn on one thread, which must give the same bytes.
void testFrameTableAgainstOldKernel() {
	std::printf("Shapes frame table and curved cells: REPEAT off, forced frame tables, thin ramps with MELT, against the old kernel\n");
	std::vector<TableCase> v;
	ShapesParams heavy = shape(7.5f, 0.3f, 2.f);
	heavy.round = 0.5f;
	heavy.copies = 8;
	heavy.aspect = 2.f;
	heavy.repeat = 1;
	for (float melt : {0.f, 0.5f, 1.f}) {
		for (float spread : {0.02f, 0.12f, 0.5f, 2.f}) {
			for (int copies : {3, 8}) {
				TableCase c{heavy, 240, 135};  // 32400 pixels, just over the smallest frame that is tabulated
				c.p.melt = melt;
				c.p.spread = spread;
				c.p.copies = copies;
				v.push_back(c);
			}
		}
	}
	for (float spread : {2.f, 0.12f}) {
		TableCase c{heavy, 960, 540};
		c.p.melt = 0.5f;
		c.p.spread = spread;
		v.push_back(c);
	}
	const size_t repeatOff = v.size();
	for (int repeat : {2, 3}) {
		for (float melt : {0.5f, 1.f}) {
			for (float spread : {0.02f, 0.05f, 0.12f}) {
				for (int copies : {3, 8}) {
					TableCase c{heavy, 480, 270};
					c.p.repeat = repeat;
					c.p.melt = melt;
					c.p.spread = spread;
					c.p.copies = copies;
					v.push_back(c);
				}
			}
		}
	}
	long long pixels = 0, offByOne = 0;
	int worstDiff = 0, worstCase = -1, threadsDiffer = 0, threadChecks = 0, framed = 0, tabulated = 0;
	for (size_t n = 0; n < v.size(); ++n) {
		const TableCase& c = v[n];
		const sf::Scene sc = sf::makeScene(c.p, c.w, c.h);
		tabulated += sc.table ? 1 : 0;
		framed += sc.table && n < repeatOff ? 1 : 0;
		const Frame fast = render(c.p, c.w, c.h);
		const Frame old = oldRender(sc, c.w, c.h);
		for (size_t i = 0; i < fast.size(); i += 4) {
			const int diff = std::abs(static_cast<int>(fast[i]) - static_cast<int>(old[i]));
			offByOne += diff == 1 ? 1 : 0;
			if (diff > worstDiff) {
				worstDiff = diff;
				worstCase = static_cast<int>(n);
			}
		}
		pixels += static_cast<long long>(c.w) * c.h;
		if (sc.table && (n < repeatOff || n % 3 == 0)) {
			threadsDiffer += maxDiff(fast, referenceRender(c.p, c.w, c.h)) != 0 ? 1 : 0;
			++threadChecks;
		}
	}
	std::printf("  %zu scenes (%zu with REPEAT off, %d of them over a frame table; %d tabulated in all), %lld pixels compared, %lld differ by one grey level, worst difference %d (scene %d); %d drawn on one thread too\n",
	            v.size(), repeatOff, framed, tabulated, pixels, offByOne, worstDiff, worstCase, threadChecks);
	check(framed >= 20, label("a busy scene with REPEAT off is tabulated over the frame (%d)", framed));
	check(sf::makeScene(v[repeatOff - 2].p, 960, 540).table != nullptr, "REPEAT off, COPIES 8, MELT .5 at 960x540 is tabulated over the frame");
	check(worstDiff <= 1, label("every pixel is within one grey level of the old kernel (worst %d)", worstDiff));
	check(threadsDiffer == 0, "and the same bytes come from one thread and from several");

	// The frame table forced on, at full budget so that busy squares become curved surfaces rather than running out
	// of budget, for random scenes with REPEAT off in wide, tall and square frames.
	Rng rng(5151);
	auto pick = [&](int k) { return std::min(k - 1, static_cast<int>(rng.u(0.f, static_cast<float>(k)))); };
	const int frames[3][2] = {{240, 135}, {135, 240}, {200, 200}};
	long long forcedPixels = 0, forcedOffByOne = 0, smoothLeaves = 0, flatLeaves = 0;
	int forcedWorst = 0, forcedWorstCase = -1, forcedBuilt = 0;
	const int forcedCount = 90;
	for (int it = 0; it < forcedCount; ++it) {
		ShapesParams p;
		p.repeat = 1;
		p.copies = 1 + pick(8);
		p.sides = pick(2) ? static_cast<float>(3 + pick(10)) : rng.u(3.f, 12.f);
		p.size = std::exp(rng.u(std::log(0.08f), std::log(1.5f)));
		const int r = pick(10);
		p.round = r < 4 ? 0.f : (r < 7 ? rng.u(0.f, 1.f) : 1.f);
		p.point = pick(10) < 6 ? 0.f : rng.u(0.f, 1.f);
		p.aspect = pick(10) < 3 ? 1.f : std::exp(rng.u(std::log(0.25f), std::log(4.f)));
		p.ring = rng.u(0.f, 1.3f);
		const int m = pick(20);
		p.melt = m < 7 ? 0.f : (m < 12 ? std::exp(rng.u(std::log(0.005f), std::log(0.12f))) : (m < 16 ? rng.u(0.1f, 0.5f) : 1.f));
		p.spread = std::exp(rng.u(std::log(0.001f), std::log(0.4f)));
		p.matte = pick(10) < 2 ? 1 : 0;
		p.turn = rng.u(0.f, 1.f);
		p.x = rng.u(-1.f, 1.f);
		p.y = rng.u(-1.f, 1.f);
		const int w = frames[it % 3][0], h = frames[it % 3][1];
		sf::Scene sc = sf::makeScene(p, w, h);
		if (!sc.table) {
			sc.table = sf::buildFieldTable(sc, sf::kTableMaxEvals, 3000.f);
		}
		forcedBuilt += sc.table && !sc.table->leaf.empty() ? 1 : 0;
		if (sc.table) {
			for (const sf::FieldTable::Leaf& leaf : sc.table->leaf) {
				smoothLeaves += leaf.kind == sf::FieldTable::kSmooth ? 1 : 0;
				flatLeaves += leaf.kind == sf::FieldTable::kLinear ? 1 : 0;
			}
		}
		const Frame fast = sceneRender(sc, w, h);
		const Frame old = oldRender(sc, w, h);
		for (size_t i = 0; i < fast.size(); i += 4) {
			const int diff = std::abs(static_cast<int>(fast[i]) - static_cast<int>(old[i]));
			forcedOffByOne += diff == 1 ? 1 : 0;
			if (diff > forcedWorst) {
				forcedWorst = diff;
				forcedWorstCase = it;
			}
		}
		forcedPixels += static_cast<long long>(w) * h;
	}
	std::printf("  %d more with REPEAT off and the frame table forced on (%d built, %lld flat and %lld curved cells), %lld pixels compared, %lld differ by one grey level, worst difference %d (scene %d)\n",
	            forcedCount, forcedBuilt, flatLeaves, smoothLeaves, forcedPixels, forcedOffByOne, forcedWorst, forcedWorstCase);
	check(forcedBuilt == forcedCount && smoothLeaves > 0 && flatLeaves > 0, "every one of them gets a frame table, with flat and curved cells among them");
	check(forcedWorst <= 1, label("and each stays within one grey level of the old kernel (worst %d)", forcedWorst));
}

void testExactDistance() {
	std::printf("Shapes distance: the kernel against exact references that share none of its code\n");
	Rng rng(2026);
	auto point = [&]() { return std::make_pair(static_cast<double>(rng.u(-1.4f, 1.4f)), static_cast<double>(rng.u(-1.f, 1.f))); };

	// Whole numbers of sides, no stretch. A polygon, a star and a circle are measured against the exact distance to
	// the edges (a rounded convex shape is its core grown by a disc, so its distance is the core's minus the disc's
	// radius). A partly rounded star has sharp notches the disc does not reach, so inside it the true distance is the
	// distance to its outline, which is walked out ray by ray as for the stretched shapes below.
	double worstPlain = 0.0, worstRoundedStar = 0.0;
	int cases = 0, roundedStars = 0;
	for (int m = 3; m <= 12; ++m) {
		for (double pt : {0.0, 0.5, 1.0}) {
			for (double rd : {0.0, 0.35, 1.0}) {
				ShapesParams p = shape(static_cast<float>(m), 0.6f);
				p.point = static_cast<float>(pt);
				p.round = static_cast<float>(rd);
				const sf::Scene sc = sf::makeScene(p, kBigW, kBigH);
				const bool partlyRoundedStar = pt > 0.0 && rd > 0.0 && rd < 1.0;
				const Core core = coreStar(m, 0.6, pt, rd);
				const Outline outline = partlyRoundedStar ? referenceOutline(m, 0.6, pt, rd, 1.0, 1.0, 2880) : Outline();
				double worst = 0.0;
				for (int k = 0; k < 160; ++k) {
					const auto xy = point();
					const double ref = partlyRoundedStar ? polygonSignedDistance(outline.x, outline.y, xy.first, xy.second)
					                                     : polygonSignedDistance(core.x, core.y, xy.first, xy.second) - core.r;
					const double got = sf::sdShape(sc.shape, static_cast<float>(xy.first), static_cast<float>(xy.second));
					worst = std::max(worst, std::fabs(got - ref) / kBigPixel);
				}
				if (partlyRoundedStar) {
					worstRoundedStar = std::max(worstRoundedStar, worst);
					++roundedStars;
				}
				else {
					worstPlain = std::max(worstPlain, worst);
					++cases;
				}
			}
		}
	}
	std::printf("  %d polygons, stars and circles, worst error %.5f pixel; %d partly rounded stars, worst error %.4f pixel\n", cases, worstPlain, roundedStars, worstRoundedStar);
	checkAtMost(worstPlain, 0.02, "SIDES 3 to 12 with POINT and ROUND: the distance is exact to a fiftieth of a pixel");
	checkAtMost(worstRoundedStar, 0.1, "partly rounded stars: the distance to the outline, in and out, is within a tenth of a pixel");

	// Stretched shapes: the distance to the true stretched outline, sharp corners and rounded.
	double worstSharp = 0.0, worstRounded = 0.0;
	int stretched = 0;
	const double pts[5] = {0.0, 1.0, 0.0, 0.6, 0.0};
	const double rds[5] = {0.0, 0.0, 0.4, 0.5, 1.0};
	for (int m : {3, 4, 5, 6, 8, 12}) {
		for (double ratio : {0.25, 0.5, 2.0, 4.0}) {
			for (int c = 0; c < 5; ++c) {
				ShapesParams p = shape(static_cast<float>(m), 0.5f);
				p.point = static_cast<float>(pts[c]);
				p.round = static_cast<float>(rds[c]);
				p.aspect = static_cast<float>(ratio);
				const sf::Scene sc = sf::makeScene(p, kBigW, kBigH);
				const double sx = std::sqrt(ratio);
				const Outline outline = referenceOutline(m, 0.5, pts[c], rds[c], sx, 1.0 / sx, 2880);
				double worst = 0.0;
				for (int k = 0; k < 160; ++k) {
					const auto xy = point();
					const double ref = polygonSignedDistance(outline.x, outline.y, xy.first, xy.second);
					const double got = sf::sdShapeStretched(sc.shape, static_cast<float>(xy.first), static_cast<float>(xy.second));
					worst = std::max(worst, std::fabs(got - ref) / kBigPixel);
				}
				if (rds[c] > 0.0) {
					worstRounded = std::max(worstRounded, worst);
				}
				else {
					worstSharp = std::max(worstSharp, worst);
				}
				++stretched;
			}
		}
	}
	std::printf("  %d stretched shapes, worst error %.4f pixel sharp, %.4f pixel rounded\n", stretched, worstSharp, worstRounded);
	checkAtMost(worstSharp, 0.03, "ASPECT with sharp corners: the distance to the stretched outline is exact to a thirtieth of a pixel");
	checkAtMost(worstRounded, 0.15, "ASPECT with rounded corners: the distance is within a seventh of a pixel (chords lie just inside the curve)");
}

void testSmoothness() {
	std::printf("Shapes field: neighbouring pixels never jump by more than the ramp allows\n");
	Rng rng(5);
	std::vector<ShapesParams> scenes = sceneZoo();
	for (int it = 0; it < 40; ++it) {
		scenes.push_back(randomScene(rng, it));
	}
	const int w = 192, h = 108;
	double worstRatio = 0.0;
	for (size_t s = 0; s < scenes.size(); ++s) {
		const ShapesParams& p = scenes[s];
		const Frame f = render(p, w, h);
		const double pixel = 2.0 / h;
		const double leff = p.matte ? 0.5 * pixel : std::max(static_cast<double>(p.spread), pixel);
		// A 1-Lipschitz field changes the grey by at most pixel / (2 leff) between neighbours; add a level for rounding
		// each end and one and a quarter for the melt's tail cut.
		const double bound = 255.0 * pixel / (2.0 * leff) + 2.5;
		int worst = 0;
		for (int j = 0; j < h; ++j) {
			for (int i = 0; i < w; ++i) {
				if (i + 1 < w) {
					worst = std::max(worst, std::abs(grey(f, w, i, j) - grey(f, w, i + 1, j)));
				}
				if (j + 1 < h) {
					worst = std::max(worst, std::abs(grey(f, w, i, j) - grey(f, w, i, j + 1)));
				}
			}
		}
		worstRatio = std::max(worstRatio, worst / bound);
		check(worst <= bound, label("scene %d: neighbouring pixels differ by %d levels at most, the ramp allows %.1f", static_cast<int>(s), worst, bound));
	}
	std::printf("  %d scenes, the steepest step is %.0f%% of what the ramp allows\n", static_cast<int>(scenes.size()), 100.0 * worstRatio);
}

void testExtremes() {
	std::printf("Shapes extremes: floats and counts outside the panel's ranges\n");
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	ShapesParams base;
	base.sides = 7.3f;
	base.round = 0.3f;
	base.point = 0.2f;
	base.size = 0.35f;
	base.x = 0.1f;
	base.y = -0.1f;
	base.aspect = 1.6f;
	base.turn = 0.1f;
	base.repeat = 2;
	base.copies = 3;
	base.ring = 0.5f;
	base.melt = 0.3f;
	base.spread = 0.2f;
	struct Field {
		const char* name;
		float ShapesParams::*member;
		float fallback;
		float lo;
		float hi;
		bool clamps;
	};
	const Field fields[] = {
		{"sides", &ShapesParams::sides, 4.f, 3.f, 12.f, true},
		{"round", &ShapesParams::round, 0.f, 0.f, 1.f, true},
		{"point", &ShapesParams::point, 0.f, 0.f, 1.f, true},
		{"size", &ShapesParams::size, 0.5f, 0.f, 4.f, true},
		{"x", &ShapesParams::x, 0.f, -2.f, 2.f, true},
		{"y", &ShapesParams::y, 0.f, -2.f, 2.f, true},
		{"aspect", &ShapesParams::aspect, 1.f, 0.25f, 4.f, true},
		{"turn", &ShapesParams::turn, 0.f, 0.f, 0.f, false},
		{"ring", &ShapesParams::ring, 0.48f, 0.f, 2.f, true},
		{"melt", &ShapesParams::melt, 0.f, 0.f, 1.f, true},
		{"spread", &ShapesParams::spread, 0.12f, 0.001f, 4.f, true},
	};
	for (const Field& field : fields) {
		ShapesParams want = base;
		want.*(field.member) = field.fallback;
		const Frame fallback = render(want, 96, 54);
		for (float bad : {nan, inf, -inf}) {
			ShapesParams p = base;
			p.*(field.member) = bad;
			const Frame f = render(p, 96, 54);
			check(isOpaqueGrey(f), label("%s %s: a valid picture", field.name, std::isnan(bad) ? "not a number" : (bad > 0 ? "+infinity" : "-infinity")));
			check(maxDiff(f, fallback) == 0, label("%s %s: drawn as the default %g", field.name, std::isnan(bad) ? "not a number" : (bad > 0 ? "+infinity" : "-infinity"), static_cast<double>(field.fallback)));
		}
		for (float huge : {1e30f, -1e30f}) {
			ShapesParams p = base;
			p.*(field.member) = huge;
			const Frame f = render(p, 96, 54);
			check(isOpaqueGrey(f), label("%s %g: a valid picture", field.name, static_cast<double>(huge)));
			if (field.clamps) {
				ShapesParams edge = base;
				edge.*(field.member) = huge > 0.f ? field.hi : field.lo;
				check(maxDiff(f, render(edge, 96, 54)) == 0, label("%s %g: held to the end of its range, %g", field.name, static_cast<double>(huge), static_cast<double>(huge > 0.f ? field.hi : field.lo)));
			}
		}
	}
	// Whole-number controls and the MATTE switch.
	for (int bad : {0, -5, -1000000}) {
		ShapesParams a = base, b = base;
		a.repeat = bad;
		b.repeat = 1;
		check(maxDiff(render(a, 96, 54), render(b, 96, 54)) == 0, label("REPEAT %d: held to 1", bad));
		a = base;
		b = base;
		a.copies = bad;
		b.copies = 1;
		check(maxDiff(render(a, 96, 54), render(b, 96, 54)) == 0, label("COPIES %d: held to 1", bad));
	}
	for (int bad : {9, 100, 1000000}) {
		ShapesParams a = base, b = base;
		a.repeat = bad;
		b.repeat = 8;
		check(maxDiff(render(a, 96, 54), render(b, 96, 54)) == 0, label("REPEAT %d: held to 8", bad));
		a = base;
		b = base;
		a.copies = bad;
		b.copies = 8;
		check(maxDiff(render(a, 96, 54), render(b, 96, 54)) == 0, label("COPIES %d: held to 8", bad));
	}
	{
		ShapesParams a = base, b = base;
		a.matte = 7;
		b.matte = 1;
		check(maxDiff(render(a, 96, 54), render(b, 96, 54)) == 0, "MATTE is on for any non-zero value");
	}
	// The biggest shape the panel can ask for, and none.
	{
		ShapesParams p = shape(12.f, 4.f);
		p.round = 1.f;
		p.repeat = 8;
		p.copies = 8;
		p.ring = 2.f;
		p.melt = 1.f;
		p.spread = 4.f;
		const Frame f = render(p, 160, 90);
		check(isOpaqueGrey(f), "everything at its maximum: a valid picture");
		ShapesParams z = shape(12.f, 0.f);
		z.repeat = 8;
		z.copies = 8;
		z.melt = 1.f;
		check(isOpaqueGrey(render(z, 160, 90)), "SIZE 0 at REPEAT 8, COPIES 8 and MELT 1: a valid picture");
	}
}

// Time one drawing, in milliseconds.
template <class Draw>
double timeMs(Draw&& draw) {
	const auto t0 = std::chrono::steady_clock::now();
	draw();
	const auto t1 = std::chrono::steady_clock::now();
	return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// New against old, drawn in turns in this one run, so that a busy machine slows both alike. No absolute time is
// asserted: the Mac this runs on is often loaded to several times its core count.
void testSpeed() {
	std::printf("Shapes speed (new against the old kernel in the same run, not a benchmark)\n");
	ShapesParams typical = shape(6.f, 0.5f, 0.2f);
	typical.round = 0.3f;
	const sf::Scene typicalScene = sf::makeScene(typical, 960, 540);
	double a = 1e30, aOld = 1e30;
	Frame f;
	for (int i = 0; i < 3; ++i) {
		a = std::min(a, timeMs([&] { renderShapes(f, 960, 540, typical); }));
		aOld = std::min(aOld, timeMs([&] { (void)oldRender(typicalScene, 960, 540); }));
	}
	ShapesParams heavy = shape(7.5f, 0.3f, 2.f);
	heavy.round = 0.5f;
	heavy.repeat = 8;
	heavy.copies = 8;
	heavy.melt = 0.5f;
	heavy.aspect = 2.f;
	const sf::Scene tabulated = sf::makeScene(heavy, 960, 540);
	check(tabulated.table != nullptr, "the most expensive combination is tabulated");
	sf::Scene plain = tabulated;
	plain.table.reset();
	double b = 1e30, plainMs = 1e30, oldMs = 1e30;
	for (int i = 0; i < 2; ++i) {
		b = std::min(b, timeMs([&] { renderShapes(f, 960, 540, heavy); }));
		Frame pf;
		plainMs = std::min(plainMs, timeMs([&] { pf = sceneRender(plain, 960, 540); }));
		check(pf.size() == 960u * 540u * 4u, "the picture without the table has the right size");
		oldMs = std::min(oldMs, timeMs([&] { (void)oldRender(plain, 960, 540); }));
	}
	std::printf("  960x540: a rounded hexagon %.1f ms (old kernel %.1f ms)\n", a, aOld);
	std::printf("  960x540: REPEAT 8 with COPIES 8, fractional SIDES, ASPECT 2, ROUND and MELT: %.1f ms (without the table %.1f ms, old kernel %.1f ms)\n",
	            b, plainMs, oldMs);
	checkAtMost(a, 1.5 * aOld + 2.0, "an ordinary shape is no slower than with the old kernel");
	checkAtMost(plainMs, 1.5 * oldMs + 5.0, "the new kernel without the table is no slower than the old kernel");
	checkAtMost(4.0 * b, plainMs, "and the table makes the busiest combination at least four times as quick");
	checkAtMost(4.0 * b, oldMs, "and at least four times as quick as the old kernel");

	// The same with REPEAT off, which tabulates the frame.
	ShapesParams framed = heavy;
	framed.repeat = 1;
	const sf::Scene framedScene = sf::makeScene(framed, 960, 540);
	check(framedScene.table != nullptr, "with REPEAT off the busiest combination is tabulated over the frame");
	double c = 1e30, framedOldMs = 1e30;
	for (int i = 0; i < 2; ++i) {
		c = std::min(c, timeMs([&] { renderShapes(f, 960, 540, framed); }));
		framedOldMs = std::min(framedOldMs, timeMs([&] { (void)oldRender(framedScene, 960, 540); }));
	}
	std::printf("  960x540: the same with REPEAT off: %.1f ms (old kernel %.1f ms)\n", c, framedOldMs);
	checkAtMost(4.0 * c, framedOldMs, "and with REPEAT off the frame table makes it at least four times as quick as the old kernel");
}

// ---- the presets and the registration ----

std::string readFile(const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The text after `"key": "` up to the next quote in a piece of JSON (empty when the key is not there).
std::string jsonText(const std::string& json, const std::string& key) {
	const size_t k = json.find("\"" + key + "\"");
	if (k == std::string::npos) {
		return "";
	}
	const size_t colon = json.find(':', k);
	if (colon == std::string::npos) {
		return "";
	}
	const size_t open = json.find('"', colon);
	if (open == std::string::npos) {
		return "";
	}
	const size_t close = json.find('"', open + 1);
	if (close == std::string::npos) {
		return "";
	}
	return json.substr(open + 1, close - open - 1);
}

// Every {"id": n, "value": v} pair of a preset, in file order.
std::vector<std::pair<int, double>> presetParams(const std::string& json) {
	std::vector<std::pair<int, double>> out;
	size_t at = 0;
	while ((at = json.find("\"id\"", at)) != std::string::npos) {
		const size_t idColon = json.find(':', at);
		const size_t valueAt = json.find("\"value\"", at);
		if (idColon == std::string::npos || valueAt == std::string::npos) {
			break;
		}
		const size_t valueColon = json.find(':', valueAt);
		if (valueColon == std::string::npos) {
			break;
		}
		out.emplace_back(std::atoi(json.c_str() + idColon + 1), std::strtod(json.c_str() + valueColon + 1, nullptr));
		at = valueColon;
	}
	return out;
}

void testPresets() {
	std::printf("Shapes presets and registration\n");
	const std::string plugin = readFile("plugin.json");
	check(!plugin.empty(), "plugin.json can be read from the repository root");
	const std::string version = jsonText(plugin, "version");
	check(plugin.find("\"slug\": \"Shapes\"") != std::string::npos, "plugin.json lists the Shapes module");
	check(plugin.find("VRU\xe2\x80\x93" "21 Shapes") != std::string::npos, "its name is VRU-21 Shapes, with an en dash");
	check(readFile("res/Shapes.svg").find("<svg") != std::string::npos, "the panel file res/Shapes.svg is an SVG");

	std::vector<std::string> files;
	if (DIR* dir = opendir("presets/Shapes")) {
		while (const dirent* entry = readdir(dir)) {
			const std::string name = entry->d_name;
			if (name.size() > 5 && name.compare(name.size() - 5, 5, ".vcvm") == 0) {
				files.push_back("presets/Shapes/" + name);
			}
		}
		closedir(dir);
	}
	std::sort(files.begin(), files.end());
	check(files.size() >= 2, "the module ships at least two presets");

	struct Range {
		const char* name;
		double lo;
		double hi;
	};
	const Range ranges[14] = {
		{"SIDES", 3, 12}, {"ROUND", 0, 1},   {"POINT", 0, 1},   {"SIZE", 0, 1},    {"X", -0.5, 0.5},
		{"Y", -0.5, 0.5}, {"ASPECT", -1, 1}, {"TURN", -0.5, 0.5}, {"REPEAT", 1, 8}, {"COPIES", 1, 8},
		{"RING", 0, 1},   {"MELT", 0, 1},    {"SPREAD", 0, 1},  {"MATTE", 0, 1},
	};
	std::vector<Frame> sheet;
	for (const std::string& path : files) {
		const std::string text = readFile(path);
		check(jsonText(text, "plugin") == "VideoResearchUnit", path + ": made for the plugin slug VideoResearchUnit");
		check(jsonText(text, "model") == "Shapes", path + ": made for the Shapes model");
		check(!version.empty() && jsonText(text, "version") == version, path + ": records the plugin version " + version);
		const std::vector<std::pair<int, double>> params = presetParams(text);
		check(params.size() == 14, path + ": sets the fourteen knobs and switches");
		if (params.size() != 14) {
			continue;
		}
		bool ordered = true, inRange = true;
		for (size_t i = 0; i < 14; ++i) {
			ordered = ordered && params[i].first == static_cast<int>(i);
			if (params[i].second < ranges[i].lo || params[i].second > ranges[i].hi) {
				inRange = false;
				std::printf("  %s: %s = %g is outside %g to %g\n", path.c_str(), ranges[i].name, params[i].second, ranges[i].lo, ranges[i].hi);
			}
		}
		check(ordered, path + ": ids 0 to 13 in panel order");
		check(inRange, path + ": every value is inside its knob's range");
		shapes_math::ShapesControls c;
		c.sides = static_cast<float>(params[0].second);
		c.round = static_cast<float>(params[1].second);
		c.point = static_cast<float>(params[2].second);
		c.size = static_cast<float>(params[3].second);
		c.x = static_cast<float>(params[4].second);
		c.y = static_cast<float>(params[5].second);
		c.aspect = static_cast<float>(params[6].second);
		c.turn = static_cast<float>(params[7].second);
		c.repeat = static_cast<float>(params[8].second);
		c.copies = static_cast<float>(params[9].second);
		c.ring = static_cast<float>(params[10].second);
		c.melt = static_cast<float>(params[11].second);
		c.spread = static_cast<float>(params[12].second);
		c.matte = static_cast<float>(params[13].second);
		const ShapesParams p = shapes_math::shapesParams(c);
		const Frame f = render(p);
		check(isOpaqueGrey(f), path + ": draws a valid grey picture");
		const Measure me = measure(f, kW, kH);
		const double fraction = static_cast<double>(me.inside) / (static_cast<double>(kW) * kH);
		check(fraction > 0.03 && fraction < 0.8, label("%s: the shape fills %.0f%% of the picture", path.c_str(), 100.0 * fraction));
		check(countBlobs(f, kW, kH) >= 1, path + ": has at least one shape in it");
		int partial = 0;
		for (size_t i = 0; i < f.size(); i += 4) {
			partial += (f[i] > 8 && f[i] < 247) ? 1 : 0;
		}
		check(partial > 200, path + ": has a soft edge, not a flat matte");
		sheet.push_back(f);
	}
	if (!sheet.empty()) {
		saveSheet("shapes_presets.png", sheet, 2);
	}
}

// ---- downstream: one grey picture, five jobs ----

// The Wave Shaper's fold: 0 at whole numbers, 1 at halves.
double triangle(double v) {
	return 1.0 - std::fabs(std::fmod(2.0 * std::fabs(v), 2.0) - 1.0);
}

Frame withFullAlpha(Frame f) {
	for (size_t i = 3; i < f.size(); i += 4) {
		f[i] = 255;
	}
	return f;
}

void testDownstream() {
	std::printf("Shapes downstream: the one grey picture through the Keyer, the Wave Shaper and the Colorizer\n");
	VideoGraphRenderer renderer;
	const int w = kBigW, h = kBigH;
	const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
	const double R = 0.5;  // a disc, so every distance is known without the kernel
	const double L = 0.25;
	ShapesParams disc = shape(8.f, static_cast<float>(R), static_cast<float>(L));
	disc.round = 1.f;
	auto distance = [&](size_t px) {
		const int i = static_cast<int>(px % static_cast<size_t>(w)), j = static_cast<int>(px / static_cast<size_t>(w));
		return std::hypot(worldX(i, w, h), worldY(j, h)) - R;
	};
	auto greyOf = [&](double d) { return std::clamp(0.5 - d / (2.0 * L), 0.0, 1.0); };
	auto worstError = [&](const Frame& got, auto expected) {
		double worst = 0.0;
		for (size_t px = 0; px < n; ++px) {
			worst = std::max(worst, std::fabs(static_cast<double>(got[px * 4u]) - expected(distance(px))));
		}
		return worst;
	};
	auto rampPixels = [&](const Frame& f) {
		int count = 0;
		for (int i = 0; i < w; ++i) {
			const int v = grey(f, w, i, h / 2);
			count += (v > 0 && v < 255) ? 1 : 0;
		}
		return count;
	};

	videoBusPublishShapes(kBusA, disc);
	const Frame source = renderer.renderBus(kBusA, w, h);
	check(source.size() == n * 4u, "Shapes draws the source picture");

	// A hard matte: the Keyer at the mid grey with no softness cuts the picture at the shape's edge.
	{
		ShapesParams p = shape(5.f, 0.5f, 0.3f);
		p.round = 0.25f;
		p.point = 0.4f;
		p.turn = 0.07f;
		videoBusPublishShapes(kBusA, p);
		const Frame greyPic = renderer.renderBus(kBusA, w, h);
		KeyerParams hard;
		hard.threshold = 0.5f;
		hard.softness = 0.001f;
		videoBusPublishKeyer(kBusB, kBusA, hard);
		const Frame keyed = renderer.renderBus(kBusB, w, h);
		const Frame flat = render(matte(p), w, h);
		size_t partial = 0, wrongSide = 0, differFromMatte = 0, inside = 0;
		for (size_t px = 0; px < n; ++px) {
			const int g = greyPic[px * 4u], k = keyed[px * 4u], m = flat[px * 4u];
			partial += (k != 0 && k != 255) ? 1 : 0;
			wrongSide += ((g >= 128) != (k == 255)) ? 1 : 0;
			differFromMatte += ((m >= 128) != (k == 255)) ? 1 : 0;
			inside += (k == 255) ? 1 : 0;
		}
		std::printf("  hard matte: %zu inside pixels; %zu partial, %zu on the wrong side of mid grey, %zu unlike the MATTE picture\n", inside, partial, wrongSide, differFromMatte);
		check(partial == 0, "Keyer, no softness: every pixel of the matte is black or white");
		check(wrongSide == 0, "Keyer, no softness: white exactly where Shapes is at or above mid grey");
		checkAtMost(static_cast<double>(differFromMatte), 8.0, "and that is the shape the MATTE switch draws, give or take a pixel on the edge");
		check(inside > n / 50 && inside < n / 2, "and the shape has a sensible size");
		videoBusPublishShapes(kBusA, disc);
	}

	// A soft matte: the Keyer's softness sets how wide the edge is, in units of SPREAD.
	{
		int rampAt[2] = {0, 0};
		int slot = 0;
		for (float softness : {0.5f, 0.25f}) {
			KeyerParams k;
			k.threshold = 0.5f;
			k.softness = softness;
			videoBusPublishKeyer(kBusB, kBusA, k);
			const Frame keyed = renderer.renderBus(kBusB, w, h);
			const double S = softness;
			const double worst = worstError(keyed, [&](double d) { return 255.0 * std::clamp(0.5 - d / (2.0 * L * S), 0.0, 1.0); });
			checkAtMost(worst, 0.5 / S + 1.5, label("Keyer softness %.2f: the matte edge is 2 x SPREAD x softness wide (worst error %.2f levels)", S, worst));
			rampAt[slot] = rampPixels(keyed);
			checkNear(rampAt[slot], 4.0 * L * S / kBigPixel, 3.0, label("Keyer softness %.2f: %.0f pixels of edge across the two sides of the middle row", S, 4.0 * L * S / kBigPixel));
			++slot;
		}
		check(rampAt[0] > rampAt[1] + 10, "a softer Keyer gives a wider edge");
	}

	// An outline: the Wave Shaper's fold turns the ramp into a line along the edge.
	{
		WaveShaperParams ws;
		ws.fold = 1.f;
		videoBusPublishWaveShaper(kBusC, kBusA, ws);
		const Frame outline = renderer.renderBus(kBusC, w, h);
		const double worst = worstError(outline, [&](double d) { return 255.0 * (1.0 - std::fabs(2.0 * greyOf(d) - 1.0)); });
		checkAtMost(worst, 3.5, label("Wave Shaper fold 1: a line of brightness 1 - |distance| / SPREAD along the edge (worst error %.2f levels)", worst));
		check(isOpaqueGrey(outline), "the outline is an opaque grey picture");
		int bright = 0;
		for (int i = 0; i < w; ++i) {
			bright += grey(outline, w, i, h / 2) >= 128 ? 1 : 0;
		}
		checkNear(bright, 2.0 * L / kBigPixel, 3.0, label("the line is half bright over %.0f pixels across the two sides of the middle row", 2.0 * L / kBigPixel));
		check(grey(outline, w, w / 2, h / 2) < 8, "the inside of the shape is dark");
		check(grey(outline, w, 0, 0) < 8, "and so is the far outside");
		check(grey(outline, w, static_cast<int>(w / 2 + R / kBigPixel), h / 2) > 235, "and the line is brightest on the edge");
	}

	// Concentric rings: drive the fold harder and the line repeats.
	{
		WaveShaperParams ws;
		ws.fold = 1.f;
		ws.drive = 4.f;
		videoBusPublishWaveShaper(kBusC, kBusA, ws);
		const Frame rings = renderer.renderBus(kBusC, w, h);
		const double worst = worstError(rings, [&](double d) { return 255.0 * triangle((greyOf(d) - 0.5) * 4.0 + 0.5); });
		checkAtMost(worst, 6.0, label("Wave Shaper fold 1 and drive 4: rings of the distance (worst error %.2f levels)", worst));
		int runs = 0;
		bool dark = false;
		for (int i = w / 2; i < w; ++i) {
			const int v = grey(rings, w, i, h / 2);
			if (v < 64) {
				runs += dark ? 0 : 1;
				dark = true;
			}
			else {
				dark = false;
			}
		}
		check(runs == 4, label("four dark rings across the ramp, at 0.25 and 0.75 SPREAD on each side of the edge (found %d)", runs));
		check(grey(rings, w, w / 2, h / 2) > 235 && grey(rings, w, 0, 0) > 235, "and bright at both ends, inside and far outside");
	}

	// A halo: the Colorizer turns each grey into a colour.
	{
		ColorizerParams cz;
		videoBusPublishColorizer(kBusD, kBusA, cz);
		const Frame halo = renderer.renderBus(kBusD, w, h);
		std::array<int, 256> seen;
		seen.fill(-1);
		bool function = true, opaque = true;
		for (size_t px = 0; px < n; ++px) {
			const int g = source[px * 4u];
			const int rgb = (halo[px * 4u] << 16) | (halo[px * 4u + 1u] << 8) | halo[px * 4u + 2u];
			if (seen[static_cast<size_t>(g)] < 0) {
				seen[static_cast<size_t>(g)] = rgb;
			}
			else if (seen[static_cast<size_t>(g)] != rgb) {
				function = false;
			}
			opaque = opaque && halo[px * 4u + 3u] == 255;
		}
		check(function, "Colorizer: the same grey is always the same colour");
		check(opaque, "Colorizer: the picture is opaque");
		const unsigned char* corner = halo.data();
		check(corner[2] > corner[0] && corner[2] > corner[1] && corner[2] >= 40 && corner[2] <= 90, "Colorizer: the far outside is the dark end of the ramp, a deep blue");
		// The brightest greys of the ramp, just short of white, take the bright end of the palette, a yellow.
		const int bright = seen[250];
		check(bright >= 0 && ((bright >> 16) & 255) >= 240 && ((bright >> 8) & 255) >= 200 && (bright & 255) <= 60, "Colorizer: grey 250, near the shape's heart, is the bright end, a yellow");
		int previous = -1, wrongWay = 0, distinct = 0;
		for (size_t g = 0; g < 256; ++g) {
			if (seen[g] < 0) {
				continue;
			}
			++distinct;
			const int top = std::max({(seen[g] >> 16) & 255, (seen[g] >> 8) & 255, seen[g] & 255});
			wrongWay += (top < previous - 2) ? 1 : 0;
			previous = std::max(previous, top);
		}
		check(distinct > 100, label("the disc's edge uses %d different greys, and so %d colours", distinct, distinct));
		check(wrongWay == 0, "Colorizer: brighter grey never gives a darker colour (grey 0 to 255, so no dark hole at white)");
		// White sits at the very top of the palette. It does not come round to the first colour, so the heart of a halo
		// that is white all the way in keeps the bright end instead of showing the outside colour.
		const int white = seen[255];
		check(white >= 0, "the disc's heart reaches pure white");
		check(white >= 0 && white != seen[0] && ((white >> 16) & 255) >= 240 && ((white >> 8) & 255) >= 200 && (white & 255) <= 60,
			"Colorizer: white (grey 255) is the bright end, a yellow, and not the colour of grey 0");
		ColorizerParams banded;
		banded.bands = 4;
		videoBusPublishColorizer(kBusD, kBusA, banded);
		const Frame bands = renderer.renderBus(kBusD, w, h);
		std::array<int, 4096> colours;
		colours.fill(0);
		int uniqueColours = 0;
		for (size_t px = 0; px < n; ++px) {
			const int key = ((bands[px * 4u] >> 4) << 8) | ((bands[px * 4u + 1u] >> 4) << 4) | (bands[px * 4u + 2u] >> 4);
			if (colours[static_cast<size_t>(key)]++ == 0) {
				++uniqueColours;
			}
		}
		check(uniqueColours <= 12, label("Colorizer with 4 bands: the halo is a few flat rings (%d colours)", uniqueColours));
	}

	// The MATTE switch skips all of that when a plain white shape on black is all that is wanted.
	{
		ShapesParams m = matte(disc);
		videoBusPublishShapes(kBusA, m);
		const Frame flat = renderer.renderBus(kBusA, w, h);
		size_t whites = 0, blacks = 0, between = 0;
		for (size_t px = 0; px < n; ++px) {
			whites += flat[px * 4u] == 255 ? 1 : 0;
			blacks += flat[px * 4u] == 0 ? 1 : 0;
			between += (flat[px * 4u] > 0 && flat[px * 4u] < 255) ? 1 : 0;
		}
		// The grey is the coverage of each pixel, so its sum is the disc's area; white is the pixels wholly inside.
		double coverage = 0.0;
		for (size_t px = 0; px < n; ++px) {
			coverage += flat[px * 4u] / 255.0;
		}
		const double discArea = kPi * R * R / (kBigPixel * kBigPixel);
		checkNear(coverage, discArea, 0.003 * discArea, "MATTE: the white of the disc adds up to its area, to a third of a percent");
		check(whites + blacks + between == n && between < 3 * static_cast<size_t>(2.0 * kPi * R / kBigPixel), "MATTE: black and white with only a one-pixel edge between");
		check(whites > discArea * 0.97 && whites < discArea, label("MATTE: %zu pixels are wholly white, just under the disc's %.0f", whites, discArea));
	}

	videoBusClear(kBusD);
	videoBusClear(kBusC);
	videoBusClear(kBusB);
	videoBusClear(kBusA);
}

// ---- the proof sheets that need the other modules ----

void testProofSheets() {
	std::printf("Shapes proof sheets: placement, edge softness and the chain into other modules\n");
	// Placement, one row each, low to high from the left: SIZE, X and Y, TURN, ASPECT.
	{
		std::vector<Frame> sheet;
		for (float size : {0.15f, 0.35f, 0.6f}) {
			sheet.push_back(sheetPanel(shape(5.f, size, 0.06f)));
		}
		const float xs[3] = {-0.3f, 0.f, 0.3f};
		const float ys[3] = {0.2f, 0.f, -0.2f};
		for (int i = 0; i < 3; ++i) {
			ShapesParams p = shape(5.f, 0.2f, 0.06f);
			p.x = xs[i];
			p.y = ys[i];
			sheet.push_back(sheetPanel(p));
		}
		for (float turn : {0.f, 0.05f, 0.125f}) {
			ShapesParams p = shape(5.f, 0.35f, 0.06f);
			p.turn = turn;
			sheet.push_back(sheetPanel(p));
		}
		for (float ratio : {0.5f, 1.f, 2.f}) {
			ShapesParams p = shape(5.f, 0.35f, 0.06f);
			p.aspect = ratio;
			sheet.push_back(sheetPanel(p));
		}
		saveSheet("shapes_placement.png", sheet, 3, kSheetW, kSheetH);
	}
	// Edge softness: SPREAD at five settings, then the same shape with MATTE on.
	{
		std::vector<Frame> sheet;
		ShapesParams p = shape(6.f, 0.4f);
		p.round = 0.15f;
		for (float spread : {0.02f, 0.06f, 0.15f, 0.4f, 1.0f}) {
			p.spread = spread;
			sheet.push_back(sheetPanel(p));
		}
		sheet.push_back(sheetPanel(matte(p)));
		saveSheet("shapes_spread.png", sheet, 3, kSheetW, kSheetH);
	}
	// The chain: grey, MATTE, hard matte, soft matte, outline, rings, halo, banded halo.
	{
		VideoGraphRenderer renderer;
		ShapesParams p = shape(5.f, 0.5f, 0.3f);
		p.round = 0.25f;
		p.point = 0.4f;
		p.turn = 0.07f;
		videoBusPublishShapes(kBusA, p);
		std::vector<Frame> sheet;
		sheet.push_back(renderer.renderBus(kBusA, kW, kH));
		sheet.push_back(render(matte(p)));
		KeyerParams hard;
		hard.softness = 0.001f;
		videoBusPublishKeyer(kBusB, kBusA, hard);
		sheet.push_back(withFullAlpha(renderer.renderBus(kBusB, kW, kH)));
		KeyerParams soft;
		soft.softness = 0.5f;
		videoBusPublishKeyer(kBusB, kBusA, soft);
		sheet.push_back(withFullAlpha(renderer.renderBus(kBusB, kW, kH)));
		WaveShaperParams line;
		line.fold = 1.f;
		videoBusPublishWaveShaper(kBusC, kBusA, line);
		sheet.push_back(renderer.renderBus(kBusC, kW, kH));
		WaveShaperParams rings;
		rings.fold = 1.f;
		rings.drive = 4.f;
		videoBusPublishWaveShaper(kBusC, kBusA, rings);
		sheet.push_back(renderer.renderBus(kBusC, kW, kH));
		ColorizerParams halo;
		videoBusPublishColorizer(kBusD, kBusA, halo);
		sheet.push_back(renderer.renderBus(kBusD, kW, kH));
		halo.bands = 4;
		videoBusPublishColorizer(kBusD, kBusA, halo);
		sheet.push_back(renderer.renderBus(kBusD, kW, kH));
		videoBusClear(kBusD);
		videoBusClear(kBusC);
		videoBusClear(kBusB);
		videoBusClear(kBusA);
		check(sheet.size() == 8, "all eight chain pictures were drawn");
		saveSheet("shapes_chain.png", sheet, 4);
	}
}

}  // namespace

int main(int argc, char** argv) {
	if (argc > 1) {
		gProofDir = argv[1];
	}
	mkdir("build", 0755);
	mkdir(gProofDir.c_str(), 0755);

	testBus();
	testControlMap();
	testGreyPicture();
	testMatte();
	testDeterminism();
	testPolygons();
	testFractionalSides();
	testRoundAndPoint();
	testPlacement();
	testCvDriven();
	testRepeat();
	testCopies();
	testMelt();
	testSeams();
	testCullingAgainstBruteForce();
	testTableAgainstOldKernel();
	testBusyScenesAgainstOldKernel();
	testFrameTableAgainstOldKernel();
	testExactDistance();
	testSmoothness();
	testExtremes();
	testSpeed();
	testPresets();
	testDownstream();
	testProofSheets();

	std::printf("\n%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
