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
#include <utility>
#include <cstdint>
#include <vector>

#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/RetroAchievements.h"
#include "Common/Data/Text/I18n.h"
#include "Core/HLE/sceUtility.h"
#include "Core/KeyMap.h"
#include "Core/SaveState.h"
#include "Core/System.h"
#include "Common/StringUtils.h"
#include "Common/File/DirListing.h"

#include "PS5Log.h"
#include "PS5Paths.h"
#include "PS5UsbPad.h"
#include "ui/PS5AuroraLauncher.h"
#include "ui/PS5Prefs.h"
#include "ui/PS5Controls.h"
#include "ui/PS5Strings.h"

namespace psp5 {

// PPSSPP's own translation of a label, where it has one.
//
// psp5's interface is its own and none of its words are in PPSSPP's language
// files - but most of its settings are PPSSPP's settings, named the same way,
// and those are translated into forty-five languages already. So the rows ask
// for PPSSPP's string and fall back to their English when a language has not
// been given one.
//
// The `&` in these files marks a keyboard accelerator for the desktop builds.
// It is not part of the word and has to come out, or a French player reads
// "Synchronisation &verticale".
std::string Translate(I18NCat category, const char *key, const char *fallback) {
	std::string text(GetI18NCategory(category)->T(key, fallback));
	text.erase(std::remove(text.begin(), text.end(), '&'), text.end());
	return VisualOrder(text);
}

namespace {

// The USB row says what it found, because "On" alone cannot tell the difference
// between a pad that is working and a pad the console never showed us.
std::string UsbPadHint() {
	if (!prefs::usbPads()) {
		return Own("Controllers the console will not pair: Xbox 360, PS3, DualShock 3.");
	}
	const char *found = usb_pad_name();
	if (found == nullptr) {
		return Own("No USB controller found. A DualSense needs nothing here.");
	}
	return std::string(found) + " is connected.";
}

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
constexpr Choice kUpscale[] = {
    {1, "Off"}, {2, "2x"}, {3, "3x"}, {4, "4x"}, {5, "5x"},
};
constexpr Choice kUpscaleType[] = {
    {0, "xBRZ"}, {1, "Hybrid"}, {2, "Bicubic"}, {3, "Hybrid + Bicubic"},
};
constexpr Choice kMultiSample[] = {
    {0, "Off"}, {1, "2x"}, {2, "4x"}, {3, "8x"}, {4, "16x"},
};
constexpr Choice kFrameSkip[] = {
    {0, "Off"}, {1, "1 frame"}, {2, "2 frames"}, {3, "3 frames"}, {4, "4 frames"},
};
constexpr Choice kOnOff[] = {{0, "Off"}, {1, "On"}};

}  // namespace

// The page names, which PPSSPP has in its own main settings menu.
std::string settingCategoryName(SettingCategory category) {
	switch (category) {
		case SettingCategory::audio: return Translate(I18NCat::MAINSETTINGS, "Audio", "Sound");
		case SettingCategory::controls:
			return Translate(I18NCat::MAINSETTINGS, "Controls", "Controls");
		case SettingCategory::system:
			return Translate(I18NCat::MAINSETTINGS, "System", "System");
		default: return Translate(I18NCat::MAINSETTINGS, "Graphics", "Picture");
	}
}

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

// Asked by PPSSPP whenever it works out where texture packs live, which it
// does from the memory stick alone. A pack on a USB drive is at
// <drive>/PSP/TEXTURES/<DISC_ID>/, so psp5 answers with that drive's TEXTURES
// folder when one of them has a pack for the game being booted, and with
// nothing when none has - PPSSPP then uses the memory stick as before.
//
// The disc id comes from the launch the home screen made rather than from
// PPSSPP's own g_paramSFO, which is not loaded on every path that reaches
// here. The returned buffer is psp5's and lives until the next call; PPSSPP
// copies it into a Path straight away.
extern "C" const char *PS5_TexturesRoot(void) {
	static std::string root;
	const std::string &discId = psp5::PendingLaunchDiscId();
	root = PS5Paths::TexturesRoot(discId);
	if (!root.empty()) {
		psp5::Trace("textures: %s has a pack for %s", root.c_str(), discId.c_str());
	}
	return root.empty() ? nullptr : root.c_str();
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
	// PPSSPP's own sentence, which is translated into every language it
	// ships. psp5 worded this itself for a while, because it was the one
	// line on the home screen that followed the language and the fonts
	// could not draw a word of Japanese. They can now, so it goes back to
	// the translation.
	if (g_Config.TimeTracker().GetPlayedSeconds(discId) <= 0) {
		return std::string();
	}
	std::string text;
	if (!g_Config.TimeTracker().GetPlayedTimeString(discId, &text)) {
		return std::string();
	}
	// Straight from PPSSPP rather than through Translate, so the reordering
	// right-to-left languages need has to be asked for here too.
	return VisualOrder(text);
}

// The one phrase both the hero and the details sheet use, so the two cannot
// drift apart. PPSSPP's string already reads "Time Played: 0h 30m 30s", so
// nothing is added to it: a label in front of it said the same thing twice, and
// the middle dot that separated them is not in the kit's font atlas - which has
// about 113 glyphs - so it drew as a question mark.
namespace {

// What each of PPSSPP's languages is called, in English.
//
// langregion.ini names them in their own language - 日本語, Русский, Français -
// and the home screen's font atlas holds about a hundred glyphs, nearly all of
// them ASCII, so those come out as rows of question marks. The language id was
// shown instead at first, which is unambiguous and tells a player nothing.
//
// An English name is a name either way, and it draws. Where a language is
// missing from this table its native name is used if the atlas can draw it, and
// the id only as a last resort.
struct LanguageName {
	const char *id;
	const char *english;
};

constexpr LanguageName kLanguageNames[] = {
	{"ar_AE", "Arabic"},
	{"az_AZ", "Azerbaijani"},
	{"be_BY", "Belarusian"},
	{"bg_BG", "Bulgarian"},
	{"ca_ES", "Catalan"},
	{"cz_CZ", "Czech"},
	{"da_DK", "Danish"},
	{"de_DE", "German"},
	{"dr_ID", "Indonesian (Dari)"},
	{"en_US", "English"},
	{"es_ES", "Spanish"},
	{"es_LA", "Spanish (Latin America)"},
	{"fa_IR", "Persian"},
	{"fi_FI", "Finnish"},
	{"fr_FR", "French"},
	{"gl_ES", "Galician"},
	{"gr_EL", "Greek"},
	{"he_IL", "Hebrew"},
	{"he_IL_invert", "Hebrew (reversed)"},
	{"hr_HR", "Croatian"},
	{"hu_HU", "Hungarian"},
	{"id_ID", "Indonesian"},
	{"it_IT", "Italian"},
	{"ja_JP", "Japanese"},
	{"jv_ID", "Javanese"},
	{"ko_KR", "Korean"},
	{"ku_SO", "Kurdish"},
	{"lo_LA", "Lao"},
	{"lt-LT", "Lithuanian"},
	{"ms_MY", "Malay"},
	{"nl_NL", "Dutch"},
	{"no_NO", "Norwegian"},
	{"pl_PL", "Polish"},
	{"pt_BR", "Portuguese (Brazil)"},
	{"pt_PT", "Portuguese"},
	{"ro_RO", "Romanian"},
	{"ru_RU", "Russian"},
	{"sv_SE", "Swedish"},
	{"tg_PH", "Tagalog"},
	{"th_TH", "Thai"},
	{"tr_TR", "Turkish"},
	{"uk_UA", "Ukrainian"},
	{"vi_VN", "Vietnamese"},
	{"zh_CN", "Chinese (Simplified)"},
	{"zh_TW", "Chinese (Traditional)"},
};

bool Drawable(const std::string &text) {
	if (text.empty()) {
		return false;
	}
	for (unsigned char c : text) {
		if (c < 0x20 || c > 0x7E) {
			return false;
		}
	}
	return true;
}

// The languages PPSSPP ships, as id and label, in a fixed order.
const std::vector<std::pair<std::string, std::string>> &LanguageList() {
	static std::vector<std::pair<std::string, std::string>> list;
	if (!list.empty()) {
		return list;
	}
	for (const auto &entry : GetLangValuesMapping()) {
		const std::string &id = entry.first;
		std::string label;
		for (const LanguageName &known : kLanguageNames) {
			if (id == known.id) {
				label = known.english;
				break;
			}
		}
		if (label.empty()) {
			label = Drawable(entry.second.first) ? entry.second.first : id;
		}
		list.emplace_back(id, label);
	}
	return list;
}

}  // namespace

// A few words the home screen draws outside the settings list. Same rule: ask
// PPSSPP, keep psp5's English when it has nothing.
std::string UiWord(UiString which) {
	switch (which) {
		// Pause, not Dialog: Dialog's "Settings" is left untranslated in most of
		// PPSSPP's language files, and the pause menu's is not.
		case UiString::settings: return Translate(I18NCat::PAUSE, "Settings", "Settings");
		case UiString::cheats: return Translate(I18NCat::CWCHEATS, "Cheats", "Cheats");
		case UiString::achievements:
			return Translate(I18NCat::ACHIEVEMENTS, "Achievements", "Achievements");
		case UiString::saveState: return Translate(I18NCat::PAUSE, "Save State", "Save state");
		case UiString::loadState: return Translate(I18NCat::PAUSE, "Load State", "Load state");
		case UiString::back: return Translate(I18NCat::DIALOG, "Back", "Back");
		case UiString::select: return Translate(I18NCat::DIALOG, "Select", "Select");
		case UiString::play: return Translate(I18NCat::GAME, "Play", "Play");
		case UiString::recent: return Translate(I18NCat::MAINMENU, "Recent", "Recent");
		case UiString::games: return Translate(I18NCat::MAINMENU, "Games", "Games");
		case UiString::details: return Translate(I18NCat::DIALOG, "Details", "Details");
		case UiString::gameSettings:
			return Translate(I18NCat::GAME, "Game Settings", "Game settings");
		case UiString::resume: return Translate(I18NCat::DEVELOPER, "Resume", "Resume");
		case UiString::enableCheats:
			return Translate(I18NCat::SYSTEM, "Enable Cheats", "Cheats enabled");
		case UiString::gameSettings2:
			return Translate(I18NCat::GAME, "Game Settings", "Settings for this game");
		case UiString::retry: return Translate(I18NCat::DIALOG, "Retry", "Retry");
		case UiString::cancel: return Translate(I18NCat::DIALOG, "Cancel", "Cancel");
		case UiString::logOut: return Translate(I18NCat::DIALOG, "Log out", "Sign out");
		case UiString::loading: return Translate(I18NCat::MAINMENU, "Loading...", "Loading...");
		case UiString::off: return Translate(I18NCat::CONTROLS, "Off", "Off");
		case UiString::automatic: return Translate(I18NCat::AUDIO, "Auto", "Automatic");
		case UiString::unlimited: return Translate(I18NCat::GRAPHICS, "Unlimited", "Unlimited");
		case UiString::favorites: return Own("Favorites");
		case UiString::alphabetical: return Own("A-Z");
		case UiString::favorite: return Own("Favorite");
		default: return Translate(I18NCat::DIALOG, "Exit", "Exit");
	}
}

std::string LanguageLabel() {
	for (const auto &entry : LanguageList()) {
		if (entry.first == g_Config.sLanguageIni) {
			return entry.second;
		}
	}
	return g_Config.sLanguageIni.empty() ? std::string("en_US") : g_Config.sLanguageIni;
}

void StepLanguage(int delta) {
	const std::vector<std::pair<std::string, std::string>> &list = LanguageList();
	if (list.empty()) {
		return;
	}
	int at = 0;
	for (int i = 0; i < (int)list.size(); ++i) {
		if (list[(std::size_t)i].first == g_Config.sLanguageIni) {
			at = i;
			break;
		}
	}
	const int count = (int)list.size();
	at = ((at + delta) % count + count) % count;
	g_Config.sLanguageIni = list[(std::size_t)at].first;
	// PPSSPP's own strings, reloaded at once. The emulated PSP reads the same
	// setting through sceUtility when iLanguage is -1, which is its default, so
	// this is also the language a game is told the console is set to.
	g_i18nrepo.LoadIni(g_Config.sLanguageIni);
	LoadOwnStrings(g_Config.sLanguageIni);
	psp5::Trace("language: %s", g_Config.sLanguageIni.c_str());
}

std::uint64_t LastPlayed(const std::string &discId) {
	if (discId.empty()) {
		return 0;
	}
	return g_Config.TimeTracker().GetLastPlayed(discId);
}

std::string PlayedLabel(const std::string &discId) {
	const std::string played = PlayedTime(discId);
	return played.empty() ? Own("Not played yet") : played;
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

	// ---- Picture ----
	items_.push_back({SettingKey::resolution, SettingCategory::graphics, Translate(I18NCat::GRAPHICS, "Rendering Resolution", "Rendering resolution"),
	                  NameOf(kResolutions, g_Config.iInternalResolution, "%dx"),
	                  Own("How much sharper than a PSP the picture is drawn.")});

	items_.push_back({SettingKey::filtering, SettingCategory::graphics, Translate(I18NCat::GRAPHICS, "Texture Filtering", "Texture filtering"),
	                  NameOf(kFiltering, g_Config.iTexFiltering, "%d"),
	                  Own("Nearest keeps the original pixels; linear smooths them.")});

	items_.push_back({SettingKey::anisotropy, SettingCategory::graphics, Translate(I18NCat::GRAPHICS, "Anisotropic Filtering", "Anisotropic filtering"),
	                  NameOf(kAnisotropy, g_Config.iAnisotropyLevel, "%d"),
	                  Own("Sharpens textures seen at a steep angle, like roads and floors.")});

	items_.push_back({SettingKey::upscale, SettingCategory::graphics, Translate(I18NCat::GRAPHICS, "Upscale Level", "Texture upscaling"),
	                  NameOf(kUpscale, g_Config.iTexScalingLevel, "%dx"),
	                  Own("Redraws the game's textures larger, on the CPU. Costly.")});

	items_.push_back({SettingKey::upscaleType, SettingCategory::graphics, Translate(I18NCat::GRAPHICS, "CPU texture upscaler (slow)", "Upscaling method"),
	                  NameOf(kUpscaleType, g_Config.iTexScalingType, "%d"),
	                  Own("xBRZ keeps edges crisp; bicubic is softer and cheaper.")});

	items_.push_back({SettingKey::deposterize, SettingCategory::graphics, Translate(I18NCat::GRAPHICS, "Deposterize", "Deposterize"),
	                  g_Config.bTexDeposterize ? "On" : "Off",
	                  Own("Smooths the banding upscaling can leave in a gradient.")});

	items_.push_back({SettingKey::antialiasing, SettingCategory::graphics, Translate(I18NCat::GRAPHICS, "Antialiasing (MSAA)", "Antialiasing"),
	                  NameOf(kMultiSample, g_Config.iMultiSampleLevel, "%d"),
	                  Own("Softens jagged edges. Off when buffer effects are skipped.")});

	items_.push_back({SettingKey::smart2d, SettingCategory::graphics, Translate(I18NCat::GRAPHICS, "Smart 2D texture filtering", "Smart 2D texture filtering"),
	                  g_Config.bSmart2DTexFiltering ? "On" : "Off",
	                  Own("Leaves flat 2D art alone so menus and text stay sharp.")});

	items_.push_back({SettingKey::crop16x9, SettingCategory::graphics, Own("Crop to 16:9"),
	                  g_Config.bDisplayCropTo16x9 ? "On" : "Off",
	                  Own("Trims a sliver off the top and bottom to fill a widescreen.")});

	items_.push_back({SettingKey::vsync, SettingCategory::graphics, Translate(I18NCat::GRAPHICS, "VSync", "Vertical sync"),
	                  g_Config.bVSync ? "On" : "Off",
	                  Own("On removes tearing; off lets frames arrive as soon as they are drawn.")});

	items_.push_back({SettingKey::frameskip, SettingCategory::graphics, Translate(I18NCat::GRAPHICS, "Frame Skipping", "Frame skipping"),
	                  g_Config.bAutoFrameSkip ? std::string("Automatic")
	                                          : NameOf(kFrameSkip, g_Config.iFrameSkip, "%d"),
	                  Own("Drops frames to keep a heavy game at speed. Off looks best.")});

	items_.push_back({SettingKey::frameRate, SettingCategory::graphics, Translate(I18NCat::GRAPHICS, "Show FPS Counter", "Show frame rate"),
	                  (g_Config.iShowStatusFlags & (int)ShowStatusFlags::FPS_COUNTER) ? "On" : "Off",
	                  Own("Draws the frame rate over the game.")});

	items_.push_back({SettingKey::hardwareTransform, SettingCategory::graphics, Translate(I18NCat::GRAPHICS, "Hardware Transform", "Hardware transform"),
	                  g_Config.bHardwareTransform ? "On" : "Off",
	                  Own("Transforms geometry on the GPU. Off is slower and more accurate.")});

	items_.push_back({SettingKey::softwareSkinning, SettingCategory::graphics, Translate(I18NCat::GRAPHICS, "Software Skinning", "Software skinning"),
	                  g_Config.bSoftwareSkinning ? "On" : "Off",
	                  Own("Animates characters on the CPU. Faster in most games.")});

	items_.push_back({SettingKey::lazyTextures, SettingCategory::graphics,
	                  Translate(I18NCat::GRAPHICS, "Lazy texture caching",
	                            "Lazy texture caching"),
	                  g_Config.bTextureBackoffCache ? "On" : "Off",
	                  Own("Rechecks textures less often. Faster, and wrong in a few games.")});

	items_.push_back({SettingKey::skipBufferEffects, SettingCategory::graphics, Translate(I18NCat::GRAPHICS, "Skip Buffer Effects", "Skip buffer effects"),
	                  g_Config.bSkipBufferEffects ? "On" : "Off",
	                  Own("Much faster, and drops the effects some games draw off-screen.")});

	// ---- Sound ----
	items_.push_back({SettingKey::sound, SettingCategory::audio, Translate(I18NCat::AUDIO, "Enable Sound", "Sound"), g_Config.bEnableSound ? "On" : "Off",
	                  Own("Audio from the emulator.")});

	items_.push_back({SettingKey::volume, SettingCategory::audio, Translate(I18NCat::AUDIO, "Game volume", "Game volume"),
	                  Format("%d", g_Config.iGameVolume), Own("0 to 100, in steps of five.")});

	if (!scopedToGame()) {
		items_.push_back({SettingKey::menuSounds, SettingCategory::audio,
		                  Translate(I18NCat::AUDIO, "UI sound", "Menu sounds"),
		                  prefs::soundSetLabel(prefs::soundSet()),
		                  Own("The home screen's own sounds. A game's music is separate.")});
	}

	// ---- Controls ----
	//
	// What a game is sent. The home screen and the in-game menu read the pad
	// directly, so Cross still chooses and Circle still goes back whatever is
	// set here - which is the point: a mapping that could lock someone out of
	// the panel that made it is a mapping worth not having.
	// psp5 keeps one mapping for the whole title, as it keeps one of each of
	// the switches below, so these are shown only in the title's own panel. In
	// a game's they would read as that game's and change every game - which is
	// the mistake the cheats switch and the texture rows already made once.
	if (!scopedToGame()) {
		items_.push_back({SettingKey::mapping, SettingCategory::controls, Own("Buttons"),
		                  controls::IsDefault() ? Own("Default") : Own("Changed"),
		                  Own("Which console button each of the PSP's is on.")});

		items_.push_back({SettingKey::mappingReset, SettingCategory::controls,
		                  Own("Reset the buttons"),
		                  controls::IsDefault() ? Own("Default") : Own("Changed"),
		                  Own("Puts all twelve back where they started.")});

		items_.push_back({SettingKey::usbPads, SettingCategory::controls, Own("USB controllers"),
		                  prefs::usbPads() ? "On" : "Off", UsbPadHint()});
	}

	// ---- System ----
	items_.push_back({SettingKey::language, SettingCategory::system, Translate(I18NCat::SYSTEM, "Language", "Language"),
	                  LanguageLabel(),
	                  Own("What a game is told the console's language is, and PPSSPP's own text.")});

	items_.push_back({SettingKey::fastForward, SettingCategory::controls,
	                  Translate(I18NCat::MAPPABLECONTROLS, "Fast-forward",
	                            "Fast-forward speed"),
	                  g_Config.iAnalogFpsLimit <= 0 ? std::string("Unlimited")
	                                                : Format("%d%%", g_Config.iAnalogFpsLimit),
	                  Own("How fast the game runs while the right trigger is held.")});

	// psp5's own settings, and psp5 keeps one of each for the whole title - it
	// has nowhere to put a second. Shown only in the title's panel: in a game's
	// they looked per-game and were not, so changing one there changed it
	// everywhere, which is exactly what it appeared not to do.
	if (!scopedToGame()) {
		items_.push_back({SettingKey::hardcore, SettingCategory::system,
		                  Translate(I18NCat::ACHIEVEMENTS, "Hardcore Mode",
		                            "Achievements hardcore mode"),
		                  prefs::hardcore() ? "On" : "Off",
		                  Own("On earns hardcore unlocks but turns off save states entirely.")});

		items_.push_back({SettingKey::replaceTextures, SettingCategory::system, Translate(I18NCat::DEVELOPER, "Texture Replacement", "Texture replacement"),
		                  prefs::replaceTextures() ? "On" : "Off",
		                  Own("Uses a pack from PSP/TEXTURES/<GAMEID>/ when a game has one.")});

		items_.push_back({SettingKey::saveNewTextures, SettingCategory::system, Translate(I18NCat::DEVELOPER, "Save new textures", "Save new textures"),
		                  prefs::saveNewTextures() ? "On" : "Off",
		                  Own("Writes what a game draws to PSP/TEXTURES/<GAMEID>/new/, to build a pack.")});
	}
}

void Settings::Reload() {
	dirty_ = false;
	Rebuild();
}

void Settings::Refresh() {
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

	// On the key, not the position: the pages hold different rows and a game's
	// panel leaves four out, so where a row sits says nothing about what it is.
	switch (items_[index].key) {
		case SettingKey::resolution:
			g_Config.iInternalResolution =
			    StepThrough(kResolutions, g_Config.iInternalResolution, delta);
			break;
		case SettingKey::filtering:
			g_Config.iTexFiltering = StepThrough(kFiltering, g_Config.iTexFiltering, delta);
			break;
		case SettingKey::anisotropy:
			g_Config.iAnisotropyLevel = StepThrough(kAnisotropy, g_Config.iAnisotropyLevel, delta);
			break;
		case SettingKey::upscale:
			g_Config.iTexScalingLevel = StepThrough(kUpscale, g_Config.iTexScalingLevel, delta);
			break;
		case SettingKey::upscaleType:
			g_Config.iTexScalingType = StepThrough(kUpscaleType, g_Config.iTexScalingType, delta);
			break;
		case SettingKey::deposterize:
			g_Config.bTexDeposterize = !g_Config.bTexDeposterize;
			break;
		case SettingKey::antialiasing:
			g_Config.iMultiSampleLevel =
			    StepThrough(kMultiSample, g_Config.iMultiSampleLevel, delta);
			break;
		case SettingKey::smart2d:
			g_Config.bSmart2DTexFiltering = !g_Config.bSmart2DTexFiltering;
			break;
		case SettingKey::crop16x9:
			g_Config.bDisplayCropTo16x9 = !g_Config.bDisplayCropTo16x9;
			break;
		case SettingKey::vsync:
			g_Config.bVSync = !g_Config.bVSync;
			break;
		case SettingKey::frameskip: {
			// Automatic sits one step past the end of the list, so the row reads
			// Off, 1, 2, 3, 4, Automatic and wraps.
			const int count = (int)std::size(kFrameSkip);
			const int at =
			    g_Config.bAutoFrameSkip ? count : std::clamp(g_Config.iFrameSkip, 0, count - 1);
			const int next = ((at + delta) % (count + 1) + count + 1) % (count + 1);
			g_Config.bAutoFrameSkip = next == count;
			if (!g_Config.bAutoFrameSkip) {
				g_Config.iFrameSkip = kFrameSkip[(std::size_t)next].value;
			}
			break;
		}
		case SettingKey::frameRate:
			g_Config.iShowStatusFlags ^= (int)ShowStatusFlags::FPS_COUNTER;
			break;
		case SettingKey::hardwareTransform:
			g_Config.bHardwareTransform = !g_Config.bHardwareTransform;
			break;
		case SettingKey::softwareSkinning:
			g_Config.bSoftwareSkinning = !g_Config.bSoftwareSkinning;
			break;
		case SettingKey::lazyTextures:
			g_Config.bTextureBackoffCache = !g_Config.bTextureBackoffCache;
			break;
		case SettingKey::skipBufferEffects:
			g_Config.bSkipBufferEffects = !g_Config.bSkipBufferEffects;
			break;
		case SettingKey::sound:
			g_Config.bEnableSound = !g_Config.bEnableSound;
			break;
		case SettingKey::volume:
			// VOLUMEHI_FULL is 100; five is a step a player can hear.
			g_Config.iGameVolume = std::clamp(g_Config.iGameVolume + delta * 5, 0, VOLUMEHI_FULL);
			break;
		case SettingKey::menuSounds: {
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
		case SettingKey::language:
			StepLanguage(delta);
			break;
		case SettingKey::fastForward: {
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
		case SettingKey::mapping:
			// Opening the panel is the screen's business (ui/kit/aurora.cpp):
			// left and right have nothing to step here.
			return false;
		case SettingKey::mappingReset:
			if (controls::IsDefault()) {
				return false;
			}
			controls::Reset();
			break;
		case SettingKey::usbPads:
			prefs::setUsbPads(!prefs::usbPads());
			// Started here as well as at boot, so turning it on works without
			// closing the title; turning it off stops the thread and lets go of
			// the device, which is the half that matters.
			if (prefs::usbPads()) {
				usb_pad_start();
			} else {
				usb_pad_stop();
			}
			break;
		case SettingKey::hardcore:
			prefs::setHardcore(!prefs::hardcore());
			break;
		case SettingKey::replaceTextures:
			prefs::setReplaceTextures(!prefs::replaceTextures());
			break;
		case SettingKey::saveNewTextures:
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
