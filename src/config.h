#pragma once

struct Config {
    bool  enabled         = true;   // master switch; when false the DLL is a pure dinput8 passthrough
    bool  forceNoVsync    = true;   // the HMD paces the game, the desktop mirror must not
    int   presentLag      = 0;      // frames between a Lua camera update and the Present that shows it
    float worldScale      = 1.0f;   // >1 makes the world feel smaller
    float menuDistance    = 2.0f;   // metres to the flat menu screen
    float menuWidth       = 2.6f;   // metres
    bool  fitWindow       = true;   // shrink a window taller than the screen, keep rendering at full size
    bool  syncEyes        = true;   // freeze the simulation on second-eye frames
    int   syncPhase       = 0;      // 1 = freeze the other parity (if the engine pipelines physics)
    int   eyeOrder        = 0;      // 0 = left eye on even frames, 1 = right eye on even frames
    bool  symmetricFrustum = true;  // centred per-eye projection (screen-space effects assume it)
    bool  clipMouse       = true;   // keep the cursor inside the game window while it is focused
    bool  showCursor      = true;   // draw the mouse pointer into the headset image
    bool  asyncSubmit     = true;   // headset frame loop on its own thread; the game never waits for it
    bool  profile         = false;  // GPU timestamps + per-frame CSV
    bool  forceRender     = false;  // debug: do the full VR copy even when the headset is idle
    bool  deferPatches    = false;  // debug: skip the load-time engine scan (simulates a DRM-wrapped exe)
    bool  debugLog        = false;  // verbose per-frame logging
    // quad views (plane stereo, F11 menu): each eye's focus view, as the quad views layer without eye tracking
    float quadFocusWidth   = 0.5f;   // fraction of the eye's horizontal field of view (centred)
    float quadFocusHeight  = 0.45f;  // ... vertical
    float quadFocusSmoothing = 0.18f; // blended edge of the focus view (fraction of its size)
    // HUD panel (plane stereo): the game's HUD taken out of the eyes' images and shown as a panel in
    // front of the head (metres, head space)
    bool  hudPanel        = true;
    float hudDistance     = 1.0f;
    float hudWidth        = 1.0f;
    float hudOffsetY      = 0.0f;
};

extern Config g_config;
void LoadConfig();
