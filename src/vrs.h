#pragma once
#include <d3d12.h>

// Fixed foveated rendering with D3D12 variable rate shading (Tier 2 shading rate image).
//
// Each eye gets a shading rate image: full rate in the centre of its field of view, 2x2 in a
// ring around it and 4x4 at the edges and in the parts of the centred frustum the lens never
// shows. It is bound on the game's command lists for the full-size 3D passes (depth buffer
// bound, viewport covering the backbuffer), so post-processing and UI keep full rate.
namespace vrs {

// Start-up (from DllMain): hooks the D3D12 device vtable on a worker thread, before the game creates
// its depth views, so the depth layer can map them to the game's depth buffers.
void HookDeviceEarly();

// Render thread, once the game's D3D12 queue is known: checks GPU support, hooks the queue.
void Init(ID3D12Device* device, ID3D12CommandQueue* queue);

// Lua thread (GetView): the eye's field of view and the frustum actually rendered for it,
// as tangents of the half angles {left, right, down, up} (left and down negative).
void SetEyeFov(int eye, const float actual[4], const float rendered[4]);

// Game thread, after each Present: the eye the next frame is drawn for (-1 = flat frame).
void SetFrameEye(int eye);

// Game thread, around Present (to detect command lists recorded while a frame is presented).
void SetPresenting(bool on);

// Game thread, in Present: (re)builds the images for the render size. perEye = the eye of a
// frame is known while it is recorded (otherwise both eyes share one mask).
void Update(UINT width, UINT height, bool perEye);

bool SetEnabled(bool on);  // false when the GPU or driver does not support it
bool IsEnabled();

// Depth layer. While enabled, the scene depth is copied at the start of each full-size 3D pass
// into a per-eye capture texture (single-sample 32-bit depth only), which rests in COPY_DEST.
bool EnableDepthCapture(bool on);  // false if the depth cannot be captured
// Game thread, in Present (before Update): this frame's captured depth for the eye, if any.
bool TakeDepth(int eye, ID3D12Resource** depth);
// Depth the game clears its scene depth to: 0 (reversed depth), 1 (standard), -1 not seen yet.
int  DepthClearValue();

} // namespace vrs
