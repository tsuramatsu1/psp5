// psp5 - the achievements bar that opens over a running game.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// rcheevos builds the list, grouped the way its own clients group it: what is
// nearly done first, then what is locked, then what is already unlocked. psp5
// flattens that into rows with the group labels left in, because a bar down the
// side of a screen has no room for columns.
//
// The list is rebuilt while the bar is open, not only when it opens: an
// achievement can unlock while the player is reading about it.

#include "PS5Achievements.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "Common/Render/DrawBuffer.h"
#include "Common/UI/Context.h"
#include "Common/UI/View.h"
#include "Core/Config.h"
#include "Core/RetroAchievements.h"
#include "ext/rcheevos/include/rc_client.h"

#include "PS5Log.h"
#include "PS5OverlayDraw.h"
#include "PS5Overlay.h"

namespace psp5 {

namespace {

bool g_open = false;
int g_row = 0;
float g_since = 0.0f;

struct Row {
	std::string label;
	std::string detail;
	std::string value;
	bool header = false;
	bool unlocked = false;
	float progress = 0.0f;  // 0..1, and only where the achievement measures one
};

std::vector<Row> g_rows;
std::string g_summary;
std::string g_game;
uint32_t g_earned = 0;
uint32_t g_total = 0;
uint32_t g_points = 0;
uint32_t g_pointsTotal = 0;

rc_client_t *client_for_summary() {
	return Achievements::GetClient();
}

// Unofficial achievements are off by default and are a setting of PPSSPP's, so
// the bar shows whatever the player has asked PPSSPP for.
uint32_t ListFilter() {
	return Achievements::UnofficialEnabled() ? RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL
	                                         : RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE;
}

void Rebuild() {
	g_rows.clear();
	g_summary.clear();
	g_game.clear();
	g_earned = g_total = g_points = g_pointsTotal = 0;

	// Four different situations used to share one message, which said the game
	// was unsupported even when the truth was that nobody had signed in yet or
	// the server had not answered. Each says what it is, and the log records
	// the state behind it.
	if (!Achievements::IsLoggedIn()) {
		g_rows.push_back({"Not signed in", "Sign in from the home screen's settings, then start "
		                                   "the game again.",
		                  "", false, false, 0.0f});
		return;
	}
	if (Achievements::IsBlockingExecution()) {
		g_rows.push_back({"Identifying this game", "Asking the server what it knows about it.", "",
		                  false, false, 0.0f});
		return;
	}
	if (!Achievements::IsActive()) {
		// A game is identified when it boots, so signing in afterwards leaves
		// this one unidentified until it is started again.
		g_rows.push_back({"No achievements for this game",
		                  "Either RetroAchievements has none for it, or it was started before "
		                  "you signed in - start it again.",
		                  "", false, false, 0.0f});
		return;
	}

	g_summary = Achievements::GetGameAchievementSummary(0);

	// The counts behind the header's bar. rcheevos keeps them, so they do not
	// have to be totted up from the list.
	rc_client_user_game_summary_t summary {};
	rc_client_get_user_game_summary(client_for_summary(), &summary);
	g_earned = summary.num_unlocked_achievements;
	g_total = summary.num_core_achievements;
	g_points = summary.points_unlocked;
	g_pointsTotal = summary.points_core;
	const rc_client_game_t *game = rc_client_get_game_info(client_for_summary());
	g_game = game && game->title ? game->title : "";

	rc_client_t *client = Achievements::GetClient();
	rc_client_achievement_list_t *list = rc_client_create_achievement_list(
	    client, ListFilter(), RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
	if (!list) {
		return;
	}

	char value[48];
	for (uint32_t b = 0; b < list->num_buckets; ++b) {
		const rc_client_achievement_bucket_t &bucket = list->buckets[b];
		if (!bucket.num_achievements) {
			continue;
		}
		std::snprintf(value, sizeof(value), "%u", bucket.num_achievements);
		g_rows.push_back({bucket.label ? bucket.label : "", "", value, true, false, 0.0f});

		for (uint32_t a = 0; a < bucket.num_achievements; ++a) {
			const rc_client_achievement_t *achievement = bucket.achievements[a];
			if (!achievement) {
				continue;
			}
			std::snprintf(value, sizeof(value), "%u", achievement->points);
			Row row;
			row.label = achievement->title ? achievement->title : "";
			row.detail = achievement->description ? achievement->description : "";
			row.value = value;
			row.unlocked = achievement->unlocked != 0;
			// measured_progress is empty unless the achievement counts towards
			// something, which is the only case worth a bar.
			if (achievement->measured_progress[0] != '\0') {
				row.detail = achievement->measured_progress;
				row.progress = std::clamp(achievement->measured_percent / 100.0f, 0.0f, 1.0f);
			}
			g_rows.push_back(row);
		}
	}
	rc_client_destroy_achievement_list(list);

	if (g_rows.empty()) {
		g_rows.push_back({"Nothing to earn here", "", "", false, false, 0.0f});
	}
}

}  // namespace

bool AchievementsBarOpen() {
	return g_open;
}

void CloseAchievementsBar() {
	if (!g_open) {
		return;
	}
	g_open = false;
	g_rows.clear();
	psp5::Trace("achievements: bar closed");
}

void ToggleAchievementsBar() {
	if (g_open) {
		CloseAchievementsBar();
		return;
	}
	CloseCheatOverlay();
	g_row = 0;
	g_since = 0.0f;
	Rebuild();
	g_open = true;
	psp5::Trace("achievements: bar open, %u row(s) - signed in %d, busy %d, game active %d",
	            (unsigned)g_rows.size(), (int)Achievements::IsLoggedIn(),
	            (int)Achievements::IsBlockingExecution(), (int)Achievements::IsActive());
}

void AchievementsBarMove(int delta) {
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
		if (!g_rows[(std::size_t)at].header) {
			g_row = at;
			return;
		}
	}
}

void AchievementsBarUpdate(float dt) {
	if (!g_open) {
		return;
	}
	// Once a second is often enough for a list that changes when the player
	// earns something, and it keeps the rebuild off most frames.
	g_since += dt;
	if (g_since < 1.0f) {
		return;
	}
	g_since = 0.0f;
	const int was = g_row;
	Rebuild();
	g_row = std::clamp(was, 0, std::max(0, (int)g_rows.size() - 1));
}

}  // namespace psp5

