// psp5 - the cheat codes a game has on the memory stick.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A CWCheat file is a list of `_C0 Name` / `_C1 Name` headings, each followed by
// the `_L` lines that make up the code. `_C1` is on and `_C0` is off, so turning
// a cheat on or off is a one-character edit to its heading - which is what this
// does, leaving every other line of the file exactly as it was.
//
// PPSSPP's parser does the reading, so psp5 agrees with it about what counts as
// a cheat and where each one starts. Its own screen for this is one of the ones
// psp5 does not show.

#include "ui/PS5Cheats.h"

#include <cstdio>

#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Core/Config.h"
#include "Core/CwCheat.h"

#include "PS5Log.h"

namespace psp5 {

Cheats &CheatList() {
	static Cheats cheats;
	return cheats;
}

namespace {

bool g_enabled_dirty = false;
// Which game the switch belongs to, and whether psp5 is the one that put PPSSPP
// into game-specific mode - in a running game PPSSPP is already in it, and
// leaving it there would take the game's settings out from under it.
std::string g_switch_game;
bool g_scoped_here = false;

}  // namespace

bool CheatsEnabled() {
	return g_Config.bEnableCheats;
}

// Called when the panel opens, so the switch below reads and writes this game's
// value rather than the global one.
void ScopeCheatsToGame(const std::string &discId) {
	g_switch_game = discId;
	g_enabled_dirty = false;
	if (discId.empty() || g_Config.IsGameSpecific()) {
		// Either homebrew with no id to key a config on, or a game already
		// running - PPSSPP loaded its config when it booted.
		return;
	}
	if (g_Config.HasGameConfig(discId)) {
		g_Config.LoadGameConfig(discId);
		g_scoped_here = true;
	}
}

void SetCheatsEnabled(bool enabled) {
	if (g_Config.bEnableCheats == enabled) {
		return;
	}
	// A game with no settings of its own needs some before the switch has
	// anywhere to live.
	if (!g_switch_game.empty() && !g_Config.IsGameSpecific()) {
		if (!g_Config.HasGameConfig(g_switch_game)) {
			g_Config.CreateGameConfig(g_switch_game);
		}
		g_Config.LoadGameConfig(g_switch_game);
		g_scoped_here = true;
	}
	g_Config.bEnableCheats = enabled;
	g_enabled_dirty = true;
}

void SaveCheatsEnabled() {
	const bool scoped = g_scoped_here;
	if (g_enabled_dirty) {
		// Deferred to here rather than done on the press: PPSSPP's save rewrites
		// the whole ini.
		if (!g_switch_game.empty() && g_Config.IsGameSpecific()) {
			g_Config.SaveGameConfig(g_switch_game, g_switch_game);
			psp5::Trace("cheats: %s for %s", g_Config.bEnableCheats ? "on" : "off",
			            g_switch_game.c_str());
		} else {
			g_Config.Save("psp5 cheats");
			psp5::Trace("cheats: master switch %s", g_Config.bEnableCheats ? "on" : "off");
		}
		g_enabled_dirty = false;
	}
	// Only the mode psp5 entered: in a running game PPSSPP owns it.
	if (scoped && g_Config.IsGameSpecific()) {
		g_Config.UnloadGameConfig();
	}
	g_scoped_here = false;
	g_switch_game.clear();
}

void Cheats::Clear() {
	entries_.clear();
	path_.clear();
	discId_.clear();
	dirty_ = false;
}

bool Cheats::Load(const std::string &discId) {
	Clear();
	if (discId.empty()) {
		ScopeCheatsToGame(discId);
		return false;
	}
	discId_ = discId;
	// So the master switch below reads this game's value, not the title's.
	ScopeCheatsToGame(discId);

	CWCheatEngine engine(discId);
	path_ = engine.CheatFilename().ToString();
	if (!File::Exists(engine.CheatFilename())) {
		return false;
	}

	for (const CheatFileInfo &info : engine.FileInfo()) {
		// Headings that only group the list are not codes to switch on.
		std::string_view ignored;
		if (info.IsTitle(&ignored) || info.IsPostComment(&ignored)) {
			continue;
		}
		entries_.push_back({info.name, info.enabled, info.lineNum});
	}
	psp5::Trace("cheats: %u code(s) for %s", (unsigned)entries_.size(), discId.c_str());
	return !entries_.empty();
}

bool Cheats::Toggle(std::size_t index) {
	if (index >= entries_.size()) {
		return false;
	}
	entries_[index].enabled = !entries_[index].enabled;
	dirty_ = true;
	return true;
}

void Cheats::Save() {
	if (!dirty_ || entries_.empty() || path_.empty()) {
		return;
	}

	const Path file(path_);
	std::vector<std::string> lines;
	{
		FILE *in = File::OpenCFile(file, "r");
		if (!in) {
			psp5::Trace("cheats: cannot read %s", path_.c_str());
			return;
		}
		char text[4096];
		while (fgets(text, sizeof(text), in)) {
			std::string line(text);
			while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
				line.pop_back();
			}
			lines.push_back(std::move(line));
		}
		fclose(in);
	}

	// The file may have been replaced since it was read - over FTP, between one
	// launch and the next - so each heading is checked before it is rewritten.
	// A line that no longer looks like the cheat it was is left alone.
	int written = 0;
	for (const CheatEntry &entry : entries_) {
		const std::size_t at = (std::size_t)(entry.line - 1);
		if (entry.line <= 0 || at >= lines.size()) {
			continue;
		}
		std::string &line = lines[at];
		if (line.find("_C") == std::string::npos || entry.name.empty() ||
		    line.find(entry.name) == std::string::npos) {
			continue;
		}
		line = (entry.enabled ? "_C1 " : "_C0 ") + entry.name;
		++written;
	}

	FILE *out = File::OpenCFile(file, "w");
	if (!out) {
		psp5::Trace("cheats: cannot write %s", path_.c_str());
		return;
	}
	for (std::size_t i = 0; i < lines.size(); ++i) {
		fputs(lines[i].c_str(), out);
		if (i + 1 != lines.size()) {
			fputc('\n', out);
		}
	}
	fclose(out);

	dirty_ = false;
	psp5::Trace("cheats: wrote %d heading(s) to %s", written, path_.c_str());
}

}  // namespace psp5
