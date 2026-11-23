#pragma once

// Shapes (VRU-21) on the graphics card: the GLSL 1.20 shader and the code that hands it a shapes_field::Scene.
//
// The shader works out the "long way" field of src/shapes_field.hpp (shadeFolded) for every pixel, from the same
// Scene the CPU builds. GLSL 1.20 is what Rack's OpenGL 2 context offers on macOS, Windows and Linux.
//
// This header names no OpenGL header: include one first (Rack's plugin.hpp in the plugin, <OpenGL/gl.h> in
// tests/shapes_gpu_bench.cpp). Used by src/shapes_gpu.cpp and the bench, so both run the same shader.

#include "shapes_field.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace shapes_gpu_program {

inline const char* const kVertex = R"(#version 120
attribute vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

inline const char* const kFragment = R"(#version 120
uniform vec2 uSize;
uniform float uAspect;
uniform vec2 uOrigin;
uniform float uPitch;
uniform float uTiled;
uniform int uTileCount;
uniform vec2 uTile[49];
uniform int uCopies;
uniform vec4 uCopy[8];     // centre x, y, cos, sin of the turn
uniform float uStretched;
uniform vec2 uBox;
uniform float uCullOffset;
uniform float uTileReach;
uniform float uKm;
uniform float uInvKm;
uniform float uTail;
uniform float uLc;
uniform float uInvTwoLc;
uniform float uDeep2;
uniform float uTwo;
uniform float uMix;
uniform float uPA[15];     // Poly a: m, stepsPerRad, vix, ex, ey, invLen2, r, cx, cy, startX, startY, u0x, u0y, u1x, u1y
uniform float uPB[15];
uniform sampler2D uData;   // the polylines: segments and angle tables
uniform vec2 uDataSize;
uniform vec4 uLineA0;      // segment offset, segment count, table offset, table size
uniform vec4 uLineA1;      // base angle, table bins per radian, reach, radial
uniform vec4 uLineB0;
uniform vec4 uLineB1;
uniform float uScale;   // 1 for an 8-bit target, 255 for a float one (the graph's float pictures are in 0..255 units)

const float kBig = 1e6;
const float kPi = 3.14159265358979;

vec4 fetch(float index) {
	float row = floor(index / uDataSize.x);
	float col = index - row * uDataSize.x;
	return texture2D(uData, (vec2(col, row) + 0.5) / uDataSize);
}

float angleOf(vec2 p) {
	return (p.x == 0.0 && p.y == 0.0) ? 0.0 : atan(p.y, p.x);
}

float sdWedge(float P[15], float qx, float qy) {
	qy = abs(qy);
	float best = kBig;
	bool inside = false;
	float vix = P[2], ex = P[3], ey = P[4], invLen2 = P[5], r = P[6];
	if (invLen2 > 0.0) {
		float wx = qx - vix;
		float t = clamp((wx * ex + qy * ey) * invLen2, 0.0, 1.0);
		float dx = wx - t * ex;
		float dy = qy - t * ey;
		best = dx * dx + dy * dy;
		inside = ex * qy - ey * wx > 0.0;
	}
	else if (r <= 0.0) {
		float wx = qx - vix;
		best = wx * wx + qy * qy;
	}
	if (r > 0.0) {
		float wx = qx - P[7];
		float wy = qy - P[8];
		bool afterStart = P[11] * wy - P[12] * wx >= 0.0;
		bool beforeApex = wx * P[14] - wy * P[13] >= 0.0;
		if (afterStart && beforeApex) {
			float d = sqrt(wx * wx + wy * wy) - r;
			if (d * d < best) {
				best = d * d;
				inside = d < 0.0;
			}
		}
		else if (!afterStart) {
			if (invLen2 <= 0.0) {
				float dx = qx - P[9];
				float dy = qy - P[10];
				float d2 = dx * dx + dy * dy;
				if (d2 < best) {
					best = d2;
					inside = true;
				}
			}
		}
		else {
			float dx = qx - (P[7] + r * P[13]);
			float dy = qy - (P[8] + r * P[14]);
			float d2 = dx * dx + dy * dy;
			if (d2 < best) {
				best = d2;
				inside = wx * wx + wy * wy < r * r;
			}
		}
	}
	float dist = sqrt(best);
	return inside ? -dist : dist;
}

