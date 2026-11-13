#pragma once

#include "video_engine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

// Shapes (VRU-21) picture maths. Pure float maths on ShapesParams, no Rack types, shared by renderShapes()
// in video_engine.cpp and by the tests. See docs/architecture.md, "Shapes".
//
// The picture is a signed distance field turned into grey: grey = 0.5 - d / (2 L), clamped to 0..1, where d is
// the distance from the shape's edge (negative inside) and L is SPREAD. Mid grey is the edge, white is L inside
// it, black is L outside it. MATTE swaps L for half a pixel, which gives a white shape on black with a
// one-pixel antialiased edge.
//
// Units. World coordinates have y up and 1.0 is half the picture height, so the picture spans x in
// [-A, A] (A is width / height) and y in [-1, 1]. REPEAT cuts the picture into square cells and draws the
// whole composition (the shape, or the ring of copies) once in each cell. Shapes are laid out in composition
// units, where the cell spans [-1, 1] on both axes; with REPEAT off a composition unit is one world unit.
namespace shapes_field {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kMinSides = 3;
constexpr int kMaxSides = 12;
constexpr int kMaxCopies = 8;
constexpr int kMaxRepeat = 8;
constexpr int kMaxTileRadius = 3;
constexpr int kMaxTiles = (2 * kMaxTileRadius + 1) * (2 * kMaxTileRadius + 1);
// POINT at its maximum pulls the middle of each side in to this fraction of the corner radius.
constexpr float kInnerRatio = 0.38f;
// MELT at its maximum smooths neighbouring shapes together over this many composition units.
constexpr float kMeltSoftness = 0.28f;  // log-sum-exp width at full MELT, composition units
constexpr float kMeltTail = 8.f;        // a shape this many widths past the nearest adds under 0.04% and is skipped
constexpr float kBig = 1e6f;

// Angle in radians of (x, y), absolute error under 3e-4. The shape field only uses it to pick which side
// is nearest, and the field is continuous across the boundary between two sides, so the error is harmless.
inline float fastAtan2(float y, float x) {
	const float ax = std::fabs(x);
	const float ay = std::fabs(y);
	const float hi = std::max(ax, ay);
	if (hi <= 0.f) {
		return 0.f;
	}
	const float lo = std::min(ax, ay);
	const float a = lo / hi;
	const float s = a * a;
	float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
	if (ay > ax) {
		r = 1.57079637f - r;
	}
	if (x < 0.f) {
		r = 3.14159274f - r;
	}
	return y < 0.f ? -r : r;
}

// Cosine and sine of the direction of each side's outward normal for an m-sided shape. Side 0 faces down, so
// odd shapes stand on a flat side with a corner at the top and even shapes have flat tops and bottoms.
struct SideTable {
	float cs[kMaxSides];
	float sn[kMaxSides];
};

inline const SideTable& sideTable(int m) {
	static const std::array<SideTable, kMaxSides + 1> tables = [] {
		std::array<SideTable, kMaxSides + 1> t{};
		const double pi = 3.14159265358979323846;
		for (int mm = kMinSides; mm <= kMaxSides; ++mm) {
			for (int k = 0; k < mm; ++k) {
				const double phi = -0.5 * pi + 2.0 * pi * k / mm;
				t[static_cast<size_t>(mm)].cs[k] = static_cast<float>(std::cos(phi));
				t[static_cast<size_t>(mm)].sn[k] = static_cast<float>(std::sin(phi));
			}
		}
		return t;
	}();
	return tables[static_cast<size_t>(std::clamp(m, kMinSides, kMaxSides))];
}

// One regular polygon or star, reduced to the piece of outline that repeats round it. In the frame of a side (its
// normal along +x, mirrored so y >= 0) the outline runs from the middle of the side V = (rho, 0) to the corner K.
// A polygon has rho equal to the apothem; POINT pulls rho in, which makes it a star.
//
// ROUND keeps every edge line where it is and fillets each corner with a circle tangent to both of its edges.
// The circle's centre C is the corner shrunk towards the middle by 1 - ROUND, and its radius r is ROUND times the
// distance of the edge lines from the middle, so ROUND = 1 is the circle that touches every edge line. The piece
// is then a straight run from V to the point T where the arc starts, and the arc round C out to the apex A on the
// line to the corner. When the arcs of two neighbouring corners overlap, the straight run is gone and the arc
// starts where it crosses the axis, at the sharp inner corner X where it meets its mirror image.
struct Poly {
	const SideTable* table = nullptr;
	int m = 4;
	float stepsPerRad = 0.f;  // m / 2 pi
	float vix = 0.f;          // V.x
	float ex = 0.f;           // the straight run, from V to T (to the corner when the corners are sharp)
	float ey = 0.f;
	float invLen2 = 0.f;      // 1 / |e|^2, 0 when there is no straight run
	float r = 0.f;            // radius of the rounded corners, 0 when they are sharp
	float cx = 0.f;           // centre of the arc
	float cy = 0.f;
	float startX = 0.f;       // where the arc starts: T, or X on the axis when the straight run is gone
	float startY = 0.f;
	float u0x = 1.f;          // unit vectors from the centre of the arc to its start and to its apex
	float u0y = 0.f;
	float u1x = 1.f;
	float u1y = 0.f;
	float inradius = 0.f;     // a disc this size about the middle is always inside the shape
	float outer = 0.f;        // a disc this size about the middle always contains the shape
};

inline Poly makePoly(int m, float apothem, float point, float round) {
	Poly p;
	m = std::clamp(m, kMinSides, kMaxSides);
	p.m = m;
	p.table = &sideTable(m);
	p.stepsPerRad = static_cast<float>(m) / (2.f * kPi);
	const float beta = kPi / static_cast<float>(m);
	const float cb = std::cos(beta);
	const float sb = std::sin(beta);
	const float corner = apothem / cb;
	const float rho = apothem + (kInnerRatio * corner - apothem) * point;
	const float vox = corner * cb;
	const float voy = corner * sb;
	const float ex0 = vox - rho;
	const float len = std::sqrt(ex0 * ex0 + voy * voy);
	// Every edge line of the polygon or star is this far from the middle.
	const float c = len > 1e-9f ? rho * voy / len : rho;
	p.vix = rho;
	p.ex = ex0;
	p.ey = voy;
	p.invLen2 = len > 1e-9f ? 1.f / (len * len) : 0.f;
	p.u1x = cb;
	p.u1y = sb;
	p.inradius = c;
	p.outer = corner;
	if (round <= 0.f || len <= 1e-9f) {
		return p;
	}
	const float keep = 1.f - round;
	p.r = round * c;
	p.cx = keep * vox;
	p.cy = keep * voy;
	p.outer = keep * corner + p.r;
	// The circle touches the edge a distance r from its centre, along the edge's outward normal.
	const float nx = voy / len;
	const float ny = -ex0 / len;
	const float tx = p.cx + p.r * nx;
	const float ty = p.cy + p.r * ny;
	const float run2 = (tx - rho) * (tx - rho) + ty * ty;
	if (ty > 0.f && run2 > 1e-12f) {
		p.ex = tx - rho;
		p.ey = ty;
		p.invLen2 = 1.f / run2;
		p.startX = tx;
		p.startY = ty;
		p.u0x = nx;
		p.u0y = ny;
	}
	else {
		// The circle reaches the axis before the edge does: it crosses at X, the outer one of its two crossings.
		const float half = std::sqrt(std::max(p.r * p.r - p.cy * p.cy, 0.f));
		p.invLen2 = 0.f;
		p.startX = p.cx + half;
		p.startY = 0.f;
		p.u0x = half / p.r;
		p.u0y = -p.cy / p.r;
	}
	return p;
}

// Signed distance to the shape in the frame of one side, negative inside. (qx, qy) are the point's coordinates
// along the side's normal and along the side; the sign of qy does not matter, because the outline is mirrored
// about the normal. The nearest point of the whole outline is always on the piece of this side for a point in
// this side's wedge, so the distance to the piece is the distance to the shape, exactly.
inline float sdWedge(const Poly& s, float qx, float qy) {
	qy = std::fabs(qy);
	float best = kBig;  // squared distance to the nearest point of the piece
	bool inside = false;
	if (s.invLen2 > 0.f) {
		const float wx = qx - s.vix;
		const float t = std::clamp((wx * s.ex + qy * s.ey) * s.invLen2, 0.f, 1.f);
		const float dx = wx - t * s.ex;
		const float dy = qy - t * s.ey;
		best = dx * dx + dy * dy;
		inside = s.ex * qy - s.ey * wx > 0.f;
	}
	else if (s.r <= 0.f) {
		// A shape of no size: the point it shrank to, which has no inside.
		const float wx = qx - s.vix;
		best = wx * wx + qy * qy;
	}
	if (s.r > 0.f) {
		const float wx = qx - s.cx;
		const float wy = qy - s.cy;
		const bool afterStart = s.u0x * wy - s.u0y * wx >= 0.f;
		const bool beforeApex = wx * s.u1y - wy * s.u1x >= 0.f;
		if (afterStart && beforeApex) {
			// The nearest point of the circle is on the arc.
			const float d = std::sqrt(wx * wx + wy * wy) - s.r;
			if (d * d < best) {
				best = d * d;
				inside = d < 0.f;
			}
		}
		else if (!afterStart) {
			// Before the arc: with a straight run the nearest point is its end, which the run already covered.
			// Without one it is the sharp inner corner, and everything that is nearest to it is inside.
			if (s.invLen2 <= 0.f) {
				const float dx = qx - s.startX;
				const float dy = qy - s.startY;
				const float d2 = dx * dx + dy * dy;
				if (d2 < best) {
					best = d2;
					inside = true;
				}
			}
		}
		else {
			// Past the apex, which only the rounding in picking the side can cause.
			const float dx = qx - (s.cx + s.r * s.u1x);
			const float dy = qy - (s.cy + s.r * s.u1y);
			const float d2 = dx * dx + dy * dy;
			if (d2 < best) {
				best = d2;
				inside = wx * wx + wy * wy < s.r * s.r;
			}
		}
	}
	const float dist = std::sqrt(best);
	return inside ? -dist : dist;
}

// Signed distance from (px, py) to the shape, negative inside.
inline float sdPoly(const Poly& s, float px, float py) {
	const float theta = fastAtan2(py, px);
	int k = static_cast<int>(std::floor((theta + 0.5f * kPi) * s.stepsPerRad + 0.5f));
	if (k < 0) {
		k += s.m;
	}
	else if (k >= s.m) {
		k -= s.m;
	}
	const float cs = s.table->cs[k];
	const float sn = s.table->sn[k];
	return sdWedge(s, cs * px + sn * py, -sn * px + cs * py);
}

// The most steps one rounded corner's half arc is cut into.
constexpr int kMaxArcSteps = 160;

// One straight stretch of the outline of a stretched shape.
struct Segment {
	float ax = 0.f;       // start
	float ay = 0.f;
	float ex = 0.f;       // end minus start
	float ey = 0.f;
	float invLen2 = 0.f;  // 1 / |e|^2, 0 for a point
	float ux = 0.f;       // unit vector from the middle to the start
	float uy = 0.f;
};

// The outline of a stretched shape as a closed polyline, counterclockwise, in the shape's own frame: after TURN,
// about its own middle, with the stretch already applied. Without ROUND it is the polygon or star itself, to the
// last bit. With ROUND the arcs are cut into chords that stray from them by at most a tenth of a pixel, which is
// what lets every distance be the true one in the picture.
//
// Every ray from the middle crosses the outline once (radial), so a point finds the segment it faces straight
// away, which gives it its sign, and the nearest segment is then looked for outwards from there.
struct Polyline {
	std::vector<Segment> segment;
	std::vector<int> lut;   // polar angle -> the segment facing it
	float base = 0.f;       // polar angle of the first vertex
	float lutScale = 0.f;   // lut bins per radian
	bool radial = false;
	float boxX = 0.f;       // the polyline lies inside |x| <= boxX and |y| <= boxY
	float boxY = 0.f;
	float reach = 0.f;      // and inside this distance of the middle
};

// The outline of the polygon or star stretched by sx across and sy up, as a closed polyline. tol is how far from
// the true outline it may stray, in the same units as the shape.
inline Polyline makePolyline(const Poly& p, float sx, float sy, float tol) {
	Polyline o;
	const int m = p.m;
	const float beta = kPi / static_cast<float>(m);
	// Half of one side in the side's own frame (x along the side's normal, y >= 0 along the side): from the axis
	// to the line to the corner.
	std::vector<float> hx;
	std::vector<float> hy;
	if (p.r <= 0.f) {
		hx = {p.vix, p.vix + p.ex};
		hy = {0.f, p.ey};
	}
	else {
		if (p.invLen2 > 0.f) {
			hx.push_back(p.vix);
			hy.push_back(0.f);
		}
		hx.push_back(p.startX);
		hy.push_back(p.startY);
		// A chord over a turn of d radians strays by at most r d^2 / 8, and the stretch can lengthen that by sx or sy.
		const float psi0 = std::atan2(p.u0y, p.u0x);
		const float span = beta - psi0;
		const float bend = p.r * std::max(sx, sy);
		const int steps = std::clamp(static_cast<int>(std::ceil(span * std::sqrt(bend / (8.f * std::max(tol, 1e-9f))))), 1, kMaxArcSteps);
		for (int j = 1; j < steps; ++j) {
			const float a = psi0 + span * static_cast<float>(j) / static_cast<float>(steps);
			hx.push_back(p.cx + p.r * std::cos(a));
			hy.push_back(p.cy + p.r * std::sin(a));
		}
		hx.push_back(p.cx + p.r * p.u1x);
		hy.push_back(p.cy + p.r * p.u1y);
	}
	const int n = static_cast<int>(hx.size());

	// All sides, counterclockwise. Side k faces the direction table[k]; the mirrored half comes first (from the
	// corner before the side to its middle), then the half on the other side of the middle. A side's last point
	// is the next side's first, so it is written once.
	std::vector<float> vx;
	std::vector<float> vy;
	vx.reserve(static_cast<size_t>(2 * m * n));
	vy.reserve(static_cast<size_t>(2 * m * n));
	for (int k = 0; k < m; ++k) {
		const float cs = p.table->cs[k];
		const float sn = p.table->sn[k];
		auto push = [&](float a, float b) {
			vx.push_back(sx * (a * cs - b * sn));
			vy.push_back(sy * (a * sn + b * cs));
		};
		for (int i = n - 1; i >= 1; --i) {
			push(hx[static_cast<size_t>(i)], -hy[static_cast<size_t>(i)]);
		}
		push(hx[0], 0.f);
		for (int i = 1; i <= n - 2; ++i) {
			push(hx[static_cast<size_t>(i)], hy[static_cast<size_t>(i)]);
		}
	}

	// Drop points that lie on the straight line between their neighbours, such as the middle of a plain side.
	const int total = static_cast<int>(vx.size());
	std::vector<float> kx;
	std::vector<float> ky;
	kx.reserve(vx.size());
	ky.reserve(vy.size());
	for (int i = 0; i < total; ++i) {
		const size_t a = static_cast<size_t>((i + total - 1) % total);
		const size_t b = static_cast<size_t>(i);
		const size_t c = static_cast<size_t>((i + 1) % total);
		const float cx = vx[c] - vx[a];
		const float cy = vy[c] - vy[a];
		const float bx = vx[b] - vx[a];
		const float by = vy[b] - vy[a];
		const float len2 = cx * cx + cy * cy;
		bool drop = false;
		if (len2 > 1e-12f) {
			const float cross = cx * by - cy * bx;
			const float along = cx * bx + cy * by;
			drop = cross * cross <= 1e-10f * len2 && along >= 0.f && along <= len2;
		}
		if (!drop) {
			kx.push_back(vx[b]);
			ky.push_back(vy[b]);
		}
	}

	const size_t count = kx.size();
	o.segment.resize(count);
	bool radial = count >= 3;
	float turned = 0.f;
	std::vector<float> angle(count, 0.f);  // polar angle of each vertex, counted on from the first
	for (size_t i = 0; i < count; ++i) {
		const size_t j = (i + 1) % count;
		Segment& s = o.segment[i];
		s.ax = kx[i];
		s.ay = ky[i];
		s.ex = kx[j] - kx[i];
		s.ey = ky[j] - ky[i];
		const float len2 = s.ex * s.ex + s.ey * s.ey;
		s.invLen2 = len2 > 1e-20f ? 1.f / len2 : 0.f;
		const float rad = std::sqrt(kx[i] * kx[i] + ky[i] * ky[i]);
		o.boxX = std::max(o.boxX, std::fabs(kx[i]));
		o.boxY = std::max(o.boxY, std::fabs(ky[i]));
		o.reach = std::max(o.reach, rad);
		if (rad > 1e-9f) {
			s.ux = kx[i] / rad;
			s.uy = ky[i] / rad;
		}
		else {
			radial = false;
		}
		const float cross = kx[i] * ky[j] - ky[i] * kx[j];
		const float dot = kx[i] * kx[j] + ky[i] * ky[j];
		if (!(cross > 0.f)) {
			radial = false;
		}
		const float step = std::atan2(cross, dot);
		turned += step;
		if (j != 0) {
			angle[j] = angle[i] + step;
		}
	}
	if (radial && std::fabs(turned - 2.f * kPi) < 1e-2f) {
		o.radial = true;
		o.base = std::atan2(ky[0], kx[0]);
		const int bins = std::max(32, 4 * static_cast<int>(count));
		o.lutScale = static_cast<float>(bins) / (2.f * kPi);
		o.lut.resize(static_cast<size_t>(bins));
		size_t at = 0;
		for (int b = 0; b < bins; ++b) {
			const float a = static_cast<float>(b) / o.lutScale;
			while (at + 1 < count && angle[at + 1] <= a) {
				++at;
			}
			o.lut[static_cast<size_t>(b)] = static_cast<int>(at);
		}
	}
	return o;
}

inline float segmentDistance2(const Segment& s, float px, float py) {
	const float wx = px - s.ax;
	const float wy = py - s.ay;
	const float t = std::clamp((wx * s.ex + wy * s.ey) * s.invLen2, 0.f, 1.f);
	const float vx = wx - t * s.ex;
	const float vy = wy - t * s.ey;
	return vx * vx + vy * vy;
}

// Distance from (px, py), in the shape's own frame, to every segment in turn, negative inside. The sign is the
// even-odd count of segments a ray going right crosses. This is the reference; polylineDistance gives the same
// answer faster whenever the outline is radial.
inline float polylineDistanceBrute(const Polyline& o, float px, float py) {
	const Segment* seg = o.segment.data();
	const int count = static_cast<int>(o.segment.size());
	float best = kBig;
	bool in = false;
	for (int i = 0; i < count; ++i) {
		const Segment& s = seg[i];
		best = std::min(best, segmentDistance2(s, px, py));
		const float wx = px - s.ax;
		const float wy = py - s.ay;
		const bool c1 = py >= s.ay;
		const bool c2 = py < s.ay + s.ey;
		const bool c3 = s.ex * wy > s.ey * wx;
		in ^= (c1 && c2 && c3) || (!c1 && !c2 && !c3);
	}
	const float dist = std::sqrt(best);
	return in ? -dist : dist;
}

// How near, squared, a point at distance rho from the middle can be to an outline point that lies at angle a or
// more from it, when no outline point is farther than `reach` from the middle. dp = rho cos a and cr = rho sin a
// are the point's components along and across the ray at angle a. The outline point is at radius r in 0..reach, and
// its squared distance rho^2 + r^2 - 2 r dp is least at r = dp (rho^2 - dp^2 = cr^2), which is out of reach once dp
// is above reach (then the least is at reach), and below zero for a right angle or more (then at the middle).
inline float angleBound2(float dp, float cr, float rho2, float reach) {
	if (dp <= 0.f) {
		return rho2;
	}
	if (dp >= reach) {
		return rho2 + reach * (reach - 2.f * dp);
	}
	return cr * cr;
}

// Distance from (px, py) to the closed polyline, negative inside. A radial outline puts the middle on the left of
// every segment, so a point is inside exactly when it is on the left of the segment it faces. The nearest segment
// is looked for outwards from that one in both directions, and each direction stops as soon as every segment
// beyond is farther than the best found (see angleBound2).
inline float polylineDistance(const Polyline& o, float px, float py) {
	if (!o.radial) {
		return polylineDistanceBrute(o, px, py);
	}
	const Segment* seg = o.segment.data();
	const int n = static_cast<int>(o.segment.size());
	const float rho2 = px * px + py * py;
	float t = fastAtan2(py, px) - o.base;
	if (t < 0.f) {
		t += 2.f * kPi;
	}
	int i = o.lut[static_cast<size_t>(std::min(static_cast<int>(t * o.lutScale), static_cast<int>(o.lut.size()) - 1))];
	// The table is exact, the angle is not, so settle on the segment whose rays hold the point.
	for (int guard = 0; guard < 16; ++guard) {
		const int nx = i + 1 == n ? 0 : i + 1;
		if (seg[nx].ux * py - seg[nx].uy * px >= 0.f && seg[nx].ux * px + seg[nx].uy * py > 0.f) {
			i = nx;
			continue;
		}
		if (seg[i].ux * py - seg[i].uy * px < 0.f && seg[i].ux * px + seg[i].uy * py > 0.f) {
			i = i == 0 ? n - 1 : i - 1;
			continue;
		}
		break;
	}
	const Segment& facing = seg[i];
	const bool inside = facing.ex * (py - facing.ay) - facing.ey * (px - facing.ax) > 0.f;
	float best = segmentDistance2(facing, px, py);
	int f = i + 1 == n ? 0 : i + 1;
	int b = i == 0 ? n - 1 : i - 1;
	int left = n - 1;
	bool goingOn = true;
	bool backOn = true;
	while (left > 0 && (goingOn || backOn)) {
		if (goingOn) {
			const Segment& s = seg[f];
			const float bound = angleBound2(s.ux * px + s.uy * py, s.ux * py - s.uy * px, rho2, o.reach);
			if (bound >= best) {
				goingOn = false;
			}
			else {
				best = std::min(best, segmentDistance2(s, px, py));
				f = f + 1 == n ? 0 : f + 1;
				--left;
			}
		}
		if (backOn && left > 0) {
			const Segment& e = seg[b + 1 == n ? 0 : b + 1];  // the end of segment b is the start of the next
			const float bound = angleBound2(e.ux * px + e.uy * py, e.ux * py - e.uy * px, rho2, o.reach);
			if (bound >= best) {
				backOn = false;
			}
			else {
				best = std::min(best, segmentDistance2(seg[b], px, py));
				b = b == 0 ? n - 1 : b - 1;
				--left;
			}
		}
	}
	const float dist = std::sqrt(best);
	return inside ? -dist : dist;
}

// The shape itself: one polygon, or a blend of two when SIDES is between whole numbers.
struct ShapeSdf {
	Poly a;
	Poly b;
	Polyline oa;  // the same two as outlines of the stretched shape
	Polyline ob;
	bool two = false;
	float mix = 0.f;  // weight of b
};

inline float sdShape(const ShapeSdf& s, float px, float py) {
	const float d = sdPoly(s.a, px, py);
	if (!s.two) {
		return d;
	}
	return d + (sdPoly(s.b, px, py) - d) * s.mix;
}

inline float sdShapeStretched(const ShapeSdf& s, float px, float py) {
	const float d = polylineDistance(s.oa, px, py);
	if (!s.two) {
		return d;
	}
	return d + (polylineDistance(s.ob, px, py) - d) * s.mix;
}

// MELT joins every copy and every cell with a soft union, d = -k ln(sum of exp(-d_i / k)). Unlike a pairwise smooth
// minimum it does not depend on the order the shapes arrive in, so the ring of copies stays even and the seams
// between lattice cells stay invisible. Two shapes whose edges are less than 1.39 k apart fuse. The sum is kept
// relative to the nearest distance so it cannot overflow.
struct Melt {
	float nearest = kBig;
	float sum = 0.f;
};

inline void meltAdd(Melt& m, float d, float invK) {
	if (d >= m.nearest) {
		m.sum += std::exp(-(d - m.nearest) * invK);
	}
	else {
		m.sum = m.sum * std::exp(-(m.nearest - d) * invK) + 1.f;
		m.nearest = d;
	}
}

inline float meltValue(const Melt& m, float k) {
	return m.sum > 1.f ? m.nearest - k * std::log(m.sum) : m.nearest;
}

// A table of the field over one lattice cell, for scenes too busy to work out for every pixel. The cell is cut into
// a quadtree of squares, each judged from nine samples of the long way's field (corners, edge middles, middle).
// A square that is flat black or white is settled by the field being 1-Lipschitz: black when even the floor under
// the field (see fieldFolded) stays above the ramp all the way to the farthest point from a sample, white when the
// field itself stays below it. A square the field crosses smoothly keeps its nine sample values and is read back as
// the smooth curved surface through them (the quadratic in each direction that passes through its three samples
// along that direction), but only when all nine samples saw every outline that could matter and the field lies on
// that surface, to a fraction of a grey level, at every node of a five by five grid across it. A curved surface
// follows the bend of the field near an outline and in MELT's blends, so such a square can be several times wider
// than a flat blend between four corners could be, and needs far fewer samples. Where the flat blend passes the
// same test it is kept instead, because it is quicker to read back. That last test is checked, not
// proved: a trough or a crease narrower than the grid's spacing could hide between its nodes, so
// tests/shapes_test.cpp compares the picture with the long way's on a few hundred random scenes, every control at
// both ends of its range included. A square that is none of these is cut in four, and the smallest stays "exact"
// and is worked out the long way, per pixel. makeScene() builds the table, shade() reads it, so the picture does
// not depend on how the frame is cut into threads.
struct FieldTable {
	enum Kind : uint8_t { kExact = 0, kSmooth = 1, kBlack = 2, kWhite = 3, kLinear = 4 };
	struct Leaf {
		float x0 = 0.f;  // lower corner, composition units
		float y0 = 0.f;
		float inv = 0.f;  // 1 / side
		float c[9] = {};  // the smooth surface through the nine samples (see smoothCoefficients()), or the four corners
		uint8_t kind = kExact;
	};
	static constexpr int kNodes = 256;        // node intervals across the cell
	static constexpr int kSide = kNodes / 2;  // lookup entries per side (the smallest square is two intervals)
	std::vector<uint16_t> entry;              // kSide * kSide, index of the leaf covering each entry
	std::vector<Leaf> leaf;
	// The square the table covers, composition units: the home cell, [-1, 1] on both axes, when REPEAT is on, and
	// a square around the whole frame when it is off (see buildFieldTable).
	float left = -1.f;
	float bottom = -1.f;
	float span = 2.f;
	float lookup = 0.5f * kSide;  // kSide / span
};

// Everything the per-pixel loop needs, worked out once per frame.
struct Scene {
	int width = 0;
	int height = 0;
	float pixel = 0.f;       // world size of one pixel
	bool tiled = false;      // REPEAT above 1
	float s = 1.f;           // world size of one composition unit
	float originX = 0.f;     // world position of the composition origin (and the lattice origin)
	float originY = 0.f;
	float pitch = 2.f;       // world spacing of the lattice, 2 s
	int tileCount = 1;
	std::array<signed char, kMaxTiles> tileI{};
	std::array<signed char, kMaxTiles> tileJ{};
	int copies = 1;
	std::array<float, kMaxCopies> cx{};  // copy centres, composition units from the origin
	std::array<float, kMaxCopies> cy{};
	std::array<float, kMaxCopies> rc{};  // cosine and sine of each copy's turn
	std::array<float, kMaxCopies> rs{};
	bool stretched = false;  // ASPECT is not 1: the shape is drawn from its polyline
	ShapeSdf shape;
	float cullOffset = 0.f;  // a copy at distance r from the centre has field >= r - cullOffset
	float boxX = 0.f;        // a stretched copy lies inside |x| <= boxX, |y| <= boxY in its own frame
	float boxY = 0.f;
	float tileReach = 0.f;   // farthest a copy's content can lie from its tile's origin
	float km = 0.f;          // MELT softness, composition units (0 is a plain union)
	float invKm = 0.f;
	float tail = 0.f;        // how far past the nearest a shape can lie and still matter
	float lc = 0.1f;         // grey ramp half-width, composition units
	float invTwoLc = 5.f;
	float deepRadius2 = -1.f;
	std::shared_ptr<const FieldTable> table;  // set when the scene is busy enough to be worth tabulating
};

inline float finiteOr(float v, float fallback) {
	return std::isfinite(v) ? v : fallback;
}

// The long way: grey (0..1) at (ux, uy), composition units inside the home cell, summing every shape that can
// reach it. With Count it also adds the number of outlines it measured to *calls, which makeScene uses to see
// how busy a scene is.
template <bool Count>
inline float shadeFolded(const Scene& S, float ux, float uy, int* calls) {
	Melt melt;
	const bool soft = S.km > 0.f;
	for (int t = 0; t < S.tileCount; ++t) {
		const float px = ux - 2.f * static_cast<float>(S.tileI[static_cast<size_t>(t)]);
		const float py = uy - 2.f * static_cast<float>(S.tileJ[static_cast<size_t>(t)]);
		if (S.tiled) {
			// The bound only holds outside the copies' bounding discs, so it is never used below zero.
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
				// Outside the shape's bounding box the field is at least the distance to the box.
				const float bx = std::max(std::fabs(qx) - S.boxX, 0.f);
				const float by = std::max(std::fabs(qy) - S.boxY, 0.f);
				if (bx * bx + by * by > cut * cut) {
					continue;
				}
				d = sdShapeStretched(S.shape, qx, qy);
			}
			else {
				d = sdShape(S.shape, qx, qy);
			}
			if constexpr (Count) {
				*calls += S.shape.two ? 2 : 1;
			}
			if (soft) {
				meltAdd(melt, d, S.invKm);
			}
			else {
				melt.nearest = std::min(melt.nearest, d);
			}
		}
	}
	const float field = soft ? meltValue(melt, S.km) : melt.nearest;
	return std::clamp(0.5f - field * S.invTwoLc, 0.f, 1.f);
}

