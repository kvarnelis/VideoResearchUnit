// Round-trip test for the VRU Syphon bridge, run in one process.
//
// It creates an offscreen legacy OpenGL 2.1 context (the same profile Rack's
// NanoVG GL2 backend uses), publishes a known RGBA pattern through the output
// bridge, and reads it back through the input bridge. The received bytes must
// match the published bytes exactly.
//
// Build and run with: make test   (requires RASTERLAB_SYPHON=1 on macOS)

#include "syphon_bridge.hpp"

#import <Foundation/Foundation.h>
#import <OpenGL/gl.h>
#import <OpenGL/OpenGL.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

const int kWidth = 64;
const int kHeight = 48;
const double kTimeoutSeconds = 10.0;

void pumpRunLoop(double seconds) {
	[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

// Each pixel encodes its own position so that any flip, swap or offset shows up.
void fillPattern(std::vector<unsigned char>& rgba) {
	rgba.resize(static_cast<size_t>(kWidth) * kHeight * 4);
	for (int y = 0; y < kHeight; ++y) {
		for (int x = 0; x < kWidth; ++x) {
			unsigned char* px = &rgba[(static_cast<size_t>(y) * kWidth + x) * 4];
			px[0] = static_cast<unsigned char>(x * 4);
			px[1] = static_cast<unsigned char>(y * 5);
			px[2] = static_cast<unsigned char>(x ^ y);
			px[3] = 255;
		}
	}
	// Mark the top-left pixel so an upside-down result is easy to recognise.
	rgba[0] = 250;
	rgba[1] = 3;
	rgba[2] = 7;
}

CGLContextObj createOffscreenContext() {
	CGLPixelFormatAttribute attributes[] = {
		kCGLPFAAccelerated,
		kCGLPFAOpenGLProfile, static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_Legacy),
		kCGLPFAColorSize, static_cast<CGLPixelFormatAttribute>(24),
		kCGLPFAAlphaSize, static_cast<CGLPixelFormatAttribute>(8),
		static_cast<CGLPixelFormatAttribute>(0)
	};
	CGLPixelFormatObj format = nullptr;
	GLint count = 0;
	if (CGLChoosePixelFormat(attributes, &format, &count) != kCGLNoError || !format) {
		return nullptr;
	}
	CGLContextObj context = nullptr;
	CGLError error = CGLCreateContext(format, nullptr, &context);
	CGLDestroyPixelFormat(format);
	if (error != kCGLNoError) {
		return nullptr;
	}
	return context;
}

}

int main() {
	@autoreleasepool {
		CGLContextObj context = createOffscreenContext();
		if (!context) {
			std::fprintf(stderr, "FAIL: could not create an offscreen OpenGL context\n");
			return 2;
		}
		CGLSetCurrentContext(context);

		const std::string serverName = "VRU Test " + std::to_string(static_cast<long>([[NSProcessInfo processInfo] processIdentifier]));
		std::vector<unsigned char> sent;
		fillPattern(sent);

		GLuint sourceTexture = 0;
		glGenTextures(1, &sourceTexture);
		glBindTexture(GL_TEXTURE_2D, sourceTexture);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kWidth, kHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, sent.data());
		glBindTexture(GL_TEXTURE_2D, 0);

		std::unique_ptr<SyphonOutputBridge> output = createSyphonOutputBridge();
		std::unique_ptr<SyphonInputBridge> input = createSyphonInputBridge();
		if (!output->isAvailable() || !input->isAvailable()) {
			std::fprintf(stderr, "FAIL: Syphon bridge reports unavailable (%s)\n", output->lastError().c_str());
			return 2;
		}
		output->setServerName(serverName);

		SyphonTextureInfo published;
		published.textureName = sourceTexture;
		published.textureTarget = GL_TEXTURE_2D;
		published.width = kWidth;
		published.height = kHeight;
		published.flipped = true;

		// Publish and wait for the server to show up in the directory.
		std::string matchedName;
		NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:kTimeoutSeconds];
		while (matchedName.empty() && [deadline timeIntervalSinceNow] > 0) {
			output->publishTexture(published);
			pumpRunLoop(0.1);
			for (const std::string& name : input->serverNames()) {
				if (name.find(serverName) != std::string::npos) {
					matchedName = name;
					break;
				}
			}
		}
		if (matchedName.empty()) {
			std::fprintf(stderr, "FAIL: server '%s' never appeared in the Syphon directory\n", serverName.c_str());
			return 1;
		}
		if (!input->connect(matchedName)) {
			std::fprintf(stderr, "FAIL: connect failed: %s\n", input->lastError().c_str());
			return 1;
		}

		// Keep publishing until a frame arrives, then compare it byte for byte.
		std::vector<unsigned char> received;
		SyphonFrameInfo info;
		bool gotFrame = false;
		deadline = [NSDate dateWithTimeIntervalSinceNow:kTimeoutSeconds];
		while (!gotFrame && [deadline timeIntervalSinceNow] > 0) {
			output->publishTexture(published);
			pumpRunLoop(0.05);
			gotFrame = input->copyLatestRgbaFrame(received, info);
		}
		if (!gotFrame) {
			std::fprintf(stderr, "FAIL: no frame received: %s\n", input->lastError().c_str());
			return 1;
		}

		size_t mismatches = 0;
		if (info.width != kWidth || info.height != kHeight || received.size() != sent.size()) {
			std::fprintf(stderr, "FAIL: size mismatch: got %dx%d (%zu bytes), want %dx%d\n",
						 info.width, info.height, received.size(), kWidth, kHeight);
			return 1;
		}
		for (size_t i = 0; i < sent.size(); ++i) {
			if (sent[i] != received[i]) {
				++mismatches;
			}
		}

		std::printf("server: %s\n", matchedName.c_str());
		std::printf("frame: %dx%d, %zu of %zu bytes differ\n", info.width, info.height, mismatches, sent.size());
		output.reset();
		input.reset();
		glDeleteTextures(1, &sourceTexture);
		CGLSetCurrentContext(nullptr);
		CGLDestroyContext(context);

		if (mismatches != 0) {
			std::printf("FAIL\n");
			return 1;
		}
		std::printf("PASS\n");
		return 0;
	}
}
