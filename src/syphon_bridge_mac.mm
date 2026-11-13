#include "syphon_bridge.hpp"

#ifdef RASTERLAB_WITH_SYPHON

#import <Cocoa/Cocoa.h>
#import <OpenGL/gl.h>
#import <OpenGL/glext.h>
#import <OpenGL/OpenGL.h>
// Vendored, prefixed copy of Syphon.framework (see scripts/build-syphon-framework.sh).
#import <RLSyphon/RLSyphon.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <sstream>

namespace {
NSString* nsStringFromStd(const std::string& value) {
	return [NSString stringWithUTF8String:value.empty() ? "Video Research Unit" : value.c_str()];
}

std::string stdStringFromNSString(NSString* value) {
	return value ? std::string([value UTF8String]) : std::string();
}

// Name shown in the Syphon server list, "App / Server" when both are known.
std::string displayNameForServer(NSDictionary* serverDescription) {
	NSString* name = [serverDescription objectForKey:RLSyphonServerDescriptionNameKey];
	NSString* appName = [serverDescription objectForKey:RLSyphonServerDescriptionAppNameKey];
	const std::string serverName = stdStringFromNSString(name);
	const std::string applicationName = stdStringFromNSString(appName);
	if (!applicationName.empty() && !serverName.empty()) {
		return applicationName + " / " + serverName;
	}
	if (!serverName.empty()) {
		return serverName;
	}
	return applicationName.empty() ? "Unnamed Syphon Server" : applicationName;
}

#ifndef GL_TEXTURE_BINDING_RECTANGLE_EXT
#define GL_TEXTURE_BINDING_RECTANGLE_EXT 0x84F6
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING GL_VERTEX_ARRAY_BINDING_APPLE
#endif

// Saves the GL state that Syphon's publish/bind calls and the plugin's readback may change, and
// restores it on destruction. Rack draws every widget into one shared GL context, so any binding,
// enable or pixel-store value left behind here leaks into the widgets drawn after this one.
class GlStateGuard {
public:
	GlStateGuard() {
		glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding2D);
		glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE_EXT, &textureBindingRectangle);
		glGetIntegerv(GL_CURRENT_PROGRAM, &program);
		glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
		glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_EXT, &drawFramebuffer);
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, &readFramebuffer);
		glGetIntegerv(GL_VIEWPORT, viewport);
		glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
		glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
		glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment);
		glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpackRowLength);
		glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
		glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
		glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEquationRgb);
		glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEquationAlpha);
		glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
		glGetBooleanv(GL_COLOR_WRITEMASK, colorWriteMask);
		blendEnabled = glIsEnabled(GL_BLEND);
		depthTestEnabled = glIsEnabled(GL_DEPTH_TEST);
		stencilTestEnabled = glIsEnabled(GL_STENCIL_TEST);
		cullFaceEnabled = glIsEnabled(GL_CULL_FACE);
		scissorTestEnabled = glIsEnabled(GL_SCISSOR_TEST);
	}

	~GlStateGuard() {
		// Restore bindings on the unit that was active, then the unit itself.
		glActiveTexture(static_cast<GLenum>(activeTexture));
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(textureBinding2D));
		glBindTexture(GL_TEXTURE_RECTANGLE_EXT, static_cast<GLuint>(textureBindingRectangle));
		glBindVertexArrayAPPLE(static_cast<GLuint>(vertexArray));
		glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
		glUseProgram(static_cast<GLuint>(program));
		glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, static_cast<GLuint>(drawFramebuffer));
		glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, static_cast<GLuint>(readFramebuffer));
		glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
		glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
		glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
		glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, unpackRowLength);
		glBlendFuncSeparate(static_cast<GLenum>(blendSrcRgb), static_cast<GLenum>(blendDstRgb),
							static_cast<GLenum>(blendSrcAlpha), static_cast<GLenum>(blendDstAlpha));
		glBlendEquationSeparate(static_cast<GLenum>(blendEquationRgb), static_cast<GLenum>(blendEquationAlpha));
		glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
		glColorMask(colorWriteMask[0], colorWriteMask[1], colorWriteMask[2], colorWriteMask[3]);
		setEnabled(GL_BLEND, blendEnabled);
		setEnabled(GL_DEPTH_TEST, depthTestEnabled);
		setEnabled(GL_STENCIL_TEST, stencilTestEnabled);
		setEnabled(GL_CULL_FACE, cullFaceEnabled);
		setEnabled(GL_SCISSOR_TEST, scissorTestEnabled);
	}

	GlStateGuard(const GlStateGuard&) = delete;
	GlStateGuard& operator=(const GlStateGuard&) = delete;

