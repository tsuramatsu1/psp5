// psp5 - the words that are psp5's own, in the player's language.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace psp5 {

// Most of what psp5 draws is a setting PPSSPP also has, so it asks PPSSPP for
// the translation and gets one that someone who speaks the language has read.
// The rest - favourites, the A-Z view, what a row of the cheat panel says - are
// psp5's own, and PPSSPP has no word for them.
//
// Those live in assets/lang/psp5/<id>.ini, one file per language, keyed by the
// English. A key with no translation falls back to the English, so a part-
// translated language is no worse than an untranslated one.
//
// Read the header of those files before trusting them: they were written by a
// machine, not by speakers, and only a few of the languages have been read by
// anyone at all.
std::string Own(const char *english);

// Called when the language changes, and once at start-up.
void LoadOwnStrings(const std::string &languageId);

// A string in the order it should appear on screen rather than the order it is
// spoken. Hebrew and Arabic are stored the way they are read; the kit draws
// left to right, so the right-to-left runs are reversed here, once, at the
// point psp5 produces the string. Anything without them comes back unchanged.
std::string VisualOrder(const std::string &text);

}  // namespace psp5
