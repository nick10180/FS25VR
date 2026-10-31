#pragma once

struct Config {
    bool  enabled         = true;   // master switch; when false the DLL is a pure dinput8 passthrough
    bool  forceNoVsync    = true;   // the HMD paces the game, the desktop mirror must not
    int   presentLag      = 0;      // frames between a Lua camera update and the Present that shows it
    float worldScale      = 1.0f;   // >1 makes the world feel smaller
    float menuDistance    = 2.0f;   // metres to the flat menu screen
    float menuWidth       = 2.6f;   // metres
    float hudArea         = 0.45f;  // share of the field of view the HUD is moved into (0 = leave it)
    float hudDepth        = 0.0f;   // metres the HUD appears at (0 = infinity, as drawn by the game)
    float hudLower        = 0.08f;  // moves the HUD area down by this share of the image height
    int   hudEye          = 0;      // HUD in 0 = both eyes, 1 = left eye only, 2 = right eye only
    bool  fitWindow       = true;   // shrink a window taller than the screen, keep rendering at full size
    bool  syncEyes        = true;   // freeze the simulation on second-eye frames
    int   syncPhase       = 0;      // 1 = freeze the other parity (if the engine pipelines physics)
    int   eyeOrder        = 0;      // 0 = left eye on even frames, 1 = right eye on even frames
    bool  symmetricFrustum = true;  // centred per-eye projection (screen-space effects assume it)
    bool  clipMouse       = true;   // keep the cursor inside the game window while it is focused
    bool  showCursor      = true;   // draw the mouse pointer into the headset image
    bool  asyncSubmit     = true;   // headset frame loop on its own thread; the game never waits for it
    bool  depthLayer      = true;   // send the game's depth to the headset runtime (better reprojection)
    int   vrMode          = 0;      // 0 = VR if a headset is ready at start, 1 = always VR, 2 = never
    int   renderWidth     = 0;      // render size written into the game's game.xml at start-up
    int   renderHeight    = 0;      //   (0 = leave game.xml alone)
    bool  foveated        = true;   // fixed foveated rendering (variable rate shading), F5 toggles
    float foveaInner      = 0.40f;  // full-rate centre, as a fraction of the field of view to each side
    float foveaOuter      = 0.65f;  // 2x2 ring up to here, 4x4 beyond (and where the lens never shows)
    bool  profile         = false;  // GPU timestamps + per-frame CSV
    bool  forceRender     = false;  // debug: do the full VR copy even when the headset is idle
    bool  deferPatches    = false;  // debug: skip the load-time engine scan (simulates a DRM-wrapped exe)
    bool  debugLog        = false;  // verbose per-frame logging
};

extern Config g_config;
void LoadConfig();
