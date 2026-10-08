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

// Reads config/psp5.txt. Called once, before the home screen is built.
void Load();

}  // namespace prefs
}  // namespace psp5
