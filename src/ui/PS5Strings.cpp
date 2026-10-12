// psp5 - the words that are psp5's own, in the player's language.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One file per language under /app0/assets/lang/psp5/, in the same shape as
// PPSSPP's own: `English = translation`, one per line, '#' starts a comment.
// The English is the key, so a missing line and a missing file both fall back
// to English rather than to a blank row.

#include "ui/PS5Strings.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "PS5Log.h"

namespace psp5 {
namespace {

std::map<std::string, std::string> g_strings;
std::string g_loaded;

// ---- right-to-left -------------------------------------------------------
//
// Hebrew, Arabic, Persian and Urdu are stored in the order they are spoken and
// drawn in the order they are read, which is the other way round. The kit's
// text drawing walks a string from its first byte to its last and puts each
// glyph to the right of the one before, so a Hebrew label came out backwards.
//
// Nothing downstream is touched: the reordering happens here, where psp5 turns
// a key into a string, so by the time the kit sees it the bytes are already in
// the order they should appear. It is the same trick PPSSPP's own `he_IL_invert`
// language plays by hand, done for every right-to-left language instead.
//
// This is the useful part of UAX #9 and not the whole of it: runs of
// right-to-left text are reversed, runs of anything else - Latin, digits,
// a version number - keep their own direction. Nested bracket pairs and
// explicit direction marks are not handled, and no label here has them.

bool IsRtl(std::uint32_t code) {
	return (code >= 0x0590 && code <= 0x05FF) ||  // Hebrew
	       (code >= 0x0600 && code <= 0x06FF) ||  // Arabic
	       (code >= 0x0700 && code <= 0x074F) ||  // Syriac
	       (code >= 0x0750 && code <= 0x077F) ||  // Arabic Supplement
	       (code >= 0xFB1D && code <= 0xFDFF) ||  // Hebrew and Arabic forms
	       (code >= 0xFE70 && code <= 0xFEFF);
}

bool IsNeutral(std::uint32_t code) {
	// Spaces and punctuation take the direction of what surrounds them.
	return code == ' ' || code == '\t' ||
	       (code >= 0x21 && code <= 0x2F) || (code >= 0x3A && code <= 0x40) ||
	       (code >= 0x5B && code <= 0x60) || (code >= 0x7B && code <= 0x7E);
}

// UTF-8 in, codepoints out.
std::vector<std::uint32_t> Decode(const std::string &text) {
	std::vector<std::uint32_t> out;
	for (std::size_t i = 0; i < text.size();) {
		const unsigned char lead = (unsigned char)text[i];
		int length = 1;
		std::uint32_t value = lead;
		if (lead >= 0xF0) {
			length = 4;
			value = lead & 0x07u;
		} else if (lead >= 0xE0) {
			length = 3;
			value = lead & 0x0Fu;
		} else if (lead >= 0xC0) {
			length = 2;
			value = lead & 0x1Fu;
		}
		if (i + (std::size_t)length > text.size()) {
			out.push_back(0xFFFD);
			break;
		}
		for (int k = 1; k < length; ++k) {
			value = (value << 6) | ((unsigned char)text[i + (std::size_t)k] & 0x3Fu);
		}
		out.push_back(value);
		i += (std::size_t)length;
	}
	return out;
}

void Encode(std::uint32_t code, std::string *out) {
	if (code < 0x80) {
		out->push_back((char)code);
	} else if (code < 0x800) {
		out->push_back((char)(0xC0 | (code >> 6)));
		out->push_back((char)(0x80 | (code & 0x3F)));
	} else if (code < 0x10000) {
		out->push_back((char)(0xE0 | (code >> 12)));
		out->push_back((char)(0x80 | ((code >> 6) & 0x3F)));
		out->push_back((char)(0x80 | (code & 0x3F)));
	} else {
		out->push_back((char)(0xF0 | (code >> 18)));
		out->push_back((char)(0x80 | ((code >> 12) & 0x3F)));
		out->push_back((char)(0x80 | ((code >> 6) & 0x3F)));
		out->push_back((char)(0x80 | (code & 0x3F)));
	}
}

std::string Trim(const std::string &text) {
	const std::size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) {
		return std::string();
	}
	const std::size_t last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

}  // namespace

// The string as it should appear, left to right on the screen.
std::string VisualOrder(const std::string &text) {
	const std::vector<std::uint32_t> codes = Decode(text);
	bool any = false;
	for (std::uint32_t code : codes) {
		if (IsRtl(code)) {
			any = true;
			break;
		}
	}
	if (!any) {
		return text;
	}

	// Runs of one direction, with neutrals joining whichever run precedes them.
	struct Run {
		std::size_t from;
		std::size_t to;
		bool rtl;
	};
	std::vector<Run> runs;
	for (std::size_t i = 0; i < codes.size(); ++i) {
		const bool rtl = IsRtl(codes[i]);
		if (!runs.empty() && (rtl == runs.back().rtl || IsNeutral(codes[i]))) {
			runs.back().to = i + 1;
			continue;
		}
		runs.push_back({i, i + 1, rtl});
	}

	// The whole label reads right to left, so its runs come out in reverse,
	// and the characters inside a right-to-left run reverse with them.
	std::string out;
	for (std::size_t r = runs.size(); r-- > 0;) {
		const Run &run = runs[r];
		if (run.rtl) {
			for (std::size_t i = run.to; i-- > run.from;) {
				Encode(codes[i], &out);
			}
		} else {
			for (std::size_t i = run.from; i < run.to; ++i) {
				Encode(codes[i], &out);
			}
		}
	}
	return out;
}

void LoadOwnStrings(const std::string &languageId) {
	if (languageId == g_loaded) {
		return;
	}
	g_loaded = languageId;
	g_strings.clear();
	if (languageId.empty() || languageId == "en_US") {
		return;  // the keys are already English
	}

	const std::string path = "/app0/assets/lang/psp5/" + languageId + ".ini";
	FILE *fh = fopen(path.c_str(), "rb");
	if (!fh) {
		psp5::Trace("strings: no psp5 translations for %s", languageId.c_str());
		return;
	}
	char line[1024];
	while (fgets(line, sizeof(line), fh)) {
		std::string text(line);
		if (text.empty() || text[0] == '#' || text[0] == '[') {
			continue;
		}
		const std::size_t equals = text.find('=');
		if (equals == std::string::npos) {
			continue;
		}
		const std::string key = Trim(text.substr(0, equals));
		const std::string value = Trim(text.substr(equals + 1));
		if (!key.empty() && !value.empty()) {
			g_strings[key] = value;
		}
	}
	fclose(fh);
	psp5::Trace("strings: %u psp5 translation(s) for %s", (unsigned)g_strings.size(),
	            languageId.c_str());
}

std::string Own(const char *english) {
	const auto found = g_strings.find(english);
	return VisualOrder(found == g_strings.end() ? std::string(english)
	                                            : found->second);
}

}  // namespace psp5
