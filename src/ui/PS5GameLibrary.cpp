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
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>

#include "PS5Log.h"
#include "PS5Paths.h"
#include "ui/PS5GameArt.h"

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

const char *GameViewName(GameView view) {
	switch (view) {
		case GameView::recent: return "Recent";
		case GameView::alphabetical: return "A-Z";
		case GameView::favorites: return "Favorites";
		default: return "";
	}
}

namespace {

// One path per line. Paths, not disc ids, because homebrew often has neither a
// disc id nor a unique name, and the path is what the shelf already knows.
std::string FavoritesPath() {
	return PS5Paths::Config() + "/favorites.txt";
}

}  // namespace

GameLibrary &Library() {
	static GameLibrary library;
	return library;
}

void GameLibrary::Scan() {
	entries_.clear();

	// An explicit stack, not recursion: the depth is bounded here, and nothing
	// about the shape of a memory stick should be able to grow the call stack.
	struct Pending {
		std::string path;
		int depth;
	};
	std::vector<Pending> pending;
	pending.push_back({PS5Paths::Memstick(), 0});

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
			const char *format = FormatName(Extension(name));
			if (!format) {
				continue;
			}

			GameEntry entry;
			entry.path = full;
			entry.format = format;
			// An EBOOT.PBP is named after its folder, not after itself: without
			// this every one of them would sit on the shelf as "EBOOT".
			const std::size_t slash = here.path.find_last_of('/');
			if (Lower(name) == "eboot.pbp" && here.depth > 0 && slash != std::string::npos) {
				entry.title = PrettyTitle(here.path.substr(slash + 1));
			} else {
				entry.title = PrettyTitle(name);
			}
			entry.size = static_cast<std::uint64_t>(info.st_size);
			entry.mtime = static_cast<std::uint64_t>(info.st_mtime);
			entry.size_text = SizeText(entry.size);

			char when[48];
			const std::time_t stamp = static_cast<std::time_t>(info.st_mtime);
			if (const std::tm *parts = std::gmtime(&stamp)) {
				std::strftime(when, sizeof(when), "%d %b %Y", parts);
				entry.year = parts->tm_year + 1900;
			} else {
				std::snprintf(when, sizeof(when), "date unknown");
			}
			entry.date_text = when;

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

	std::vector<int> &recent = order_[static_cast<std::size_t>(GameView::recent)];
	recent = all;
	std::stable_sort(recent.begin(), recent.end(), [this](int a, int b) {
		return entries_[static_cast<std::size_t>(a)].mtime >
		       entries_[static_cast<std::size_t>(b)].mtime;
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
	for (hui::demo::Item &item : items_) {
		if (item.cover != 0) {
			renderer.destroy_texture(item.cover);
			item.cover = 0;
		}
	}
}

bool GameLibrary::Build(hui::gfx::Renderer &renderer, const hui::ui::Fonts &fonts) {
	Release(renderer);
	items_.clear();

	Scan();
	psp5::Trace("library: %u game(s) under %s", (unsigned)entries_.size(),
	            PS5Paths::Memstick().c_str());
	if (entries_.empty()) {
		BuildOrders();
		return true;
	}

	constexpr float kSize = static_cast<float>(kCoverSize);
	items_.reserve(entries_.size());
	hui::gfx::DrawList list;

	for (std::size_t i = 0; i < entries_.size(); ++i) {
		GameEntry &entry = entries_[i];

		// What the game says about itself: its own name and its own artwork.
		GameArt art;
		LoadGameArt(entry.path, &art);
		if (!art.title.empty()) {
			// PARAM.SFO over the filename: "God of War: Ghost of Sparta" rather
			// than whatever the dump was called.
			const std::string simplified = Simplify(art.title);
			if (!simplified.empty()) {
				entry.title = simplified;
			}
		}
		entry.has_art = art.icon.valid();
		entry.disc_id = art.discId;
		// A few hundred kilobytes each. A disc with something far larger under
		// this name is not a menu loop, and is not worth the memory.
		if (art.sound.size() <= kMaxSoundBytes) {
			entry.sound = std::move(art.sound);
		}

		// The palette follows the artwork where there is any, so the Aurora
		// backdrop behind the shelf takes its colour from the focused game.
		float hue = 0.0f;
		if (!HueOfArtwork(art.background, &hue) && !HueOfArtwork(art.icon, &hue)) {
			hue = HueFor(entry.title);
		}

		hui::demo::Item item{};
		// These point into entries_, which is complete and never appended to
		// again: see the note on the member in the header.
		item.title = entry.title.c_str();
		item.studio = entry.size_text.c_str();
		item.genre = entry.format.c_str();
		item.blurb = entry.path.c_str();
		item.year = entry.year;
		item.dark = FromHsv(hue, 0.72f, 0.16f);
		item.mid = FromHsv(hue, 0.62f, 0.42f);
		// The accent sits a little way round the wheel from the body colour; that
		// offset is what keeps a cover from reading as one flat tone.
		item.accent = FromHsv(std::fmod(hue + 0.08f, 1.0f), 0.55f, 0.95f);

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
		                            : kModes[i % (sizeof(kModes) / sizeof(kModes[0]))];
		spec.colors[0] = item.dark;
		spec.colors[1] = item.mid;
		spec.colors[2] = hui::gfx::mix(item.mid, item.accent, 0.6f);
		spec.colors[3] = item.accent;
		spec.params[0] = 0.4f;
		spec.params[1] = 0.3f;
		spec.params[2] = 0.5f;
		// A different moment of the same animation per cover, so no two are the
		// same picture even where the backdrop mode repeats.
		spec.time = 11.0f + static_cast<float>(i) * 7.3f;

		list.clear();
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
			const hui::gfx::Rect rect{(kSize - width) * 0.5f, 150.0f - height * 0.5f, width,
			                          height};
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
		hui::ui::text(list, fonts.semibold, entry.format, 36, 54, 17,
		              Color{1.0f, 1.0f, 1.0f, 0.7f}, hui::gfx::Align::left, 2.5f);

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
		item.cover = renderer.render_to_texture(kCoverSize, kCoverSize, kSize, kSize);

		// render_to_texture submits and waits for the queue, so by here the draw
		// has finished with these and they can go.
		if (background != 0) {
			renderer.destroy_texture(background);
		}
		if (icon != 0) {
			renderer.destroy_texture(icon);
		}

		if (item.cover == 0) {
			psp5::Trace("library: cover %u failed, keeping %u item(s)", (unsigned)i,
			            (unsigned)items_.size());
			break;
		}
		items_.push_back(item);
	}

	unsigned with_art = 0;
	for (const GameEntry &entry : entries_) {
		with_art += entry.has_art ? 1 : 0;
	}
	psp5::Trace("library: %u of %u game(s) carry their own icon", with_art,
	            (unsigned)entries_.size());

	// A failed cover leaves entries with no item behind them. The orders index
	// items, so they are built against what actually has artwork.
	if (items_.size() != entries_.size()) {
		entries_.resize(items_.size());
	}
	// After the entries are final, so a favorite can be matched to one.
	LoadFavorites();
	BuildOrders();
	return true;
}

}  // namespace psp5
