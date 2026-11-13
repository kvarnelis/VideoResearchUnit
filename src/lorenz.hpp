#pragma once

// Lorenz attractor (sigma 10, rho 28, beta 8/3), integrated with classical RK4 in double
// precision. Chaotic at these constants. RK4 stays stable for steps up to about 0.01 time units,
// and the module always steps by less than that. Pure math, shared by the plugin and the tests.
struct LorenzState {
	double x = 1.0;
	double y = 1.0;
	double z = 20.0;
};

namespace lorenz {
constexpr double kSigma = 10.0;
constexpr double kRho = 28.0;
constexpr double kBeta = 8.0 / 3.0;
constexpr double kMaxStep = 0.01;

inline void derivative(const LorenzState& s, LorenzState& d) {
	d.x = kSigma * (s.y - s.x);
	d.y = s.x * (kRho - s.z) - s.y;
	d.z = s.x * s.y - kBeta * s.z;
}

// One RK4 step of size h (time units).
inline void step(LorenzState& s, double h) {
	LorenzState k1, k2, k3, k4, t;
	derivative(s, k1);
	t = {s.x + 0.5 * h * k1.x, s.y + 0.5 * h * k1.y, s.z + 0.5 * h * k1.z};
	derivative(t, k2);
	t = {s.x + 0.5 * h * k2.x, s.y + 0.5 * h * k2.y, s.z + 0.5 * h * k2.z};
	derivative(t, k3);
	t = {s.x + h * k3.x, s.y + h * k3.y, s.z + h * k3.z};
	derivative(t, k4);
	s.x += h / 6.0 * (k1.x + 2.0 * k2.x + 2.0 * k3.x + k4.x);
	s.y += h / 6.0 * (k1.y + 2.0 * k2.y + 2.0 * k3.y + k4.y);
	s.z += h / 6.0 * (k1.z + 2.0 * k2.z + 2.0 * k3.z + k4.z);
}
}
