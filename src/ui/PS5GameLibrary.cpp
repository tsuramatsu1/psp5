// psp5 - the games on the memory stick, as the home screen's shelf.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The kit ships a catalogue of invented titles so its designs have something to
// show. psp5 has something real to show, so this replaces it: a walk of the
// memory stick, and a cover rendered for each game found.
//
// Covers are drawn, not extracted. A PSP disc carries an ICON0.PNG, but reading
// it means parsing ISO9660 - and CSO, and the PBP container - before the first
// frame. Until that exists, a title set over a backdrop keyed to the game's own
// name is the honest placeholder: it never pretends to be the publisher's art,
// and it gives each game a stable identity on the shelf between runs.

#include "PS5GameLibrary.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>

#include "PS5Log.h"
#include "PS5Paths.h"
#include "ui/PS5GameArt.h"
#include "ui/PS5Settings.h"
#include "ui/PS5Strings.h"

#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"

namespace psp5 {
namespace {

using hui::gfx::Color;

// How deep to walk below the memory stick. PSP/GAME/<folder>/EBOOT.PBP is three,
// so four leaves room for one stray subfolder without letting a wrong root turn
// this into a walk of the whole stick.
constexpr int kMaxDepth = 4;

constexpr hui::gfx::BackdropMode kModes[] = {
    hui::gfx::BackdropMode::aurora, hui::gfx::BackdropMode::waves,
    hui::gfx::BackdropMode::bokeh,  hui::gfx::BackdropMode::grid,
    hui::gfx::BackdropMode::vista,  hui::gfx::BackdropMode::stars,
    hui::gfx::BackdropMode::dots,   hui::gfx::BackdropMode::phosphor,
};

// One UTF-8 character, and where the next one starts.
std::uint32_t NextCodepoint(const std::string &text, std::size_t *at) {
	const unsigned char lead = (unsigned char)text[*at];
	int extra = 0;
	std::uint32_t value = lead;
	if (lead >= 0xF0) {
		extra = 3;
		value = lead & 0x07u;
	} else if (lead >= 0xE0) {
		extra = 2;
		value = lead & 0x0Fu;
	} else if (lead >= 0xC0) {
		extra = 1;
		value = lead & 0x1Fu;
	}
	if (*at + (std::size_t)extra >= text.size()) {
		*at = text.size();
		return lead;
	}
	for (int i = 0; i < extra; ++i) {
		value = (value << 6) | ((unsigned char)text[*at + 1 + (std::size_t)i] & 0x3Fu);
	}
	*at += (std::size_t)extra + 1;
	return value;
}

// The kit's fonts are baked atlases of about 113 glyphs - printable ASCII and a
// handful of symbols - and anything outside that is drawn as a question mark.
// Game titles are not written for that: a PARAM.SFO carries whatever the
// publisher used, and an Asian release of a western game is full of fullwidth
// punctuation, so "God of War - Ghost of Sparta" arrived with a dash the atlas
// had never heard of and came out as "God of War ? Ghost of Sparta".
//
// So the few characters that have an obvious ASCII equivalent are given it, and
// the rest are dropped rather than drawn as noise. A title in a script the atlas
// cannot show at all - Japanese, Korean - is left to its filename, which is
// usually transliterated already.
std::string Simplify(const std::string &text) {
	std::string out;
	out.reserve(text.size());
	std::size_t at = 0;
	while (at < text.size()) {
		const std::uint32_t c = NextCodepoint(text, &at);
		if (c >= 0x20 && c < 0x7F) {
			out.push_back((char)c);
			continue;
		}
		switch (c) {
			case 0x00A0:  // no-break space
			case 0x3000:  // ideographic space
				out.push_back(' ');
				break;
			case 0x00AD:  // soft hyphen
			case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015:
			case 0x2212:  // minus sign
			case 0x30FC:  // katakana prolonged sound mark
			case 0xFF0D:  // fullwidth hyphen-minus
				out.push_back('-');
				break;
			case 0x2018: case 0x2019: case 0x02BC:
				out.push_back('\'');
				break;
			case 0x201C: case 0x201D:
				out.push_back('"');
				break;
			case 0x2026:
				out.append("...");
				break;
			case 0x00D7:
				out.push_back('x');
				break;
			default:
				// Fullwidth forms sit one block above their ASCII originals.
				if (c >= 0xFF01 && c <= 0xFF5E) {
					out.push_back((char)(c - 0xFF00 + 0x20));
				}
				// Everything else - (R), (TM), CJK - is dropped.
				break;
		}
	}
	// Dropping a character can leave two spaces where there was one.
	std::string tidy;
	bool gap = false;
	for (const char c : out) {
		if (c == ' ') {
			gap = true;
			continue;
		}
		if (gap && !tidy.empty()) {
			tidy.push_back(' ');
		}
		gap = false;
		tidy.push_back(c);
	}
	while (!tidy.empty() && (tidy.back() == ' ' || tidy.back() == '-')) {
		tidy.pop_back();
	}
	return tidy;
}

std::string Lower(const std::string &text) {
	std::string out = text;
	for (char &c : out) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return out;
}

// Lower-cased, so the match below is case-insensitive: a stick that has been
// through a FAT volume may hold .ISO as readily as .iso.
std::string Extension(const std::string &name) {
	const std::size_t dot = name.find_last_of('.');
	if (dot == std::string::npos) {
		return std::string();
	}
	return Lower(name.substr(dot + 1));
}

// The label the shelf shows for a format, or nullptr when psp5 does not boot it.
const char *FormatName(const std::string &extension) {
	if (extension == "iso") {
		return "ISO";
	}
	if (extension == "cso") {
		return "CSO";
	}
	if (extension == "chd") {
		return "CHD";
	}
	if (extension == "pbp") {
		return "PBP";
	}
	return nullptr;
}

// The name a player recognises, from the name a dumper wrote. Region, language
// and revision tags live in brackets by convention and are noise on a shelf;
// separators vary with whatever filesystem the file crossed on its way here.
std::string PrettyTitle(const std::string &filename) {
	std::string name = filename;
	const std::size_t dot = name.find_last_of('.');
	if (dot != std::string::npos && dot > 0) {
		name.erase(dot);
	}

	// Every (...) and [...] group goes. Scanned rather than cut at the first
	// bracket, so a title that contains one keeps the text on both sides.
	std::string stripped;
	int depth = 0;
	for (const char c : name) {
		if (c == '(' || c == '[') {
			++depth;
		} else if (c == ')' || c == ']') {
			if (depth > 0) {
				--depth;
			}
		} else if (depth == 0) {
			stripped.push_back(c);
		}
	}
	for (char &c : stripped) {
		if (c == '_' || c == '.') {
			c = ' ';
		}
	}

	std::string out;
	bool gap = false;
	for (const char c : stripped) {
		if (c == ' ') {
			gap = true;
			continue;
		}
		if (gap && !out.empty()) {
			out.push_back(' ');
		}
		gap = false;
		out.push_back(c);
	}
	while (!out.empty() && (out.back() == ' ' || out.back() == '-')) {
		out.pop_back();
	}
	return out.empty() ? filename : Simplify(out);
}

std::string SizeText(std::uint64_t bytes) {
	char text[32];
	const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
	if (mb >= 1024.0) {
		std::snprintf(text, sizeof(text), "%.1f GB", mb / 1024.0);
	} else if (mb >= 1.0) {
		std::snprintf(text, sizeof(text), "%.0f MB", mb);
	} else {
		std::snprintf(text, sizeof(text), "%.0f KB", static_cast<double>(bytes) / 1024.0);
	}
	return text;
}

// A position on the colour wheel derived from the name, so a game keeps the same
// palette between runs and neighbours on the shelf rarely collide.
float HueFor(const std::string &text) {
	std::uint32_t hash = 2166136261u;
	for (const unsigned char c : text) {
		hash = (hash ^ c) * 16777619u;
	}
	return static_cast<float>(hash % 3600u) / 3600.0f;
}

// The hue of a game's own artwork, so the shelf's backdrop takes its colour from
// the cover rather than from a hash of the filename. Pixels are weighted by how
// saturated they are: a plain average of a mostly dark icon comes out grey, and
// the one bright thing in it is usually what the art is actually about.
bool HueOfArtwork(const Artwork &art, float *hue) {
	if (!art.valid()) {
		return false;
	}
	double sum[3] = {0.0, 0.0, 0.0};
	double weight = 0.0;
	const std::size_t pixels = art.rgba.size() / 4;
	// Every fourth pixel: enough for an average, and it keeps a 480x272 PIC1
	// down to a few thousand samples.
	for (std::size_t i = 0; i < pixels; i += 4) {
		const std::uint8_t *p = &art.rgba[i * 4];
		if (p[3] < 32) {
			continue;  // transparent corners must not drag the average to black
		}
		const double r = p[0] / 255.0;
		const double g = p[1] / 255.0;
		const double b = p[2] / 255.0;
		const double high = std::max(r, std::max(g, b));
		const double low = std::min(r, std::min(g, b));
		const double w = 0.15 + (high - low);
		sum[0] += r * w;
		sum[1] += g * w;
		sum[2] += b * w;
		weight += w;
	}
	if (weight <= 0.0) {
		return false;
	}
	const float r = (float)(sum[0] / weight);
	const float g = (float)(sum[1] / weight);
	const float b = (float)(sum[2] / weight);
	const float high = std::max(r, std::max(g, b));
	const float low = std::min(r, std::min(g, b));
	const float chroma = high - low;
	if (chroma < 0.02f) {
		return false;  // grey artwork has no hue worth taking
	}
	float h;
	if (high == r) {
		h = (g - b) / chroma;
	} else if (high == g) {
		h = 2.0f + (b - r) / chroma;
	} else {
		h = 4.0f + (r - g) / chroma;
	}
	h /= 6.0f;
	*hue = h < 0.0f ? h + 1.0f : h;
	return true;
}

// The sub-rectangle of a texture that fills a square without distorting it:
// what a centre crop looks like in UV space. PIC1 is 480x272, so about a fifth
// is cropped from each side.
hui::gfx::Rect FillSquareUv(int width, int height) {
	if (width <= 0 || height <= 0) {
		return hui::gfx::kFullUv;
	}
	const float aspect = (float)width / (float)height;
	if (aspect > 1.0f) {
		const float w = 1.0f / aspect;
		return {(1.0f - w) * 0.5f, 0.0f, w, 1.0f};
	}
	const float h = aspect;
	return {0.0f, (1.0f - h) * 0.5f, 1.0f, h};
}

Color FromHsv(float h, float s, float v) {
	const float sector = h * 6.0f;
	const int i = static_cast<int>(sector) % 6;
	const float f = sector - std::floor(sector);
	const float p = v * (1.0f - s);
	const float q = v * (1.0f - s * f);
	const float t = v * (1.0f - s * (1.0f - f));
	switch (i) {
		case 0: return {v, t, p, 1.0f};
		case 1: return {q, v, p, 1.0f};
		case 2: return {p, v, t, 1.0f};
		case 3: return {p, q, v, 1.0f};
		case 4: return {t, p, v, 1.0f};
		default: return {v, p, q, 1.0f};
	}
}

}  // namespace

std::string GameViewName(GameView view) {
	switch (view) {
		case GameView::recent: return UiWord(UiString::recent);
		case GameView::alphabetical: return UiWord(UiString::alphabetical);
		case GameView::favorites: return UiWord(UiString::favorites);
		default: return "";
	}
}

namespace {

// One path per line. Paths, not disc ids, because homebrew often has neither a
// disc id nor a unique name, and the path is what the shelf already knows.
std::string FavoritesPath() {
	return PS5Paths::Config() + "/favorites.txt";
}

std::string KnownPath() {
	return PS5Paths::Config() + "/games.txt";
}

// One shelf entry from what a directory said about a file, or false if the
// file is not a game.
//
// `folder` is the name of the directory the file sits in, or empty at the top
// of a walk. Not the whole path.
bool MakeEntry(const std::string &folder, const std::string &name, std::uint64_t size,
               std::uint64_t mtime, GameEntry *entry) {
	const char *format = FormatName(Extension(name));
	if (!format) {
		return false;
	}
	entry->format = format;
	// An EBOOT.PBP is named after its folder, not after itself: without this
	// every one of them would sit on the shelf as "EBOOT".
	if (Lower(name) == "eboot.pbp" && !folder.empty()) {
		entry->title = PrettyTitle(folder);
	} else {
		entry->title = PrettyTitle(name);
	}
	entry->size = size;
	entry->mtime = mtime;
	entry->size_text = SizeText(size);

	char when[48];
	const std::time_t stamp = static_cast<std::time_t>(mtime);
	if (const std::tm *parts = std::gmtime(&stamp)) {
		std::strftime(when, sizeof(when), "%d %b %Y", parts);
		entry->year = parts->tm_year + 1900;
	} else {
		std::snprintf(when, sizeof(when), "date unknown");
	}
	entry->date_text = when;
	return true;
}

// The last component of a path, which is the folder name MakeEntry wants.
std::string FolderName(const std::string &path) {
	const std::size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

GameLibrary &Library() {
	static GameLibrary library;
	return library;
}

void GameLibrary::Scan() {
	entries_.clear();
	ScanLocal();
}

void GameLibrary::ScanLocal() {
	// An explicit stack, not recursion: the depth is bounded here, and nothing
	// about the shape of a memory stick should be able to grow the call stack.
	struct Pending {
		std::string path;
		int depth;
	};
	std::vector<Pending> pending;
	pending.push_back({PS5Paths::Memstick(), 0});
	// The drives, under the same tree the title's own memory stick uses, so a
	// game is at <drive>/PSP/GAME. Nothing comes back when psp5 is sandboxed:
	// a folder that cannot be listed holds nothing psp5 can find, which is why
	// PS5_Unlock runs before anything else (PS5Unlock.h).
	//
	// The depth starts at one, not zero. These roots are already the PSP folder
	// rather than the stick above it, and starting at zero would let the walk
	// one level deeper on a drive than on the memory stick - which on a drive
	// full of other things is a long walk for nothing.
	for (const std::string &drive : PS5Paths::Drives()) {
		pending.push_back({drive, 1});
	}

	while (!pending.empty() && entries_.size() < kMaxGames) {
		const Pending here = pending.back();
		pending.pop_back();

		DIR *dir = opendir(here.path.c_str());
		if (!dir) {
			continue;
		}
		while (const dirent *ent = readdir(dir)) {
			const std::string name = ent->d_name;
			if (name == "." || name == "..") {
				continue;
			}
			const std::string full = here.path + "/" + name;
			// d_type is not dependable here, so the kind comes from stat - which
			// also carries the size and the date the shelf shows.
			struct stat info;
			if (stat(full.c_str(), &info) != 0) {
				continue;
			}
			if (S_ISDIR(info.st_mode)) {
				if (here.depth + 1 < kMaxDepth) {
					pending.push_back({full, here.depth + 1});
				}
				continue;
			}
			GameEntry entry;
			const std::string folder = here.depth > 0 ? FolderName(here.path) : std::string();
			if (!MakeEntry(folder, name, static_cast<std::uint64_t>(info.st_size),
			               static_cast<std::uint64_t>(info.st_mtime), &entry)) {
				continue;
			}
			entry.path = full;

			entries_.push_back(std::move(entry));
			if (entries_.size() >= kMaxGames) {
				break;
			}
		}
		closedir(dir);
	}
}

void GameLibrary::BuildOrders() {
	std::vector<int> all(entries_.size());
	for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
		all[static_cast<std::size_t>(i)] = i;
	}

	// Recent means recently played, and then recently added: the game just put
	// down is the one most likely to be picked up again, ahead of a file that
	// was only copied across. A game that has never been played has no time of
	// its own, so it falls back to the file's - which is what the whole view
	// used to be sorted by, and why playing a game left it where it was.
	std::vector<int> &recent = order_[static_cast<std::size_t>(GameView::recent)];
	recent = all;
	std::stable_sort(recent.begin(), recent.end(), [this](int a, int b) {
		const GameEntry &left = entries_[static_cast<std::size_t>(a)];
		const GameEntry &right = entries_[static_cast<std::size_t>(b)];
		const std::uint64_t lp = psp5::LastPlayed(left.disc_id);
		const std::uint64_t rp = psp5::LastPlayed(right.disc_id);
		if ((lp != 0) != (rp != 0)) {
			return lp != 0;  // anything played outranks anything not
		}
		if (lp != rp) {
			return lp > rp;
		}
		return left.mtime > right.mtime;
	});

	std::vector<int> &alpha = order_[static_cast<std::size_t>(GameView::alphabetical)];
	alpha = all;
	std::stable_sort(alpha.begin(), alpha.end(), [this](int a, int b) {
		return Lower(entries_[static_cast<std::size_t>(a)].title) <
		       Lower(entries_[static_cast<std::size_t>(b)].title);
	});

	// Favorites keeps the alphabetical order of whatever is marked, so the shelf
	// does not reshuffle itself as games are marked and unmarked.
	std::vector<int> &favorites = order_[static_cast<std::size_t>(GameView::favorites)];
	favorites.clear();
	for (const int index : alpha) {
		if (entries_[static_cast<std::size_t>(index)].favorite) {
			favorites.push_back(index);
		}
	}

	BuildFound();
}

// Every view, narrowed to whatever the query matches, in that view's own
// order. An empty query leaves these empty and order() reads the full lists
// instead, so there is no state to undo when a search is cleared.
void GameLibrary::BuildFound() {
	for (std::vector<int> &found : found_) {
		found.clear();
	}
	if (query_.empty()) {
		return;
	}
	// Matched once per game rather than once per game per view: the three
	// orders are three arrangements of one library, and lowercasing every
	// title three times is three times the work for the same answer.
	const std::string needle = Lower(query_);
	std::vector<bool> matched(entries_.size(), false);
	for (std::size_t i = 0; i < entries_.size(); ++i) {
		const GameEntry &entry = entries_[i];
		// The disc id as well as the name, so ULUS10512 finds a game whose
		// title nobody can spell.
		matched[i] = Lower(entry.title).find(needle) != std::string::npos ||
		             Lower(entry.disc_id).find(needle) != std::string::npos;
	}
	for (std::size_t view = 0; view < static_cast<std::size_t>(GameView::count); ++view) {
		for (const int index : order_[view]) {
			if (matched[static_cast<std::size_t>(index)]) {
				found_[view].push_back(index);
			}
		}
	}
}

void GameLibrary::LoadFavorites() {
	FILE *fh = fopen(FavoritesPath().c_str(), "rb");
	if (!fh) {
		return;
	}
	char line[1024];
	while (fgets(line, sizeof(line), fh)) {
		std::string path(line);
		while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) {
			path.pop_back();
		}
		for (GameEntry &entry : entries_) {
			if (entry.path == path) {
				entry.favorite = true;
				break;
			}
		}
	}
	fclose(fh);
}

void GameLibrary::SaveFavorites() const {
	FILE *fh = fopen(FavoritesPath().c_str(), "wb");
	if (!fh) {
		psp5::Trace("library: cannot write %s", FavoritesPath().c_str());
		return;
	}
	for (const GameEntry &entry : entries_) {
		if (entry.favorite) {
			fprintf(fh, "%s\n", entry.path.c_str());
		}
	}
	fclose(fh);
	chmod(FavoritesPath().c_str(), 0666);
}

// One line a game: disc id, title, date, size, path. The path is last and is
// whatever follows the fourth tab, so a path with a tab in it still reads back
// whole; the title is tab-free by the time Simplify is done with it.
void GameLibrary::LoadKnown() {
	known_.clear();
	known_dirty_ = false;
	FILE *fh = fopen(KnownPath().c_str(), "rb");
	if (!fh) {
		return;
	}
	char line[2048];
	while (fgets(line, sizeof(line), fh)) {
		std::string text(line);
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
			text.pop_back();
		}
		std::size_t at = 0;
		std::string field[4];
		bool whole = true;
		for (std::string &part : field) {
			const std::size_t tab = text.find('\t', at);
			if (tab == std::string::npos) {
				whole = false;
				break;
			}
			part = text.substr(at, tab - at);
			at = tab + 1;
		}
		if (!whole || at >= text.size()) {
			continue;
		}
		Known known;
		known.disc_id = field[0];
		known.title = field[1];
		known.mtime = strtoull(field[2].c_str(), nullptr, 10);
		known.size = strtoull(field[3].c_str(), nullptr, 10);
		known_[text.substr(at)] = known;
	}
	fclose(fh);
}