// What the table needs to know about one point. The long way stops looking at an outline once it is too far to
// matter, and when everything near is out of range it reads a huge number, so the table cannot take its field at
// face value: it also needs to know how near the outlines that were left out could still be.
struct FieldSample {
	float value = 0.f;   // the long way's field
	float bound = kBig;  // no outline that was left out is nearer than this (kBig when none was)
	float lower = 0.f;   // never above the field with every outline kept, so never above value either
};

// The field itself at (ux, uy), composition units inside the home cell: the same sum as shadeFolded, with the same
// outlines left out, but kept whole. It does not stop early inside a shape, where the field is below the grey ramp
// anyway, so it is a field at every point, which a table needs. Alongside it comes how near the outlines it left
// out could lie (each is at least its tile, copy or box distance away) and, from that, a floor under the field
// with nothing left out: the soft union of all of them cannot be lower than the kept ones joined with that many
// outlines at the nearest bound.
inline FieldSample fieldFolded(const Scene& S, float ux, float uy) {
	Melt melt;
	const bool soft = S.km > 0.f;
	float skippedTile2 = kBig;  // nearest tile centre (squared) among the tiles left out
	float skippedCopy2 = kBig;  // nearest copy centre (squared) among the copies left out by distance
	float skippedBox2 = kBig;   // nearest bounding box (squared) among the copies left out by their box
	int skipped = 0;
	for (int t = 0; t < S.tileCount; ++t) {
		const float px = ux - 2.f * static_cast<float>(S.tileI[static_cast<size_t>(t)]);
		const float py = uy - 2.f * static_cast<float>(S.tileJ[static_cast<size_t>(t)]);
		if (S.tiled) {
			const float cutTile = std::max(std::min(melt.nearest, S.lc) + S.tail, 0.f);
			const float reach = S.tileReach + cutTile;
			const float c2 = px * px + py * py;
			if (c2 >= reach * reach) {
				skippedTile2 = std::min(skippedTile2, c2);
				skipped += S.copies;
				continue;
			}
		}
		for (int k = 0; k < S.copies; ++k) {
			const float dx = px - S.cx[static_cast<size_t>(k)];
			const float dy = py - S.cy[static_cast<size_t>(k)];
			const float r2 = dx * dx + dy * dy;
			const float cut = std::max(std::min(melt.nearest, S.lc) + S.tail, 0.f);
			const float limit = S.cullOffset + cut;
			if (r2 >= limit * limit) {
				skippedCopy2 = std::min(skippedCopy2, r2);
				++skipped;
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
				const float b2 = bx * bx + by * by;
				if (b2 > cut * cut) {
					skippedBox2 = std::min(skippedBox2, b2);
					++skipped;
					continue;
				}
				d = sdShapeStretched(S.shape, qx, qy);
			}
			else {
				d = sdShape(S.shape, qx, qy);
			}
			if (soft) {
				meltAdd(melt, d, S.invKm);
			}
			else {
				melt.nearest = std::min(melt.nearest, d);
			}
		}
	}
	FieldSample out;
	out.value = soft ? meltValue(melt, S.km) : melt.nearest;
	out.lower = out.value;
	if (skipped > 0) {
		float bound = kBig;
		if (skippedTile2 < kBig) {
			bound = std::min(bound, std::sqrt(skippedTile2) - S.tileReach);
		}
		if (skippedCopy2 < kBig) {
			bound = std::min(bound, std::sqrt(skippedCopy2) - S.cullOffset);
		}
		if (skippedBox2 < kBig) {
			bound = std::min(bound, std::sqrt(skippedBox2));
		}
		out.bound = bound;
		if (!soft) {
			out.lower = std::min(out.value, bound);
		}
		else {
			// -k ln(e^(-value/k) + skipped e^(-bound/k)), worked out relative to the smaller of the two.
			const float m = std::min(out.value, bound);
			const float mass = std::exp(-(out.value - m) * S.invKm) + static_cast<float>(skipped) * std::exp(-(bound - m) * S.invKm);
			out.lower = m - S.km * std::log(mass);
		}
	}
	return out;
}

