#pragma once

#include <algorithm>
#include <cmath>

// Mirror folds the position back and forth across the frame edges (a triangle wave with period 2 * (size - 1)).
enum class InverseMapEdge : int { Clamp = 0, Wrap = 1, Black = 2, Mirror = 3 };
struct InverseMap {
	float centreX, centreY;
	float invZoom;
	float cosR, sinR;
	float offsetX, offsetY;
	int edge;
};

// Zoom magnifies about the centre; positive rotation turns counter-clockwise on screen.
// Offsets are fractions of the frame width and height.
inline InverseMap makeInverseMap(int width, int height, float zoom, float rotateTurns,
	float offsetFracX, float offsetFracY, InverseMapEdge edge) {
	const float angle = rotateTurns * 6.2831853071795864769f;
	return {(width - 1) * 0.5f, (height - 1) * 0.5f, 1.f / std::max(zoom, 0.000001f),
		std::cos(angle), std::sin(angle), offsetFracX * width, offsetFracY * height, static_cast<int>(edge)};
}

inline bool inverseMapSource(const InverseMap& m, int width, int height, float x, float y, float& sx, float& sy) {
	if (width <= 0 || height <= 0) return false;
	const float dx = x - m.offsetX - m.centreX;
	const float dy = y - m.offsetY - m.centreY;
	// Screen y points down, so undoing a counter-clockwise turn uses this sign order.
	sx = (m.cosR * dx - m.sinR * dy) * m.invZoom + m.centreX;
	sy = (m.sinR * dx + m.cosR * dy) * m.invZoom + m.centreY;
	if (m.edge == static_cast<int>(InverseMapEdge::Black)) {
		return sx >= 0.f && sy >= 0.f && sx <= width - 1 && sy <= height - 1;
	}
	if (m.edge == static_cast<int>(InverseMapEdge::Wrap)) {
		sx -= std::floor(sx / width) * width;
		sy -= std::floor(sy / height) * height;
	}
	else if (m.edge == static_cast<int>(InverseMapEdge::Mirror)) {
		auto fold = [](float v, int size) {
			if (size <= 1) return 0.f;
			const float period = 2.f * static_cast<float>(size - 1);
			v -= std::floor(v / period) * period;
			return v > static_cast<float>(size - 1) ? period - v : v;
		};
		sx = fold(sx, width);
		sy = fold(sy, height);
	}
	else {
		sx = std::clamp(sx, 0.f, static_cast<float>(width - 1));
		sy = std::clamp(sy, 0.f, static_cast<float>(height - 1));
	}
	return true;
}
