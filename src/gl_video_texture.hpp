#pragma once

struct OpenGlTextureInfo {
	unsigned int textureName = 0;
	unsigned int textureTarget = 0x0DE1u;
	int width = 0;
	int height = 0;
	bool flipped = true;
	double timestampSeconds = 0.0;
};

class OpenGlVideoTexture {
public:
	bool uploadRgba(const unsigned char* rgba, int width, int height, bool flipped = true);
	void destroy();
	bool isReady() const;
	OpenGlTextureInfo info() const;

private:
	unsigned int textureName = 0;
	int textureWidth = 0;
	int textureHeight = 0;
	bool textureFlipped = true;
};
