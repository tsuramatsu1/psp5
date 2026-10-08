// psp5 - typing on a controller, in the home screen's style.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The layout and the shortcuts are taken from the kit's own keyboard design
// (src/concepts/keyboard.cpp): the same four rows over a row of wide keys, the
// same letters, and the same habits - a face button for the frequent actions so
// the highlight never has to travel for them, and a shift that arms for one
// letter and locks on a second press. That design is a 1,600-line setup wizard
// and not a widget, so this is the board out of it and nothing else.

#include "ui/PS5Keyboard.h"

#include <algorithm>
#include <string>
#include <array>
#include <cmath>

namespace psp5 {
namespace {

using hui::gfx::Color;
using hui::gfx::Rect;

constexpr int kColumns = 10;
constexpr int kCharRows = 4;
constexpr int kRows = kCharRows + 1;  // the characters, then the wide keys
constexpr int kKeys = kColumns * kCharRows + 4;
constexpr int kShiftKey = kColumns * kCharRows;
constexpr int kSpaceKey = kShiftKey + 1;
constexpr int kEraseKey = kShiftKey + 2;
constexpr int kDoneKey = kShiftKey + 3;

constexpr float kKeyW = 104.0f;
constexpr float kKeyH = 78.0f;
constexpr float kKeyGap = 10.0f;
constexpr float kBoardW = kColumns * kKeyW + (kColumns - 1) * kKeyGap;
constexpr float kBoardH = kRows * kKeyH + (kRows - 1) * kKeyGap;
constexpr float kBoardX = (1920.0f - kBoardW) * 0.5f;
constexpr float kBoardY = 470.0f;

constexpr std::size_t kMaxLength = 64;

const Color kInk = Color::rgb(0xf2f4f8);
const Color kPanel = Color::rgb(0x080a12);

enum class KeyKind : std::uint8_t {
	character,
	shift,
	space,
	erase,
	done,
};

struct Key {
	KeyKind kind = KeyKind::character;
	int row = 0;
	int column = 0;
	int span = 1;
	char lower = 0;
	char upper = 0;
};

// Shift is what reaches the symbols, as on the keyboard this is imitating: the
// board has forty character keys, twenty-six letters and ten digits fill all but
// four of them, and the second face of every key is where the rest live. Without
// this only @ - _ and . could be typed at all, which is not enough for a
// password.
constexpr const char *kLower[kCharRows] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl.",
    "zxcvbnm-_'",
};
constexpr const char *kUpper[kCharRows] = {
    "!@#$%^&*()",
    "QWERTYUIOP",
    "ASDFGHJKL,",
    "ZXCVBNM+=\"",
};

constexpr std::array<Key, kKeys> MakeKeys() {
	std::array<Key, kKeys> keys{};
	for (int row = 0; row < kCharRows; ++row) {
		for (int column = 0; column < kColumns; ++column) {
			keys[(std::size_t)(row * kColumns + column)] = {
			    KeyKind::character, row, column, 1, kLower[row][column], kUpper[row][column]};
		}
	}
	keys[kShiftKey] = {KeyKind::shift, kCharRows, 0, 2, 0, 0};
	keys[kSpaceKey] = {KeyKind::space, kCharRows, 2, 4, ' ', ' '};
	keys[kEraseKey] = {KeyKind::erase, kCharRows, 6, 2, 0, 0};
	keys[kDoneKey] = {KeyKind::done, kCharRows, 8, 2, 0, 0};
	return keys;
}

constexpr std::array<Key, kKeys> kKeyTable = MakeKeys();

const Key &Info(int index) {
	return kKeyTable[(std::size_t)std::clamp(index, 0, kKeys - 1)];
}

// The key covering a cell, so a wide key is reached from any column above it.
int KeyAt(int row, int column) {
	row = (row % kRows + kRows) % kRows;
	column = (column % kColumns + kColumns) % kColumns;
	for (int i = 0; i < kKeys; ++i) {
		const Key &key = kKeyTable[(std::size_t)i];
		if (key.row == row && column >= key.column && column < key.column + key.span) {
			return i;
		}
	}
	return 0;
}

Rect KeyRect(const Key &key) {
	const float w = (float)key.span * kKeyW + (float)(key.span - 1) * kKeyGap;
	return {kBoardX + (float)key.column * (kKeyW + kKeyGap),
	        kBoardY + (float)key.row * (kKeyH + kKeyGap), w, kKeyH};
}

const char *KeyLabel(const Key &key, int shift) {
	static char text[2];
	switch (key.kind) {
		case KeyKind::shift: return shift == 2 ? "CAPS" : "SHIFT";
		case KeyKind::space: return "SPACE";
		case KeyKind::erase: return "DELETE";
		case KeyKind::done: return "DONE";
		default:
			text[0] = shift ? key.upper : key.lower;
			text[1] = 0;
			return text;
	}
}

}  // namespace

