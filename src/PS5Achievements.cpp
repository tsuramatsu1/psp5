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
#include "ui/PS5GameAchievements.h"
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

// The bar says why it is empty rather than showing nothing. These rows carry
// a title and a sentence and nothing else. A helper, because a positional
// brace list has to be revisited every time Row gains a member.
Row Notice(std::string label, std::string detail) {
	Row row;
	row.label = std::move(label);
	row.detail = std::move(detail);
	return row;
}
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
		g_rows.push_back(
			Notice("Not signed in",
			       "Sign in from the home screen's settings, then start the "
			       "game again."));
		return;
	}
	if (Achievements::IsBlockingExecution()) {
		g_rows.push_back(
			Notice("Identifying this game",
			       "Asking the server what it knows about it."));
		return;
	}
	if (!Achievements::IsActive()) {
		// A game is identified when it boots, so signing in afterwards leaves
		// this one unidentified until it is started again.
		g_rows.push_back(
			Notice("No achievements for this game",
			       "Either RetroAchievements has none for it, or it was "
			       "started before you signed in - start it again."));
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
		// What this group has left after the filter. A label with nothing under
		// it is not worth a row.
		uint32_t passing = 0;
		for (uint32_t a = 0; a < bucket.num_achievements; ++a) {
			const rc_client_achievement_t *achievement = bucket.achievements[a];
			if (achievement && psp5::achievementPasses(achievement->unlocked != 0,
			                                           psp5::achievementFilter())) {
				++passing;
			}
		}
		if (!passing) {
			continue;
		}
		std::snprintf(value, sizeof(value), "%u", passing);
		Row head;
			head.label = bucket.label ? bucket.label : "";
			head.value = value;
			head.header = true;
			g_rows.push_back(head);

		for (uint32_t a = 0; a < bucket.num_achievements; ++a) {
			const rc_client_achievement_t *achievement = bucket.achievements[a];
			if (!achievement || !psp5::achievementPasses(achievement->unlocked != 0,
			                                            psp5::achievementFilter())) {
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
		g_rows.push_back(
			Notice("Nothing to earn here",
			       ""));
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

void AchievementsBarFilter(int delta) {
	if (!g_open) {
		return;
	}
	psp5::stepAchievementFilter(delta);
	Rebuild();
	g_row = 0;
	// Past any group label, onto something that can be selected.
	for (int i = 0; i < (int)g_rows.size(); ++i) {
		if (!g_rows[(std::size_t)i].header) {
			g_row = i;
			break;
		}
	}
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

	// The same bar the home screen draws for a game before it is played: the
	// right-hand edge, the rest of the screen dimmed, a head that says how far
	// along the game is and a row per achievement with its badge. Written twice
	// because the two are drawn by different renderers - this one by PPSSPP's,
	// over a running game - but they are meant to be the same panel.
	const Bounds screen = ui->GetBounds();
	const float width = std::min(620.0f, screen.w * 0.46f);
	const float x = screen.w - width;
	const float pad = 44.0f;
	const float left = x + pad;
	const float inner = width - pad * 2.0f;
	const float scale = screen.h / 1080.0f;

	ui->FillRect(UI::Drawable(psp5::kShade), Bounds(0.0f, 0.0f, x, screen.h));
	ui->FillRect(UI::Drawable(psp5::kPage), Bounds(x, 0.0f, width, screen.h));
	ui->FillRect(UI::Drawable(0x2EFFFFFF), Bounds(x, 0.0f, 1.5f, screen.h));

	ui->SetFontStyle(ui->GetTheme().uiFont);

	// ---- the head ----
	ui->SetFontScale(0.52f, 0.52f);
	ui->DrawText("ACHIEVEMENTS", left, 78.0f * scale, psp5::kPrimary, ALIGN_LEFT | ALIGN_TOP);
	ui->DrawTextRect(psp5::achievementFilterName(psp5::achievementFilter()),
	                 Bounds(x + width - pad - 200.0f, 78.0f * scale, 200.0f, 24.0f), psp5::kInkDim,
	                 ALIGN_RIGHT | ALIGN_TOP);
	if (!psp5::g_game.empty()) {
		ui->SetFontScale(0.95f, 0.95f);
		ui->DrawTextRect(psp5::g_game, Bounds(left, 118.0f * scale, inner, 44.0f), psp5::kInk,
		                 ALIGN_LEFT | ALIGN_TOP);
	}

	float y = 188.0f * scale;
	if (psp5::g_total > 0) {
		char text[96];
		std::snprintf(text, sizeof(text), "%u of %u", psp5::g_earned, psp5::g_total);
		ui->SetFontScale(0.8f, 0.8f);
		ui->DrawText(text, left, y, psp5::kInk, ALIGN_LEFT | ALIGN_TOP);
		std::snprintf(text, sizeof(text), "%u / %u points", psp5::g_points, psp5::g_pointsTotal);
		ui->SetFontScale(0.58f, 0.58f);
		ui->DrawTextRect(text, Bounds(x + width - pad - 220.0f, y + 6.0f, 220.0f, 24.0f),
		                 psp5::kInkDim, ALIGN_RIGHT | ALIGN_TOP);
		y += 34.0f;
		psp5::FillRound(ui, Bounds(left, y, inner, 6.0f), 3.0f, 0x24FFFFFF);
		psp5::FillRound(ui,
		                Bounds(left, y, inner * (float)psp5::g_earned / (float)psp5::g_total, 6.0f),
		                3.0f, psp5::kPrimary);
		y += 24.0f;
	}

	// ---- the list ----
	const float top = y;
	const float badgeSize = 56.0f;
	const float textX = left + 72.0f;
	const float textW = inner - 72.0f - 54.0f;
	const int rows = (int)psp5::g_rows.size();

	const auto row_height = [&](int index) {
		const psp5::Row &row = psp5::g_rows[(std::size_t)index];
		if (row.header) {
			return 44.0f;
		}
		if (index != psp5::g_row || row.detail.empty()) {
			return 72.0f;
		}
		float w = 0.0f;
		float h = 0.0f;
		ui->MeasureTextRect(ui->GetTheme().uiFont, 0.55f, 0.55f, row.detail, textW + 40.0f, &w, &h,
		                    ALIGN_LEFT | FLAG_WRAP_TEXT);
		return 58.0f + h + 14.0f;
	};

	// Enough of the list is skipped to keep the focused row on screen.
	const float space = screen.h - top - 70.0f;
	int first = psp5::g_row;
	float used = rows > 0 ? row_height(psp5::g_row) : 0.0f;
	while (first > 0) {
		const float next = row_height(first - 1);
		if (used + next > space) {
			break;
		}
		used += next;
		--first;
	}

	for (int i = first; i < rows && y < screen.h - 70.0f; ++i) {
		const psp5::Row &row = psp5::g_rows[(std::size_t)i];
		const bool focused = i == psp5::g_row;
		const float height = row_height(i);

		if (row.header) {
			ui->SetFontScale(0.5f, 0.5f);
			ui->DrawTextRect(row.label, Bounds(left, y, inner - 60.0f, height), psp5::kInkFaint,
			                 ALIGN_LEFT | ALIGN_VCENTER);
			y += height;
			continue;
		}

		if (focused) {
			// Square, and the full width of the bar: the row itself is lit,
			// rather than a pill being laid over it.
			ui->FillRect(UI::Drawable(0x1AFFFFFF), Bounds(x, y, width, height - 8.0f));
			ui->FillRect(UI::Drawable(psp5::kPrimary), Bounds(x, y, 4.0f, height - 8.0f));
		}

		// The badge RetroAchievements shows, from PPSSPP's own icon cache.
		const Bounds badge(left, y + 12.0f, badgeSize, badgeSize);
		bool drew = false;
		if (!row.badge.empty() && g_iconCache.BindIconTexture(ui, row.badge)) {
			ui->Draw()->DrawTexRect(badge, 0.0f, 0.0f, 1.0f, 1.0f,
			                        row.unlocked ? 0xFFFFFFFF : 0x8CFFFFFF);
			ui->Flush();
			ui->RebindTexture();
			drew = true;
		}
		if (!drew) {
			if (row.unlocked) {
				psp5::FillRound(ui, badge, 10.0f, psp5::kPrimary);
			} else {
				psp5::FillRoundOutlined(ui, badge, 10.0f, 0x0AFFFFFF, 0x42FFFFFF, 2.0f);
			}
		}

		ui->SetFontStyle(ui->GetTheme().uiFont);
		ui->SetFontScale(0.68f, 0.68f);
		ui->DrawTextRect(row.label, Bounds(textX, y + 14.0f, textW, 26.0f),
		                 row.unlocked ? psp5::kInk : psp5::kInkDim,
		                 ALIGN_LEFT | ALIGN_TOP | FLAG_ELLIPSIZE_TEXT);
		if (!row.value.empty()) {
			ui->SetFontScale(0.58f, 0.58f);
			ui->DrawTextRect(row.value, Bounds(x + width - pad - 50.0f, y + 16.0f, 50.0f, 24.0f),
			                 psp5::kInkFaint, ALIGN_RIGHT | ALIGN_TOP);
		}
		float ty = y + 44.0f;
		if (focused && !row.detail.empty()) {
			float w = 0.0f;
			float h = 0.0f;
			ui->SetFontScale(0.55f, 0.55f);
			ui->MeasureTextRect(ui->GetTheme().uiFont, 0.55f, 0.55f, row.detail, textW + 40.0f, &w,
			                    &h, ALIGN_LEFT | FLAG_WRAP_TEXT);
			ui->DrawTextRect(row.detail, Bounds(textX, ty, textW + 40.0f, h), psp5::kInkFaint,
			                 ALIGN_LEFT | ALIGN_TOP | FLAG_WRAP_TEXT);
			ty += h + 4.0f;
		}
		if (focused && !row.measure.empty()) {
			ui->SetFontScale(0.54f, 0.54f);
			ui->DrawTextRect(row.measure, Bounds(textX, ty, textW + 40.0f, 20.0f), psp5::kInkDim,
			                 ALIGN_LEFT | ALIGN_TOP);
		}
		if (row.progress > 0.0f && !row.unlocked) {
			psp5::FillRound(ui, Bounds(textX, y + height - 18.0f, textW, 3.0f), 2.0f, 0x24FFFFFF);
			psp5::FillRound(ui, Bounds(textX, y + height - 18.0f, textW * row.progress, 3.0f), 2.0f,
			                psp5::kPrimary);
		}
		y += height;
	}

	ui->SetFontScale(1.0f, 1.0f);
	ui->Flush();
}