void GameLibrary::SaveKnown() const {
	FILE *fh = fopen(KnownPath().c_str(), "wb");
	if (!fh) {
		psp5::Trace("library: cannot write %s", KnownPath().c_str());
		return;
	}
	// Everything, not only what is on the shelf now: a drive that is unplugged
	// today is the same drive tomorrow, and forgetting its games would put them
	// back at the bottom of Recent when it returns.
	for (const auto &entry : known_) {
		fprintf(fh, "%s\t%s\t%llu\t%llu\t%s\n", entry.second.disc_id.c_str(),
		        entry.second.title.c_str(), (unsigned long long)entry.second.mtime,
		        (unsigned long long)entry.second.size, entry.first.c_str());
	}
	fclose(fh);
	chmod(KnownPath().c_str(), 0666);
}

void GameLibrary::Remember(std::size_t index) {
	if (index >= entries_.size()) {
		return;
	}
	const GameEntry &entry = entries_[index];
	if (entry.disc_id.empty()) {
		// Homebrew and anything else without a PARAM.SFO. There is nothing to
		// file its play time under, so there is nothing worth keeping.
		return;
	}
	Known &known = known_[entry.path];
	if (known.disc_id == entry.disc_id && known.title == entry.title &&
	    known.mtime == entry.mtime && known.size == entry.size) {
		return;
	}
	known.disc_id = entry.disc_id;
	known.title = entry.title;
	known.mtime = entry.mtime;
	known.size = entry.size;
	known_dirty_ = true;
}

