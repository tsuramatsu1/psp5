// psp5 - the handful of settings that are psp5's own.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/PS5Prefs.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "PS5Log.h"
#include "PS5Paths.h"

namespace psp5 {
namespace prefs {
namespace {

SoundSet g_soundSet = SoundSet::glass;
bool g_achievements = false;
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
	fprintf(fh, "soundset=%d\n", (int)g_soundSet);
	fprintf(fh, "achievements=%d\n", g_achievements ? 1 : 0);
	fclose(fh);
	chmod(Path().c_str(), 0666);
}

}  // namespace

const char *soundSetName(SoundSet set) {
	switch (set) {
		case SoundSet::glass: return "Chimes";
		case SoundSet::paper: return "Wooden";
		default: return "Off";
	}
}

SoundSet soundSet() {
	return g_soundSet;
}

bool achievements() {
	return g_achievements;
}

void setAchievements(bool on) {
	if (on == g_achievements) {
		return;
	}
	g_achievements = on;
	Save();
	psp5::Trace("prefs: achievements %s", on ? "on" : "off");
}

void setSoundSet(SoundSet set) {
	if (set == g_soundSet) {
		return;
	}
	g_soundSet = set;
	Save();
	psp5::Trace("prefs: menu sounds %s", soundSetName(set));
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
		if (sscanf(line, "soundset=%d", &value) == 1 && value >= 0 && value <= 2) {
			g_soundSet = (SoundSet)value;
		}
		if (sscanf(line, "achievements=%d", &value) == 1) {
			g_achievements = value != 0;
		}
	}
	fclose(fh);
}

}  // namespace prefs
}  // namespace psp5