// How far, in grey levels (1/255 of the ramp), the field at a checked point may lie from a square's smooth surface
// before the square is cut instead of read from the surface. The picture has to stay within one level of the long
// way's and the points in between are not checked, so this stays well under that.
constexpr float kTableTolLevels = 0.25f;
// How much of a grey level the long way's small steps (see cutOffWobble) may add up to across one smooth square.
constexpr float kTableWobbleLevels = 0.25f;
// The widest and the narrowest square (in node intervals) that is ever read from its smooth surface. A smooth
// square is checked on a grid of five by five nodes, which a square of two intervals does not have.
constexpr int kTableMaxLinear = 16;
constexpr int kTableMinLinear = 4;
// A table is only built for a frame of at least this many pixels whose pixels each cost at least this many
// nanoseconds the long way. It may spend at most one field evaluation per kTableBudgetDiv pixels, and no more than
// there are nodes in all (a node is never measured twice). The squares that a spent budget leaves undecided are
// worked out the long way, pixel by pixel, so a tight budget costs speed and never the picture.
constexpr long long kTableMinPixels = 30000;
constexpr float kTableMinNs = 150.f;
constexpr int kTableBudgetDiv = 8;
constexpr long long kTableMaxEvals = (FieldTable::kNodes + 1) * (FieldTable::kNodes + 1);