bool GameLibrary::IsFavorite(std::size_t index) const {
	return index < entries_.size() && entries_[index].favorite;
}

void GameLibrary::ToggleFavorite(std::size_t index) {
	if (index >= entries_.size()) {
		return;
	}
	entries_[index].favorite = !entries_[index].favorite;
	BuildOrders();
	SaveFavorites();
}

void GameLibrary::Release(hui::gfx::Renderer &renderer) {
	// Before the textures go: the worker hands finished art to PumpCovers, and
	// nothing may reach the renderer after this.
	StopWorker();
	{
		std::lock_guard<std::mutex> held(lock_);
		wanted_.clear();
		finished_.clear();
	}
	recent_covers_.clear();
	std::fill(asked_.begin(), asked_.end(), false);
	for (hui::demo::Item &item : items_) {
		if (item.cover != 0) {
			renderer.destroy_texture(item.cover);
			item.cover = 0;
		}
	}
}

void GameLibrary::MakeCover(hui::gfx::Renderer &renderer, const hui::ui::Fonts &fonts,
                            std::size_t index, const GameArt &art) {
	constexpr float kSize = static_cast<float>(kCoverSize);
	const GameEntry &entry = entries_[index];
	hui::demo::Item &item = items_[index];
	hui::gfx::DrawList list;

	// The game's own images, uploaded just long enough to be drawn into the
	// cover. They are released below: what the shelf keeps is the one square
	// texture, not three.
	const std::uint32_t background =
	    art.background.valid()
	        ? renderer.create_texture(art.background.width, art.background.height,
	                                  art.background.rgba.data())
	        : 0;
	const std::uint32_t icon =
	    art.icon.valid()
	        ? renderer.create_texture(art.icon.width, art.icon.height, art.icon.rgba.data())
	        : 0;

	hui::gfx::BackdropSpec spec;
	// With key art there is nothing for a procedural backdrop to do but show
	// through it.
	spec.mode = background != 0 ? hui::gfx::BackdropMode::none
	                            : kModes[index % (sizeof(kModes) / sizeof(kModes[0]))];
	spec.colors[0] = item.dark;
	spec.colors[1] = item.mid;
	spec.colors[2] = hui::gfx::mix(item.mid, item.accent, 0.6f);
	spec.colors[3] = item.accent;
	spec.params[0] = 0.4f;
	spec.params[1] = 0.3f;
	spec.params[2] = 0.5f;
	// A different moment of the same animation per cover, so no two are the
	// same picture even where the backdrop mode repeats.
	spec.time = 11.0f + static_cast<float>(index) * 7.3f;

	if (background != 0) {
		// PIC1 is 480x272 and the card is square, so it is centre-cropped and
		// dimmed - it is the setting here, not the subject.
		list.image(background, {0, 0, kSize, kSize},
		           FillSquareUv(art.background.width, art.background.height),
		           Color{0.62f, 0.62f, 0.62f, 1.0f});
	}
	if (icon != 0) {
		// ICON0 at its own aspect, which is 144x80 on a UMD title. Stretching
		// it to the square would be the one thing worse than not showing it.
		const float width = 368.0f;
		const float height = width * (float)art.icon.height / (float)art.icon.width;
		const hui::gfx::Rect rect{(kSize - width) * 0.5f, 150.0f - height * 0.5f, width, height};
		list.shadow({rect.x, rect.y + 14, rect.w, rect.h}, 18, 30,
		            Color{0.0f, 0.0f, 0.0f, 0.55f});
		list.image(icon, rect, hui::gfx::kFullUv, Color{1.0f, 1.0f, 1.0f, 1.0f}, 10);
		list.bordered_rect(rect, 10, Color{0.0f, 0.0f, 0.0f, 0.0f}, 2,
		                   Color{1.0f, 1.0f, 1.0f, 0.22f});
	} else if (background == 0) {
		// No artwork at all - homebrew, usually. A disc, so the card still
		// reads as a game rather than as a gradient.
		list.glow({146, 86, 220, 220}, 110, 70, item.accent.with_alpha(0.35f));
		list.circle(256, 196, 96, item.accent.with_alpha(0.92f));
		list.circle(256, 196, 34, item.dark.with_alpha(0.95f));
		list.arc(256, 196, 128, 5, 2.0f, 2.4f, Color{1.0f, 1.0f, 1.0f, 0.55f});
	}

	// A dark foot keeps the title legible over any artwork.
	list.gradient_rect({0, 250, kSize, 262}, 0, item.dark.with_alpha(0.0f),
	                   item.dark.with_alpha(0.92f));
	hui::ui::text(list, fonts.semibold, entry.format, 36, 54, 17, Color{1.0f, 1.0f, 1.0f, 0.7f},
	              hui::gfx::Align::left, 2.5f);

	const std::vector<std::string> lines = fonts.display.font->wrap(entry.title, 48, kSize - 72);
	// Three lines at most: a long title must not climb off the top of its own
	// cover and over the artwork.
	const std::size_t shown = std::min<std::size_t>(lines.size(), 3);
	float baseline = kSize - 44.0f - static_cast<float>(shown ? shown - 1 : 0) * 52.0f;
	for (std::size_t line = 0; line < shown; ++line) {
		hui::ui::text(list, fonts.display, lines[line], 36, baseline, 48,
		              Color{1.0f, 1.0f, 1.0f, 1.0f});
		baseline += 52.0f;
	}

	renderer.begin();
	renderer.backdrop(spec);
	renderer.draw(list);
	const std::uint32_t cover = renderer.render_to_texture(kCoverSize, kCoverSize, kSize, kSize);

	// render_to_texture submits and waits for the queue, so by here the draw
	// has finished with these and they can go.
	if (background != 0) {
		renderer.destroy_texture(background);
	}
	if (icon != 0) {
		renderer.destroy_texture(icon);
	}
	if (cover == 0) {
		psp5::Trace("library: cover %u failed", (unsigned)index);
		return;
	}
	if (item.cover != 0) {
		renderer.destroy_texture(item.cover);
	}
	item.cover = cover;
}

