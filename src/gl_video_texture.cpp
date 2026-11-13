#include "gl_video_texture.hpp"
#include "plugin.hpp"

bool OpenGlVideoTexture::uploadRgba(const unsigned char* rgba, int width, int height, bool flipped) {
	if (!rgba || width <= 0 || height <= 0) {
		return false;
	}

	if (!textureName) {
		GLuint newTexture = 0;
		glGenTextures(1, &newTexture);
		textureName = newTexture;
		textureWidth = 0;
		textureHeight = 0;
	}

	if (!textureName) {
		return false;
	}

	// Rack's own widgets share this GL context, so put back the 2D binding and unpack alignment
	// that were current before the upload.
	GLint previousTexture = 0;
	GLint previousUnpackAlignment = 4;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousUnpackAlignment);

	glBindTexture(GL_TEXTURE_2D, textureName);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	if (textureWidth != width || textureHeight != height) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
		textureWidth = width;
		textureHeight = height;
	}
	else {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	}

	glPixelStorei(GL_UNPACK_ALIGNMENT, previousUnpackAlignment);
	glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
	textureFlipped = flipped;
	return true;
}

void OpenGlVideoTexture::destroy() {
	if (!textureName) {
		return;
	}

	GLuint doomedTexture = textureName;
	glDeleteTextures(1, &doomedTexture);
	textureName = 0;
	textureWidth = 0;
	textureHeight = 0;
}

bool OpenGlVideoTexture::isReady() const {
	return textureName != 0 && textureWidth > 0 && textureHeight > 0;
}

OpenGlTextureInfo OpenGlVideoTexture::info() const {
	OpenGlTextureInfo texture;
	texture.textureName = textureName;
	texture.textureTarget = GL_TEXTURE_2D;
	texture.width = textureWidth;
	texture.height = textureHeight;
	texture.flipped = textureFlipped;
	return texture;
}
