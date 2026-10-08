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

	// ---- one game's own settings ----
	//
	// PPSSPP keeps per-game overrides as a second ini and a mode it switches
	// into: while it is in that mode every setting read or written is the
	// game's. So the panel borrows that mode for as long as it is showing a
	// game, and always leaves it before it closes - a global panel left in
	// game-specific mode would quietly edit the game's file instead.
	void BeginGame(const std::string &discId, const std::string &title);
	void EndGame();

	bool scopedToGame() const { return !gameId_.empty(); }
	const std::string &gameTitle() const { return gameTitle_; }

	// Whether this game has settings of its own. Off means the rows below show
	// the global ones and cannot be changed here.
	bool overrides() const { return overrides_; }
	void SetOverrides(bool on);

private:
	void Rebuild();

	std::vector<SettingItem> items_;
	std::string gameId_;
	std::string gameTitle_;
	bool overrides_ = false;
	bool dirty_ = false;
};

// The one settings panel the home screen shows.
Settings &SettingsPanel();

// RetroAchievements. PPSSPP keeps the whole system - it starts it at NativeInit
// and hands it each game as it boots - so psp5 only signs in and reads back
// what it says. The password is never kept: the server answers with a token,
// and PPSSPP stores that where it stores its other secrets.
// Whether the home screen makes a sound as the player moves around it. Only
// that: a game's own menu loop, which plays while it is under the cursor, is a
// different thing and keeps playing either way.
//
// Kept in PPSSPP's iUIVolume, which is what that setting already means, so it
// persists with everything else rather than in a file of psp5's own.
bool MenuSoundsEnabled();

// Whether this build can reach the server at all. See the comment on the
// definition: it cannot yet, and the panel says so instead of inviting a
// sign-in that goes nowhere.
bool AchievementsAvailable();
bool AchievementsLoggedIn();
std::string AchievementsUser();
// Starts a sign-in. It finishes on another thread; AchievementsLoggedIn says
// when, and the home screen simply shows the row's state each frame.
void AchievementsLogin(const std::string &user, const std::string &password);
void AchievementsLogout();

// How a sign-in is going. It happens on another thread and can take a few
// seconds, which with nothing on screen looked like the password had been
// thrown away.
enum class SignIn {
	idle,       // nothing to say
	working,    // talking to the server
	succeeded,  // signed in
	failed,     // it came back no
};
SignIn AchievementsSignIn();
// Dismisses the result and forgets the credentials held for a retry.
void ClearAchievementsSignIn();
// Sends the same credentials again. They are kept only while the dialog is up.
void RetryAchievementsSignIn();
bool CanRetryAchievementsSignIn();

}  // namespace psp5
