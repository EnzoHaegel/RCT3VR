#pragma once
#include <d3d9.h>

// Diagnostics for locating the game's camera field of view: finds stored copies
// of the current scene projection matrix in memory and traps the instructions
// that write them (hardware data breakpoints), logging code addresses and stack.
namespace Probe {

// Remembers the projection of the first perspective scene draw of each frame.
void OnDraw();
void OnSetVertexShaderConstantF(UINT start, const float* data, UINT count, const void* caller);
void OnFrameEnd();
// Traces the next frame's projection uploads, then arms the breakpoints.
void Run();

// Projection (row-vector) of the first perspective scene draw of the last frame.
bool LastSceneProjection(float out[4][4]);

} // namespace Probe