private:
	static void setEnabled(GLenum capability, GLboolean enabled) {
		if (enabled) {
			glEnable(capability);
		}
		else {
			glDisable(capability);
		}
	}

	GLint activeTexture = GL_TEXTURE0;
	GLint textureBinding2D = 0;
	GLint textureBindingRectangle = 0;
	GLint program = 0;
	GLint arrayBuffer = 0;
	GLint vertexArray = 0;
	GLint drawFramebuffer = 0;
	GLint readFramebuffer = 0;
	GLint viewport[4] = {0, 0, 0, 0};
	GLint scissorBox[4] = {0, 0, 0, 0};
	GLint packAlignment = 4;
	GLint unpackAlignment = 4;
	GLint unpackRowLength = 0;
	GLint blendSrcRgb = GL_ONE;
	GLint blendDstRgb = GL_ZERO;
	GLint blendSrcAlpha = GL_ONE;
	GLint blendDstAlpha = GL_ZERO;
	GLint blendEquationRgb = GL_FUNC_ADD;
	GLint blendEquationAlpha = GL_FUNC_ADD;
	GLfloat clearColor[4] = {0.f, 0.f, 0.f, 0.f};
	GLboolean colorWriteMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
	GLboolean blendEnabled = GL_FALSE;
	GLboolean depthTestEnabled = GL_FALSE;
	GLboolean stencilTestEnabled = GL_FALSE;
	GLboolean cullFaceEnabled = GL_FALSE;
	GLboolean scissorTestEnabled = GL_FALSE;
};
}

class MacSyphonOutputBridge final : public SyphonOutputBridge {
public:
	~MacSyphonOutputBridge() override {
		@autoreleasepool {
			if (server) {
				[server stop];
				[server release];
				server = nil;
			}
		}
	}

	bool isAvailable() const override {
		return true;
	}

	void setServerName(const std::string& name) override {
		const std::string requested = name.empty() ? "Video Research Unit" : name;
		if (requested == serverName) {
			return;
		}
		serverName = requested;
		@autoreleasepool {
			if (server) {
				// Changing the name re-announces the server, so do it only on change.
				server.name = nsStringFromStd(serverName);
			}
		}
	}

	bool publishTexture(const SyphonTextureInfo& texture) override {
		if (!texture.textureName || texture.width <= 0 || texture.height <= 0) {
			lastErrorText = "No OpenGL texture to publish";
			return false;
		}

		@autoreleasepool {
			CGLContextObj currentContext = CGLGetCurrentContext();
			if (!currentContext) {
				lastErrorText = "No current CGL context for Syphon output";
				return false;
			}

			GlStateGuard glState;
			if (!server || serverContext != currentContext) {
				if (server) {
					[server stop];
					[server release];
					server = nil;
				}

				server = [[RLSyphonOpenGLServer alloc] initWithName:nsStringFromStd(serverName) context:currentContext options:nil];
				serverContext = currentContext;
				if (!server) {
					lastErrorText = "Could not create Syphon output server";
					return false;
				}
			}

			NSRect region = NSMakeRect(0.0, 0.0, texture.width, texture.height);
			NSSize size = NSMakeSize(texture.width, texture.height);
			[server publishFrameTexture:static_cast<GLuint>(texture.textureName)
						  textureTarget:static_cast<GLenum>(texture.textureTarget)
							imageRegion:region
					  textureDimensions:size
								flipped:texture.flipped ? YES : NO];
		}

		lastErrorText.clear();
		return true;
	}

	bool publishRgbaFrame(const unsigned char*, int, int) override {
		lastErrorText = "Syphon output requires an OpenGL texture";
		return false;
	}

	std::string lastError() const override {
		return lastErrorText;
	}

private:
	RLSyphonOpenGLServer* server = nil;
	CGLContextObj serverContext = nullptr;
	std::string serverName = "Video Research Unit";
	std::string lastErrorText;
};

class MacSyphonInputBridge final : public SyphonInputBridge {
public:
	~MacSyphonInputBridge() override {
		@autoreleasepool {
			if (currentImage) {
				[currentImage release];
				currentImage = nil;
			}
			if (client) {
				[client stop];
				[client release];
				client = nil;
			}
		}
	}

	bool isAvailable() const override {
		return true;
	}

