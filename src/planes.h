#pragma once

// Plane stereo: both eyes through the engine's main render path.
//
// Every frame the engine asks a "display plane provider" how many views to render and, per view,
// for its output, size, frustum and an offset from the camera (the infrastructure for several
// screens). Each view gets the whole main pipeline: shadows, decals, mirrors, post-processing with
// its own history. The provider normally has one view: the game window.
//
// Here a second view is added: the right eye, offset from the camera (the left eye) by the eye
// distance, with its own frustum, rendered into the output texture of a render overlay that the
// Lua mod created for this (its DisplayTexture plane). The bridge picks that texture up like the
// overlay-stereo image. Quad views add two more such views: each eye's focus view.
namespace planes {

// Finds the engine functions (after the game code is decrypted); hooks the provider lookup.
bool Install();

// Plane 0 is the window (left eye); planes 1.. are render overlays' textures: the right eye, and
// with quad views the left and right focus views.
constexpr int kMaxPlanes = 4;

// Lua thread: the last `overlays` render overlays just queued with updateRenderOverlay become the
// outputs of planes 1.. in queue order (true when found). Off: one view again.
bool SetStereo(bool on, int overlays = 1);
bool Active();

// A plane's view relative to the left eye (camera space, metres; x right, y up, -z forward) and its
// frustum tangents (left, right, down, up), for the frame being rendered.
void SetPlaneView(int plane, const float offset[3], const float tans[4]);

} // namespace planes
