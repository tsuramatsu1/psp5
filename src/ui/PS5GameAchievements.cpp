// psp5 - a game's achievements, read on the home screen before it is played.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PPSSPP identifies a game for RetroAchievements as it boots it: it hashes the
// disc image, asks the server which game that is, and rcheevos holds the list
// from then on. None of that has happened on the home screen, so this does it -
// the same call PPSSPP makes, Achievements::SetGame, driven from here.
//
// The asking is not instant: hashing a UMD image reads it through, and the
// server is a network round trip away. Both happen while the shelf keeps
// drawing, which is why this is a state machine the screen polls rather than a
// function that returns a list.

#include "ui/PS5GameAchievements.h"

#include <algorithm>
#include <cstdio>

#include "Common/File/Path.h"
#include "Common/Net/HTTPRequest.h"
#include "Core/Config.h"  // g_DownloadManager
#include "Core/Loaders.h"
#include "Core/RetroAchievements.h"
#include "ext/rcheevos/include/rc_client.h"

#include "PS5Log.h"

namespace psp5 {

GameAchievements &GameAchievementList() {
	static GameAchievements list;
	return list;
}

void GameAchievements::Open(const std::string &path, const std::string &discId) {
	Close();
	open_ = true;
	path_ = path;
	(void)discId;

	if (!Achievements::IsLoggedIn()) {
		state_ = State::notSignedIn;
		return;
	}

	psp5::Trace("achievements: identifying %s", path.c_str());
	state_ = State::working;
	asked_ = true;
	pendingPath_ = path;
	busy_ = true;
	worker_ = std::thread([this] {
		Identify();
		busy_ = false;
	});
}

// Runs on the worker. Everything it touches belongs to this view or is
// PPSSPP's own file loading; the home screen stops pumping the request queue
// while it runs (see AchievementsIdentifying), so rcheevos is not being driven
// from two threads at once.
void GameAchievements::Identify() {
	// ConstructFileLoader hands back a loader for whatever the path is; Identify
	// tells SetGame which hashing method to use. A type it cannot hash is
	// refused inside SetGame with a log line and nothing else, so it is caught
	// here instead, where there is somewhere to say so.
	FileLoader *loader = ConstructFileLoader(Path(pendingPath_));
	if (!loader) {
		state_ = State::unsupported;
		return;
	}
	std::string error;
	const IdentifiedFileType type = Identify_File(loader, &error);
	if (type != IdentifiedFileType::PSP_ISO && type != IdentifiedFileType::PSP_ISO_NP &&
	    type != IdentifiedFileType::PSP_PBP_DIRECTORY) {
		psp5::Trace("achievements: %s is not a type RetroAchievements can hash",
		            pendingPath_.c_str());
		delete loader;
		state_ = State::unsupported;
		return;
	}
	Achievements::SetGame(Path(pendingPath_), type, loader);
	// SetGame does not take the loader: the block device built from it borrows
	// it, and PPSSPP's own caller keeps ownership too.
	delete loader;
}

void GameAchievements::Update() {
	if (busy_) {
		return;  // still hashing, on the worker
	}
	if (worker_.joinable()) {
		worker_.join();
	}
	if (state_ != State::working) {
		return;  // the worker settled it: an unsupported file, or no game
	}
	// Waiting on the server now. The shelf pumps the request queue every frame
	// once the worker is done, so this does settle.
	if (Achievements::IsBlockingExecution()) {
		return;
	}
	Read();
}

// Whether the worker is reading the disc image. While it is, the home screen
// leaves the request queue alone: rcheevos is being called from the worker and
// driving it from the frame loop at the same time is a race.
bool AchievementsIdentifying() {
	return GameAchievementList().identifying();
}

void GameAchievements::Read() {
	if (!Achievements::IsActive()) {
		// Identified, and the server has nothing for it - or it is not a game
		// RetroAchievements carries.
		state_ = State::none;
		psp5::Trace("achievements: none for %s", path_.c_str());
		return;
	}

	rc_client_t *client = Achievements::GetClient();
	rc_client_user_game_summary_t summary {};
	rc_client_get_user_game_summary(client, &summary);

	char text[96];
	std::snprintf(text, sizeof(text), "%u of %u", summary.num_unlocked_achievements,
	              summary.num_core_achievements);
	summary_ = text;
	std::snprintf(text, sizeof(text), "%u / %u points", summary.points_unlocked,
	              summary.points_core);
	points_ = text;
	fraction_ = summary.num_core_achievements > 0
	                ? (float)summary.num_unlocked_achievements / (float)summary.num_core_achievements
	                : 0.0f;

	rc_client_achievement_list_t *list = rc_client_create_achievement_list(
	    client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
	    RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
	if (!list) {
		state_ = State::none;
		return;
	}

	for (uint32_t b = 0; b < list->num_buckets; ++b) {
		const rc_client_achievement_bucket_t &bucket = list->buckets[b];
		if (!bucket.num_achievements) {
			continue;
		}
		GameAchievement head;
		head.title = bucket.label ? bucket.label : "";
		head.header = true;
		std::snprintf(text, sizeof(text), "%u", bucket.num_achievements);
		head.points = text;
		rows_.push_back(head);

		for (uint32_t a = 0; a < bucket.num_achievements; ++a) {
			const rc_client_achievement_t *achievement = bucket.achievements[a];
			if (!achievement) {
				continue;
			}
			GameAchievement row;
			row.title = achievement->title ? achievement->title : "";
			row.detail = achievement->description ? achievement->description : "";
			std::snprintf(text, sizeof(text), "%u", achievement->points);
			row.points = text;
			row.unlocked = achievement->unlocked != 0;
			// The plain badge once it is earned, the greyed one until then,
			// which is how RetroAchievements' own pages read.
			const char *badge =
			    row.unlocked ? achievement->badge_url : achievement->badge_locked_url;
			if (badge && badge[0]) {
				row.badgeUrl = badge;
			}
			if (achievement->measured_progress[0] != '\0') {
				row.progress = std::clamp(achievement->measured_percent / 100.0f, 0.0f, 1.0f);
			}
			rows_.push_back(row);
		}
	}
	rc_client_destroy_achievement_list(list);

	state_ = rows_.empty() ? State::none : State::ready;
	psp5::Trace("achievements: %u row(s) for %s", (unsigned)rows_.size(), path_.c_str());

	// The badges. One small PNG each, queued rather than all started at once -
	// see queue_. They arrive over the next second or two and a row draws a
	// plain mark until its own does.
	for (const GameAchievement &row : rows_) {
		if (row.badgeUrl.empty() ||
		    std::find(queue_.begin(), queue_.end(), row.badgeUrl) != queue_.end()) {
			continue;
		}
		queue_.push_back(row.badgeUrl);
	}
	psp5::Trace("achievements: %u badge(s) to fetch", (unsigned)queue_.size());
}

void GameAchievements::PumpBadges() {
	// Four at a time. Enough to fill a screenful quickly, few enough that the
	// console's network stack is never asked for more than it has.
	constexpr int kAtOnce = 4;
	while (inFlight_ < kAtOnce && !queue_.empty()) {
		const std::string url = queue_.back();
		queue_.pop_back();
		++inFlight_;
		g_DownloadManager.StartDownload(
		    url, Path(), http::RequestFlags::Default, nullptr, "",
		    [this, url](http::Request &download) {
			    --inFlight_;
			    if (download.ResultCode() != 200) {
				    psp5::Trace("achievements: badge %s failed (%d)", url.c_str(),
				                download.ResultCode());
				    return;
			    }
			    std::string data;
			    download.buffer().TakeAll(&data);
			    std::lock_guard<std::mutex> guard(arrivedLock_);
			    arrived_.push_back({url, std::move(data)});
		    });
	}
}

void GameAchievements::UploadBadges(
    const std::function<std::uint32_t(const Artwork &)> &upload) {
	std::vector<PendingBadge> ready;
	{
		std::lock_guard<std::mutex> guard(arrivedLock_);
		ready.swap(arrived_);
	}
	PumpBadges();
	for (const PendingBadge &badge : ready) {
		Artwork art;
		if (!DecodePng(badge.png, &art)) {
			psp5::Trace("achievements: a badge would not decode");
			continue;
		}
		const std::uint32_t texture = upload(art);
		if (!texture) {
			continue;
		}
		// One image can belong to several rows - every locked badge in a set
		// can be the same picture - so every row wearing it gets the handle,
		// and ReleaseBadges destroys each one once.
		for (GameAchievement &row : rows_) {
			if (row.badgeUrl == badge.url) {
				row.badge = texture;
			}
		}
	}
}

void GameAchievements::ReleaseBadges(const std::function<void(std::uint32_t)> &destroy) {
	// Only what Close has retired. This runs every frame, and retiring the live
	// rows here would have destroyed each badge on the frame after it was made
	// - including ones the screen was still drawing.
	for (std::uint32_t texture : retired_) {
		destroy(texture);
	}
	retired_.clear();
}

// Moves the rows' textures aside, deduplicated - one image can belong to many
// rows, and destroying a handle twice is not a thing to do.
void GameAchievements::Retire() {
	for (GameAchievement &row : rows_) {
		if (!row.badge) {
			continue;
		}
		if (std::find(retired_.begin(), retired_.end(), row.badge) == retired_.end()) {
			retired_.push_back(row.badge);
		}
		row.badge = 0;
	}
}

void GameAchievements::Close() {
	if (worker_.joinable()) {
		// It will finish on its own; nothing here can hurry a disc read. Waiting
		// is the only safe thing to do with a thread that writes to this object.
		worker_.join();
	}
	busy_ = false;
	if (asked_) {
		// The game was loaded into the client only to be read. Leaving it there
		// would mean the next game to boot found one already loaded.
		Achievements::UnloadGame();
		asked_ = false;
	}
	{
		std::lock_guard<std::mutex> guard(arrivedLock_);
		arrived_.clear();
	}
	Retire();
	queue_.clear();
	rows_.clear();
	summary_.clear();
	points_.clear();
	path_.clear();
	fraction_ = 0.0f;
	state_ = State::idle;
	open_ = false;
}

}  // namespace psp5