// What the game says about itself, once the worker has read it.
void GameLibrary::Apply(std::size_t index, GameArt &art) {
	GameEntry &entry = entries_[index];
	hui::demo::Item &item = items_[index];

	if (!art.title.empty()) {
		// PARAM.SFO over the filename: "God of War: Ghost of Sparta" rather
		// than whatever the dump was called.
		const std::string simplified = Simplify(art.title);
		if (!simplified.empty()) {
			entry.title = simplified;
			// The item points into the entry's string, and assigning it may
			// have moved that string's buffer.
			item.title = entry.title.c_str();
		}
	}
	entry.has_art = art.icon.valid();
	entry.disc_id = art.discId;
	// Now that the game has been opened, so the next run can order Recent
	// without opening it again. PumpCovers writes the file once the batch is
	// done rather than once a game.
	Remember(index);

	// One menu loop in memory, not one per game: a few hundred kilobytes each
	// would be hundreds of megabytes across a library this is now allowed to
	// have. The shelf only ever plays the game it is sitting on.
	for (GameEntry &other : entries_) {
		other.sound.clear();
	}
	if (art.sound.size() <= kMaxSoundBytes) {
		entry.sound = std::move(art.sound);
	}

	// The palette follows the artwork where there is any, so the Aurora
	// backdrop behind the shelf takes its colour from the focused game.
	float hue = 0.0f;
	if (!HueOfArtwork(art.background, &hue) && !HueOfArtwork(art.icon, &hue)) {
		hue = HueFor(entry.title);
	}
	item.dark = FromHsv(hue, 0.72f, 0.16f);
	item.mid = FromHsv(hue, 0.62f, 0.42f);
	// The accent sits a little way round the wheel from the body colour; that
	// offset is what keeps a cover from reading as one flat tone.
	item.accent = FromHsv(std::fmod(hue + 0.08f, 1.0f), 0.55f, 0.95f);
}

