#pragma once
#include <d3d12.h>

// The game's HUD as a texture of its own. The engine draws the whole 2D GUI (base game and mods)
// into a window-sized render target named "textureOverlayTexture" and blends it over the scene
// only in its final pass. While active, the HUD is copied out of that target when the game makes it
// readable, and the target is cleared, so the eyes' images come without the flat HUD and the bridge
// can place the copy in 3D itself.
namespace hud {

void Install(ID3D12Device* device);  // once the game's device is known

// Copy (and remove from the game's image) from now on, or leave the game's HUD alone.
void SetActive(bool on);

// At Present (game thread): the copy made in the frame being presented (PIXEL_SHADER_RESOURCE,
// sRGB format, premultiplied colours, alpha = how much of the scene shows through), or null if the
// frame drew no HUD.
ID3D12Resource* FrameImage();

} // namespace hud
