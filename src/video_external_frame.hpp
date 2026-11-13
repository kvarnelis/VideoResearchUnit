#pragma once

#include <vector>

// External frames (Syphon In). Each receiver claims a frame id and releases it on destruction.
constexpr int kExternalFrameCount = 64;

struct ExternalVideoFrame {
	std::vector<unsigned char> rgba;
	int width = 0;
	int height = 0;
	double timestampSeconds = 0.0;
};

// Returns 0 when every id is in use.
int videoExternalFrameAllocate();
// Clears the frame and makes its id available again.
void videoExternalFrameRelease(int frameId);
void videoExternalFramePublish(int frameId, const unsigned char* rgba, int width, int height, double timestampSeconds);
// Copies the latest frame into frame, reusing its capacity. False when no frame is held.
bool videoExternalFrameRead(int frameId, ExternalVideoFrame& frame);
void videoExternalFrameClear(int frameId);
