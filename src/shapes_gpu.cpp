// Shapes (VRU-21) drawn on the graphics card.
//
// The picture is drawn by a GLSL 1.20 shader (src/shapes_gpu_program.hpp) into an offscreen target and read back, so
// the rest of the chain goes on working on CPU pictures. It has its own hidden OpenGL context, made with GLFW on the
// UI thread and not shared with Rack's: Rack's context can only be used on the UI thread, and the pictures are often
// rendered on the render service's thread. Whichever thread renders makes the context current for the length of one
// picture and lets it go again, under a lock. If the lock is taken, or anything about the card or driver is not as
// needed, the call returns false and renderShapes() uses the CPU code, which stays the reference.

#include "shapes_gpu.hpp"

#include "plugin.hpp"
#include "shapes_gpu_program.hpp"
#include "video_engine.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace shapes_gpu {
namespace {

#if defined(ARCH_MAC)
constexpr bool kDefaultOn = true;
#else
// Not yet tried on Windows or Linux, so it is there to turn on but starts off.
constexpr bool kDefaultOn = false;
#endif

std::atomic<bool> wanted{kDefaultOn};
std::atomic<bool> ready{false};
std::atomic<bool> failed{false};
std::string failure;  // written once, before `failed` is set

struct Target {
	int width = 0;
	int height = 0;
	bool isFloat = false;
	GLuint texture = 0;
	GLuint fbo = 0;
	GLuint pbo = 0;  // pixel buffer the picture is read back through; 0 when the driver has none
	uint64_t used = 0;
};

struct State {
	std::mutex mutex;  // one thread in the context at a time
	GLFWwindow* window = nullptr;
	bool built = false;
	bool announced = false;
	bool pixelBuffers = false;  // read back through a pixel buffer object (see readBack)
	shapes_gpu_program::Gpu gpu;
	std::vector<Target> targets;
	uint64_t clock = 0;
};

State& state() {
	static State s;
	return s;
}

void fail(const std::string& why) {
	if (!failed.load()) {
		failure = why;
		failed.store(true);
		WARN("Shapes on the graphics card is off: %s", why.c_str());
	}
}

bool hasExtension(const char* name) {
	const char* all = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
	return all && std::strstr(all, name);
}

// With the context current: the shader and the fixed state. Once.
bool build(State& s) {
	if (!hasExtension("GL_EXT_framebuffer_object") || !hasExtension("GL_ARB_texture_float")) {
		fail("this graphics driver has no offscreen float pictures in OpenGL 2");
		return false;
	}
	std::string error;
	if (!s.gpu.build(error)) {
		fail("the shader did not compile: " + error);
		return false;
	}
	// Pixel buffer objects are part of OpenGL 2.1, and an extension before it.
	const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
	s.pixelBuffers = (version && std::atof(version) >= 2.1) || hasExtension("GL_ARB_pixel_buffer_object");
	glUseProgram(s.gpu.program);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_DITHER);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	static const float quad[8] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	s.built = true;
	return true;
}

// The offscreen target of this size and kind, made on first use. A few are kept, since a Display, a Video Probe and
// Syphon Out can each ask for their own size.
Target* targetFor(State& s, int width, int height, bool isFloat) {
	for (Target& t : s.targets) {
		if (t.width == width && t.height == height && t.isFloat == isFloat) {
			t.used = ++s.clock;
			return &t;
		}
	}
	constexpr size_t kMaxTargets = 6;
	if (s.targets.size() >= kMaxTargets) {
		size_t oldest = 0;
		for (size_t i = 1; i < s.targets.size(); ++i) {
			if (s.targets[i].used < s.targets[oldest].used) {
				oldest = i;
			}
		}
		glDeleteFramebuffersEXT(1, &s.targets[oldest].fbo);
		glDeleteTextures(1, &s.targets[oldest].texture);
		if (s.targets[oldest].pbo) {
			glDeleteBuffers(1, &s.targets[oldest].pbo);
		}
		s.targets.erase(s.targets.begin() + static_cast<long>(oldest));
	}
	Target t;
	t.width = width;
	t.height = height;
	t.isFloat = isFloat;
	t.used = ++s.clock;
	glGenTextures(1, &t.texture);
	glBindTexture(GL_TEXTURE_2D, t.texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	if (isFloat) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
	}
	else {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	}
	glGenFramebuffersEXT(1, &t.fbo);
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, t.fbo);
	glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, t.texture, 0);
	if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT || glGetError() != GL_NO_ERROR) {
		glDeleteFramebuffersEXT(1, &t.fbo);
		glDeleteTextures(1, &t.texture);
		return nullptr;
	}
	if (s.pixelBuffers) {
		const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u * (isFloat ? sizeof(float) : 1u);
		glGenBuffers(1, &t.pbo);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, t.pbo);
		glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, GL_STREAM_READ);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		if (glGetError() != GL_NO_ERROR) {
			glDeleteBuffers(1, &t.pbo);
			t.pbo = 0;
		}
	}
	s.targets.push_back(t);
	return &s.targets.back();
}

