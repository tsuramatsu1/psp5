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
#include <sys/stat.h>
#include <cstring>
#include <algorithm>

#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Core/Config.h"
#include "Core/CwCheat.h"
#include "Core/System.h"

#include "PS5Log.h"
#include "ui/PS5Prefs.h"

namespace psp5 {

Cheats &CheatList() {
	static Cheats cheats;
	return cheats;
}

namespace {

// Which game the switch belongs to.
std::string g_switch_game;

}  // namespace

bool CheatsEnabled() {
	return prefs::cheatsFor(g_switch_game);
}

// What PPSSPP is told as a game boots, after its own config has been read.
extern "C" bool PS5_CheatsEnabled() {
	return psp5::prefs::cheatsFor(psp5::g_switch_game);
}

// Called when the panel opens, and as a game boots, so the switch below is
// read and written for this game.
//
// Nothing is done to PPSSPP's configuration here any more. The switch lives in
// psp5's own settings, keyed by disc id: PPSSPP's bEnableCheats is one of its
// per-game settings, so holding the answer there meant entering its
// game-specific config mode to read it and leaving that mode afterwards - and
// any route out of the panel that skipped the leaving left the next game being
// edited inside the last game's settings, which is what made one game's switch
// appear to be every game's. It also meant switching cheats on had to create
// the game's second ini, which is what "Settings for this game" reports, so
// cheats and per-game settings came on together.
void ScopeCheatsToGame(const std::string &discId) {
	g_switch_game = discId;
	// The running engine re-reads this a few times a second (see hleCheat), so
	// a game already playing picks the change up without a reload.
	g_Config.bEnableCheats = prefs::cheatsFor(discId);
}

void SetCheatsEnabled(bool enabled) {
	if (g_switch_game.empty() || enabled == prefs::cheatsFor(g_switch_game)) {
		return;
	}
	prefs::setCheatsFor(g_switch_game, enabled);
	g_Config.bEnableCheats = enabled;
}

// Nothing to do any more: the switch is written the moment it changes, because
// psp5's settings file is a few short lines rather than PPSSPP's whole ini.
// Kept because the panels call it on the way out, and because something has to
// be the place where that becomes untrue again.
void SaveCheatsEnabled() {
}

std::string Cheats::DatabasePath() {
	return (GetSysDirectory(DIRECTORY_CHEATS) / "cheat.db").ToString();
}

bool Cheats::DatabaseExists() {
	return File::Exists(GetSysDirectory(DIRECTORY_CHEATS) / "cheat.db");
}

namespace {

// fgets, with the line ending taken off. The database is a text file of unknown
// provenance and its lines can be long; PPSSPP allows 2048 and so does this.
char *ReadLine(char *buffer, int size, FILE *fh) {
	char *line = fgets(buffer, size, fh);
	if (!line) {
		return nullptr;
	}
	std::size_t length = std::strlen(line);
	while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
		line[--length] = '\0';
	}
	return line;
}

// "_S", "_C", "_L" and so on: the two characters that open a CWCheat line.
bool Tagged(const char *line, char tag) {
	return line[0] == '_' && line[1] == tag;
}

}  // namespace

Cheats::Import Cheats::ImportFromDatabase(int *added) {
	if (added) {
		*added = 0;
	}
	// A section heading is the disc id split with a dash: ULUS10490 becomes
	// "_S ULUS-10490". An id that is not nine characters cannot have one.
	if (discId_.size() != 9) {
		return Import::noGame;
	}
	const Path database = GetSysDirectory(DIRECTORY_CHEATS) / "cheat.db";
	FILE *in = File::OpenCFile(database, "rt");
	if (!in) {
		psp5::Trace("cheats: no database at %s", database.c_str());
		return Import::noFile;
	}

	const std::string heading = "_S " + discId_.substr(0, 4) + "-" + discId_.substr(4);

	// What the game's file already has, by name, so importing a second time
	// does not double every cheat.
	std::vector<std::string> known;
	known.reserve(entries_.size());
	for (const CheatEntry &entry : entries_) {
		known.push_back(entry.name);
	}

	std::vector<std::string> title;
	std::vector<std::string> lines;
	char buffer[2048] {};
	bool inGame = false;
	bool inCheat = false;
	int found = 0;

	while (!feof(in)) {
		const char *line = ReadLine(buffer, sizeof(buffer), in);
		if (!line) {
			continue;
		}
		if (Tagged(line, 'S')) {
			inGame = heading == line;
			inCheat = false;
		} else if (inGame && Tagged(line, 'C')) {
			// "_C0 " and "_C1 " are both four characters before the name.
			const std::string name = std::string(line).substr(4);
			inCheat = std::find(known.begin(), known.end(), name) == known.end();
		}
		if (!inGame) {
			// Only the first matching section is taken: a database can list an
			// id twice, and reading both would import the same codes twice.
			if (!lines.empty()) {
				break;
			}
			continue;
		}
		if ((Tagged(line, 'S') || Tagged(line, 'G')) && title.size() < 2) {
			title.push_back(line);
		} else if (inCheat && (Tagged(line, 'C') || Tagged(line, 'L') || line[0] == '/' ||
		                       line[0] == '#')) {
			lines.push_back(line);
			++found;
		}
	}
	fclose(in);

	if (lines.empty()) {
		psp5::Trace("cheats: nothing new in the database for %s", discId_.c_str());
		return Import::none;
	}

	// A cheat file opens with its own `_S`/`_G` heading. Where the game has no
	// file yet, or one that does not begin with a heading, the database's goes
	// in front of what is being appended.
	const Path file = CWCheatEngine(discId_).CheatFilename();
	std::string first;
	if (FILE *existing = File::OpenCFile(file, "rt")) {
		char head[2048];
		if (const char *line = ReadLine(head, sizeof(head), existing)) {
			first = line;
		}
		fclose(existing);
	}
	if (first.size() < 2 || first[0] != '_' || first[1] != 'S') {
		lines.insert(lines.begin(), title.begin(), title.end());
	}

	FILE *out = File::OpenCFile(file, "at");
	if (!out) {
		psp5::Trace("cheats: cannot write %s", file.c_str());
		return Import::failed;
	}
	fputc('\n', out);
	for (const std::string &line : lines) {
		fprintf(out, "%s\n", line.c_str());
	}
	fclose(out);
	// Readable over FTP, like everything else psp5 writes under the title.
	chmod(file.c_str(), 0666);

	psp5::Trace("cheats: imported %d line(s) for %s", found, discId_.c_str());
	if (added) {
		*added = found;
	}

	// Re-read, so the panel shows what was just added.
	const std::string disc = discId_;
	Load(disc);
	return Import::added;
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