// The smooth surface through a square's nine samples: in each direction, the quadratic through the three samples
// along that direction. smoothCoefficients() turns the samples (f[3 * row + column], rows and columns at 0, 1/2 and
// 1 across the square) into the surface's nine coefficients, c[3 * k + l] for u^k v^l, and smoothField() reads the
// surface at (u, v) in 0..1. The table build checks a square with smoothField() itself, so what it checked is what
// the picture reads.
inline void smoothCoefficients(const float* f, float* c) {
	float row[3][3];  // per row of samples, the quadratic in u: constant, u, u^2
	for (int b = 0; b < 3; ++b) {
		const float p0 = f[3 * b];
		const float p1 = f[3 * b + 1];
		const float p2 = f[3 * b + 2];
		row[b][0] = p0;
		row[b][1] = 4.f * p1 - 3.f * p0 - p2;
		row[b][2] = 2.f * (p0 + p2) - 4.f * p1;
	}
	for (int k = 0; k < 3; ++k) {
		const float q0 = row[0][k];
		const float q1 = row[1][k];
		const float q2 = row[2][k];
		c[3 * k] = q0;
		c[3 * k + 1] = 4.f * q1 - 3.f * q0 - q2;
		c[3 * k + 2] = 2.f * (q0 + q2) - 4.f * q1;
	}
}

