// Trial: Shapes (VRU-21) drawn on the graphics card, timed against the CPU renderer.
//
// The shader is GLSL 1.20, what Rack's OpenGL 2 context offers on macOS, Windows and Linux. It works out the
// "long way" field of src/shapes_field.hpp (shadeFolded) for every pixel, from the same Scene the CPU builds, so
// the two pictures can be compared. This program makes its own offscreen context with CGL, so it is macOS only;
// the shader and the uniform upload are not.
//
//   make build/shapes_gpu_bench && build/shapes_gpu_bench [width height]

#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

#include "shapes_field.hpp"
#include "shapes_gpu_program.hpp"
#include "shapes_math.hpp"
#include "video_engine.hpp"
#include "video_frame.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using shapes_gpu_program::Gpu;

double nowMs() {
	using clock = std::chrono::steady_clock;
	return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

template <typename F>
double medianMs(int runs, const F& fn) {
	std::vector<double> t;
	for (int i = 0; i < runs; ++i) {
		const double a = nowMs();
		fn(i);
		t.push_back(nowMs() - a);
	}
	std::sort(t.begin(), t.end());
	return t[t.size() / 2];
}

struct Case {
	const char* name;
	shapes_math::ShapesControls c;
};

}  // namespace

int main(int argc, char** argv) {
	const int width = argc > 2 ? std::atoi(argv[1]) : 1440;
	const int height = argc > 2 ? std::atoi(argv[2]) : 1080;

	// A legacy (OpenGL 2.1) accelerated context, the kind Rack makes.
	const CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, kCGLPFAColorSize, static_cast<CGLPixelFormatAttribute>(24), static_cast<CGLPixelFormatAttribute>(0)};
	CGLPixelFormatObj format = nullptr;
	GLint formats = 0;
	if (CGLChoosePixelFormat(attrs, &format, &formats) != kCGLNoError || !format) {
		std::fprintf(stderr, "no OpenGL pixel format\n");
		return 1;
	}
	CGLContextObj context = nullptr;
	CGLCreateContext(format, nullptr, &context);
	CGLSetCurrentContext(context);
	std::printf("OpenGL %s, GLSL %s, %s\n", glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION), glGetString(GL_RENDERER));

	Gpu gpu;
	std::string error;
	if (!gpu.build(error)) {
		std::fprintf(stderr, "shader failed:\n%s\n", error.c_str());
		return 1;
	}
	glUseProgram(gpu.program);

	// Two targets: 8-bit, and float for "Full precision between modules".
	GLuint target[2];
	GLuint fbo[2];
	glGenTextures(2, target);
	glGenFramebuffersEXT(2, fbo);
	for (int i = 0; i < 2; ++i) {
		glBindTexture(GL_TEXTURE_2D, target[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		if (i == 0) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		}
		else {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
		}
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo[i]);
		glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, target[i], 0);
		if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) {
			std::fprintf(stderr, "framebuffer %d incomplete\n", i);
			return 1;
		}
	}
	glViewport(0, 0, width, height);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	const float quad[8] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);

	std::vector<Case> cases;
	{
		Case c{"default square", {}};
		cases.push_back(c);
	}
	{
		// The two Shapes in "Sounding Line", knobs as saved (their CV is left out).
		Case a{"Sounding Line shape 1", {}};
		a.c.sides = 3.f; a.c.round = 0.22f; a.c.point = 0.2f; a.c.size = 0.42f; a.c.x = -0.09f; a.c.y = 0.04f;
		a.c.aspect = 0.33f; a.c.copies = 2.f; a.c.ring = 0.54f; a.c.spread = 0.78f;
		cases.push_back(a);
		Case b{"Sounding Line shape 2", {}};
		b.c.sides = 5.f; b.c.round = 0.23f; b.c.point = 0.72f; b.c.size = 0.38f; b.c.x = 0.1f; b.c.y = 0.03f;
		b.c.aspect = 0.12f; b.c.turn = 0.08f; b.c.copies = 3.f; b.c.ring = 0.56f; b.c.spread = 0.73f;
		cases.push_back(b);
	}
	{
		Case c{"busy: 4.5 sides, repeat 4, copies 6, melt", {}};
		c.c.sides = 4.5f; c.c.round = 0.3f; c.c.point = 0.5f; c.c.size = 0.5f; c.c.repeat = 4.f; c.c.copies = 6.f;
		c.c.melt = 0.6f; c.c.turn = 0.07f;
		cases.push_back(c);
		Case d{"busy and stretched", c.c};
		d.c.aspect = 0.4f;
		cases.push_back(d);
	}

	const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
	std::vector<unsigned char> gpuBytes(pixels * 4);
	std::vector<float> gpuFloat(pixels);
	std::vector<unsigned char> cpuBytes;
	VideoFrame cpuFrame;
	constexpr int kRuns = 60;

	std::printf("%d x %d, median of %d pictures, milliseconds. The knobs move a little each picture, as CV would move them.\n\n", width, height, kRuns);
	std::printf("%-44s %8s %8s | %8s %8s %8s | %s\n", "", "CPU 8bit", "CPU flt", "GPU draw", "+read 8", "+read f", "difference, 8-bit levels");
	for (const Case& cs : cases) {
		auto paramsAt = [&](int i) {
			shapes_math::ShapesControls c = cs.c;
			c.turn += 0.0007f * static_cast<float>(i);
			c.size += 0.0003f * static_cast<float>(i % 7);
			return shapes_math::shapesParams(c);
		};
		const double cpu8 = medianMs(kRuns, [&](int i) { renderShapes(cpuBytes, width, height, paramsAt(i)); });
		const double cpuF = medianMs(kRuns, [&](int i) { renderShapes(cpuFrame, width, height, paramsAt(i), true); });

		auto draw = [&](int i, int which) {
			const shapes_field::Scene S = shapes_field::makeScene(paramsAt(i), width, height);
			glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo[which]);
			gpu.setScene(S, 1.f);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		};
		for (int i = 0; i < 5; ++i) {
			draw(i, 0);
			draw(i, 1);
		}
		glFinish();
		const double gpuDraw = medianMs(kRuns, [&](int i) {
			draw(i, 0);
			glFinish();
		});
		const double gpuRead8 = medianMs(kRuns, [&](int i) {
			draw(i, 0);
			glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, gpuBytes.data());
		});
		const double gpuReadF = medianMs(kRuns, [&](int i) {
			draw(i, 1);
			glReadPixels(0, 0, width, height, GL_RED, GL_FLOAT, gpuFloat.data());
		});

		// Same knobs on both sides, 8-bit pictures compared.
		renderShapes(cpuBytes, width, height, paramsAt(3));
		draw(3, 0);
		glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, gpuBytes.data());
		size_t differ = 0;
		size_t over1 = 0;
		int worst = 0;
		for (size_t p = 0; p < pixels; ++p) {
			const int d = std::abs(static_cast<int>(cpuBytes[p * 4]) - static_cast<int>(gpuBytes[p * 4]));
			differ += d != 0;
			over1 += d > 1;
			worst = std::max(worst, d);
		}
		std::printf("%-44s %8.2f %8.2f | %8.2f %8.2f %8.2f | worst %d, %.3f%% of pixels differ, %.4f%% by more than 1\n", cs.name, cpu8, cpuF,
			gpuDraw, gpuRead8, gpuReadF, worst, 100.0 * static_cast<double>(differ) / static_cast<double>(pixels),
			100.0 * static_cast<double>(over1) / static_cast<double>(pixels));
	}
	return 0;
}