Keyboard &KeyboardPanel() {
	static Keyboard keyboard;
	return keyboard;
}

void Keyboard::Open(const std::string &title, const std::string &prompt,
                    const std::string &initial, bool masked) {
	open_ = true;
	accepted_ = false;
	masked_ = masked;
	title_ = title;
	prompt_ = prompt;
	text_ = initial;
	cursor_ = text_.size();  // at the end of whatever was already there
	key_ = 24;
	column_ = 4;
	shift_ = 0;
	age_ = 0.0f;
}

void Keyboard::Move(int dx, int dy, hui::ui::Feedback &feedback) {
	(void)feedback;
	const Key &from = Info(key_);
	if (dx != 0) {
		// Along a row, a wide key is one step rather than as many steps as it is
		// columns wide: leaving it means clearing its whole span.
		int column = from.column + (dx > 0 ? from.span : -1);
		column = (column % kColumns + kColumns) % kColumns;
		key_ = KeyAt(from.row, column);
		column_ = Info(key_).column;
	} else if (dy != 0) {
		// Rows wrap, and a wide key remembers the column the player came from,
		// so up and down are always each other's undo.
		key_ = KeyAt(from.row + dy, column_);
	}
}

// No sound: a key answers with the letter appearing, and a cue on every press
// made typing a password sound like an alarm.
void Keyboard::Press(int key, hui::ui::Feedback &feedback) {
	(void)feedback;
	const Key &info = Info(key);
	switch (info.kind) {
		case KeyKind::shift:
			// Off, armed for one letter, then locked - the keyboards players
			// already know.
			shift_ = shift_ == 0 ? 1 : (shift_ == 1 ? 2 : 0);
			return;
		case KeyKind::erase:
			if (cursor_ == 0) {
				return;
			}
			text_.erase(cursor_ - 1, 1);
			--cursor_;
			return;
		case KeyKind::done:
			accepted_ = true;
			open_ = false;
			return;
		default: break;
	}

	if (text_.size() >= kMaxLength) {
		return;
	}
	text_.insert(cursor_, 1, shift_ ? info.upper : info.lower);
	++cursor_;
	if (shift_ == 1) {
		shift_ = 0;  // armed for one letter only
	}
}

bool Keyboard::Update(const hui::InputFrame &input, hui::ui::Feedback &feedback, bool *accepted) {
	if (!open_) {
		return false;
	}
	age_ += 1.0f / 60.0f;

	using hui::Action;
	using hui::Direction;

	switch (input.nav) {
		case Direction::left: Move(-1, 0, feedback); break;
		case Direction::right: Move(1, 0, feedback); break;
		case Direction::up: Move(0, -1, feedback); break;
		case Direction::down: Move(0, 1, feedback); break;
		default: break;
	}

	if (input.is_pressed(Action::confirm)) {
		Press(key_, feedback);
	}
	// The frequent actions never need the focus.
	if (input.is_pressed(Action::west)) {
		Press(kEraseKey, feedback);
	}
	if (input.is_pressed(Action::north)) {
		Press(kSpaceKey, feedback);
	}
	// L1 and R1 move the text cursor, as they do on the console's keyboard.
	if (input.is_pressed(Action::page_prev) && cursor_ > 0) {
		--cursor_;
		feedback.play(hui::audio::Cue::focus, 1.0f, 0.0f);
	}
	if (input.is_pressed(Action::page_next) && cursor_ < text_.size()) {
		++cursor_;
		feedback.play(hui::audio::Cue::focus, 1.0f, 0.0f);
	}
	if (input.is_pressed(Action::jump_prev)) {
		Press(kShiftKey, feedback);  // L2
	}
	if (input.is_pressed(Action::jump_next)) {
		Press(kDoneKey, feedback);  // R2
	}
	if (input.is_pressed(Action::back)) {
		accepted_ = false;
		open_ = false;
	}

	if (!open_) {
		*accepted = accepted_;
		return true;
	}
	return false;
}

