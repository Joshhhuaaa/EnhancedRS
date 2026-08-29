#pragma once

// Maps a screen-space blit rectangle lying on the HUD block to its scaled position. Called by
// menu.cpp's blit sinks, which own the sites the HUD draws through.
bool HudScaleRect(const int* rect, int* scaled);
