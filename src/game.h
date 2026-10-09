#pragma once
#include <cstdint>

// Locates the engine's Luau API and patches two engine script bindings:
//  - setStereoRendering (an empty stub in FS25) becomes the entry point that hands the Lua mod
//    a table of VR functions;
//  - isHeadTrackingAvailable reports true while the headset is active, which makes vehicle
//    interior cameras use a stable, unsmoothed seat-fixed head node.
// Call once at load (final=false) and again once the game is running (final=true): with Steam's
// DRM wrapper the game code is only decrypted after the game starts. Safe to call repeatedly.
bool InstallGamePatches(bool final);

// Byte pattern search in the game code ("48 8B ?? ..."); null if not found or ambiguous.
uint8_t* GameFindPattern(const char* pattern);