namespace {

// The button that works a key without the cursor having to be on it. Shown on
// the key itself, the way the console's own keyboard shows them, so the
// shortcuts can be read off the screen instead of remembered.
hui::ui::Button KeyBadge(KeyKind kind) {
	switch (kind) {
		case KeyKind::shift: return hui::ui::Button::l2;
		case KeyKind::space: return hui::ui::Button::triangle;
		case KeyKind::erase: return hui::ui::Button::square;
		case KeyKind::done: return hui::ui::Button::r2;
		default: return hui::ui::Button::none;
	}
}

}  // namespace

void Keyboard::Draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts,
                    hui::gfx::Color accent) const {
	if (!open_) {
		return;
	}

	// Over everything, on its own ground: what is behind is not part of this.
	list.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x05070f, 0.82f));

	hui::ui::text(list, fonts.semibold, title_, kBoardX, 200, 22, accent, hui::gfx::Align::left,
	              4.0f);
	hui::ui::text(list, fonts.display, prompt_, kBoardX - 2, 266, 52, kInk);

	// The field. Masked for a password, because this is on a television.
	const Rect field{kBoardX, 320, kBoardW, 84};
	list.rounded_rect(field, 16, Color::rgb(0xffffff, 0.08f));
	list.rounded_rect({field.x, field.y + field.h - 3, field.w, 3}, 2, accent);
	std::string shown;
	if (masked_) {
		shown.assign(text_.size(), '*');
	} else {
		shown = text_;
	}
	const float textWidth =
	    hui::ui::text(list, fonts.regular, shown, field.x + 26, field.y + 56, 34, kInk);
	// A caret that blinks, so an empty field still looks like one that takes
	// typing rather than one that is broken.
	if (std::fmod(age_, 1.0f) < 0.6f) {
		list.rounded_rect({field.x + 28 + textWidth, field.y + 24, 3, 40}, 1, accent);
	}

	for (int i = 0; i < kKeys; ++i) {
		const Key &key = kKeyTable[(std::size_t)i];
		const Rect rect = KeyRect(key);
		const bool focused = i == key_;
		const bool lit = key.kind == KeyKind::shift && shift_ != 0;

		if (focused) {
			list.glow(rect, 20, 22, accent.with_alpha(0.45f));
			list.rounded_rect(rect, 16, accent);
		} else {
			list.rounded_rect(rect, 16, Color::rgb(0xffffff, lit ? 0.26f : 0.10f));
		}
		const Color ink = focused ? kPanel : kInk;
		const bool wide = key.kind != KeyKind::character;
		const hui::ui::Button badge = KeyBadge(key.kind);
		if (badge == hui::ui::Button::none) {
			hui::ui::text(list, fonts.regular, KeyLabel(key, shift_), rect.x + rect.w * 0.5f,
			              rect.y + rect.h * 0.5f + 12.0f, 34.0f, ink, hui::gfx::Align::center);
			continue;
		}

		// The glyph and the label as one block, centred on the key.
		const hui::ui::GlyphStyle style =
		    focused ? hui::ui::GlyphStyle::light() : hui::ui::GlyphStyle::dark();
		const std::string label = KeyLabel(key, shift_);
		constexpr float kGlyph = 26.0f;
		constexpr float kGap = 9.0f;
		constexpr float kText = 20.0f;
		// draw_button takes the glyph's LEFT edge, not its centre, and a glyph is
		// as wide as its own shape - L2 and R2 are pills, Square and Triangle are
		// round. Passing a centre and assuming one width put them through the
		// label.
		const float glyphW = hui::ui::button_width(badge, kGlyph);
		const float width = glyphW + kGap + fonts.semibold.font->measure(label, kText);
		const float left = rect.x + (rect.w - width) * 0.5f;
		hui::ui::draw_button(list, fonts, style, badge, left, rect.y + rect.h * 0.5f, kGlyph);
		hui::ui::text(list, fonts.semibold, label, left + glyphW + kGap,
		              rect.y + rect.h * 0.5f + 7.0f, kText, ink, hui::gfx::Align::left);
	}
}

}  // namespace psp5