// The picture just drawn into `target`, copied to `dst` (width x height x 4 values of `type`).
//
// It goes through the target's pixel buffer when there is one: glReadPixels into a pixel buffer, then a copy out of
// the mapped buffer, gives the same bytes as glReadPixels straight into `dst`, and on Apple's driver takes well under
// half the time (measured on two 1440 x 1080 pictures: 1.1 ms against 2.5 ms). If the buffer cannot be mapped, or the
// driver says its contents were lost, the picture is read the plain way.
void readBack(Target& target, int width, int height, GLenum type, void* dst, size_t bytes) {
	if (target.pbo) {
		bool done = false;
		glBindBuffer(GL_PIXEL_PACK_BUFFER, target.pbo);
		glReadPixels(0, 0, width, height, GL_RGBA, type, nullptr);
		if (const void* mapped = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY)) {
			std::memcpy(dst, mapped, bytes);
			done = glUnmapBuffer(GL_PIXEL_PACK_BUFFER) == GL_TRUE;
		}
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		if (done && glGetError() == GL_NO_ERROR) {
			return;
		}
		// Not usable on this driver: drop the buffer and read this target the plain way from now on.
		for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
		}
		glDeleteBuffers(1, &target.pbo);
		target.pbo = 0;
	}
	glReadPixels(0, 0, width, height, GL_RGBA, type, dst);
}

bool render(VideoFrame& out, int width, int height, const ShapesParams& params, bool floatOut) {
	if (!wanted.load(std::memory_order_relaxed) || failed.load(std::memory_order_relaxed) || !ready.load(std::memory_order_relaxed)) {
		return false;
	}
	GLint maxSize = 0;
	State& s = state();
	std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock);
	if (!lock.owns_lock()) {
		return false;  // another thread is drawing a Shapes picture; this one goes to the CPU
	}
	GLFWwindow* const previous = glfwGetCurrentContext();
	glfwMakeContextCurrent(s.window);
	bool ok = false;
	if (s.built || build(s)) {
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
		Target* target = (width <= maxSize && height <= maxSize) ? targetFor(s, width, height, floatOut) : nullptr;
		if (target) {
			const shapes_field::Scene scene = shapes_field::makeScene(params, width, height, false);
			glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, target->fbo);
			glViewport(0, 0, width, height);
			s.gpu.setScene(scene, floatOut ? 255.f : 1.f);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
			if (floatOut) {
				out.beginF32(count);
				readBack(*target, width, height, GL_FLOAT, out.f.data(), count * sizeof(float));
			}
			else {
				out.format = FrameFormat::U8;
				out.resize(count);
				readBack(*target, width, height, GL_UNSIGNED_BYTE, out.data(), count);
			}
			ok = glGetError() == GL_NO_ERROR;
			if (!ok) {
				fail("OpenGL reported an error while drawing");
			}
			else if (!s.announced) {
				s.announced = true;
				INFO("Shapes is drawn on the graphics card (%s, %s)", glGetString(GL_RENDERER), glGetString(GL_VERSION));
			}
		}
	}
	glfwMakeContextCurrent(previous);
	return ok;
}

}  // namespace

void setEnabled(bool on) {
	wanted.store(on, std::memory_order_relaxed);
}

bool enabled() {
	return wanted.load(std::memory_order_relaxed);
}

void uiStep() {
	if (ready.load(std::memory_order_relaxed) || failed.load(std::memory_order_relaxed) || !wanted.load(std::memory_order_relaxed)) {
		return;
	}
	State& s = state();
	// A hidden 1x1 window is how GLFW gives out a context. The hints are GLFW-wide, so they go back to the defaults;
	// Rack sets its own again before it makes a window.
	glfwDefaultWindowHints();
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
	glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
	glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
	s.window = glfwCreateWindow(1, 1, "", nullptr, nullptr);
	glfwDefaultWindowHints();
	if (!s.window) {
		fail("no second OpenGL context could be made");
		return;
	}
	setShapesAccelerator(&render);
	ready.store(true);
}

const char* status() {
	return failed.load() ? failure.c_str() : "";
}

}  // namespace shapes_gpu
