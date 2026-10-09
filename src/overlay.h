#pragma once
#include <d3d12.h>

// Second eye from an engine render overlay. The Lua mod renders the right eye through a render
// overlay (createRenderOverlay / updateRenderOverlay: the engine renders its whole scene a second
// time in the same frame) at a size no other render target has. The render targets created at
// that size are the overlay's; the one last made readable by shaders in a frame is its final image.
// Several overlays (quad views: the focus views too) are told apart by their sizes.
namespace overlay {

constexpr int kMaxSlots = 3;

void Install(ID3D12Device* device);  // once the game's device is known
// The overlays' sizes, all different (count 0 = no overlay).
void SetSizes(int count, const UINT* w, const UINT* h);
void OnFrameEnd();                   // at Present (game thread), before Image()

// An overlay's final image of the frame being presented, its view format and the state the game
// left it in; null if the overlay was not rendered this frame.
ID3D12Resource* Image(int slot, DXGI_FORMAT& format, D3D12_RESOURCE_STATES& state);

// Keeps the current images as the overlays' outputs from now on (true), or picks them per frame again.
void LockImage(bool on);

} // namespace overlay
