// psp5 - the artwork a PSP game carries inside it.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace psp5 {

// Decoded image, tightly packed RGBA8, top row first.
struct Artwork {
	std::vector<std::uint8_t> rgba;
	int width = 0;
	int height = 0;

	bool valid() const { return width > 0 && height > 0 && !rgba.empty(); }
};

// What a PSP title says about itself. Every field is optional: homebrew often
// has no icon, and a bad dump may have nothing at all.
struct GameArt {
	std::string title;   // PARAM.SFO TITLE - the name the publisher gave it
	std::string discId;  // PARAM.SFO DISC_ID, eg ULUS10512 - names the cheat file
	Artwork icon;        // ICON0.PNG, 144x80 on a UMD title
	Artwork background;  // PIC1.PNG, 480x272 - the key art, often absent
	// SND0.AT3 as it sits on the disc, still encoded. The menu loop the PSP
	// plays under a highlighted game; decoded only when one is chosen, because
	// decoded it is twenty times the size.
	std::string sound;
};

// Opens a PSP image and reads its artwork, exactly as PPSSPP's own game list
// does: an ISO through a block device and ISO9660, a PBP through its container.
//
// Returns false when the file is not a PSP title psp5 can read. A true return
// with every field empty means the file is a game but carries no artwork.
bool LoadGameArt(const std::string &path, GameArt *out);

// PNG bytes to RGBA. Used for the artwork inside a game image, and for the
// achievement badges psp5 downloads.
bool DecodePng(const std::string &bytes, Artwork *out);

}  // namespace psp5