	std::vector<std::string> serverNames() const override {
		std::vector<std::string> names;
		@autoreleasepool {
			NSArray* servers = [[RLSyphonServerDirectory sharedDirectory] servers];
			for (NSDictionary* serverDescription in servers) {
				names.push_back(displayNameForServer(serverDescription));
			}
		}
		return names;
	}

	bool connect(const std::string& serverName) override {
		@autoreleasepool {
			NSDictionary* match = nil;
			NSArray* servers = [[RLSyphonServerDirectory sharedDirectory] servers];
			for (NSDictionary* serverDescription in servers) {
				if (!serverName.empty() && displayNameForServer(serverDescription) == serverName) {
					match = serverDescription;
					break;
				}
			}

			if (!match) {
				lastErrorText = "Syphon server is not running: " + serverName;
				return false;
			}

			CGLContextObj currentContext = CGLGetCurrentContext();
			if (!currentContext) {
				lastErrorText = "No current CGL context for Syphon input";
				return false;
			}

			if (client) {
				[client stop];
				[client release];
				client = nil;
			}
			if (currentImage) {
				[currentImage release];
				currentImage = nil;
			}

			client = [[RLSyphonOpenGLClient alloc] initWithServerDescription:match context:currentContext options:nil newFrameHandler:nil];
			clientContext = currentContext;
			selectedServerName = displayNameForServer(match);
			if (!client || !client.isValid) {
				lastErrorText = "Could not connect to Syphon input server";
				return false;
			}
		}

		lastErrorText.clear();
		return true;
	}

	bool copyLatestTexture(SyphonTextureInfo& texture) override {
		@autoreleasepool {
			CGLContextObj currentContext = CGLGetCurrentContext();
			if (!currentContext) {
				lastErrorText = "No current CGL context for Syphon input";
				return false;
			}

			GlStateGuard glState;
			if (!client || clientContext != currentContext || !client.isValid) {
				if (!connect(selectedServerName)) {
					return false;
				}
			}

			RLSyphonOpenGLImage* image = [client newFrameImage];
			if (!image) {
				lastErrorText = "Syphon input has no frame yet";
				return false;
			}

			if (currentImage) {
				[currentImage release];
			}
			currentImage = image;

			texture.textureName = currentImage.textureName;
			texture.textureTarget = GL_TEXTURE_RECTANGLE_EXT;
			texture.width = static_cast<int>(currentImage.textureSize.width);
			texture.height = static_cast<int>(currentImage.textureSize.height);
			texture.flipped = false;
		}

		lastErrorText.clear();
		return texture.textureName != 0 && texture.width > 0 && texture.height > 0;
	}

	bool copyLatestRgbaFrame(std::vector<unsigned char>& rgba, SyphonFrameInfo& info) override {
		SyphonTextureInfo texture;
		if (!copyLatestTexture(texture)) {
			return false;
		}

		GlStateGuard glState;

		const size_t rowBytes = static_cast<size_t>(texture.width) * 4u;
		const size_t byteCount = rowBytes * static_cast<size_t>(texture.height);
		std::vector<unsigned char> glRows(byteCount);
		GLuint readFramebuffer = 0;
		glGenFramebuffersEXT(1, &readFramebuffer);
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, readFramebuffer);
		glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, static_cast<GLenum>(texture.textureTarget),
								  static_cast<GLuint>(texture.textureName), 0);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, texture.width, texture.height, GL_RGBA, GL_UNSIGNED_BYTE, glRows.data());
		glDeleteFramebuffersEXT(1, &readFramebuffer);

		// GL rows run bottom-up. VRU frames are top-down, like the CPU renderer's output.
		rgba.resize(byteCount);
		for (int row = 0; row < texture.height; ++row) {
			std::memcpy(rgba.data() + static_cast<size_t>(row) * rowBytes,
						glRows.data() + static_cast<size_t>(texture.height - 1 - row) * rowBytes,
						rowBytes);
		}
		info.width = texture.width;
		info.height = texture.height;
		info.timestampSeconds = texture.timestampSeconds;
		return true;
	}

	std::string lastError() const override {
		return lastErrorText;
	}

private:
	RLSyphonOpenGLClient* client = nil;
	RLSyphonOpenGLImage* currentImage = nil;
	CGLContextObj clientContext = nullptr;
	std::string selectedServerName;
	std::string lastErrorText;
};

std::unique_ptr<SyphonOutputBridge> createSyphonOutputBridge() {
	return std::make_unique<MacSyphonOutputBridge>();
}

std::unique_ptr<SyphonInputBridge> createSyphonInputBridge() {
	return std::make_unique<MacSyphonInputBridge>();
}

#endif
