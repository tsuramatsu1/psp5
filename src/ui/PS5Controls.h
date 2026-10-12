// psp5 - which PlayStation 5 buttons each PSP button sits on.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace psp5 {
namespace controls {

// All twelve of the PSP's buttons - the four face buttons, the two shoulders,
// Start, Select and the four directions - and the console buttons each answers.
//
// Every one of them can be moved, the d-pad included. That costs the prompt its
// cancel: it used to be "a direction on the d-pad leaves it alone", which only
// worked while a direction could not be a binding. It is a time-out now, the
// way RetroArch does it, because once every button is bindable there is no
// button left over to mean "not that one".
//
// Two console buttons per PSP button. A PSP has fewer buttons than a DualSense,
// and the second is what lets Select sit on both the touch pad and L3, or L on
// both L1 and L2, without the player choosing one and regretting it.
//
// The remapping happens where psp5 turns the console's buttons into PPSSPP's
// key codes (PS5Main.cpp), not in PPSSPP's own KeyMap. PPSSPP's mapping table
// is one of the settings LoadGameConfig rewrites partway through a boot, so a
// binding kept there would last until the first game started. This one is
// psp5's, saved in config/psp5.txt, and nothing else writes it.
//
// It changes what a game is sent and nothing else: the home screen and the
// in-game menu read the pad directly, so Cross still chooses and Circle still
// goes back however these are set.

// Which of the two console buttons a row's entry is.
enum Slot {
	primary = 0,
	secondary = 1,
	slots = 2,
};

// How many PSP buttons there are: twelve.
int Count();

// "Cross", "L", "Start", "Up" - the PSP's name for row `index`.
const char *PspName(int index);

// The console button in `slot`, or empty when that slot is unset.
std::string PadName(int index, int slot);

// What the row shows: "Cross", "Cross + L2", or "Not set" with no primary.
std::string PadSummary(int index);

// Puts row `index`'s `slot` on a console button.
//
// A console button answers one PSP button, so this takes it from wherever it
// was - the other row's slot is simply emptied. There is no swap: with twelve
// buttons in twenty-four slots a row without a console button is a state the
// panel has to be able to show anyway, and "it moved here" is easier to follow
// than "these two traded".
void Bind(int index, int slot, std::uint32_t bit);

// Empties a slot. A row with no primary sends nothing.
void Clear(int index, int slot);

// Back to the defaults: Cross on Cross, L on L1, Select on the touch pad, the
// d-pad on the d-pad, and no second button anywhere.
void Reset();

// The console buttons bound to row `index`, as one mask of PAD_* bits, and the
// key code PPSSPP knows that PSP button by. PS5Main sends one when it sees any
// bit of the other.
std::uint32_t PadMask(int index);
int KeyCode(int index);

// Reads the bindings from config/psp5.txt. Called once at start-up, after
// prefs::Load.
void Load();

// Whether every row is where it started, for the reset row to say so and to
// refuse when there is nothing to undo.
bool IsDefault();

}  // namespace controls
}  // namespace psp5
