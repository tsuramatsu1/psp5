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
#include <vector>

#include "Common/Render/DrawBuffer.h"
#include "Common/System/System.h"
#include "Common/UI/Context.h"
#include "Common/UI/View.h"
#include "Core/Config.h"
#include "Core/ELF/ParamSFO.h"
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
			g_rows.push_back({Kind::save, "Save to this slot", "", 0, false, false, false, true});
			const bool has = SaveState::HasSaveInSlot(GamePrefix(), g_slot);
			g_rows.push_back({Kind::load, "Load from this slot", has ? "" : "empty", 0, false,
			                  false, false, has});
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
		case Kind::slot:
			g_slot = (g_slot + 1) % 8;
			break;
		case Kind::save: {
			const std::string prefix = GamePrefix();
			const int slot = g_slot;
			psp5::Trace("save state: saving %s slot %d", prefix.c_str(), slot + 1);
			// The work happens later, inside the emulation loop, so the outcome
			// arrives here rather than at the call. Swallowing it is what made
			// the missing PPSSPP_STATE directory invisible.
			SaveState::SaveSlot(prefix, slot, [slot](SaveState::Status status,
			                                        std::string_view message) {
				psp5::Trace("save state: slot %d %s%s%.*s", slot + 1,
				            status == SaveState::Status::FAILURE ? "failed" : "saved",
				            message.empty() ? "" : ": ", (int)message.size(), message.data());
			});
			CloseCheatOverlay();
			return;
		}
		case Kind::load: {
			const std::string prefix = GamePrefix();
			const int slot = g_slot;
			if (!SaveState::HasSaveInSlot(prefix, slot)) {
				return;
			}
			psp5::Trace("save state: loading %s slot %d", prefix.c_str(), slot + 1);
			SaveState::LoadSlot(prefix, slot, [slot](SaveState::Status status,
			                                        std::string_view message) {
				psp5::Trace("save state: slot %d %s%s%.*s", slot + 1,
				            status == SaveState::Status::FAILURE ? "failed" : "loaded",
				            message.empty() ? "" : ": ", (int)message.size(), message.data());
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
	return psp5::CheatOverlayOpen() || psp5::AchievementsBarOpen();
}

extern "C" void PS5_DrawOverlays(UIContext *ui) {
	if (!ui) {
		return;
	}
	PS5_DrawAchievementsBar(ui);
	if (!psp5::CheatOverlayOpen()) {
		return;
	}

	// A bar down the left edge, the full height of the screen. Not a floating
	// box: this is where a game's own controls live while it runs, and a panel
	// anchored to an edge reads as part of the screen rather than as something
	// dropped on top of it.
	const Bounds screen = ui->GetBounds();
	const float width = std::min(430.0f, screen.w * 0.42f);
	const float pad = 28.0f;
	const float inner = width - pad * 2.0f;
	const float rowHeight = 48.0f;
	const float rowGap = 6.0f;

	ui->FillRect(UI::Drawable(psp5::kShade), Bounds(width, 0.0f, screen.w - width, screen.h));
	ui->FillRect(UI::Drawable(psp5::kPage), Bounds(0.0f, 0.0f, width, screen.h));
	// A hairline, not a bar of colour: this theme separates with a rule.
	ui->FillRect(UI::Drawable(psp5::kOutline), Bounds(width - 1.0f, 0.0f, 1.0f, screen.h));

	// ---- the head ----
	ui->SetFontStyle(ui->GetTheme().uiFont);
	ui->SetFontScale(1.0f, 1.0f);
	ui->DrawText(psp5::PageTitle(), pad, pad + 2.0f, psp5::kInk, ALIGN_LEFT | ALIGN_TOP);
	ui->SetFontScale(0.6f, 0.6f);
	ui->DrawText(psp5::g_page == psp5::Page::root ? "L1 + L3  CLOSE" : "CIRCLE  BACK", pad,
	             pad + 36.0f, psp5::kInkFaint, ALIGN_LEFT | ALIGN_TOP);
	ui->FillRect(UI::Drawable(psp5::kOutline), Bounds(pad, pad + 60.0f, inner, 1.0f));

	const float top = pad + 80.0f;
	const int rows = (int)psp5::g_rows.size();
	const int visible = std::max(1, (int)((screen.h - top - pad) / (rowHeight + rowGap)));
	int first = std::clamp(psp5::g_row - visible / 2, 0, std::max(0, rows - visible));
	const int last = std::min(rows, first + visible);

	float y = top;
	for (int i = first; i < last; ++i) {
		const auto &row = psp5::g_rows[(std::size_t)i];
		const bool focused = i == psp5::g_row;
		const Bounds card(pad, y, inner, rowHeight);

		if (row.kind == psp5::Kind::note) {
			// Not a row to land on: said quietly, with no card behind it.
			ui->SetFontScale(0.64f, 0.64f);
			ui->DrawTextRect(row.label, Bounds(pad + 2.0f, y, inner - 4.0f, rowHeight),
			                 psp5::kInkFaint, ALIGN_LEFT | ALIGN_VCENTER);
			y += rowHeight + rowGap;
			continue;
		}

		// Every row is a card with a hairline, so the list reads as a set of
		// things rather than as text with a highlight wandering over it. Focus
		// is the blue edge, as this theme does it.
		psp5::FillRoundOutlined(ui, card, psp5::kRadius,
		                        focused ? psp5::kSurfaceHigh : psp5::kSurface,
		                        focused ? psp5::kAccent : psp5::kOutline, focused ? 2.0f : 1.0f);

		const float textLeft = card.x + 18.0f;
		float textRight = card.x + card.w - 16.0f;
		if (row.toggle) {
			psp5::DrawToggle(ui, card.x + card.w - 16.0f - 50.0f,
			                 card.y + (rowHeight - 26.0f) * 0.5f, row.on);
			textRight -= 62.0f;
		} else if (row.submenu) {
			ui->SetFontScale(0.74f, 0.74f);
			ui->DrawTextRect(">", Bounds(card.x + card.w - 30.0f, card.y, 18.0f, rowHeight),
			                 focused ? psp5::kInk : psp5::kInkFaint, ALIGN_LEFT | ALIGN_VCENTER);
			textRight -= 24.0f;
		}

		if (!row.value.empty()) {
			ui->SetFontScale(0.64f, 0.64f);
			const float w = 86.0f;
			ui->DrawTextRect(row.value, Bounds(textRight - w, card.y, w, rowHeight),
			                 psp5::kInkDim, ALIGN_RIGHT | ALIGN_VCENTER);
			textRight -= w + 10.0f;
		}

		ui->SetFontScale(0.76f, 0.76f);
		ui->DrawTextRect(row.label, Bounds(textLeft, card.y, textRight - textLeft, rowHeight),
		                 row.selectable ? psp5::kInk : psp5::kInkFaint,
		                 ALIGN_LEFT | ALIGN_VCENTER);
		y += rowHeight + rowGap;
	}

	ui->SetFontScale(1.0f, 1.0f);
	ui->Flush();
}