void GameLibrary::StartWorker() {
	if (worker_.joinable()) {
		return;
	}
	stopping_ = false;
	worker_ = std::thread([this]() {
		for (;;) {
			std::size_t index = 0;
			{
				std::unique_lock<std::mutex> held(lock_);
				wake_.wait(held, [this]() { return stopping_ || !wanted_.empty(); });
				if (stopping_) {
					return;
				}
				index = wanted_.front();
				wanted_.pop_front();
			}
			// Outside the lock: this mounts an ISO and decodes two PNGs, and
			// holding the lock through it would stall every frame that asks
			// for a cover.
			GameArt art;
			LoadGameArt(entries_[index].path, &art);
			{
				std::lock_guard<std::mutex> held(lock_);
				finished_.push_back({index, std::move(art)});
			}
		}
	});
}

void GameLibrary::StopWorker() {
	if (!worker_.joinable()) {
		return;
	}
	{
		std::lock_guard<std::mutex> held(lock_);
		stopping_ = true;
	}
	wake_.notify_all();
	worker_.join();
}

void GameLibrary::Want(std::size_t index) {
	if (index >= entries_.size()) {
		return;
	}
	// Wanted again moves it to the end of the queue of things to keep, which
	// is what makes the eviction below least-recently-wanted.
	const auto at = std::find(recent_covers_.begin(), recent_covers_.end(), index);
	if (at != recent_covers_.end()) {
		recent_covers_.erase(at);
	}
	recent_covers_.push_back(index);

	if (asked_[index]) {
		return;
	}
	asked_[index] = true;
	{
		std::lock_guard<std::mutex> held(lock_);
		wanted_.push_back(index);
	}
	wake_.notify_one();
}

