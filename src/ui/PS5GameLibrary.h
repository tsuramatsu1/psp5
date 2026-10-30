// psp5 - the games on the memory stick, as the home screen's shelf.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "demo/catalog.hpp"
#include "ui/PS5GameArt.h"
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

std::string GameViewName(GameView view);

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

	// ---- covers, which arrive after the shelf does ----------------------
	//
	// Build no longer opens a single game. It used to open all of them - mount
	// the ISO, read PARAM.SFO, decode ICON0 and PIC1, render a 512x512 cover -
	// before the first frame, which is why the shelf was capped at 128 games
	// and spent 128 MiB of texture memory reaching it.
	//
	// Now a game arrives as its filename and a colour, and the real cover is
	// fetched when the shelf comes near it. Covers are kept to kMaxCovers, the
	// least recently wanted thrown away first, so a library of five thousand
	// costs the same as one of fifty.

	// Ask for a game's cover. Cheap and idempotent: call it for everything on
	// screen, every frame. The work happens on a worker thread.
	void Want(std::size_t index);

	// Turns what the worker has finished into textures, and throws away the
	// covers that have fallen out of use. Must be called on the thread that
	// owns the renderer, once a frame.
	void PumpCovers(hui::gfx::Renderer &renderer, const hui::ui::Fonts &fonts);

	// A game's disc id, read now if the worker has not reached it yet. Used on
	// the way into a game, where the answer cannot wait.
	const std::string &DiscIdNow(std::size_t index);

	// ---- search ----------------------------------------------------------

	// Narrows every view to the games whose name or disc id contains this,
	// ignoring case.
	//
	// Search used to be a fourth view, reached by stopping on its tab. That
	// made it a place rather than a tool: it had one order, it could not be
	// used on the tab already open, and arriving at it was the only way to
	// start one. It is a filter now - whichever view is open stays open and
	// keeps its order while a query narrows it, so searching inside Favorites
	// searches the favorites and Recent stays sorted by when things were
	// played.
	//
	// An empty query is no filter at all, which is what clearing it means.
	void SetQuery(const std::string &query);
	const std::string &query() const { return query_; }
	bool searching() const { return !query_.empty(); }

	std::span<const hui::demo::Item> items() const { return items_; }
	std::size_t size() const { return items_.size(); }
	bool empty() const { return items_.empty(); }

	// Bumped by every Build. The shelf keeps a list of indices into the
	// library, so a rebuild that happened under it would leave it pointing at
	// games that are no longer there; watching this is how it knows to build
	// that list again.
	std::uint32_t generation() const { return generation_; }

	const hui::demo::Item &item(std::size_t index) const { return items_[index]; }
	const GameEntry &entry(std::size_t index) const { return entries_[index]; }

	// The game's menu loop, still encoded, or empty if it carries none.
	const std::string &sound(std::size_t index) const { return entries_[index].sound; }

	// Whether the worker has fetched this game's artwork. Until it has, the
	// menu loop is not in memory, and sound() is empty because it has not
	// arrived rather than because the game carries none - two different
	// things to anyone deciding whether to start playing it.
	//
	// Read on the frame loop, which is also the only thing that writes
	// resolved_ (Build, PumpCovers, DiscIdNow), so this needs no lock.
	bool artResolved(std::size_t index) const {
		return index < resolved_.size() && resolved_[index];
	}

	// Indices into items(), in this view's order, narrowed to the search query
	// while there is one. The favorites view holds only the games marked as
	// such, so it can be empty while the others are not - and so can any of
	// them while a query is set.
	std::span<const int> order(GameView view) const {
		const std::size_t at = static_cast<std::size_t>(view);
		return query_.empty() ? order_[at] : found_[at];
	}

	// Marks or unmarks a game and writes the list to the memory stick, so a
	// favorite survives the title being closed.
	void ToggleFavorite(std::size_t index);
	bool IsFavorite(std::size_t index) const;

	// A bound on the shelf rather than on the memory stick: entries are a few
	// hundred bytes each, so this is about a megabyte of them. The covers are
	// what used to cost, and they are bounded separately now.
	static constexpr std::size_t kMaxGames = 4096;
	// 512x512 RGBA is a megabyte each, so this is the shelf's texture budget.
	static constexpr std::size_t kMaxCovers = 48;
	static constexpr std::size_t kMaxSoundBytes = 2u * 1024u * 1024u;
	static constexpr int kCoverSize = 512;

private:
	void Scan();
	void ScanLocal();
	void BuildOrders();
	// order_, narrowed to query_. Run on its own when the query changes: a
	// search is not a reason to sort the library again.
	void BuildFound();
	void LoadFavorites();
	void SaveFavorites() const;
	void LoadKnown();
	void SaveKnown() const;
	void Remember(std::size_t index);
	void StartWorker();
	void StopWorker();
	void Apply(std::size_t index, GameArt &art);
	void MakeCover(hui::gfx::Renderer &renderer, const hui::ui::Fonts &fonts,
	               std::size_t index, const GameArt &art);
	void Evict(hui::gfx::Renderer &renderer);

	// What the worker has been asked for and what it has finished. The art is
	// decoded off the main thread; everything that touches the renderer happens
	// in PumpCovers.
	struct Finished {
		std::size_t index = 0;
		GameArt art;
	};

	// Built in full before items_, so the const char * in each Item stays valid:
	// nothing is appended to this once the items point into it.
	std::vector<GameEntry> entries_;
	std::vector<hui::demo::Item> items_;
	std::vector<int> order_[static_cast<std::size_t>(GameView::count)];
	// The same orders with the query applied. Kept beside the full ones
	// rather than replacing them, so clearing a search is reading the other
	// array and never a rescan.
	std::vector<int> found_[static_cast<std::size_t>(GameView::count)];
	std::string query_;

	// What psp5 learned the last time it opened a game, by path.
	//
	// The shelf stopped opening every game at startup when the library grew
	// past a hundred (see Build), and a game's disc id comes out of its
	// PARAM.SFO - so at the moment the shelf is ordered, nothing is known but
	// filenames. Recent is ordered by when each game was last played, and
	// PPSSPP files play times under the disc id, so with no disc ids the whole
	// view fell back to the file's date and playing a game stopped moving it.
	//
	// This is the way out that does not cost a startup: a game can only have
	// been played if it was opened once, and it was read then. The size and
	// date are kept with it so a file that has been replaced is not described
	// by what used to be there.
	struct Known {
		std::string disc_id;
		std::string title;
		std::uint64_t mtime = 0;
		std::uint64_t size = 0;
	};
	std::map<std::string, Known> known_;
	std::uint32_t generation_ = 0;
	bool known_dirty_ = false;

	// Covers in the order they were last wanted, least recent first.
	std::vector<std::size_t> recent_covers_;
	// Per entry: asked for already, and resolved already. Both are read by the
	// worker and written by it, so both live under the lock.
	std::vector<bool> asked_;
	std::vector<bool> resolved_;

	std::thread worker_;
	std::mutex lock_;
	std::condition_variable wake_;
	std::deque<std::size_t> wanted_;   // under lock_
	std::vector<Finished> finished_;   // under lock_
	bool stopping_ = false;            // under lock_
};

// The one library the launcher and the home screen share.
GameLibrary &Library();

}  // namespace psp5
