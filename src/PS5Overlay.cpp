// psp5 - the menu that opens over a running game.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drawn with PPSSPP's UIContext, because during a game PPSSPP owns the frame and
// there is no second renderer to borrow. It is not one of PPSSPP's screens
// though: it has no views, takes no part in the screen stack, and psp5 draws
// every rectangle of it - so none of PPSSPP's own interface comes with it.
//
// Two levels, not one. A game with thirty codes in its cheat file turned a flat
// list into something taller than the screen, with the way out of the game
// somewhere past the bottom of it. The root holds three entries, and each opens
// what belongs to it.
//
// Input never reaches PPSSPP while this is up. PS5Main stops forwarding the pad,
// which is the only way a press can move the selection without also moving the
// player.

#include "PS5Overlay.h"

#include <algorithm>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "Common/Render/DrawBuffer.h"
#include "Common/TimeUtil.h"
#include "Common/System/System.h"
#include "Common/UI/Context.h"
#include "Common/UI/View.h"
#include "Core/Config.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/RetroAchievements.h"
#include "Core/SaveState.h"
#include "Core/System.h"

#include "PS5Achievements.h"
#include "PS5Log.h"
#include "PS5OverlayDraw.h"
#include "platform/platform.h"
#include "ui/PS5Cheats.h"

// Defined by PPSSPP (patches/ppsspp/ps5-standalone.patch): stops and restarts
// the cheat engine so a file edited underneath it is read again.
extern "C" void PS5_ReloadCheats();

