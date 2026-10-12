// psp5 - which PlayStation 5 buttons each PSP button sits on.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/PS5Controls.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Common/Input/KeyCodes.h"

#include "PS5Log.h"
#include "platform/platform.h"
#include "ui/PS5Prefs.h"
#include "ui/PS5Strings.h"

namespace psp5 {
namespace controls {
namespace {

// The console buttons a PSP button may be put on. The d-pad is among them now:
// the PSP's directions are rows like any other, so the console's directions
// have to be choices like any other.
struct PadChoice {
	std::uint32_t bit;
	const char *name;
	const char *key;  // what config/psp5.txt calls it
};

constexpr PadChoice kChoices[] = {
    {PAD_CROSS, "Cross", "cross"},
    {PAD_CIRCLE, "Circle", "circle"},
    {PAD_SQUARE, "Square", "square"},
    {PAD_TRIANGLE, "Triangle", "triangle"},
    {PAD_L1, "L1", "l1"},
    {PAD_R1, "R1", "r1"},
    {PAD_L2, "L2", "l2"},
    {PAD_R2, "R2", "r2"},
    {PAD_L3, "L3", "l3"},
    {PAD_R3, "R3", "r3"},
    {PAD_OPTIONS, "Options", "options"},
    {PAD_TOUCH_PAD, "Touch pad", "touchpad"},
    {PAD_UP, "D-pad Up", "dup"},
    {PAD_RIGHT, "D-pad Right", "dright"},
    {PAD_DOWN, "D-pad Down", "ddown"},
    {PAD_LEFT, "D-pad Left", "dleft"},
};

// The PSP's twelve, with the key code PPSSPP knows each by. These are the codes
// its own defaultPadMap binds for a pad (Core/KeyMapDefaults.cpp) - not the
// NKCODE_BUTTON_A/B/X/Y names they read like, which nothing in that map
// mentions.
struct PspEntry {
	const char *name;
	const char *key;  // config/psp5.txt again
	int code;
	std::uint32_t fallback;  // where it sits unless told otherwise
};

constexpr PspEntry kPsp[] = {
    {"Cross", "cross", NKCODE_BUTTON_2, PAD_CROSS},
    {"Circle", "circle", NKCODE_BUTTON_3, PAD_CIRCLE},
    {"Square", "square", NKCODE_BUTTON_4, PAD_SQUARE},
    {"Triangle", "triangle", NKCODE_BUTTON_1, PAD_TRIANGLE},
    {"L", "l", NKCODE_BUTTON_7, PAD_L1},
    {"R", "r", NKCODE_BUTTON_8, PAD_R1},
    {"Start", "start", NKCODE_BUTTON_10, PAD_OPTIONS},
    {"Select", "select", NKCODE_BUTTON_9, PAD_TOUCH_PAD},
    {"Up", "up", NKCODE_DPAD_UP, PAD_UP},
    {"Down", "down", NKCODE_DPAD_DOWN, PAD_DOWN},
    {"Left", "left", NKCODE_DPAD_LEFT, PAD_LEFT},
    {"Right", "right", NKCODE_DPAD_RIGHT, PAD_RIGHT},
};

constexpr int kCount = (int)(sizeof(kPsp) / sizeof(kPsp[0]));

std::uint32_t g_bound[kCount][slots];
bool g_loaded = false;

const PadChoice *ChoiceOf(std::uint32_t bit) {
	for (const PadChoice &choice : kChoices) {
		if (choice.bit == bit) {
			return &choice;
		}
	}
	return nullptr;
}

const PadChoice *ChoiceNamed(const std::string &name) {
	for (const PadChoice &choice : kChoices) {
		if (name == choice.key) {
			return &choice;
		}
	}
	return nullptr;
}

void Defaults() {
	for (int i = 0; i < kCount; ++i) {
		g_bound[i][primary] = kPsp[i].fallback;
		g_bound[i][secondary] = 0;
	}
}

// A row with a second button and no first reads oddly and saves oddly, so the
// one that is set moves up.
void Settle(int index) {
	if (g_bound[index][primary] == 0 && g_bound[index][secondary] != 0) {
		g_bound[index][primary] = g_bound[index][secondary];
		g_bound[index][secondary] = 0;
	}
}

// "cross=circle,select=touchpad+l3" - only the rows that differ from the
// default, so a config written by an older build and one written by this build
// both read as "everything not named is where it has always been".
void Unpack(const std::string &packed) {
	Defaults();
	std::size_t at = 0;
	while (at < packed.size()) {
		const std::size_t comma = packed.find(',', at);
		const std::string pair =
		    packed.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
		at = comma == std::string::npos ? packed.size() : comma + 1;
		const std::size_t equals = pair.find('=');
		if (equals == std::string::npos) {
			continue;
		}
		const std::string psp = pair.substr(0, equals);
		int row = -1;
		for (int i = 0; i < kCount; ++i) {
			if (psp == kPsp[i].key) {
				row = i;
				break;
			}
		}
		if (row < 0) {
			continue;
		}
		// The value is one console button, or two joined by '+'.
		const std::string value = pair.substr(equals + 1);
		const std::size_t plus = value.find('+');
		const PadChoice *first =
		    ChoiceNamed(plus == std::string::npos ? value : value.substr(0, plus));
		const PadChoice *second =
		    plus == std::string::npos ? nullptr : ChoiceNamed(value.substr(plus + 1));
		g_bound[row][primary] = first != nullptr ? first->bit : 0;
		g_bound[row][secondary] = second != nullptr ? second->bit : 0;
		Settle(row);
	}
}

std::string Pack() {
	std::string out;
	for (int i = 0; i < kCount; ++i) {
		if (g_bound[i][primary] == kPsp[i].fallback && g_bound[i][secondary] == 0) {
			continue;
		}
		if (!out.empty()) {
			out += ',';
		}
		out += kPsp[i].key;
		out += '=';
		const PadChoice *first = ChoiceOf(g_bound[i][primary]);
		out += first != nullptr ? first->key : "none";
		const PadChoice *second = ChoiceOf(g_bound[i][secondary]);
		if (second != nullptr) {
			out += '+';
			out += second->key;
		}
	}
	return out;
}

void Ensure() {
	if (!g_loaded) {
		Load();
	}
}

}  // namespace

void Load() {
	g_loaded = true;
	Unpack(prefs::buttonMap());
	psp5::Trace("controls: %s", prefs::buttonMap().empty() ? "the default mapping"
	                                                       : prefs::buttonMap().c_str());
}

int Count() {
	return kCount;
}

const char *PspName(int index) {
	return index >= 0 && index < kCount ? kPsp[index].name : "";
}

std::string PadName(int index, int slot) {
	Ensure();
	if (index < 0 || index >= kCount || slot < 0 || slot >= slots) {
		return std::string();
	}
	const PadChoice *choice = ChoiceOf(g_bound[index][slot]);
	return choice != nullptr ? choice->name : std::string();
}

std::string PadSummary(int index) {
	Ensure();
	const std::string first = PadName(index, primary);
	const std::string second = PadName(index, secondary);
	if (first.empty() && second.empty()) {
		return Own("Not set");
	}
	if (second.empty()) {
		return first;
	}
	return first + " + " + second;
}

std::uint32_t PadMask(int index) {
	Ensure();
	if (index < 0 || index >= kCount) {
		return 0;
	}
	return g_bound[index][primary] | g_bound[index][secondary];
}

int KeyCode(int index) {
	return index >= 0 && index < kCount ? kPsp[index].code : 0;
}

bool IsDefault() {
	Ensure();
	for (int i = 0; i < kCount; ++i) {
		if (g_bound[i][primary] != kPsp[i].fallback || g_bound[i][secondary] != 0) {
			return false;
		}
	}
	return true;
}

void Bind(int index, int slot, std::uint32_t bit) {
	Ensure();
	if (index < 0 || index >= kCount || slot < 0 || slot >= slots || bit == 0) {
		return;
	}
	// One console button answers one PSP button. Taking it from wherever it was
	// is the whole rule: a second row left holding it would fire two PSP buttons
	// from one press, and nothing on the panel would say which.
	for (int row = 0; row < kCount; ++row) {
		for (int other = 0; other < slots; ++other) {
			if ((row != index || other != slot) && g_bound[row][other] == bit) {
				g_bound[row][other] = 0;
				Settle(row);
			}
		}
	}
	g_bound[index][slot] = bit;
	Settle(index);
	prefs::setButtonMap(Pack());
}

void Clear(int index, int slot) {
	Ensure();
	if (index < 0 || index >= kCount || slot < 0 || slot >= slots ||
	    g_bound[index][slot] == 0) {
		return;
	}
	g_bound[index][slot] = 0;
	Settle(index);
	prefs::setButtonMap(Pack());
}

void Reset() {
	Ensure();
	Defaults();
	prefs::setButtonMap(std::string());
}

}  // namespace controls
}  // namespace psp5