float sdPoly(float P[15], vec2 p) {
	float m = P[0];
	float k = floor((angleOf(p) + 0.5 * kPi) * P[1] + 0.5);
	if (k < 0.0) k += m;
	else if (k >= m) k -= m;
	float phi = -0.5 * kPi + 2.0 * kPi * k / m;
	float cs = cos(phi);
	float sn = sin(phi);
	return sdWedge(P, cs * p.x + sn * p.y, -sn * p.x + cs * p.y);
}

float segDist2(vec4 s, float invLen2, vec2 p) {
	vec2 w = p - s.xy;
	float t = clamp(dot(w, s.zw) * invLen2, 0.0, 1.0);
	vec2 v = w - t * s.zw;
	return dot(v, v);
}

float angleBound2(float dp, float cr, float rho2, float reach) {
	if (dp <= 0.0) return rho2;
	if (dp >= reach) return rho2 + reach * (reach - 2.0 * dp);
	return cr * cr;
}

float lineDistance(vec4 L0, vec4 L1, vec2 p) {
	float off = L0.x;
	float n = L0.y;
	if (L1.w < 0.5) {
		// Not radial: every segment, even-odd count for the sign.
		float best = kBig;
		bool inside = false;
		for (int i = 0; i < 8192; ++i) {
			if (float(i) >= n) break;
			vec4 s = fetch(off + 2.0 * float(i));
			vec4 e = fetch(off + 2.0 * float(i) + 1.0);
			best = min(best, segDist2(s, e.x, p));
			vec2 w = p - s.xy;
			bool c1 = p.y >= s.y;
			bool c2 = p.y < s.y + s.w;
			bool c3 = s.z * w.y > s.w * w.x;
			if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) inside = !inside;
		}
		float dist = sqrt(best);
		return inside ? -dist : dist;
	}
	float rho2 = dot(p, p);
	float t = angleOf(p) - L1.x;
	if (t < 0.0) t += 2.0 * kPi;
	float i = floor(fetch(L0.z + min(floor(t * L1.y), L0.w - 1.0)).x + 0.5);
	for (int guard = 0; guard < 16; ++guard) {
		float nx = i + 1.0 >= n ? 0.0 : i + 1.0;
		vec2 un = fetch(off + 2.0 * nx + 1.0).yz;
		if (un.x * p.y - un.y * p.x >= 0.0 && dot(un, p) > 0.0) {
			i = nx;
			continue;
		}
		vec2 ui = fetch(off + 2.0 * i + 1.0).yz;
		if (ui.x * p.y - ui.y * p.x < 0.0 && dot(ui, p) > 0.0) {
			i = i < 0.5 ? n - 1.0 : i - 1.0;
			continue;
		}
		break;
	}
	vec4 facing = fetch(off + 2.0 * i);
	vec4 facingE = fetch(off + 2.0 * i + 1.0);
	bool inside = facing.z * (p.y - facing.y) - facing.w * (p.x - facing.x) > 0.0;
	float best = segDist2(facing, facingE.x, p);
	float f = i + 1.0 >= n ? 0.0 : i + 1.0;
	float b = i < 0.5 ? n - 1.0 : i - 1.0;
	float left = n - 1.0;
	bool goingOn = true;
	bool backOn = true;
	for (int step = 0; step < 8192; ++step) {
		if (!(left > 0.0 && (goingOn || backOn))) break;
		if (goingOn) {
			vec4 s = fetch(off + 2.0 * f);
			vec4 e = fetch(off + 2.0 * f + 1.0);
			float bound = angleBound2(dot(e.yz, p), e.y * p.y - e.z * p.x, rho2, L1.z);
			if (bound >= best) {
				goingOn = false;
			}
			else {
				best = min(best, segDist2(s, e.x, p));
				f = f + 1.0 >= n ? 0.0 : f + 1.0;
				left -= 1.0;
			}
		}
		if (backOn && left > 0.0) {
			float nb = b + 1.0 >= n ? 0.0 : b + 1.0;
			vec2 u = fetch(off + 2.0 * nb + 1.0).yz;
			float bound = angleBound2(dot(u, p), u.x * p.y - u.y * p.x, rho2, L1.z);
			if (bound >= best) {
				backOn = false;
			}
			else {
				vec4 s = fetch(off + 2.0 * b);
				vec4 e = fetch(off + 2.0 * b + 1.0);
				best = min(best, segDist2(s, e.x, p));
				b = b < 0.5 ? n - 1.0 : b - 1.0;
				left -= 1.0;
			}
		}
	}
	float dist = sqrt(best);
	return inside ? -dist : dist;
}

