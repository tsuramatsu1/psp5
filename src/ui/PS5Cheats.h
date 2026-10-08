// psp5 - the cheat codes a game has on the memory stick.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace psp5 {

// One `_C` line of a CWCheat file: a named group of codes, on or off.
struct CheatEntry {
	std::string name;
	bool enabled = false;
	int line = 0;  // one-based, as the file counts
};

// The cheat file for one game, as the home screen shows it.
//
// PPSSPP reads memstick/PSP/Cheats/<DISC_ID>.ini when a game boots and applies
// whatever is marked on, so choosing codes is a matter of rewriting that file -
// which is all this does. Nothing here touches a running game: the panel only
// exists on the home screen, before a game starts.
class Cheats {
public:
	// Reads the file for this disc. False when the game has none, which is the
	// ordinary case and not an error.
	bool Load(const std::string &discId);
	void Clear();

	std::span<const CheatEntry> items() const { return entries_; }
	std::size_t size() const { return entries_.size(); }
	bool empty() const { return entries_.empty(); }

	// Where the file is, or would be. Shown when there is none, so there is
	// somewhere to copy one to.
	const std::string &path() const { return path_; }

	bool Toggle(std::size_t index);

	// Rewrites the `_C0`/`_C1` prefixes in place, leaving every other line of the
	// file alone - the codes themselves, the comments, the byte order mark.
	void Save();

	bool dirty() const { return dirty_; }

private:
	std::vector<CheatEntry> entries_;
	std::string path_;
	std::string discId_;
	bool dirty_ = false;
};

// The one cheat list the home screen shows, for whichever game is focused.
Cheats &CheatList();

// PPSSPP's master switch. Without it on, the codes in the file are read and
// ignored, so the panel that lists them is also where it belongs.
//
// Here rather than reached through Core/Config.h directly, so the screen that
// draws the panel needs none of PPSSPP's headers: it is compiled with volk's
// Vulkan prototypes, and PPSSPP keeps its own in another namespace.
bool CheatsEnabled();
void SetCheatsEnabled(bool enabled);
// Writes the switch back to PPSSPP's configuration, if it changed.
void SaveCheatsEnabled();

}  // namespace psp5
