// psp5 - the handful of settings that are psp5's own.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace psp5 {

// PPSSPP's configuration is the right home for anything PPSSPP understands, and
// most of psp5's settings live there. These do not: they are about the home
// screen, which PPSSPP knows nothing about, and inventing a PPSSPP setting to
// carry one would mean patching its config table for something it never reads.
//
// One key per line in config/psp5.txt, read once and written when something
// changes.
namespace prefs {

// Which recorded sound set the home screen uses, or none.
enum class SoundSet : int {
	off = 0,
	glass = 1,  // soft, airy, tuned chimes
	paper = 2,  // warm, wooden, tactile
};

SoundSet soundSet();
void setSoundSet(SoundSet set);
const char *soundSetName(SoundSet set);

// Whether the player has signed in to RetroAchievements and wants it on.
//
// Kept here rather than read back from PPSSPP's bAchievementsEnable, which is
// one of its per-game settings: a game config written before the player signed
// in carries that setting as false, and loading it at boot would switch
// achievements off for that game for ever. psp5 offers achievements in the
// title's settings, because an account belongs to the player and not to a game,
// so this is where the answer lives.
bool achievements();
void setAchievements(bool on);

// Reads config/psp5.txt. Called once, before the home screen is built.
void Load();

}  // namespace prefs
}  // namespace psp5