void GameLibrary::Evict(hui::gfx::Renderer &renderer) {
	// Only covers count against the budget: an entry with no texture costs
	// nothing and can stay in the list.
	std::size_t covers = 0;
	for (const hui::demo::Item &item : items_) {
		covers += item.cover != 0 ? 1 : 0;
	}
	for (std::size_t at = 0; at < recent_covers_.size() && covers > kMaxCovers; ++at) {
		const std::size_t index = recent_covers_[at];
		if (index >= items_.size() || items_[index].cover == 0) {
			continue;
		}
		renderer.destroy_texture(items_[index].cover);
		items_[index].cover = 0;
		// Asked again next time it comes near: the art is not kept, only the
		// texture was, and reading it again is what the worker is for.
		asked_[index] = false;
		--covers;
	}
	// Anything that lost its cover above is no longer worth remembering.
	recent_covers_.erase(std::remove_if(recent_covers_.begin(), recent_covers_.end(),
	                                    [this](std::size_t index) {
		                                    return index >= items_.size() ||
		                                           items_[index].cover == 0;
	                                    }),
	                     recent_covers_.end());
}

void GameLibrary::PumpCovers(hui::gfx::Renderer &renderer, const hui::ui::Fonts &fonts) {
	std::vector<Finished> ready;
	{
		std::lock_guard<std::mutex> held(lock_);
		// A few a frame. render_to_texture waits for the queue each time, so
		// draining the lot would be a visible stall on the first pass over a
		// large library.
		const std::size_t take = std::min<std::size_t>(finished_.size(), 2);
		ready.assign(std::make_move_iterator(finished_.begin()),
		             std::make_move_iterator(finished_.begin() + (std::ptrdiff_t)take));
		finished_.erase(finished_.begin(), finished_.begin() + (std::ptrdiff_t)take);
	}
	for (Finished &done : ready) {
		if (done.index >= items_.size()) {
			continue;
		}
		Apply(done.index, done.art);
		MakeCover(renderer, fonts, done.index, done.art);
		resolved_[done.index] = true;
	}
	if (!ready.empty()) {
		Evict(renderer);
	}
	// Once the batch is done, not once a game: the worker resolves a whole
	// library over the first minute and this would otherwise rewrite the file
	// a few hundred times for the same end.
	if (known_dirty_) {
		SaveKnown();
		known_dirty_ = false;
	}
}

