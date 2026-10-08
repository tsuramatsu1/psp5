// psp5 - the games on the memory stick, as the home screen's shelf.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "demo/catalog.hpp"
#include "gfx/renderer.hpp"
#include "ui/fonts.hpp"

namespace psp5 {

// One game found on the memory stick. Every string here is drawn somewhere, and
// every one of them is read off the filesystem: nothing is invented.
struct GameEntry {
	std::string path;      // what PPSSPP is asked to boot
	std::string title;     // the file or folder name, tidied up
	std::string disc_id;   // PARAM.SFO DISC_ID; empty when the game carries none
	std::string format;    // ISO, CSO, CHD, PBP
	std::string size_text; // "1.6 GB"
	std::string date_text; // "12 Mar 2026"
	std::uint64_t size = 0;
	std::uint64_t mtime = 0;
	int year = 0;
	bool has_art = false; // the game carried its own ICON0; the cover is not drawn
	bool favorite = false;
	// SND0.AT3 as it sits on the disc. Encoded, because decoded a three minute
	// loop is forty megabytes and the shelf would hold one per game.
	std::string sound;
};

// How the shelf is ordered. These are the home screen's tabs, switched with
// L1/R1: three views of the same real library rather than three invented
// sections, so none of them can be empty while the others have content.
enum class GameView : int {
	recent,
	alphabetical,
	favorites,
	count,
};

const char *GameViewName(GameView view);

// What the Aurora shelf shows, in place of the kit's invented catalogue.
//
// The drawable items are hui::demo::Item because that is the shape every design
// in the kit reads - title, palette, cover. Only the shape survives; the content
// is the memory stick's.
class GameLibrary {
public:
	// Scans the memory stick and renders a cover per game. Succeeds with an empty
	// library: a console with no games is a normal state, not a failure, and the
	// launcher shows an empty shelf rather than refusing to start.
	bool Build(hui::gfx::Renderer &renderer, const hui::ui::Fonts &fonts);
	void Release(hui::gfx::Renderer &renderer);

	std::span<const hui::demo::Item> items() const { return items_; }
	std::size_t size() const { return items_.size(); }
	bool empty() const { return items_.empty(); }

	const hui::demo::Item &item(std::size_t index) const { return items_[index]; }
	const GameEntry &entry(std::size_t index) const { return entries_[index]; }

	// The game's menu loop, still encoded, or empty if it carries none.
	const std::string &sound(std::size_t index) const { return entries_[index].sound; }

	// Indices into items(), in this view's order. The favorites view holds only
	// the games marked as such, so it can be empty while the others are not.
	std::span<const int> order(GameView view) const {
		return order_[static_cast<std::size_t>(view)];
	}

	// Marks or unmarks a game and writes the list to the memory stick, so a
	// favorite survives the title being closed.
	void ToggleFavorite(std::size_t index);
	bool IsFavorite(std::size_t index) const;

	// Covers are GPU textures; a memory stick with hundreds of games would
	// otherwise spend a gigabyte of them before the first frame.
	static constexpr std::size_t kMaxGames = 128;
	static constexpr std::size_t kMaxSoundBytes = 2u * 1024u * 1024u;
	static constexpr int kCoverSize = 512;

private:
	void Scan();
	void BuildOrders();
	void LoadFavorites();
	void SaveFavorites() const;

	// Built in full before items_, so the const char * in each Item stays valid:
	// nothing is appended to this once the items point into it.
	std::vector<GameEntry> entries_;
	std::vector<hui::demo::Item> items_;
	std::vector<int> order_[static_cast<std::size_t>(GameView::count)];
};

// The one library the launcher and the home screen share.
GameLibrary &Library();

}  // namespace psp5
