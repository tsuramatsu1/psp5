// psp5 - the handful of settings that are psp5's own.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/PS5Prefs.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>
#include <sys/stat.h>

#include "PS5Log.h"
#include "PS5Paths.h"

namespace psp5 {
namespace prefs {
namespace {

std::string g_soundSet = "glass";
std::vector<std::string> g_soundSets;
bool g_achievements = false;
bool g_hardcore = false;
std::vector<std::string> g_cheatGames;
bool g_loaded = false;

std::string Path() {
	return PS5Paths::Config() + "/psp5.txt";
}

void Save() {
	FILE *fh = fopen(Path().c_str(), "wb");
	if (!fh) {
		psp5::Trace("prefs: cannot write %s", Path().c_str());
		return;
	}
	fprintf(fh, "soundset=%s\n", g_soundSet.empty() ? "off" : g_soundSet.c_str());
	fprintf(fh, "achievements=%d\n", g_achievements ? 1 : 0);
	fprintf(fh, "hardcore=%d\n", g_hardcore ? 1 : 0);
	// One line per game that has them on. Absent means off, so a game that is
	// never touched costs nothing.
	for (const std::string &game : g_cheatGames) {
		fprintf(fh, "cheats=%s\n", game.c_str());
	}
	fclose(fh);
	chmod(Path().c_str(), 0666);
}

}  // namespace

std::string soundSetLabel(const std::string &name) {
	if (name.empty()) {
		return "Off";
	}
	std::string label = name;
	if (label[0] >= 'a' && label[0] <= 'z') {
		label[0] = (char)(label[0] - 'a' + 'A');
	}
	return label;
}

const std::string &soundSet() {
	return g_soundSet;
}

const std::vector<std::string> &soundSets() {
	return g_soundSets;
}

void setSoundSets(std::vector<std::string> names) {
	g_soundSets = std::move(names);
	// A set that was chosen and is no longer there - a folder removed between
	// runs - would otherwise leave the home screen silent with no way back.
	if (!g_soundSet.empty() &&
	    std::find(g_soundSets.begin(), g_soundSets.end(), g_soundSet) == g_soundSets.end()) {
		g_soundSet = g_soundSets.empty() ? std::string() : g_soundSets.front();
	}
}

bool achievements() {
	return g_achievements;
}

bool hardcore() {
	return g_hardcore;
}

bool cheatsFor(const std::string &discId) {
	if (discId.empty()) {
		return false;
	}
	return std::find(g_cheatGames.begin(), g_cheatGames.end(), discId) != g_cheatGames.end();
}

void setCheatsFor(const std::string &discId, bool on) {
	if (discId.empty() || on == cheatsFor(discId)) {
		return;
	}
	if (on) {
		g_cheatGames.push_back(discId);
	} else {
		g_cheatGames.erase(std::remove(g_cheatGames.begin(), g_cheatGames.end(), discId),
		                   g_cheatGames.end());
	}
	Save();
	psp5::Trace("prefs: cheats %s for %s", on ? "on" : "off", discId.c_str());
}

void setHardcore(bool on) {
	if (on == g_hardcore) {
		return;
	}
	g_hardcore = on;
	Save();
	psp5::Trace("prefs: hardcore mode %s", on ? "on" : "off");
}

void setAchievements(bool on) {
	if (on == g_achievements) {
		return;
	}
	g_achievements = on;
	Save();
	psp5::Trace("prefs: achievements %s", on ? "on" : "off");
}

void setSoundSet(const std::string &name) {
	if (name == g_soundSet) {
		return;
	}
	g_soundSet = name;
	Save();
	psp5::Trace("prefs: menu sounds %s", soundSetLabel(name).c_str());
}

void Load() {
	if (g_loaded) {
		return;
	}
	g_loaded = true;
	FILE *fh = fopen(Path().c_str(), "rb");
	if (!fh) {
		return;
	}
	char line[256];
	while (fgets(line, sizeof(line), fh)) {
		int value = 0;
		char name[64];
		if (sscanf(line, "soundset=%63s", name) == 1) {
			g_soundSet = std::string(name) == "off" ? std::string() : name;
		}
		if (sscanf(line, "achievements=%d", &value) == 1) {
			g_achievements = value != 0;
		}
		if (sscanf(line, "hardcore=%d", &value) == 1) {
			g_hardcore = value != 0;
		}
		char game[64];
		if (sscanf(line, "cheats=%63s", game) == 1) {
			g_cheatGames.push_back(game);
		}
	}
	fclose(fh);
}

}  // namespace prefs
}  // namespace psp5
