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
#include "Common/UI/IconCache.h"
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
	std::string badge;   // the image RetroAchievements shows for it
	std::string measure; // "3/10", where the achievement counts towards something
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
		Row head;
			head.label = bucket.label ? bucket.label : "";
			head.value = value;
			head.header = true;
			g_rows.push_back(head);

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
			// The badge RetroAchievements shows: the plain one once it is earned,
			// the greyed one until then, which is how its own site reads.
			const char *badge =
				row.unlocked ? achievement->badge_url : achievement->badge_locked_url;
			if (badge && badge[0]) {
				row.badge = badge;
				Achievements::DownloadImageIfMissing(row.badge);
			}
			// measured_progress is empty unless the achievement counts towards
			// something, which is the only case worth a bar. It goes beside the
			// description rather than over it - what the achievement asks for is
			// the part worth reading.
			if (achievement->measured_progress[0] != '\0') {
				row.measure = achievement->measured_progress;
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

	// Laid out the way the console lists trophies: a head that says how far along
	// the game is, then a row per achievement with its badge on the left, its name
	// and what it asks for, and its points on the right.
	//
	// The badges are the real ones. PPSSPP already downloads and caches them for
	// its own achievement screens, so this asks for the same image by the same URL
	// and gets whatever has arrived; a row whose badge has not landed yet draws the
	// plain mark instead of an empty box, and picks the image up on a later frame.
	const Bounds screen = ui->GetBounds();
	const float width = std::min(560.0f, screen.w * 0.46f);
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
	//
	// A description is a sentence, and a sentence does not fit on one line beside
	// a badge. Every row wraps its description, so a row is as tall as its own
	// text needs - the focused one included, which is the one being read.
	const float top = y;
	const float badgeSize = 56.0f;
	const float headerHeight = 34.0f;
	const float textX = x + pad + badgeSize + 16.0f;
	const float textW = width - (textX - x) - pad - 54.0f;
	const int rows = (int)psp5::g_rows.size();

	const auto row_height = [&](const psp5::Row &row) {
		if (row.header) {
			return headerHeight;
		}
		float w = 0.0f;
		float h = 0.0f;
		ui->MeasureTextRect(ui->GetTheme().uiFont, 0.6f, 0.6f, row.detail, textW + 40.0f, &w, &h,
		                    ALIGN_LEFT | FLAG_WRAP_TEXT);
		float height = 16.0f + 26.0f + h + 14.0f;
		if (!row.measure.empty()) {
			height += 22.0f;
		}
		return std::max(badgeSize + 24.0f, height);
	};

	// Enough of the list is skipped to keep the focused row on screen. Heights
	// vary, so the first visible row is found by walking back from the cursor
	// until the rows below it fill the bar.
	const float space = screen.h - top - pad;
	int first = psp5::g_row;
	float used = rows > 0 ? row_height(psp5::g_rows[(std::size_t)psp5::g_row]) : 0.0f;
	while (first > 0) {
		const float next = row_height(psp5::g_rows[(std::size_t)(first - 1)]);
		if (used + next > space) {
			break;
		}
		used += next;
		--first;
	}

	for (int i = first; i < rows && y < screen.h - pad; ++i) {
		const psp5::Row &row = psp5::g_rows[(std::size_t)i];
		const bool focused = i == psp5::g_row;
		const float height = row_height(row);

		if (row.header) {
			ui->SetFontScale(0.56f, 0.56f);
			ui->DrawTextRect(row.label, Bounds(x + pad, y, inner - 60.0f, headerHeight),
			                 psp5::kInkDim, ALIGN_LEFT | ALIGN_VCENTER);
			if (!row.value.empty()) {
				ui->DrawTextRect(row.value,
				                 Bounds(x + width - pad - 56.0f, y, 56.0f, headerHeight),
				                 psp5::kInkFaint, ALIGN_RIGHT | ALIGN_VCENTER);
			}
			y += headerHeight;
			continue;
		}

		if (focused) {
			ui->FillRect(UI::Drawable(0x18FFFFFF), Bounds(x + 1.0f, y, width - 1.0f, height));
			ui->FillRect(UI::Drawable(psp5::kAccent), Bounds(x, y, 3.0f, height));
		}

		const Bounds badge(x + pad, y + 14.0f, badgeSize, badgeSize);
		bool drew = false;
		if (!row.badge.empty() && g_iconCache.BindIconTexture(ui, row.badge)) {
			ui->Draw()->DrawTexRect(badge, 0.0f, 0.0f, 1.0f, 1.0f,
			                        row.unlocked ? 0xFFFFFFFF : 0xB0FFFFFF);
			ui->Flush();
			ui->RebindTexture();
			drew = true;
		}
		if (!drew) {
			// Until the image arrives: filled when earned, an empty ring when not.
			if (row.unlocked) {
				psp5::FillRound(ui, badge, 12.0f, psp5::kPrimary);
				ui->FillRect(UI::Drawable(psp5::kPage),
				             Bounds(badge.x + 16.0f, badge.y + 29.0f, 10.0f, 4.0f));
				ui->FillRect(UI::Drawable(psp5::kPage),
				             Bounds(badge.x + 24.0f, badge.y + 21.0f, 4.0f, 14.0f));
			} else {
				psp5::FillRoundOutlined(ui, badge, 12.0f, psp5::kPage, psp5::kOutline, 2.0f);
			}
		}

		float ty = y + 14.0f;
		ui->SetFontStyle(ui->GetTheme().uiFont);
		ui->SetFontScale(0.74f, 0.74f);
		ui->DrawTextRect(row.label, Bounds(textX, ty, textW, 24.0f),
		                 row.unlocked ? psp5::kInk : psp5::kInkDim, ALIGN_LEFT | ALIGN_TOP);
		if (!row.value.empty()) {
			ui->SetFontScale(0.62f, 0.62f);
			ui->DrawTextRect(row.value, Bounds(x + width - pad - 50.0f, ty + 2.0f, 50.0f, 22.0f),
			                 psp5::kInkFaint, ALIGN_RIGHT | ALIGN_TOP);
		}
		ty += 26.0f;
		if (!row.detail.empty()) {
			float w = 0.0f;
			float h = 0.0f;
			ui->SetFontScale(0.6f, 0.6f);
			ui->MeasureTextRect(ui->GetTheme().uiFont, 0.6f, 0.6f, row.detail, textW + 40.0f, &w,
			                    &h, ALIGN_LEFT | FLAG_WRAP_TEXT);
			ui->DrawTextRect(row.detail, Bounds(textX, ty, textW + 40.0f, h), psp5::kInkFaint,
			                 ALIGN_LEFT | ALIGN_TOP | FLAG_WRAP_TEXT);
			ty += h + 4.0f;
		}
		if (!row.measure.empty()) {
			ui->SetFontScale(0.58f, 0.58f);
			ui->DrawTextRect(row.measure, Bounds(textX, ty, textW + 40.0f, 18.0f), psp5::kInkDim,
			                 ALIGN_LEFT | ALIGN_TOP);
			ty += 18.0f;
		}
		if (row.progress > 0.0f && !row.unlocked) {
			psp5::DrawMeter(ui, Bounds(textX, y + height - 14.0f, textW, 4.0f), row.progress,
			                psp5::kAccent);
		}

		ui->FillRect(UI::Drawable(psp5::kOutline),
		             Bounds(x + pad, y + height - 1.0f, inner, 1.0f));
		y += height;
	}

	ui->SetFontScale(1.0f, 1.0f);
	ui->Flush();
}