extern "C" void PS5_DrawAchievementsBar(UIContext *ui) {
	if (!ui || !psp5::AchievementsBarOpen()) {
		return;
	}

	// Laid out the way the console lists trophies: a head that says how far
	// along the game is, then a row per achievement with its mark on the left,
	// its name and what it asks for, and its points on the right. The marks are
	// drawn rather than downloaded - rcheevos gives a badge URL, and fetching
	// one image per achievement to show a list is a lot of network for a panel.
	const Bounds screen = ui->GetBounds();
	const float width = std::min(520.0f, screen.w * 0.46f);
	const float x = screen.w - width;
	const float pad = 30.0f;
	const float inner = width - pad * 2.0f;

	ui->FillRect(UI::Drawable(psp5::kShade), Bounds(0.0f, 0.0f, x, screen.h));
	ui->FillRect(UI::Drawable(psp5::kPage), Bounds(x, 0.0f, width, screen.h));
	ui->FillRect(UI::Drawable(psp5::kOutline), Bounds(x, 0.0f, 1.0f, screen.h));

	ui->SetFontStyle(ui->GetTheme().uiFont);

	// ---- the head ----
	float y = pad;
	ui->SetFontScale(0.58f, 0.58f);
	ui->DrawText("ACHIEVEMENTS", x + pad, y, psp5::kInkDim, ALIGN_LEFT | ALIGN_TOP);
	y += 26.0f;
	if (!psp5::g_game.empty()) {
		ui->SetFontScale(0.95f, 0.95f);
		ui->DrawTextRect(psp5::g_game, Bounds(x + pad, y, inner, 34.0f), psp5::kInk,
		                 ALIGN_LEFT | ALIGN_TOP);
		y += 38.0f;
	}

	if (psp5::g_total > 0) {
		char text[96];
		std::snprintf(text, sizeof(text), "%u of %u", psp5::g_earned, psp5::g_total);
		ui->SetFontScale(0.72f, 0.72f);
		ui->DrawText(text, x + pad, y + 2.0f, psp5::kInk, ALIGN_LEFT | ALIGN_TOP);
		std::snprintf(text, sizeof(text), "%u / %u points", psp5::g_points, psp5::g_pointsTotal);
		ui->SetFontScale(0.6f, 0.6f);
		ui->DrawTextRect(text, Bounds(x + width - pad - 200.0f, y + 4.0f, 200.0f, 22.0f),
		                 psp5::kInkDim, ALIGN_RIGHT | ALIGN_TOP);
		y += 30.0f;
		psp5::DrawMeter(ui, Bounds(x + pad, y, inner, 6.0f),
		                (float)psp5::g_earned / (float)psp5::g_total, psp5::kPrimary);
		y += 20.0f;
	}
	ui->FillRect(UI::Drawable(psp5::kOutline), Bounds(x + pad, y, inner, 1.0f));
	y += 14.0f;

	// ---- the list ----
	const float top = y;
	const float rowHeight = 86.0f;
	const float headerHeight = 34.0f;
	const int rows = (int)psp5::g_rows.size();
	const int visible = std::max(1, (int)((screen.h - top - pad) / rowHeight));
	int first = std::clamp(psp5::g_row - visible / 2, 0, std::max(0, rows - visible));

	for (int i = first; i < rows && y < screen.h - pad; ++i) {
		const psp5::Row &row = psp5::g_rows[(std::size_t)i];
		const bool focused = i == psp5::g_row;

		if (row.header) {
			ui->SetFontScale(0.56f, 0.56f);
			ui->DrawTextRect(row.label, Bounds(x + pad, y, inner - 60.0f, headerHeight),
			                 psp5::kInkDim, ALIGN_LEFT | ALIGN_VCENTER);
			if (!row.value.empty()) {
				ui->DrawTextRect(row.value, Bounds(x + width - pad - 56.0f, y, 56.0f,
				                                   headerHeight),
				                 psp5::kInkFaint, ALIGN_RIGHT | ALIGN_VCENTER);
			}
			y += headerHeight;
			continue;
		}

		if (focused) {
			ui->FillRect(UI::Drawable(0x18FFFFFF), Bounds(x + 1.0f, y, width - 1.0f, rowHeight));
			ui->FillRect(UI::Drawable(psp5::kAccent), Bounds(x, y, 3.0f, rowHeight));
		}

		// The mark: filled when earned, an empty ring when not, as a trophy
		// list shows one earned and one still to get.
		const float mark = 54.0f;
		const Bounds badge(x + pad, y + (rowHeight - mark) * 0.5f, mark, mark);
		if (row.unlocked) {
			psp5::FillRound(ui, badge, 12.0f, psp5::kPrimary);
			// A tick, as two strokes.
			ui->FillRect(UI::Drawable(psp5::kPage),
			             Bounds(badge.x + 16.0f, badge.y + 27.0f, 10.0f, 4.0f));
			ui->FillRect(UI::Drawable(psp5::kPage),
			             Bounds(badge.x + 24.0f, badge.y + 19.0f, 4.0f, 14.0f));
		} else {
			psp5::FillRoundOutlined(ui, badge, 12.0f, psp5::kPage, psp5::kOutline, 2.0f);
		}

		const float textX = badge.x + mark + 16.0f;
		float textW = width - (textX - x) - pad - 54.0f;

		ui->SetFontScale(0.74f, 0.74f);
		ui->DrawTextRect(row.label, Bounds(textX, y + 16.0f, textW, 24.0f),
		                 row.unlocked ? psp5::kInk : psp5::kInkDim, ALIGN_LEFT | ALIGN_TOP);
		if (!row.value.empty()) {
			ui->SetFontScale(0.62f, 0.62f);
			ui->DrawTextRect(row.value, Bounds(x + width - pad - 50.0f, y + 18.0f, 50.0f, 22.0f),
			                 psp5::kInkFaint, ALIGN_RIGHT | ALIGN_TOP);
		}
		if (!row.detail.empty()) {
			ui->SetFontScale(0.6f, 0.6f);
			ui->DrawTextRect(row.detail, Bounds(textX, y + 42.0f, textW + 40.0f, 20.0f),
			                 psp5::kInkFaint, ALIGN_LEFT | ALIGN_TOP);
		}
		if (row.progress > 0.0f && !row.unlocked) {
			psp5::DrawMeter(ui, Bounds(textX, y + rowHeight - 20.0f, textW, 4.0f), row.progress,
			                psp5::kAccent);
		}

		ui->FillRect(UI::Drawable(psp5::kOutline),
		             Bounds(x + pad, y + rowHeight - 1.0f, inner, 1.0f));
		y += rowHeight;
	}

	ui->SetFontScale(1.0f, 1.0f);
	ui->Flush();
}