float shapeDistance(vec2 q) {
	if (uStretched > 0.5) {
		float d = lineDistance(uLineA0, uLineA1, q);
		if (uTwo > 0.5) d += (lineDistance(uLineB0, uLineB1, q) - d) * uMix;
		return d;
	}
	float d = sdPoly(uPA, q);
	if (uTwo > 0.5) d += (sdPoly(uPB, q) - d) * uMix;
	return d;
}

void main() {
	vec2 w = vec2((gl_FragCoord.x / uSize.x - 0.5) * 2.0 * uAspect, (0.5 - gl_FragCoord.y / uSize.y) * 2.0);
	vec2 u = w - uOrigin;
	if (uTiled > 0.5) {
		vec2 f = u / uPitch;
		u = (f - floor(f + 0.5)) * 2.0;
	}
	float nearest = kBig;
	float sum = 0.0;
	bool soft = uKm > 0.0;
	bool deep = false;
	for (int t = 0; t < 49; ++t) {
		if (t >= uTileCount || deep) break;
		vec2 p = u - 2.0 * uTile[t];
		if (uTiled > 0.5) {
			float reach = uTileReach + max(min(nearest, uLc) + uTail, 0.0);
			if (dot(p, p) >= reach * reach) continue;
		}
		for (int k = 0; k < 8; ++k) {
			if (k >= uCopies) break;
			vec2 d2 = p - uCopy[k].xy;
			float r2 = dot(d2, d2);
			if (r2 < uDeep2) {
				deep = true;
				break;
			}
			float cut = max(min(nearest, uLc) + uTail, 0.0);
			float limit = uCullOffset + cut;
			if (r2 >= limit * limit) continue;
			float rc = uCopy[k].z;
			float rs = uCopy[k].w;
			vec2 q = vec2(rc * d2.x + rs * d2.y, -rs * d2.x + rc * d2.y);
			if (uStretched > 0.5) {
				vec2 bx = max(abs(q) - uBox, 0.0);
				if (dot(bx, bx) > cut * cut) continue;
			}
			float d = shapeDistance(q);
			if (soft) {
				if (d >= nearest) {
					sum += exp(-(d - nearest) * uInvKm);
				}
				else {
					sum = sum * exp(-(nearest - d) * uInvKm) + 1.0;
					nearest = d;
				}
			}
			else {
				nearest = min(nearest, d);
			}
		}
	}
	float field = (soft && sum > 1.0) ? nearest - uKm * log(sum) : nearest;
	float v = deep ? 1.0 : clamp(0.5 - field * uInvTwoLc, 0.0, 1.0);
	gl_FragColor = vec4(v, v, v, 1.0) * uScale;
}
)";

inline GLuint compile(GLenum type, const char* source, std::string& error) {
	const GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);
	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[4096] = {};
		glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
		error = log;
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

inline void polyUniform(GLuint program, const char* name, const shapes_field::Poly& p) {
	const float v[15] = {static_cast<float>(p.m), p.stepsPerRad, p.vix, p.ex, p.ey, p.invLen2, p.r, p.cx, p.cy,
		p.startX, p.startY, p.u0x, p.u0y, p.u1x, p.u1y};
	glUniform1fv(glGetUniformLocation(program, name), 15, v);
}

inline constexpr int kDataWidth = 1024;

// Appends one polyline to the texel list and sets its two uniforms.
inline void lineUniform(GLuint program, const char* name0, const char* name1, const shapes_field::Polyline& o, std::vector<float>& texels) {
	const float segOffset = static_cast<float>(texels.size() / 4);
	for (const shapes_field::Segment& s : o.segment) {
		const float a[8] = {s.ax, s.ay, s.ex, s.ey, s.invLen2, s.ux, s.uy, 0.f};
		texels.insert(texels.end(), a, a + 8);
	}
	const float lutOffset = static_cast<float>(texels.size() / 4);
	for (int i : o.lut) {
		const float a[4] = {static_cast<float>(i), 0.f, 0.f, 0.f};
		texels.insert(texels.end(), a, a + 4);
	}
	glUniform4f(glGetUniformLocation(program, name0), segOffset, static_cast<float>(o.segment.size()), lutOffset, static_cast<float>(o.lut.size()));
	glUniform4f(glGetUniformLocation(program, name1), o.base, o.lutScale, o.reach, o.radial ? 1.f : 0.f);
}

struct Gpu {
	GLuint program = 0;