// The flat blend between a square's four corner samples (c[0] at u = v = 0, c[1] at u = 1, c[2] at v = 1, c[3] at
// both), read the same way by the table build's check and by the picture.
inline float linearField(const float* c, float u, float v) {
	const float lower = c[0] + (c[1] - c[0]) * u;
	const float upper = c[2] + (c[3] - c[2]) * u;
	return lower + (upper - lower) * v;
}

inline float smoothField(const float* c, float u, float v) {
	const float a0 = c[0] + v * (c[1] + v * c[2]);
	const float a1 = c[3] + v * (c[4] + v * c[5]);
	const float a2 = c[6] + v * (c[7] + v * c[8]);
	return a0 + u * (a1 + u * a2);
}

// The long way stops looking at an outline once it is too far to matter, so as a pixel moves, an outline now and
// then slips past that distance and the field takes a small step: one outline moves it by at most km * e^-kMeltTail.
// The table is built from the long way's own field, so it follows those steps only in a straight line between its
// samples. This is how many field units the steps add up to per composition unit moved, for the scene's densest case:
// outlines are spread over the lattice at `copies` per cell of 4 square composition units, the ones that can slip
// past lie on a circle around the pixel, and the factor 2 allows for how unevenly they fall.
inline float cutOffWobble(const Scene& S) {
	if (S.km <= 0.f) {
		return 0.f;
	}
	const float density = 0.25f * static_cast<float>(S.copies);
	const float radius = S.cullOffset + S.lc + S.tail + S.km;
	return 2.f * S.km * std::exp(-kMeltTail) * density * 2.f * kPi * radius;
}

