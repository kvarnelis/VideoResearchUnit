#pragma once

#include "gl_video_texture.hpp"

#include <memory>
#include <string>
#include <vector>

using SyphonTextureInfo = OpenGlTextureInfo;

struct SyphonFrameInfo {
	int width = 0;
	int height = 0;
	double timestampSeconds = 0.0;
};

class SyphonOutputBridge {
public:
	virtual ~SyphonOutputBridge() = default;
	virtual bool isAvailable() const = 0;
	virtual void setServerName(const std::string& name) = 0;
	virtual bool publishTexture(const SyphonTextureInfo& texture) = 0;
	virtual bool publishRgbaFrame(const unsigned char* rgba, int width, int height) = 0;
	virtual std::string lastError() const = 0;
};

class SyphonInputBridge {
public:
	virtual ~SyphonInputBridge() = default;
	virtual bool isAvailable() const = 0;
	virtual std::vector<std::string> serverNames() const = 0;
	virtual bool connect(const std::string& serverName) = 0;
	virtual bool copyLatestTexture(SyphonTextureInfo& texture) = 0;
	virtual bool copyLatestRgbaFrame(std::vector<unsigned char>& rgba, SyphonFrameInfo& info) = 0;
	virtual std::string lastError() const = 0;
};

class NullSyphonOutputBridge final : public SyphonOutputBridge {
public:
	bool isAvailable() const override {
		return false;
	}

	void setServerName(const std::string&) override {
	}

	bool publishTexture(const SyphonTextureInfo&) override {
		return false;
	}

	bool publishRgbaFrame(const unsigned char*, int, int) override {
		return false;
	}

	std::string lastError() const override {
		return "Syphon bridge is not linked in this build";
	}
};

class NullSyphonInputBridge final : public SyphonInputBridge {
public:
	bool isAvailable() const override {
		return false;
	}

	std::vector<std::string> serverNames() const override {
		return {};
	}

	bool connect(const std::string&) override {
		return false;
	}

	bool copyLatestTexture(SyphonTextureInfo&) override {
		return false;
	}

	bool copyLatestRgbaFrame(std::vector<unsigned char>&, SyphonFrameInfo&) override {
		return false;
	}

	std::string lastError() const override {
		return "Syphon bridge is not linked in this build";
	}
};

std::unique_ptr<SyphonOutputBridge> createSyphonOutputBridge();
std::unique_ptr<SyphonInputBridge> createSyphonInputBridge();