	// Compiles and links; false (with the driver's message) when this card or driver will not take the shader.
	bool build(std::string& error) {
		const GLuint vs = compile(GL_VERTEX_SHADER, kVertex, error);
		const GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, kFragment, error) : 0;
		if (!vs || !fs) {
			return false;
		}
		program = glCreateProgram();
		glAttachShader(program, vs);
		glAttachShader(program, fs);
		glBindAttribLocation(program, 0, "aPos");
		glLinkProgram(program);
		GLint linked = 0;
		glGetProgramiv(program, GL_LINK_STATUS, &linked);
		if (!linked) {
			char log[4096] = {};
			glGetProgramInfoLog(program, sizeof(log), nullptr, log);
			error = log;
			return false;
		}
		glGenTextures(1, &dataTexture);
		glBindTexture(GL_TEXTURE_2D, dataTexture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		return true;
	}

	GLuint dataTexture = 0;
	std::vector<float> texels;
	std::map<std::string, GLint> locations;

	// Everything that changes with the knobs: a few dozen uniforms and, for a stretched shape, one small texture.
	void setScene(const shapes_field::Scene& S, float scale) {
		auto loc = [&](const char* n) {
			auto it = locations.find(n);
			if (it == locations.end()) {
				it = locations.emplace(n, glGetUniformLocation(program, n)).first;
			}
			return it->second;
		};
		glUniform2f(loc("uSize"), static_cast<float>(S.width), static_cast<float>(S.height));
		glUniform1f(loc("uAspect"), static_cast<float>(S.width) / static_cast<float>(S.height));
		glUniform2f(loc("uOrigin"), S.originX, S.originY);
		glUniform1f(loc("uPitch"), S.pitch);
		glUniform1f(loc("uTiled"), S.tiled ? 1.f : 0.f);
		glUniform1i(loc("uTileCount"), S.tileCount);
		float tiles[2 * shapes_field::kMaxTiles] = {};
		for (int t = 0; t < S.tileCount; ++t) {
			tiles[2 * t] = static_cast<float>(S.tileI[static_cast<size_t>(t)]);
			tiles[2 * t + 1] = static_cast<float>(S.tileJ[static_cast<size_t>(t)]);
		}
		glUniform2fv(loc("uTile"), shapes_field::kMaxTiles, tiles);
		glUniform1i(loc("uCopies"), S.copies);
		float copies[4 * shapes_field::kMaxCopies] = {};
		for (int k = 0; k < S.copies; ++k) {
			const size_t i = static_cast<size_t>(k);
			copies[4 * k] = S.cx[i];
			copies[4 * k + 1] = S.cy[i];
			copies[4 * k + 2] = S.rc[i];
			copies[4 * k + 3] = S.rs[i];
		}
		glUniform4fv(loc("uCopy"), shapes_field::kMaxCopies, copies);
		glUniform1f(loc("uStretched"), S.stretched ? 1.f : 0.f);
		glUniform2f(loc("uBox"), S.boxX, S.boxY);
		glUniform1f(loc("uCullOffset"), S.cullOffset);
		glUniform1f(loc("uTileReach"), S.tileReach);
		glUniform1f(loc("uKm"), S.km);
		glUniform1f(loc("uInvKm"), S.invKm);
		glUniform1f(loc("uTail"), S.tail);
		glUniform1f(loc("uLc"), S.lc);
		glUniform1f(loc("uInvTwoLc"), S.invTwoLc);
		glUniform1f(loc("uDeep2"), S.deepRadius2);
		glUniform1f(loc("uTwo"), S.shape.two ? 1.f : 0.f);
		glUniform1f(loc("uMix"), S.shape.mix);
		polyUniform(program, "uPA", S.shape.a);
		polyUniform(program, "uPB", S.shape.two ? S.shape.b : S.shape.a);
		texels.clear();
		if (S.stretched) {
			lineUniform(program, "uLineA0", "uLineA1", S.shape.oa, texels);
			if (S.shape.two) {
				lineUniform(program, "uLineB0", "uLineB1", S.shape.ob, texels);
			}
		}
		const int rows = std::max(1, static_cast<int>((texels.size() / 4 + kDataWidth - 1) / kDataWidth));
		texels.resize(static_cast<size_t>(rows) * kDataWidth * 4, 0.f);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, dataTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, kDataWidth, rows, 0, GL_RGBA, GL_FLOAT, texels.data());
		glUniform1i(loc("uData"), 0);
		glUniform1f(loc("uScale"), scale);
		glUniform2f(loc("uDataSize"), static_cast<float>(kDataWidth), static_cast<float>(rows));
	}
};

}  // namespace shapes_gpu_program
