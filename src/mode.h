#pragma once

// VR or flat play (monitor, TV, projector), decided automatically when the game starts: VR if a
// headset is ready, otherwise the bridge stays idle and the game keeps its own display settings.
namespace mode {

// DllMain: hooks the game's file opens, to catch the moment it reads its game.xml.
void Install();

// Decided once per start (vrMode in fs25vr.ini: 0 = auto, 1 = always VR, 2 = never).
bool VrWanted();

// The render size the headset needs (the runtime's pixel density over the frustum the game
// renders), measured while VR runs. With renderWidth/renderHeight=0 in fs25vr.ini it is kept in
// fs25vr_size.ini and written into game.xml at the next VR start.
void SaveMeasuredSize(unsigned width, unsigned height, unsigned currentWidth, unsigned currentHeight);

// A note for the player while VR runs below that size ("restart for full sharpness"), or nullptr.
const char* RenderSizeNote();

} // namespace mode
