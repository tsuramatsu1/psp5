// psp5 - the settings the home screen can change.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PPSSPP has several hundred settings and a screen for each group of them. psp5
// does not show those screens, so this is a deliberately short list: the ones
// worth reaching from a console home screen, and only the ones that are safe to
// change with no game running, because that is the only time this panel exists.
//
// Each row is described once, in kRows, as a field of g_Config plus how to step
// it and how to name its current value. Adding a setting means adding a row.

#include "ui/PS5Settings.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <vector>

#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/RetroAchievements.h"
#include "Core/KeyMap.h"
#include "Core/SaveState.h"
#include "Core/System.h"
#include "Common/StringUtils.h"
#include "Common/File/DirListing.h"

#include "PS5Log.h"
#include "ui/PS5Prefs.h"

namespace psp5 {
namespace {

std::string Format(const char *format, int value) {
	char text[64];
	std::snprintf(text, sizeof(text), format, value);
	return text;
}

// A setting that steps through a short list of named choices, wrapping.
struct Choice {
	int value;
	const char *name;
};

std::string NameOf(std::span<const Choice> choices, int value, const char *fallback) {
	for (const Choice &choice : choices) {
		if (choice.value == value) {
			return choice.name;
		}
	}
	return Format(fallback, value);
}

int StepThrough(std::span<const Choice> choices, int value, int delta) {
	int at = 0;
	for (int i = 0; i < (int)choices.size(); ++i) {
		if (choices[(std::size_t)i].value == value) {
			at = i;
			break;
		}
	}
	const int count = (int)choices.size();
	const int next = ((at + delta) % count + count) % count;
	return choices[(std::size_t)next].value;
}

constexpr Choice kResolutions[] = {
    {0, "Auto"},  {1, "1x  480x272"}, {2, "2x  960x544"},
    {3, "3x"},    {4, "4x"},          {5, "5x"},
};
constexpr Choice kFiltering[] = {
    {1, "Auto"}, {2, "Nearest"}, {3, "Linear"}, {4, "Auto, max quality"},
};
// iAnisotropyLevel is an exponent: 0 is 1x, 4 is 16x.
constexpr Choice kAnisotropy[] = {
    {0, "Off"}, {1, "2x"}, {2, "4x"}, {3, "8x"}, {4, "16x"},
};
constexpr Choice kFrameSkip[] = {
    {0, "Off"}, {1, "1 frame"}, {2, "2 frames"}, {3, "3 frames"}, {4, "4 frames"},
};
constexpr Choice kOnOff[] = {{0, "Off"}, {1, "On"}};

}  // namespace

Settings &SettingsPanel() {
	static Settings panel;
	return panel;
}

bool MenuSoundsEnabled() {
	return !prefs::soundSet().empty();
}

// Called once, after NativeInit has read the configuration: PPSSPP only builds
// its achievements client when the setting is on, and the setting it just read
// knows nothing about psp5's.
// Fast-forward on the right trigger, and R3 left alone.
//
// This is PPSSPP's own default for the trigger, set again here because it does
// not stay set: Config::LoadGameConfig calls KeyMap::LoadFromIni on the game's
// own ini partway through a boot, so whatever mapping that file was written
// with wins over whatever is in memory. Asserting it at start-up and again as a
// game boots is the only way it holds.
//
// R3 carried PPSSPP's Speed toggle by default. psp5 opens the achievements bar
// on R1 + R3, and a lone R3 silently changing the emulation speed is not what
// anyone pressing it is after, so that mapping goes.
void ApplyControlMapping() {
	for (int guard = 0; guard < 8 && KeyMap::PspButtonHasMappings(VIRTKEY_SPEED_TOGGLE); ++guard) {
		KeyMap::DeleteNthMapping(VIRTKEY_SPEED_TOGGLE, 0);
	}
	KeyMap::SetInputMapping(
		VIRTKEY_FASTFORWARD,
		KeyMap::MultiInputMapping(
			InputMapping(DEVICE_ID_PAD_0, JOYSTICK_AXIS_RTRIGGER, +1)),
		true);
	psp5::Trace("controls: fast-forward on the right trigger, R3 free");
}

// Asked by PPSSPP as a game boots, after it has read the game's own ini. Both
// of these are PER_GAME settings, so that ini would otherwise win.
extern "C" bool PS5_ReplaceTextures() {
	return psp5::prefs::replaceTextures();
}

extern "C" bool PS5_SaveNewTextures() {
	return psp5::prefs::saveNewTextures();
}

// Asked by PPSSPP as a game boots, after it has read the game's own ini.
extern "C" void PS5_ApplyControls() {
	psp5::ApplyControlMapping();
}

void ApplyAchievementsPreference() {
	if (!prefs::achievements()) {
		return;
	}
	// UpdateSettings is what builds the rcheevos client, and it is what logs the
	// player back in from the token saved under PSP/SYSTEM. It has to be called
	// even when bAchievementsEnable is already true in ppsspp.ini - that flag
	// says what the player wants, not that the client exists, and on the home
	// screen nothing else creates one. Skipping it here was why a player who had
	// signed in was told they were not: the token was on disk, but there was no
	// client to present it.
	g_Config.bAchievementsEnable = true;
	g_Config.bAchievementsHardcoreMode = prefs::hardcore();
	Achievements::UpdateSettings();
	psp5::Trace("achievements: on, from psp5's own settings; client %s, hardcore %s",
	            Achievements::GetClient() ? "ready" : "not created",
	            prefs::hardcore() ? "on" : "off");
}

// Asked by PPSSPP as a game boots (see tools/mkpatch.py): whether achievements
// are on is the player's answer, not whatever a per-game config recorded before
// they signed in.
extern "C" bool PS5_AchievementsEnabled() {
	return psp5::prefs::achievements();
}

// Likewise: bAchievementsHardcoreMode is per-game too, and PPSSPP's default for
// it is on - which silently disables every save state.
extern "C" bool PS5_HardcoreEnabled() {
	return psp5::prefs::hardcore();
}

std::string PlayedTime(const std::string &discId) {
	if (discId.empty()) {
		return std::string();
	}
	std::string text;
	if (!g_Config.TimeTracker().GetPlayedTimeString(discId, &text)) {
		return std::string();
	}
	return text;
}

// The one phrase both the hero and the details sheet use, so the two cannot
// drift apart. PPSSPP's string already reads "Time Played: 0h 30m 30s", so
// nothing is added to it: a label in front of it said the same thing twice, and
// the middle dot that separated them is not in the kit's font atlas - which has
// about 113 glyphs - so it drew as a question mark.
std::string PlayedLabel(const std::string &discId) {
	const std::string played = PlayedTime(discId);
	return played.empty() ? std::string("Not played yet") : played;
}

bool HasSaveState(const std::string &discId) {
	if (discId.empty()) {
		return false;
	}
	// The state directory is read here rather than asked of SaveState, for two
	// reasons. Its answer comes from a listing it builds in Rescan, and Rescan
	// is only ever called for a game that is running - on the shelf that listing
	// is empty or belongs to whatever was played last. And its file names carry
	// the disc version as well as the id, which is in the game image and not in
	// anything the shelf has read, so assuming "1.00" missed every game that is
	// not that - which is what hid Resume on The 3rd Birthday.
	//
	// So: any <disc id>_<version>_<slot>.ppst at all.
	std::vector<File::FileInfo> files;
	if (!File::GetFilesInDir(GetSysDirectory(DIRECTORY_SAVESTATE), &files, nullptr, 0,
					  discId + "_")) {
		return false;
	}
	for (const File::FileInfo &file : files) {
		if (endsWith(file.name, ".ppst") && !endsWith(file.name, ".undo.ppst")) {
			return true;
		}
	}
	return false;
}

bool AchievementsLoggedIn() {
	return Achievements::IsLoggedIn();
}

std::string AchievementsUser() {
	return g_Config.sAchievementsUserName;
}

bool AchievementsAvailable() {
	// Set by the build when psp5's own https transport is linked in - PacBrew's
	// libcurl, driven from src/net. Not the absence of HTTPS_NOT_AVAILABLE,
	// which stays defined on purpose: that flag is what keeps PPSSPP's own naett
	// out of a build it cannot work in, and testing it reported no transport
	// long after there was one.
#ifdef PSP5_HAVE_HTTPS
	return true;
#else
	return false;
#endif
}

namespace {

// Whether a sign-in has been asked for since the dialog was opened. Without it a
// failure from some earlier attempt would be reported as if it were this one.
bool g_signInAttempted = false;
// The answer, once it has come, held until the player has seen it - otherwise
// the dialog would vanish the instant it succeeded, which is the one moment
// worth showing.
SignIn g_signInResult = SignIn::idle;

// Held only while the dialog is up, so Retry does not ask for the password
// again. ClearAchievementsSignIn forgets them.
std::string g_signInUser;
std::string g_signInPassword;

}  // namespace

SignIn AchievementsSignIn() {
	if (!g_signInAttempted) {
		return SignIn::idle;
	}
	if (Achievements::IsBlockingExecution()) {
		return SignIn::working;
	}
	if (g_signInResult == SignIn::working || g_signInResult == SignIn::idle) {
		// It has come back. Which way is the only thing left to ask.
		g_signInResult = Achievements::IsLoggedIn() ? SignIn::succeeded : SignIn::failed;
	}
	return g_signInResult;
}

void ClearAchievementsSignIn() {
	g_signInAttempted = false;
	g_signInResult = SignIn::idle;
	g_signInUser.clear();
	g_signInPassword.clear();
}

bool CanRetryAchievementsSignIn() {
	return !g_signInUser.empty() && !g_signInPassword.empty();
}

void RetryAchievementsSignIn() {
	if (!CanRetryAchievementsSignIn()) {
		return;
	}
	const std::string user = g_signInUser;
	const std::string password = g_signInPassword;
	AchievementsLogin(user, password);
}

void AchievementsLogin(const std::string &user, const std::string &password) {
	if (user.empty() || password.empty()) {
		return;
	}
	if (!AchievementsAvailable()) {
		psp5::Trace("achievements: no HTTPS transport in this build; sign-in not sent");
		return;
	}
	// Turning the system on is part of signing in: a login against a disabled
	// client would succeed and then do nothing.
	g_Config.bAchievementsEnable = true;
	prefs::setAchievements(true);
	Achievements::UpdateSettings();
	psp5::Trace("achievements: signing in as %s", user.c_str());
	g_signInAttempted = true;
	g_signInResult = SignIn::working;
	g_signInUser = user;
	g_signInPassword = password;
	Achievements::LoginAsync(user.c_str(), password.c_str());
	g_Config.Save("psp5 achievements");
}

void AchievementsLogout() {
	psp5::Trace("achievements: signing out %s", g_Config.sAchievementsUserName.c_str());
	prefs::setAchievements(false);
	Achievements::Logout();
	g_Config.Save("psp5 achievements");
	psp5::Trace("achievements: signed out");
}

void Settings::Rebuild() {
	items_.clear();

	items_.push_back({"Rendering resolution",
	                  NameOf(kResolutions, g_Config.iInternalResolution, "%dx"),
	                  "How much sharper than a PSP the picture is drawn."});

	items_.push_back({"Texture filtering", NameOf(kFiltering, g_Config.iTexFiltering, "%d"),
	                  "Nearest keeps the original pixels; linear smooths them."});

	items_.push_back({"Anisotropic filtering", NameOf(kAnisotropy, g_Config.iAnisotropyLevel, "%d"),
	                  "Sharpens textures seen at a steep angle, like roads and floors."});

	items_.push_back({"Vertical sync", g_Config.bVSync ? "On" : "Off",
	                  "On removes tearing; off lets frames arrive as soon as they are drawn."});

	items_.push_back({"Frame skipping",
	                  g_Config.bAutoFrameSkip ? std::string("Automatic")
	                                          : NameOf(kFrameSkip, g_Config.iFrameSkip, "%d"),
	                  "Drops frames to keep a heavy game at speed. Off looks best."});

	items_.push_back({"Sound", g_Config.bEnableSound ? "On" : "Off", "Audio from the emulator."});

	items_.push_back({"Game volume", Format("%d", g_Config.iGameVolume),
	                  "0 to 100, in steps of five."});

	items_.push_back({"Menu sounds", prefs::soundSetLabel(prefs::soundSet()),
	                  "The home screen's own sounds. A game's music is separate."});

	items_.push_back({"Show frame rate",
	                  (g_Config.iShowStatusFlags & (int)ShowStatusFlags::FPS_COUNTER) ? "On" : "Off",
	                  "Draws the frame rate over the game."});

	items_.push_back({"Fast-forward speed",
	                  g_Config.iAnalogFpsLimit <= 0 ? std::string("Unlimited")
	                                                : Format("%d%%", g_Config.iAnalogFpsLimit),
	                  "How fast the game runs while the right trigger is held."});

	items_.push_back({"Achievements hardcore mode", prefs::hardcore() ? "On" : "Off",
	                  "On earns hardcore unlocks but turns off save states entirely."});

	items_.push_back({"Texture replacement", prefs::replaceTextures() ? "On" : "Off",
	                  "Uses a pack from PSP/TEXTURES/<GAMEID>/ when a game has one."});

	items_.push_back({"Save new textures", prefs::saveNewTextures() ? "On" : "Off",
	                  "Writes what a game draws to PSP/TEXTURES/<GAMEID>/new/, to build a pack."});
}

void Settings::Reload() {
	dirty_ = false;
	Rebuild();
}

void Settings::BeginGame(const std::string &discId, const std::string &title) {
	EndGame();
	if (discId.empty()) {
		// Homebrew with no DISC_ID: PPSSPP keys its per-game ini on that id, so
		// there is nothing to key one on here either.
		return;
	}
	gameId_ = discId;
	gameTitle_ = title;
	overrides_ = g_Config.HasGameConfig(gameId_);
	if (overrides_) {
		g_Config.LoadGameConfig(gameId_);
	}
	dirty_ = false;
	Rebuild();
}

void Settings::SetOverrides(bool on) {
	if (gameId_.empty() || on == overrides_) {
		return;
	}
	if (on) {
		g_Config.CreateGameConfig(gameId_);
		g_Config.LoadGameConfig(gameId_);
		overrides_ = true;
		dirty_ = true;
	} else {
		// Leave the mode before the file goes, so the settings in memory go back
		// to the global ones rather than keeping the game's last values.
		if (g_Config.IsGameSpecific()) {
			g_Config.UnloadGameConfig();
		}
		g_Config.DeleteGameConfig(gameId_);
		overrides_ = false;
		dirty_ = false;
	}
	Rebuild();
}

void Settings::EndGame() {
	if (gameId_.empty()) {
		return;
	}
	if (g_Config.IsGameSpecific()) {
		if (dirty_) {
			g_Config.SaveGameConfig(gameId_, gameTitle_);
			psp5::Trace("settings: saved for %s", gameId_.c_str());
		}
		g_Config.UnloadGameConfig();
	}
	gameId_.clear();
	gameTitle_.clear();
	overrides_ = false;
	dirty_ = false;
	Rebuild();
}

bool Settings::Adjust(std::size_t index, int delta) {
	if (index >= items_.size() || delta == 0) {
		return false;
	}

	switch (index) {
		case 0:
			g_Config.iInternalResolution =
			    StepThrough(kResolutions, g_Config.iInternalResolution, delta);
			break;
		case 1:
			g_Config.iTexFiltering = StepThrough(kFiltering, g_Config.iTexFiltering, delta);
			break;
		case 2:
			g_Config.iAnisotropyLevel = StepThrough(kAnisotropy, g_Config.iAnisotropyLevel, delta);
			break;
		case 3:
			g_Config.bVSync = !g_Config.bVSync;
			break;
		case 4: {
			// Automatic sits one step past the end of the list, so the row reads
			// Off, 1, 2, 3, 4, Automatic and wraps.
			const int count = (int)std::size(kFrameSkip);
			const int at = g_Config.bAutoFrameSkip ? count : std::clamp(g_Config.iFrameSkip, 0, count - 1);
			const int next = ((at + delta) % (count + 1) + count + 1) % (count + 1);
			g_Config.bAutoFrameSkip = next == count;
			if (!g_Config.bAutoFrameSkip) {
				g_Config.iFrameSkip = kFrameSkip[(std::size_t)next].value;
			}
			break;
		}
		case 5:
			g_Config.bEnableSound = !g_Config.bEnableSound;
			break;
		case 6:
			// VOLUMEHI_FULL is 100; five is a step a player can hear.
			g_Config.iGameVolume = std::clamp(g_Config.iGameVolume + delta * 5, 0, VOLUMEHI_FULL);
			break;
		case 7: {
			// Off, then every set found under /app0/ui/sfx. Which one suits is a
			// matter of taste, so it is a choice rather than something psp5
			// decides - and adding one is adding a folder.
			const std::vector<std::string> &sets = prefs::soundSets();
			const int count = (int)sets.size() + 1;
			int at = 0;
			for (int i = 0; i < (int)sets.size(); ++i) {
				if (sets[(std::size_t)i] == prefs::soundSet()) {
					at = i + 1;
					break;
				}
			}
			at = ((at + delta) % count + count) % count;
			prefs::setSoundSet(at == 0 ? std::string() : sets[(std::size_t)(at - 1)]);
			break;
		}
		case 8:
			g_Config.iShowStatusFlags ^= (int)ShowStatusFlags::FPS_COUNTER;
			break;
		case 9: {
			// 0 means unlimited, and it sits past the top of the range rather
			// than at the bottom, where it would be reached by slowing down.
			int limit = g_Config.iAnalogFpsLimit <= 0 ? 1000 : g_Config.iAnalogFpsLimit;
			limit += delta * 50;
			if (limit > 1000) {
				limit = 0;
			} else if (limit < 100) {
				limit = g_Config.iAnalogFpsLimit == 0 ? 1000 : 100;
			}
			g_Config.iAnalogFpsLimit = limit;
			break;
		}
		case 10:
			prefs::setHardcore(!prefs::hardcore());
			break;
		case 11:
			prefs::setReplaceTextures(!prefs::replaceTextures());
			break;
		case 12:
			prefs::setSaveNewTextures(!prefs::saveNewTextures());
			break;
		default:
			return false;
	}

	dirty_ = true;
	Rebuild();
	return true;
}

void Settings::Save() {
	if (!dirty_) {
		return;
	}
	// A game's settings are written by EndGame, which also has to leave the mode
	// they are written in; saving the global ini here would write the game's
	// values into it.
	if (scopedToGame()) {
		return;
	}
	g_Config.Save("psp5 settings");
	dirty_ = false;
	psp5::Trace("settings saved");
}

}  // namespace psp5
