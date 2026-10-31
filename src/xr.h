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
void SetSyncPhase(int phase);  // which frame of a pair the simulation is frozen on (see fs25vr.ini)
int  GetSyncPhase();
// Screen area {left, right, bottom, top} (0..1, y up) that both eyes see within hudArea of
// their field of view; false until both eyes have been rendered.
bool GetHudRect(float rect[4]);
// Horizontal shift (screen units) of the HUD in each eye's image so it appears at hudDepth;
// false for a HUD at infinity (hudDepth=0) or before both eyes have been rendered.
bool GetHudShift(float shift[2]);
void SetHudArea(float area);   // share of the field of view for the HUD (the mod's size presets)
void SetHudLower(float lower); // moves the HUD area down (share of the image height)
void SetHudDepth(float depth); // metres the HUD appears at (0 = infinity)
void  SetWorldScale(float scale); // >1 makes the world feel smaller
float GetWorldScale();
int  GetHudEye();              // 0 = HUD in both eyes, 1 = left only, 2 = right only
// The game camera's clip distances (world units), for the depth layer.
void SetDepthRange(float nearClip, float farClip);
bool IsCalibrating();
bool NextFrameIsSecondEye();  // main thread, before the frame runs  // the mod should draw the latency marker this frame
const char* Status();

} // namespace vr