namespace psp5 {

namespace {

bool g_open = false;

// What just happened, and until when. PPSSPP used to say "Saved State" through
// its own on-screen messages; psp5 silences those, so saving appeared to do
// nothing at all. This says it instead, in psp5's own panel, and outlives the
// menu closing so the player sees it over the game.
std::string g_notice;
double g_noticeUntil = 0.0;

void Notice(std::string text) {
	g_notice = std::move(text);
	g_noticeUntil = time_now_d() + 2.5;
}
int g_row = 0;
std::string g_discId;
int g_slot = 0;

// Which list the bar is showing. The root is three entries; everything else is
// behind one of them.
enum class Page {
	root,
	cheats,
	saveState,
};

Page g_page = Page::root;

enum class Kind {
	openCheats,
	openSaveState,
	exitGame,
	cheatMaster,
	cheatImport,
	cheat,
	slot,
	save,
	load,
	note,  // says something; cannot be selected
};

struct Row {
	Kind kind = Kind::note;
	std::string label;
	std::string value;
	int index = 0;  // which cheat, for Kind::cheat
	bool on = false;
	bool toggle = false;
	bool submenu = false;
	bool selectable = true;
};

std::vector<Row> g_rows;

std::string GamePrefix() {
	return SaveState::GetGamePrefix(g_paramSFO);
}

const char *PageTitle() {
	switch (g_page) {
		case Page::cheats: return "CHEATS";
		case Page::saveState: return "SAVE STATE";
		default: return "MENU";
	}
}

void Rebuild() {
	g_rows.clear();
	char text[64];

	switch (g_page) {
		case Page::root: {
			std::snprintf(text, sizeof(text), "%u", (unsigned)CheatList().size());
			g_rows.push_back({Kind::openCheats, "Cheats", CheatList().empty() ? "none" : text, 0,
			                  false, false, true, true});
			g_rows.push_back({Kind::openSaveState, "Save state", "", 0, false, false, true, true});
			g_rows.push_back({Kind::exitGame, "Exit game", "", 0, false, false, false, true});
			break;
		}
		case Page::cheats: {
			g_rows.push_back({Kind::cheatMaster, "Cheats enabled", "", 0, CheatsEnabled(),
			                  true, false, true});
			const std::span<const CheatEntry> cheats = CheatList().items();
			if (!CheatsEnabled()) {
				// Switched off, the codes below do nothing, and a list of
				// switches that cannot take effect invites turning them on and
				// wondering why nothing happened.
				std::snprintf(text, sizeof(text), "%u code%s in this game's file",
				              (unsigned)cheats.size(), cheats.size() == 1 ? "" : "s");
				g_rows.push_back({Kind::note,
				                  cheats.empty() ? "Turn this on to use cheats" : text, "", 0,
				                  false, false, false, false});
				break;
			}
			if (Cheats::DatabaseExists()) {
				g_rows.push_back({Kind::cheatImport, "Import from cheat.db", "", 0, false,
				                  false, false, true});
			}
			for (int i = 0; i < (int)cheats.size(); ++i) {
				g_rows.push_back({Kind::cheat, cheats[(std::size_t)i].name, "", i,
				                  cheats[(std::size_t)i].enabled, true, false, true});
			}
			if (cheats.empty()) {
				g_rows.push_back(
				    {Kind::note, "No cheat file for this game", "", 0, false, false, false, false});
			}
			break;
		}
		case Page::saveState: {
			std::snprintf(text, sizeof(text), "%d", g_slot + 1);
			g_rows.push_back({Kind::slot, "Slot", text, 0, false, false, false, true});
			const bool hardcore = Achievements::HardcoreModeActive();
			g_rows.push_back({Kind::save, "Save to this slot", hardcore ? "hardcore" : "", 0,
			                  false, false, false, !hardcore});
			const bool has = !hardcore && SaveState::HasSaveInSlot(GamePrefix(), g_slot);
			g_rows.push_back({Kind::load, "Load from this slot",
			                  hardcore ? "hardcore" : (has ? "" : "empty"), 0, false, false, false,
			                  has});
			if (hardcore) {
				g_rows.push_back({Kind::note,
				                  "Hardcore mode is on, so RetroAchievements does not allow save "
				                  "states. Turn it off in settings.",
				                  "", 0, false, false, false, false});
			}
			break;
		}
	}
}

void SettleOnSelectable() {
	if (g_row >= 0 && g_row < (int)g_rows.size() && g_rows[(std::size_t)g_row].selectable) {
		return;
	}
	for (int i = 0; i < (int)g_rows.size(); ++i) {
		if (g_rows[(std::size_t)i].selectable) {
			g_row = i;
			return;
		}
	}
	g_row = 0;
}

void GoTo(Page page) {
	g_page = page;
	g_row = 0;
	Rebuild();
	SettleOnSelectable();
}

}  // namespace

void SetCheatOverlayGame(const std::string &discId) {
	g_discId = discId;
	g_open = false;
	g_row = 0;
	g_slot = 0;
	g_page = Page::root;
	CheatList().Clear();
	g_rows.clear();
}

bool CheatOverlayOpen() {
	return g_open;
}

void CloseCheatOverlay() {
	if (!g_open) {
		return;
	}
	g_open = false;
	psp5::Trace("menu: closed");
	// The switch belongs to this game, and the menu is where it was changed.
	SaveCheatsEnabled();
	if (CheatList().dirty()) {
		CheatList().Save();
		// The engine holds the codes it parsed at boot, so the file on its own
		// changes nothing until it is read again.
		PS5_ReloadCheats();
		psp5::Trace("cheats: reloaded after an edit");
	}
}

void ToggleCheatOverlay() {
	if (g_open) {
		CloseCheatOverlay();
		return;
	}
	CloseAchievementsBar();
	// Read on the way in, so a file copied while the title was running is found.
	CheatList().Load(g_discId);
	g_slot = std::clamp(SaveState::GetCurrentSlot(), 0, 7);
	GoTo(Page::root);
	g_open = true;
	// SaveState keeps a listing of the state directory rather than asking the
	// file system each time, and SaveSlot refreshes it before the write it
	// schedules has happened - so a slot saved a moment ago still reads as
	// empty. PPSSPP's own pause screen rescans whenever it opens; this is that.
	SaveState::Rescan(GamePrefix());
	psp5::Trace("menu: open, %u cheat(s)", (unsigned)CheatList().size());
}

void CheatOverlayBack() {
	if (!g_open) {
		return;
	}
	// One level at a time. The root is where closing happens, so a mistaken
	// press inside a submenu costs a step back rather than the whole menu.
	if (g_page != Page::root) {
		GoTo(Page::root);
		return;
	}
	CloseCheatOverlay();
}

void CheatOverlayMove(int delta) {
	if (!g_open || g_rows.empty()) {
		return;
	}
	const int count = (int)g_rows.size();
	int at = g_row;
	for (int guard = 0; guard < count; ++guard) {
		at += delta;
		if (at < 0 || at >= count) {
			return;  // the ends refuse rather than wrap
		}
		if (g_rows[(std::size_t)at].selectable) {
			g_row = at;
			return;
		}
	}
}

void CheatOverlayAdjust(int delta) {
	if (!g_open || g_row >= (int)g_rows.size()) {
		return;
	}
	const Row &row = g_rows[(std::size_t)g_row];
	// Right opens a submenu, which is the way its chevron points; left is the
	// way back, handled with Circle.
	if (row.submenu) {
		if (delta > 0) {
			CheatOverlaySelect();
		} else {
			CheatOverlayBack();
		}
		return;
	}
	if (row.kind == Kind::slot) {
		g_slot = std::clamp(g_slot + delta, 0, 7);
		Rebuild();
		return;
	}
	if (delta < 0) {
		CheatOverlayBack();
	}
}

void CheatOverlaySelect() {
	if (!g_open || g_row >= (int)g_rows.size()) {
		return;
	}
	const Row row = g_rows[(std::size_t)g_row];
	switch (row.kind) {
		case Kind::openCheats:
			GoTo(Page::cheats);
			return;
		case Kind::openSaveState:
			GoTo(Page::saveState);
			return;
		case Kind::cheatMaster:
			SetCheatsEnabled(!CheatsEnabled());
			// hleCheat notices this within a second by itself, but waiting a
			// second to see a switch respond reads as a panel that missed the
			// press.
			PS5_ReloadCheats();
			break;
		case Kind::cheat:
			CheatList().Toggle((std::size_t)row.index);
			break;
		case Kind::cheatImport: {
			// Whatever is switched on and unwritten goes first: the import
			// appends to the same file, and saving after it would write the
			// list as it was read and lose what was just added.
			Cheats &cheats = CheatList();
			if (cheats.dirty()) {
				cheats.Save();
			}
			int added = 0;
			const Cheats::Import result = cheats.ImportFromDatabase(&added);
			char text[96];
			switch (result) {
				case Cheats::Import::added:
					std::snprintf(text, sizeof(text), "Imported %d line%s from cheat.db", added,
					              added == 1 ? "" : "s");
					// The engine is holding the file as it was before the
					// append, so the new codes exist only on disk until it
					// reads it again.
					PS5_ReloadCheats();
					break;
				case Cheats::Import::none:
					std::snprintf(text, sizeof(text), "cheat.db has nothing new for this game");
					break;
				case Cheats::Import::noFile:
					std::snprintf(text, sizeof(text), "No cheat.db in PSP/Cheats");
					break;
				case Cheats::Import::noGame:
					std::snprintf(text, sizeof(text), "This game has no disc id to look up");
					break;
				default:
					std::snprintf(text, sizeof(text), "Could not write the cheat file");
					break;
			}
			Notice(text);
			break;
		}
		case Kind::slot:
			g_slot = (g_slot + 1) % 8;
			break;
		case Kind::save: {
			// rcheevos forbids save states in hardcore mode, and PPSSPP enforces
			// it inside SaveState::Enqueue by dropping the operation without a
			// word - no callback, no log line, nothing. Selecting Save closed
			// the panel and did nothing at all, which is what "save state is not
			// working" looked like. Checked here so the panel can say so.
			if (Achievements::HardcoreModeActive()) {
				Notice("Hardcore mode is on - save states are off");
				psp5::Trace("save state: refused, hardcore mode is active");
				return;
			}
			const std::string prefix = GamePrefix();
			const int slot = g_slot;
			psp5::Trace("save state: saving %s slot %d", prefix.c_str(), slot + 1);
			// The work happens later, inside the emulation loop, so the outcome
			// arrives here rather than at the call. Swallowing it is what made
			// the missing PPSSPP_STATE directory invisible.
			SaveState::SaveSlot(prefix, slot, [slot, prefix](SaveState::Status status,
			                                                std::string_view message) {
				const bool failed = status == SaveState::Status::FAILURE;
				psp5::Trace("save state: slot %d %s%s%.*s", slot + 1,
				            failed ? "failed" : "saved",
				            message.empty() ? "" : ": ", (int)message.size(), message.data());
				// Now the file is actually there, which is the only point at
				// which a listing of the directory is worth anything.
				SaveState::Rescan(prefix);
				char text[64];
				std::snprintf(text, sizeof(text), failed ? "Could not save to slot %d"
				                                         : "Saved to slot %d", slot + 1);
				Notice(text);
			});
			CloseCheatOverlay();
			return;
		}
		case Kind::load: {
			if (Achievements::HardcoreModeActive()) {
				Notice("Hardcore mode is on - save states are off");
				psp5::Trace("save state: refused, hardcore mode is active");
				return;
			}
			const std::string prefix = GamePrefix();
			const int slot = g_slot;
			if (!SaveState::HasSaveInSlot(prefix, slot)) {
				return;
			}
			psp5::Trace("save state: loading %s slot %d", prefix.c_str(), slot + 1);
			SaveState::LoadSlot(prefix, slot, [slot](SaveState::Status status,
			                                        std::string_view message) {
				const bool failed = status == SaveState::Status::FAILURE;
				psp5::Trace("save state: slot %d %s%s%.*s", slot + 1,
				            failed ? "failed" : "loaded",
				            message.empty() ? "" : ": ", (int)message.size(), message.data());
				char text[64];
				std::snprintf(text, sizeof(text), failed ? "Could not load slot %d"
				                                         : "Loaded slot %d", slot + 1);
				Notice(text);
			});
			CloseCheatOverlay();
			return;
		}
		case Kind::exitGame:
			psp5::Trace("menu: exit game");
			CloseCheatOverlay();
			System_PostUIMessage(UIMessage::REQUEST_GAME_STOP);
			return;
		default:
			return;
	}
	Rebuild();
	SettleOnSelectable();
}

}  // namespace psp5

