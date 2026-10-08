// psp5 - the settings the home screen can change.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace psp5 {

// One row of the settings panel. Everything the panel draws is here as text, so
// the screen that draws it needs none of PPSSPP's headers - the same split as
// PS5GameArt, and for the same reason: that file is compiled with volk's Vulkan
// prototypes and PPSSPP keeps its own in another namespace.
struct SettingItem {
	std::string label;
	std::string value;
	std::string hint;
};

// A view of PPSSPP's configuration, in the few places it is worth changing from
// a console home screen.
//
// Only settings that are safe to change with no game running are here: they are
// read when the panel opens and written when it closes, and the next game to
// boot picks them up. Nothing here can be changed while a game runs, because the
// home screen does not exist then.
class Settings {
public:
	// Reads PPSSPP's current configuration into the rows.
	void Reload();

	std::span<const SettingItem> items() const { return items_; }
	std::size_t size() const { return items_.size(); }

	// Moves one row's value by delta steps, wrapping where a setting is a short
	// list and clamping where it is a range. Returns whether anything changed,
	// so the caller can stay quiet at the end of a range.
	bool Adjust(std::size_t index, int delta);

	// Writes the configuration back to disk. Called when the panel is left, not
	// on every keypress: PPSSPP's save rewrites the whole ini.
	void Save();

	bool dirty() const { return dirty_; }

private:
	void Rebuild();

	std::vector<SettingItem> items_;
	bool dirty_ = false;
};

// The one settings panel the home screen shows.
Settings &SettingsPanel();

}  // namespace psp5
