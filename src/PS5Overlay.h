// psp5 - the cheat panel that opens over a running game.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

class UIContext;

namespace psp5 {

// A bar down the left of the screen, opened with L2 + R2 while a game runs: the
// game's cheats, its save state slots, and the way out of it.
//
// The home screen can do this too, on a game's details sheet, but only before
// the game starts. This is the same file, edited in place, with PPSSPP told to
// re-read it - so a code can be turned on in the middle of playing.
//
// While it is open the pad belongs to the panel and the game sees nothing, so a
// press that moves the selection cannot also move the player.

// Which game's cheats to show. Called when a game boots; an empty id means the
// game carries no PARAM.SFO DISC_ID and so can have no cheat file.
void SetCheatOverlayGame(const std::string &discId);

bool CheatOverlayOpen();
// Opens on the current game, reading the cheat file as it does. Does nothing if
// there is no game id to read one for.
void ToggleCheatOverlay();
void CloseCheatOverlay();

// Input while the panel is up. Each is one press, not a held state.
void CheatOverlayMove(int delta);   // up and down the list
void CheatOverlayAdjust(int delta); // left and right: into a submenu, or a value
void CheatOverlaySelect();
// Circle: one level back, and from the root, closed.
void CheatOverlayBack();

}  // namespace psp5

// Drawn by PPSSPP, at the end of the frame it draws for the game.
extern "C" void PS5_DrawOverlays(UIContext *ui);
