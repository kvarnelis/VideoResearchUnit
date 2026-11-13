#include "display_capture.hpp"

// PNG encoding for the Display module's SHOT button, using stb_image_write.h (v0.92, public domain)
// from the Rack SDK's dep/include folder. Nothing else in the plugin includes it.
//
// How the header is kept contained:
//  - Its implementation section is compiled here, inside an anonymous namespace, so none of its
//    (non-static) functions, such as stbi_write_png and stbi__crc32, can clash with another copy of
//    stb in Rack or in another plugin when the plugin is loaded.
//  - The system headers it includes are pulled in first, so they stay outside that namespace.
//  - INCLUDE_STB_IMAGE_WRITE_H is defined first, which skips the header's extern "C" declarations.
//  - Only stbi_write_png_to_mem() is used. The file and BMP/TGA functions go unused, so the
//    unused-function warning is switched off for this one include.
//  - stbi__crc32() fills a function-local static table on first use, which is not thread safe, so
//    every call into stb takes a mutex.
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mutex>

#define INCLUDE_STB_IMAGE_WRITE_H

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

namespace {
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
}  // namespace

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace display_capture {

bool encodePng(const unsigned char* rgba, int width, int height, std::vector<unsigned char>& out, std::string& error) {
	out.clear();
	// stb stores sizes in int and its filter buffer is (width * 3 + 1) * height; keep well inside that.
	if (!rgba || width <= 0 || height <= 0 || width > 16384 || height > 16384) {
		error = "bad image size";
		return false;
	}
	const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

	// The Display frame is opaque, so the PNG is plain RGB: smaller and no alpha to get wrong.
	std::vector<unsigned char> rgb(pixels * 3);
	for (std::size_t i = 0; i < pixels; i++) {
		rgb[i * 3 + 0] = rgba[i * 4 + 0];
		rgb[i * 3 + 1] = rgba[i * 4 + 1];
		rgb[i * 3 + 2] = rgba[i * 4 + 2];
	}

	static std::mutex stbMutex;
	int length = 0;
	unsigned char* png = nullptr;
	{
		std::lock_guard<std::mutex> lock(stbMutex);
		png = stbi_write_png_to_mem(rgb.data(), width * 3, width, height, 3, &length);
	}
	if (!png || length <= 0) {
		free(png);
		error = "PNG encoder failed";
		return false;
	}
	out.assign(png, png + length);
	free(png);
	return true;
}

}  // namespace display_capture
