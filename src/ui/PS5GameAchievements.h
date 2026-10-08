// psp5 - a game's achievements, read on the home screen before it is played.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "ui/PS5GameArt.h"  // Artwork

namespace psp5 {

// One achievement, as the shelf shows it.
struct GameAchievement {
	std::string title;
	std::string detail;
	std::string points;
	std::string badgeUrl;
	// The badge, once it has been downloaded and handed to the renderer. Zero
	// until then, and the row draws a plain mark in the meantime.
	std::uint32_t badge = 0;
	bool unlocked = false;
	bool header = false;  // a group's label, not something to earn
	float progress = 0.0f;
};

// The achievements RetroAchievements holds for one game.
//
// In a game this comes for free: PPSSPP identifies what it boots and rcheevos
// keeps the list. On the home screen nothing has booted, so the game has to be
// identified here - which means hashing the image and asking the server, both
// of which take a moment and neither of which can be done on the frame the
// player asks. So this is a small state machine: ask, wait, then read.
//
// Everything it exposes is plain text, because the screen that draws it is
// compiled with volk's Vulkan prototypes and cannot include PPSSPP's headers -
// the same split as PS5Settings and PS5GameArt.
class GameAchievements {
public:
	enum class State {
		idle,         // nothing asked for
		working,      // hashing the image, or waiting on the server
		ready,        // rows() holds the list
		none,         // RetroAchievements has none for this game
		notSignedIn,  // ... or psp5 cannot ask, which is not the same thing
		unsupported,  // a file type that cannot be hashed, such as a CSO or CHD
	};

	// Begins identifying the game. Returns at once; the answer arrives over the
	// next few frames, so call Update until the state settles.
	void Open(const std::string &path, const std::string &discId);
	void Update();
	void Close();

	State state() const { return state_; }
	bool identifying() const { return busy_; }
	bool open() const { return open_; }
	std::span<const GameAchievement> rows() const { return rows_; }
	std::size_t size() const { return rows_.size(); }

	// Gives every badge whose PNG has arrived to `upload`, which turns it into
	// a texture. Called from the frame loop, because that is where the renderer
	// is - this class never sees one.
	void UploadBadges(const std::function<std::uint32_t(const Artwork &)> &upload);
	// Hands back every texture made that way, to be destroyed.
	void ReleaseBadges(const std::function<void(std::uint32_t)> &destroy);

	// The headline: "7 of 40", and the points beside it. Empty until ready.
	const std::string &summary() const { return summary_; }
	const std::string &points() const { return points_; }
	float fraction() const { return fraction_; }

private:
	void Read();
	void Identify();
	void Retire();
	void PumpBadges();

	// Identifying a game means hashing PARAM.SFO and EBOOT.BIN out of the disc
	// image, which reads tens of megabytes off the memory stick. Done on the
	// frame loop that was the end of the title: the shelf stopped presenting
	// frames for several seconds and the shell took that for a hung process.
	std::thread worker_;
	std::atomic<bool> busy_ {false};
	std::string pendingPath_;

	// Badge PNGs as they arrive. The download callbacks run on whichever thread
	// pumps the request queue, so the bytes are handed over under a lock and
	// turned into textures later, on the frame loop.
	struct PendingBadge {
		std::string url;
		std::string png;
	};
	std::vector<PendingBadge> arrived_;
	std::mutex arrivedLock_;
	// Textures whose rows have gone. Closing the view cannot destroy them - it
	// happens on the screen, which has no renderer - so they wait here for the
	// frame loop to hand them back.
	std::vector<std::uint32_t> retired_;

	// Badges still to ask for, and how many are being fetched right now.
	//
	// A game can have seventy-odd achievements and they were all asked for at
	// once: seventy-four threads, each opening its own TLS connection to the
	// same host. The console ran out of whatever that needs - every one came
	// back "SSL connect error" - and once it took the title with it. A few at
	// a time is enough to fill the list in a second or two.
	std::vector<std::string> queue_;
	std::atomic<int> inFlight_ {0};

	std::vector<GameAchievement> rows_;
	std::string summary_;
	std::string points_;
	std::string path_;
	float fraction_ = 0.0f;
	State state_ = State::idle;
	bool open_ = false;
	bool asked_ = false;
};

// The one view the home screen shows, for whichever game is focused.
GameAchievements &GameAchievementList();

// Whether the disc image is being read right now.
bool AchievementsIdentifying();

}  // namespace psp5
