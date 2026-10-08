// psp5 - the achievements bar that opens over a running game.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class UIContext;

namespace psp5 {

// A bar down the right of the screen, opened with R1 + R3 while a game runs:
// what this game asks of the player, what they have already done, and how far
// along the ones that count are.
//
// The mirror of the menu on the left, and it draws the same way - psp5's own
// rectangles through PPSSPP's UIContext, because during a game PPSSPP owns the
// frame. PPSSPP's own achievement screens are among the ones psp5 keeps off the
// display.
//
// None of the work is psp5's: PPSSPP starts the client at NativeInit and hands
// it each game as it boots. This reads the list back and draws it.

bool AchievementsBarOpen();
void ToggleAchievementsBar();
void CloseAchievementsBar();
void AchievementsBarMove(int delta);

// Called once a frame while a game runs, so a bar that is up keeps up with what
// the player has just unlocked.
void AchievementsBarUpdate(float dt);

}  // namespace psp5

// Drawn by PPSSPP, at the end of the frame it draws for the game.
extern "C" void PS5_DrawAchievementsBar(UIContext *ui);
