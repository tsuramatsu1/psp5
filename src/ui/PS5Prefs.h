// psp5 - the handful of settings that are psp5's own.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace psp5 {

// PPSSPP's configuration is the right home for anything PPSSPP understands, and
// most of psp5's settings live there. These do not: they are about the home
// screen, which PPSSPP knows nothing about, and inventing a PPSSPP setting to
// carry one would mean patching its config table for something it never reads.
//
// One key per line in config/psp5.txt, read once and written when something
// changes.
namespace prefs {

// Which recorded sound set the home screen uses, by folder name, or empty for
// none. The sets are folders under /app0/ui/sfx - the kit's two, and psp5's own
// from tools/make-sounds.py - so adding one is adding a folder rather than
// another enum value.
const std::string &soundSet();
void setSoundSet(const std::string &name);

// The sets present, in the order the settings panel steps through them: "Off"
// first, then every folder found. Read once, when the home screen starts.
const std::vector<std::string> &soundSets();
void setSoundSets(std::vector<std::string> names);

// "Off", "Glass", "Arcade" - the folder name with its first letter raised.
std::string soundSetLabel(const std::string &name);

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

// RetroAchievements hardcore mode. Off unless the player turns it on.
//
// PPSSPP defaults it to true, and rcheevos refuses every save state while it is
// active - Enqueue drops the operation and says nothing, so saving and loading
// simply stopped working the moment a player signed in, with no message and
// nothing in the log. An emulator whose save states quietly stop is worse than
// one whose achievements are marked softcore, so psp5 chooses the other default
// and puts the choice in the settings panel.
//
// Here rather than in PPSSPP's bAchievementsHardcoreMode for the same reason as
// the switch above: that one is per-game, and a game's own config would put it
// back.
bool hardcore();
void setHardcore(bool on);

// Whether cheats are on for one game, by disc id.
//
// PPSSPP's own bEnableCheats is marked PER_GAME, which means the only place it
// can be kept is that game's second ini - so switching cheats on for a game had
// to create one, and creating one is exactly what "Settings for this game"
// reports. Turning on cheats turned on per-game settings with it.
//
// Keeping the answer here instead decouples the two. It also takes psp5 out of
// PPSSPP's game-specific config mode altogether, which it had to enter and
// leave around every read and write of the switch - and any path that left the
// panel without leaving that mode made the next game edit the last one's file.
bool cheatsFor(const std::string &discId);
void setCheatsFor(const std::string &discId, bool on);

// Reads config/psp5.txt. Called once, before the home screen is built.
void Load();

}  // namespace prefs
}  // namespace psp5
