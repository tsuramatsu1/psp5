// psp5 - typing on a controller, in the home screen's style.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>

#include "core/input.hpp"
#include "gfx/draw_list.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"

namespace psp5 {

// An on-screen keyboard over the shelf. PPSSPP has one of its own, and it is
// one of the screens psp5 keeps off the display, so this is psp5's.
//
// The shortcuts are the console's own on-screen keyboard's, so a player does
// not have to learn a second set:
//
//     Cross      the key under the cursor      Square   backspace
//     Triangle   space                         Circle   cancel
//     L1 / R1    move the text cursor          L2       shift
//     R2         done
//
// Shift arms for one letter and locks on a second press.
class Keyboard {
public:
	// Opens over whatever is behind it. `masked` draws dots instead of the
	// characters, for a password.
	void Open(const std::string &title, const std::string &prompt, const std::string &initial,
	          bool masked);

	bool open() const { return open_; }

	// Returns true on the frame the player accepted or gave up; `accepted` says
	// which. The text is then whatever they typed.
	bool Update(const hui::InputFrame &input, hui::ui::Feedback &feedback, bool *accepted);

	void Draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, hui::gfx::Color accent) const;

	const std::string &text() const { return text_; }

private:
	void Press(int key, hui::ui::Feedback &feedback);
	void Move(int dx, int dy, hui::ui::Feedback &feedback);

	bool open_ = false;
	bool masked_ = false;
	bool accepted_ = false;
	std::string title_;
	std::string prompt_;
	std::string text_;
	int key_ = 24;      // "g": the middle of the board, the shortest way anywhere
	int column_ = 4;    // remembered across the wide keys, so up and down undo
	int shift_ = 0;     // 0 off, 1 armed for one letter, 2 locked
	// Where the next character goes, counted in bytes from the start. The
	// console's keyboard lets the cursor be moved and typed into, so this one
	// does too - it used only ever to append.
	std::size_t cursor_ = 0;
	float age_ = 0.0f;
};

// The one keyboard the home screen opens.
Keyboard &KeyboardPanel();

}  // namespace psp5
