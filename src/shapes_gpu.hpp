#pragma once

// Shapes (VRU-21) drawn on the graphics card. See src/shapes_gpu.cpp.
namespace shapes_gpu {

// The setting ("Draw Shapes on the graphics card" in the Display menu, saved with Rack's settings).
void setEnabled(bool on);
bool enabled();

// Call from the UI thread (a widget's step). The first call with the setting on makes the hidden OpenGL context the
// renderer draws in and hooks it into renderShapes(); later calls cost one load. Never touches Rack's own context.
void uiStep();

// "" while it is working or has not been tried; otherwise why the CPU code is being used instead.
const char* status();

}  // namespace shapes_gpu
