// psp5 - typing on a controller, in the home screen's style.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "core/input.hpp"
#include "gfx/draw_list.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"

namespace psp5 {

// An on-screen keyboard over the shelf. PPSSPP has one of its own, and it is
// one of the screens psp5 keeps off the display, so this is psp5's.
//
// The layout and the shortcuts are the kit's own keyboard design's, because
// they are the ones a player of these consoles already knows: Square deletes,
// Triangle is a space, L1 shifts, R1 accepts, Circle gives up. Shift arms for
// one letter and locks on a second press.
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
	float age_ = 0.0f;
};

// The one keyboard the home screen opens.
Keyboard &KeyboardPanel();

}  // namespace psp5