extern "C" bool PS5_WantsOverlay() {
	// Either bar keeps renderUI running, which is the only reason it is called
	// at all when PPSSPP has nothing of its own to draw.
	return psp5::CheatOverlayOpen() || psp5::AchievementsBarOpen() ||
	       time_now_d() < psp5::g_noticeUntil;
}

extern "C" void PS5_DrawOverlays(UIContext *ui) {
	if (!ui) {
		return;
	}
	PS5_DrawAchievementsBar(ui);

	// The notice sits above everything, including the panel, because it is what
	// the player just did.
	if (time_now_d() < psp5::g_noticeUntil && !psp5::g_notice.empty()) {
		const Bounds screen = ui->GetBounds();
		const float w = 420.0f;
		const float h = 64.0f;
		const Bounds box((screen.w - w) * 0.5f, screen.h - h - 70.0f, w, h);
		psp5::FillRoundOutlined(ui, box, psp5::kRadius, psp5::kSurfaceHigh, psp5::kOutline, 1.0f);
		ui->SetFontStyle(ui->GetTheme().uiFont);
		ui->SetFontScale(0.78f, 0.78f);
		ui->DrawTextRect(psp5::g_notice, box, psp5::kInk, ALIGN_HCENTER | ALIGN_VCENTER);
		ui->SetFontScale(1.0f, 1.0f);
		ui->Flush();
	}

	if (!psp5::CheatOverlayOpen()) {
		return;
	}

	// The same bar the home screen draws, and the same one the achievements use
	// on R1 + R3 - but down the left edge, so the two can be told apart at a
	// glance and could in principle be open at once. A small tracked label over
	// a large title, then the rows; a row is a highlight and an accent edge
	// rather than a card with a border, because the home screen draws no cards
	// and these are meant to read as one program.
	const Bounds screen = ui->GetBounds();
	const float width = std::min(620.0f, screen.w * 0.46f);
	const float x = 0.0f;
	const float pad = 44.0f;
	const float left = x + pad;
	const float inner = width - pad * 2.0f;
	const float scale = screen.h / 1080.0f;
	const float rowHeight = 72.0f;

	ui->FillRect(UI::Drawable(psp5::kShade), Bounds(width, 0.0f, screen.w - width, screen.h));
	ui->FillRect(UI::Drawable(psp5::kPage), Bounds(x, 0.0f, width, screen.h));
	ui->FillRect(UI::Drawable(0x2EFFFFFF), Bounds(width - 1.5f, 0.0f, 1.5f, screen.h));

	// ---- the head ----
	ui->SetFontStyle(ui->GetTheme().uiFont);
	ui->SetFontScale(0.52f, 0.52f);
	ui->DrawText(psp5::g_page == psp5::Page::root ? "L2 + R2  CLOSE" : "CIRCLE  BACK", left,
	             78.0f * scale, psp5::kPrimary, ALIGN_LEFT | ALIGN_TOP);
	ui->SetFontScale(0.95f, 0.95f);
	ui->DrawTextRect(psp5::PageTitle(), Bounds(left, 118.0f * scale, inner, 44.0f), psp5::kInk,
	                 ALIGN_LEFT | ALIGN_TOP);

	const float top = 188.0f * scale;
	const int rows = (int)psp5::g_rows.size();
	const int visible = std::max(1, (int)((screen.h - top - 70.0f) / rowHeight));
	int first = std::clamp(psp5::g_row - visible / 2, 0, std::max(0, rows - visible));
	const int last = std::min(rows, first + visible);

	float y = top;
	for (int i = first; i < last; ++i) {
		const auto &row = psp5::g_rows[(std::size_t)i];
		const bool focused = i == psp5::g_row;
		// The codes are indented under the switch and the import that govern
		// them: a flat list read as though all three were the same kind of thing.
		const float indent = row.kind == psp5::Kind::cheat ? 24.0f : 0.0f;

		if (row.kind == psp5::Kind::note) {
			// Not a row to land on: said quietly, and wrapped, because these say
			// why a list is empty and that is a sentence.
			float w = 0.0f;
			float h = 0.0f;
			ui->SetFontScale(0.56f, 0.56f);
			ui->MeasureTextRect(ui->GetTheme().uiFont, 0.56f, 0.56f, row.label, inner, &w, &h,
			                    ALIGN_LEFT | FLAG_WRAP_TEXT);
			ui->DrawTextRect(row.label, Bounds(left, y + 8.0f, inner, h), psp5::kInkFaint,
			                 ALIGN_LEFT | ALIGN_TOP | FLAG_WRAP_TEXT);
			y += h + 22.0f;
			continue;
		}

		if (focused) {
			// Square, not rounded: a full-width band reads as the row itself
			// being lit rather than as a pill laid over it.
			ui->FillRect(UI::Drawable(0x1AFFFFFF),
			             Bounds(x, y, width, rowHeight - 8.0f));
			ui->FillRect(UI::Drawable(psp5::kPrimary), Bounds(x, y, 4.0f, rowHeight - 8.0f));
		}

		const float textLeft = left + indent;
		float textRight = x + width - pad;
		if (row.toggle) {
			psp5::DrawToggle(ui, textRight - 56.0f, y + (rowHeight - 8.0f - 26.0f) * 0.5f, row.on);
			textRight -= 70.0f;
		} else if (row.submenu) {
			ui->SetFontScale(0.7f, 0.7f);
			ui->DrawTextRect(">", Bounds(textRight - 18.0f, y, 18.0f, rowHeight - 8.0f),
			                 focused ? psp5::kInk : psp5::kInkFaint, ALIGN_LEFT | ALIGN_VCENTER);
			textRight -= 26.0f;
		}

		if (!row.value.empty()) {
			ui->SetFontScale(0.6f, 0.6f);
			const float w = 96.0f;
			ui->DrawTextRect(row.value, Bounds(textRight - w, y, w, rowHeight - 8.0f),
			                 psp5::kInkDim, ALIGN_RIGHT | ALIGN_VCENTER);
			textRight -= w + 12.0f;
		}

		ui->SetFontScale(0.7f, 0.7f);
		ui->DrawTextRect(row.label, Bounds(textLeft, y, textRight - textLeft, rowHeight - 8.0f),
		                 row.selectable ? psp5::kInk : psp5::kInkFaint,
		                 ALIGN_LEFT | ALIGN_VCENTER | FLAG_ELLIPSIZE_TEXT);
		y += rowHeight;
	}

	ui->SetFontScale(1.0f, 1.0f);
	ui->Flush();
}