// Cuts the home cell into the table described at FieldTable. Squares are settled level by level from the coarsest.
// Each level first picks the squares it can afford, then measures all the field values they need at once (shared
// out over several threads when that is worth the start-up, which gives the same numbers whatever the thread
// count), then decides each square in order. Once `budget` field evaluations are spent, every square still
// undecided is left "exact". `evalNs` is a rough cost of one evaluation.
inline std::shared_ptr<const FieldTable> buildFieldTable(const Scene& S, int budget, float evalNs) {
	constexpr int N = FieldTable::kNodes;
	constexpr int W = N + 1;   // nodes along a row
	constexpr int kRoot = 32;  // eight squares across to start with
	constexpr float kUnset = 3e38f;
	constexpr float kQueued = 2e38f;
	auto table = std::make_shared<FieldTable>();
	table->entry.assign(static_cast<size_t>(FieldTable::kSide) * FieldTable::kSide, 0);
	FieldSample unset;
	unset.value = kUnset;
	std::vector<FieldSample> node(static_cast<size_t>(W) * W, unset);
	const int cores = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
	// REPEAT on: the home cell. REPEAT off: nothing repeats, so the table covers the frame itself, as a square
	// around it as wide as its wider side, and squares wholly outside the frame are never measured.
	float frameX0 = -kBig;
	float frameX1 = kBig;
	float frameY0 = -kBig;
	float frameY1 = kBig;
	if (!S.tiled) {
		const float aspectWorld = static_cast<float>(S.width) / static_cast<float>(S.height);
		const float half = std::max(aspectWorld, 1.f);
		table->left = -S.originX - half;
		table->bottom = -S.originY - half;
		table->span = 2.f * half;
		table->lookup = static_cast<float>(FieldTable::kSide) / table->span;
		frameX0 = -aspectWorld - S.originX;
		frameX1 = aspectWorld - S.originX;
		frameY0 = -1.f - S.originY;
		frameY1 = 1.f - S.originY;
	}
	const float left = table->left;
	const float bottom = table->bottom;
	const float span = table->span;
	const float halfSpan = 0.5f * span;  // 1 for the home cell
	auto nodeX = [&](int i) { return left + span * static_cast<float>(i) / N; };
	auto nodeY = [&](int j) { return bottom + span * static_cast<float>(j) / N; };
	auto measure = [&](const std::vector<int>& list) {
		const int n = static_cast<int>(list.size());
		auto run = [&](int a, int b) {
			for (int k = a; k < b; ++k) {
				const int idx = list[static_cast<size_t>(k)];
				node[static_cast<size_t>(idx)] = fieldFolded(S, nodeX(idx % W), nodeY(idx / W));
			}
		};
		// Starting a thread takes tens of microseconds, so only a level with that much work to spare gets help.
		const int threads = std::min(cores, static_cast<int>(static_cast<float>(n) * evalNs / 60000.f));
		if (threads < 2) {
			run(0, n);
			return;
		}
		constexpr int kChunk = 8;
		std::atomic<int> nextChunk{0};
		auto worker = [&]() {
			for (;;) {
				const int a = nextChunk.fetch_add(kChunk);
				if (a >= n) {
					return;
				}
				run(a, std::min(n, a + kChunk));
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
	};
	struct Square {
		int i;
		int j;
		int m;
	};
	auto emit = [&](const Square& q, uint8_t kind, const float* c9) {
		FieldTable::Leaf leaf;
		leaf.x0 = nodeX(q.i);
		leaf.y0 = nodeY(q.j);
		leaf.inv = static_cast<float>(N) / (span * static_cast<float>(q.m));
		if (c9 != nullptr) {
			for (int k = 0; k < 9; ++k) {
				leaf.c[k] = c9[k];
			}
		}
		leaf.kind = kind;
		const auto id = static_cast<uint16_t>(table->leaf.size());
		table->leaf.push_back(leaf);
		for (int b = q.j / 2; b < (q.j + q.m) / 2; ++b) {
			for (int a = q.i / 2; a < (q.i + q.m) / 2; ++a) {
				table->entry[static_cast<size_t>(b) * FieldTable::kSide + static_cast<size_t>(a)] = id;
			}
		}
	};
	// The table interpolates the field itself and clamps afterwards, so squares are compared on the field, not on
	// the clamped grey: a thin ramp can slip between samples that all read black or white.
	const float tol = kTableTolLevels * 2.f * S.lc / 255.f;
	// The long way's steps (see cutOffWobble) are invisible to a surface between samples. A smooth square is checked
	// at every node of its five by five grid, steps and all, so only the steps between neighbouring nodes go unseen:
	// a square may only be smooth if those stay a small part of a grey level, and a flat square, judged from its nine
	// samples alone, needs room to spare for the steps out to its farthest point.
	const float wobble = cutOffWobble(S);
	const float wobbleTol = kTableWobbleLevels * 2.f * S.lc / 255.f;
	std::vector<Square> now;
	std::vector<Square> next;
	std::vector<Square> check;  // squares that may be smooth and now get the five-by-five test
	std::vector<int> todo;
	int evals = 0;
	for (int j = 0; j < N; j += kRoot) {
		for (int i = 0; i < N; i += kRoot) {
			now.push_back({i, j, kRoot});
		}
	}
	// The nine samples of a square: corners, edge middles and middle.
	auto samples = [&](const Square& q, int (&idx)[9]) {
		const int h = q.m / 2;
		idx[0] = q.j * W + q.i;
		idx[1] = q.j * W + q.i + q.m;
		idx[2] = (q.j + q.m) * W + q.i;
		idx[3] = (q.j + q.m) * W + q.i + q.m;
		idx[4] = (q.j + h) * W + q.i + h;
		idx[5] = q.j * W + q.i + h;
		idx[6] = (q.j + q.m) * W + q.i + h;
		idx[7] = (q.j + h) * W + q.i;
		idx[8] = (q.j + h) * W + q.i + q.m;
	};
	// Five by five nodes across a square, row by row: the corners are the first, fifth, twenty-first and last.
	auto grid = [&](const Square& q, int (&idx)[25]) {
		const int g = q.m / 4;
		for (int b = 0; b < 5; ++b) {
			for (int a = 0; a < 5; ++a) {
				idx[b * 5 + a] = (q.j + b * g) * W + q.i + a * g;
			}
		}
	};
	// Puts the nodes among idx that are not measured yet on the list to measure, unless that would take the
	// budget over, in which case it leaves the list as it was and says so.
	auto queue = [&](const int* idx, int count) {
		int fresh = 0;
		for (int k = 0; k < count; ++k) {
			fresh += node[static_cast<size_t>(idx[k])].value >= kUnset ? 1 : 0;
		}
		if (evals + static_cast<int>(todo.size()) + fresh > budget) {
			return false;
		}
		for (int k = 0; k < count; ++k) {
			float& v = node[static_cast<size_t>(idx[k])].value;
			if (v >= kUnset) {
				v = kQueued;
				todo.push_back(idx[k]);
			}
		}
		return true;
	};
	auto split = [&](const Square& q) {
		const int h = q.m / 2;
		next.push_back({q.i, q.j, h});
		next.push_back({q.i + h, q.j, h});
		next.push_back({q.i, q.j + h, h});
		next.push_back({q.i + h, q.j + h, h});
	};
	while (!now.empty()) {
		// Squares wholly outside the frame are never read.
		size_t kept = 0;
		for (size_t a = 0; a < now.size(); ++a) {
			const Square q = now[a];
			if (nodeX(q.i + q.m) < frameX0 || nodeX(q.i) > frameX1 || nodeY(q.j + q.m) < frameY0 || nodeY(q.j) > frameY1) {
				emit(q, FieldTable::kExact, nullptr);
			}
			else {
				now[kept++] = q;
			}
		}
		now.resize(kept);
		todo.clear();
		size_t afford = 0;
		for (; afford < now.size(); ++afford) {
			int idx[9];
			samples(now[afford], idx);
			if (!queue(idx, 9)) {
				break;
			}
		}
		for (size_t a = afford; a < now.size(); ++a) {
			emit(now[a], FieldTable::kExact, nullptr);
		}
		measure(todo);
		evals += static_cast<int>(todo.size());
		next.clear();
		check.clear();
		for (size_t a = 0; a < afford; ++a) {
			const Square& q = now[a];
			int idx[9];
			samples(q, idx);
			float f[9];
			float floorMin = kBig;  // the least of the samples' floors: no outline-complete field is lower
			float top = -kBig;      // the highest field read
			bool whole = true;      // every sample saw all the outlines that could be as near as what it read
			for (int k = 0; k < 9; ++k) {
				const FieldSample& smp = node[static_cast<size_t>(idx[k])];
				f[k] = smp.value;
				floorMin = std::min(floorMin, smp.lower);
				top = std::max(top, smp.value);
				whole = whole && smp.bound >= smp.value;
			}
			// Every point of the square is within this far of a sample, and the field moves at most as fast as that.
			const float reach = 0.7072f * static_cast<float>(q.m) / N * halfSpan;
			const float room = wobble * reach;
			if (floorMin - reach >= S.lc) {
				emit(q, FieldTable::kBlack, nullptr);
			}
			else if (top + reach + room <= -S.lc) {
				emit(q, FieldTable::kWhite, nullptr);
			}
			else if (whole && q.m >= kTableMinLinear && q.m <= kTableMaxLinear &&
			         wobble * 0.5f * static_cast<float>(q.m) / N * halfSpan <= wobbleTol) {
				check.push_back(q);
			}
			else if (q.m > 2) {
				split(q);
			}
			else {
				emit(q, FieldTable::kExact, nullptr);
			}
		}
		// Nine samples always lie on the curved surface through them, so a square that may be smooth is judged on the
		// sixteen nodes of a five by five grid between them: a trough or a crease narrower than the samples are far
		// apart slips between them, so every node has to lie on the surface too. The nine samples are part of that
		// grid, and if the square is cut after all, the grid holds the nine samples of each of its four parts, so the
		// test costs no field evaluation that cutting would not have needed anyway.
		todo.clear();
		size_t afford2 = 0;
		for (; afford2 < check.size(); ++afford2) {
			int idx[25];
			grid(check[afford2], idx);
			if (!queue(idx, 25)) {
				break;
			}
		}
		for (size_t a = afford2; a < check.size(); ++a) {
			emit(check[a], FieldTable::kExact, nullptr);
		}
		measure(todo);
		evals += static_cast<int>(todo.size());
		for (size_t a = 0; a < afford2; ++a) {
			const Square& q = check[a];
			int idx[25];
			grid(q, idx);
			float f9[9];
			for (int b = 0; b < 3; ++b) {
				for (int a2 = 0; a2 < 3; ++a2) {
					f9[b * 3 + a2] = node[static_cast<size_t>(idx[b * 10 + a2 * 2])].value;
				}
			}
			float c9[9];
			smoothCoefficients(f9, c9);
			const float c4[9] = {f9[0], f9[2], f9[6], f9[8]};
			// The flat blend where it is close enough (quicker to read), else the curved surface, else cut.
			bool flat = true;
			bool curved = true;
			for (int b = 0; b < 5 && (flat || curved); ++b) {
				const float v = 0.25f * static_cast<float>(b);
				for (int a2 = 0; a2 < 5; ++a2) {
					const float u = 0.25f * static_cast<float>(a2);
					const FieldSample& smp = node[static_cast<size_t>(idx[b * 5 + a2])];
					if (smp.bound < smp.value) {
						flat = curved = false;
						break;
					}
					flat = flat && std::fabs(smp.value - linearField(c4, u, v)) <= tol;
					curved = curved && std::fabs(smp.value - smoothField(c9, u, v)) <= tol;
				}
			}
			if (flat) {
				emit(q, FieldTable::kLinear, c4);
			}
			else if (curved) {
				emit(q, FieldTable::kSmooth, c9);
			}
			else {
				split(q);
			}
		}
		now.swap(next);
	}
	return table;
}

// Grey from the table at (ux, uy), inside the home cell. False when the square there has to be worked out.
inline bool tableGrey(const FieldTable& T, const Scene& S, float ux, float uy, float& grey) {
	const int ix = std::clamp(static_cast<int>((ux - T.left) * T.lookup), 0, FieldTable::kSide - 1);
	const int iy = std::clamp(static_cast<int>((uy - T.bottom) * T.lookup), 0, FieldTable::kSide - 1);
	const FieldTable::Leaf& L = T.leaf[T.entry[static_cast<size_t>(iy) * FieldTable::kSide + static_cast<size_t>(ix)]];
	switch (L.kind) {
		case FieldTable::kBlack:
			grey = 0.f;
			return true;
		case FieldTable::kWhite:
			grey = 1.f;
			return true;
		case FieldTable::kSmooth: {
			const float u = std::clamp((ux - L.x0) * L.inv, 0.f, 1.f);
			const float v = std::clamp((uy - L.y0) * L.inv, 0.f, 1.f);
			grey = std::clamp(0.5f - smoothField(L.c, u, v) * S.invTwoLc, 0.f, 1.f);
			return true;
		}
		case FieldTable::kLinear: {
			const float u = std::clamp((ux - L.x0) * L.inv, 0.f, 1.f);
			const float v = std::clamp((uy - L.y0) * L.inv, 0.f, 1.f);
			grey = std::clamp(0.5f - linearField(L.c, u, v) * S.invTwoLc, 0.f, 1.f);
			return true;
		}
		default:
			return false;
	}
}

// How many outlines the long way measures per pixel on average, from a fixed spread of points in the home cell.
inline float busyness(const Scene& S) {
	constexpr int kProbes = 64;
	int calls = 0;
	for (int n = 1; n <= kProbes; ++n) {
		// Halton points, bases 2 and 3.
		float x = 0.f;
		float fx = 0.5f;
		for (int v = n; v > 0; v /= 2, fx *= 0.5f) {
			x += static_cast<float>(v % 2) * fx;
		}
		float y = 0.f;
		float fy = 1.f / 3.f;
		for (int v = n; v > 0; v /= 3, fy /= 3.f) {
			y += static_cast<float>(v % 3) * fy;
		}
		shadeFolded<true>(S, 2.f * x - 1.f, 2.f * y - 1.f, &calls);
	}
	return static_cast<float>(calls) / kProbes;
}

inline Scene makeScene(const ShapesParams& in, int width, int height) {
	Scene S;
	S.width = width;
	S.height = height;
	const float aspectWorld = static_cast<float>(width) / static_cast<float>(height);
	S.pixel = 2.f / static_cast<float>(height);

	const float sides = std::clamp(finiteOr(in.sides, 4.f), static_cast<float>(kMinSides), static_cast<float>(kMaxSides));
	const float round = std::clamp(finiteOr(in.round, 0.f), 0.f, 1.f);
	const float point = std::clamp(finiteOr(in.point, 0.f), 0.f, 1.f);
	const float apothem = std::clamp(finiteOr(in.size, 0.5f), 0.f, 4.f);
	const float ratio = std::clamp(finiteOr(in.aspect, 1.f), 0.25f, 4.f);
	const float turn = 2.f * kPi * finiteOr(in.turn, 0.f);
	const int repeat = std::clamp(in.repeat, 1, kMaxRepeat);
	const int copies = std::clamp(in.copies, 1, kMaxCopies);
	const float ring = copies > 1 ? std::clamp(finiteOr(in.ring, 0.48f), 0.f, 2.f) : 0.f;
	const float melt = std::clamp(finiteOr(in.melt, 0.f), 0.f, 1.f);
	const float spread = std::clamp(finiteOr(in.spread, 0.12f), 0.001f, 4.f);

	// Lattice.
	S.tiled = repeat > 1;
	S.s = S.tiled ? 1.f / static_cast<float>(repeat) : 1.f;
	S.pitch = 2.f * S.s;
	const float phase = (S.tiled && repeat % 2 == 0) ? S.s : 0.f;
	S.originX = std::clamp(finiteOr(in.x, 0.f), -2.f, 2.f) * 2.f * aspectWorld + phase;
	S.originY = std::clamp(finiteOr(in.y, 0.f), -2.f, 2.f) * 2.f + phase;

	// Shape.
	int m = static_cast<int>(std::floor(sides));
	float f = sides - static_cast<float>(m);
	if (f > 0.999f) {
		++m;
		f = 0.f;
	}
	if (m >= kMaxSides) {
		m = kMaxSides;
		f = 0.f;
	}
	if (f < 0.001f) {
		f = 0.f;
	}
	S.shape.a = makePoly(m, apothem, point, round);
	S.shape.two = f > 0.f;
	S.shape.mix = f;
	if (S.shape.two) {
		S.shape.b = makePoly(m + 1, apothem, point, round);
	}
	float outer = S.shape.a.outer;
	float inradius = S.shape.a.inradius;
	if (S.shape.two) {
		outer = std::max(outer, S.shape.b.outer);
		inradius = std::min(inradius, S.shape.b.inradius);
	}

	// Stretch. ASPECT scales the shape's own frame by sx across and sy up, which keeps its area. A stretched
	// shape is drawn from its polyline, so the distance is the true one in the picture.
	const float sx = std::sqrt(ratio);
	const float sy = 1.f / sx;
	S.stretched = std::fabs(ratio - 1.f) > 1e-4f;
	const float minS = S.stretched ? std::min(sx, sy) : 1.f;
	S.cullOffset = outer;
	float polyTol = 0.f;
	if (S.stretched) {
		// The polyline's chords stray inside a rounded outline by at most a tenth of a pixel.
		const float tolPic = 0.1f * S.pixel / S.s;
		S.shape.oa = makePolyline(S.shape.a, sx, sy, tolPic);
		S.cullOffset = S.shape.oa.reach;
		S.boxX = S.shape.oa.boxX;
		S.boxY = S.shape.oa.boxY;
		if (S.shape.two) {
			S.shape.ob = makePolyline(S.shape.b, sx, sy, tolPic);
			S.cullOffset = std::max(S.cullOffset, S.shape.ob.reach);
			S.boxX = std::max(S.boxX, S.shape.ob.boxX);
			S.boxY = std::max(S.boxY, S.shape.ob.boxY);
		}
		if (S.shape.a.r > 0.f || (S.shape.two && S.shape.b.r > 0.f)) {
			polyTol = tolPic;
		}
	}

	// Copies in a ring, every one turned by the same TURN plus its share of the circle.
	S.copies = copies;
	for (int k = 0; k < copies; ++k) {
		const float share = 2.f * kPi * static_cast<float>(k) / static_cast<float>(copies);
		const float theta = 0.5f * kPi + turn + share;
		S.cx[static_cast<size_t>(k)] = ring * std::cos(theta);
		S.cy[static_cast<size_t>(k)] = ring * std::sin(theta);
		S.rc[static_cast<size_t>(k)] = std::cos(turn + share);
		S.rs[static_cast<size_t>(k)] = std::sin(turn + share);
	}
	S.tileReach = ring + S.cullOffset;
	const float extent = S.tileReach;

	// Grey ramp. MATTE ramps over one pixel, grey over SPREAD (never under a pixel, which would alias).
	const bool matte = in.matte != 0;
	const float leff = matte ? 0.5f * S.pixel : std::max(spread, S.pixel);
	S.lc = leff / S.s;
	S.invTwoLc = 0.5f / S.lc;
	S.km = melt * kMeltSoftness;
	S.invKm = S.km > 0.f ? 1.f / S.km : 0.f;
	S.tail = S.km * kMeltTail;
	const float deep = inradius * minS - S.lc - polyTol;
	S.deepRadius2 = deep > 0.f ? deep * deep : -1.f;

	// Neighbouring cells that can matter: those whose content could be the nearest, or within the MELT
	// width of it. Content of the home cell is never farther than about 1.5 + its own reach.
	int nr = 0;
	if (S.tiled) {
		nr = std::clamp(static_cast<int>(std::floor((2.5f + 2.f * extent + S.tail) * 0.5f)), 1, kMaxTileRadius);
	}
	int count = 0;
	for (int dj = -nr; dj <= nr; ++dj) {
		for (int di = -nr; di <= nr; ++di) {
			S.tileI[static_cast<size_t>(count)] = static_cast<signed char>(di);
			S.tileJ[static_cast<size_t>(count)] = static_cast<signed char>(dj);
			++count;
		}
	}
	// Nearest cells first, so the running minimum tightens early and the far cells get skipped.
	std::vector<int> order(static_cast<size_t>(count));
	for (int i = 0; i < count; ++i) {
		order[static_cast<size_t>(i)] = i;
	}
	std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
		const int da = S.tileI[static_cast<size_t>(a)] * S.tileI[static_cast<size_t>(a)] + S.tileJ[static_cast<size_t>(a)] * S.tileJ[static_cast<size_t>(a)];
		const int db = S.tileI[static_cast<size_t>(b)] * S.tileI[static_cast<size_t>(b)] + S.tileJ[static_cast<size_t>(b)] * S.tileJ[static_cast<size_t>(b)];
		return da < db;
	});
	std::array<signed char, kMaxTiles> sortedI{};
	std::array<signed char, kMaxTiles> sortedJ{};
	for (int i = 0; i < count; ++i) {
		sortedI[static_cast<size_t>(i)] = S.tileI[static_cast<size_t>(order[static_cast<size_t>(i)])];
		sortedJ[static_cast<size_t>(i)] = S.tileJ[static_cast<size_t>(order[static_cast<size_t>(i)])];
	}
	S.tileI = sortedI;
	S.tileJ = sortedJ;
	S.tileCount = count;

	// A scene that takes many outlines per pixel is cheaper to tabulate over the home cell (or, with REPEAT off,
	// over the frame) and read back.
	const long long pixels = static_cast<long long>(width) * height;
	if (pixels >= kTableMinPixels) {
		const float nsPerPixel = (S.stretched ? 46.f : 20.f) * busyness(S);
		if (nsPerPixel >= kTableMinNs) {
			S.table = buildFieldTable(S, static_cast<int>(std::clamp<long long>(pixels / kTableBudgetDiv, 600, kTableMaxEvals)), nsPerPixel);
		}
	}
	return S;
}

// Grey (0..1) at world position (wx, wy).
inline float shade(const Scene& S, float wx, float wy) {
	// Position inside the home cell, composition units, cell spanning [-1, 1].
	float ux = wx - S.originX;
	float uy = wy - S.originY;
	if (S.tiled) {
		const float fx = ux / S.pitch;
		const float fy = uy / S.pitch;
		ux = (fx - std::floor(fx + 0.5f)) * 2.f;
		uy = (fy - std::floor(fy + 0.5f)) * 2.f;
	}
	if (S.table) {
		float grey;
		if (tableGrey(*S.table, S, ux, uy, grey)) {
			return grey;
		}
	}
	return shadeFolded<false>(S, ux, uy, nullptr);
}

}  // namespace shapes_field
