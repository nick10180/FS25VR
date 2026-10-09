#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>

// What the Lua camera code needs to render one eye.
struct EyeView {
    int   eye;           // 0 = left, 1 = right
    float pos[3];        // eye position in recentred tracking space (metres * worldScale), camera axes
    float quat[4];       // eye orientation x,y,z,w in recentred tracking space
    float fovY;          // radians, for setFovY
    float offX, offY;    // for setProjectionOffset
    uint64_t frame;      // present index this view will be shown on
    bool  second;        // second frame of a stereo pair (the simulation is frozen on it)
};

namespace vr {

// Render thread (called from the swap chain hooks).
void OnSwapChainCreated(ID3D12CommandQueue* queue, IDXGISwapChain* swap);
void OnPresent(IDXGISwapChain* swap);
void OnPostPresent();
void OnResizeBuffers();

// Lua thread.
bool IsRunning();
bool GetView(EyeView& out);
void RequestRecenter();
void SetSymmetricFrustum(bool on);
// Plane stereo, step 1: the sizes the Lua mod must create its render overlays with; returns their
// number (0 = off; 1 = the right eye; 3 with quad views: + the left and right focus views). The
// bridge watches for those overlays' render targets. Off also ends plane stereo.
// renderScale: the game's 3D resolution scaling (the views' render resolution is the window's times it).
int PrepareOverlay(bool on, bool quad, float renderScale, uint32_t w[3], uint32_t h[3]);
// Step 2, once the overlays have rendered by themselves: the right eye (and the focus views) through
// the engine's main render path, into the render overlays just queued (in the order of step 1).
// Returns false if unavailable.
bool SetPlaneStereo(bool on);
bool IsCalibrating();
// The headset's recommended per-eye resolution and the focus views' share of its field of view.
void HeadsetInfo(uint32_t& recW, uint32_t& recH, float& focusW, float& focusH);
bool NextFrameIsSecondEye();  // main thread, before the frame runs  // the mod should draw the latency marker this frame
const char* Status();

} // namespace vr