const std::string &GameLibrary::DiscIdNow(std::size_t index) {
	static const std::string none;
	if (index >= entries_.size()) {
		return none;
	}
	if (resolved_[index] || !entries_[index].disc_id.empty()) {
		return entries_[index].disc_id;
	}
	// On the way into a game there is nothing to wait for the worker with, and
	// the cheat file and the achievements both need this now. One ISO open.
	GameArt art;
	LoadGameArt(entries_[index].path, &art);
	entries_[index].disc_id = art.discId;
	resolved_[index] = true;
	// Written out here and not left to the next frame: this is the way into a
	// game, and the home screen may not get another frame before the emulator
	// takes the display. This is also the one game whose play time is about to
	// change, so it is the one Recent most needs to know about next time.
	Remember(index);
	if (known_dirty_) {
		SaveKnown();
		known_dirty_ = false;
	}
	return entries_[index].disc_id;
}

void GameLibrary::SetQuery(const std::string &query) {
	query_ = query;
	BuildFound();
}

bool GameLibrary::Build(hui::gfx::Renderer &renderer, const hui::ui::Fonts &fonts) {
	(void)fonts;
	StopWorker();
	Release(renderer);
	items_.clear();

	Scan();
	psp5::Trace("library: %u game(s) under %s", (unsigned)entries_.size(),
	            PS5Paths::Memstick().c_str());

	// Before the items below, which point into these strings, and before the
	// orders are built, which is the whole point: Recent is sorted on the disc
	// id and nothing else here knows one yet.
	LoadKnown();
	int recalled = 0;
	for (GameEntry &entry : entries_) {
		const auto found = known_.find(entry.path);
		if (found == known_.end()) {
			continue;
		}
		// A file of the same name but a different size or date is a different
		// file. Describing it with what used to be there would put the wrong
		// name on the shelf and the wrong cheats in the game.
		if (found->second.mtime != entry.mtime || found->second.size != entry.size) {
			continue;
		}
		entry.disc_id = found->second.disc_id;
		if (!found->second.title.empty()) {
			entry.title = found->second.title;
		}
		++recalled;
	}
	if (recalled > 0) {
		psp5::Trace("library: %d of them already known by name and disc id", recalled);
	}

	// A shelf of names and colours, ready on the first frame. Nothing here
	// opens a game: the title is the filename until the worker says otherwise,
	// and the colour is derived from that name so the shelf has something to
	// be before any artwork exists.
	items_.reserve(entries_.size());
	for (std::size_t i = 0; i < entries_.size(); ++i) {
		const GameEntry &entry = entries_[i];
		hui::demo::Item item{};
		// These point into entries_, which is complete and never appended to
		// again: see the note on the member in the header.
		item.title = entry.title.c_str();
		item.studio = entry.size_text.c_str();
		item.genre = entry.format.c_str();
		item.blurb = entry.path.c_str();
		item.year = entry.year;
		const float hue = HueFor(entry.title);
		item.dark = FromHsv(hue, 0.72f, 0.16f);
		item.mid = FromHsv(hue, 0.62f, 0.42f);
		item.accent = FromHsv(std::fmod(hue + 0.08f, 1.0f), 0.55f, 0.95f);
		item.cover = 0;
		items_.push_back(item);
	}

	asked_.assign(entries_.size(), false);
	resolved_.assign(entries_.size(), false);
	recent_covers_.clear();

	// After the entries are final, so a favorite can be matched to one.
	LoadFavorites();
	BuildOrders();
	if (!entries_.empty()) {
		StartWorker();
	}
	++generation_;
	return true;
}

}  // namespace psp5
