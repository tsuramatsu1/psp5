// psp5 - the shapes and colours the in-game panels are drawn from.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>

#include "Common/Render/DrawBuffer.h"
#include "Common/UI/Context.h"
#include "Common/UI/View.h"

// Over a running game psp5 draws with PPSSPP's UIContext, whose FillRect is a
// rectangle and nothing else.
//
// Not DrawBuffer::FillCircle, which looks like the way to round a corner and is
// not: it sweeps its texture coordinates from 0 to 1 across the whole UI atlas,
// so every circle comes out sampling icons and pieces of font. FillRect takes
// its colour from the atlas's white pixel and is the only clean primitive here,
// so the corners below are built from rectangles - a strip per row through the
// corner, which at these radii is indistinguishable from an arc.
//
// Nor PPSSPP's own nine-slice theme images, which would round the corners and
// bring PPSSPP's whole look with them.

namespace psp5 {

// The kit's "Code" theme (src/ui/theme.cpp): ink-blue night, hairline borders,
// a green call to action, blue focus. Taken as it is defined there rather than
// approximated, so the panels match the design they name.
// The home screen's palette, because these panels open over a game that was
// launched from it and should look like the same program. Aurora's accent is
// the pale cyan; everything else is white at an alpha, over black.
constexpr uint32_t kPage = 0xFF000000;        // the bar itself
constexpr uint32_t kSurface = 0x0DFFFFFF;     // a row at rest
constexpr uint32_t kSurfaceHigh = 0x1AFFFFFF; // ... and under the cursor
constexpr uint32_t kInk = 0xFFFFFFFF;
constexpr uint32_t kInkDim = 0xB3FFFFFF;
constexpr uint32_t kInkFaint = 0x80FFFFFF;
// The console's own blue, #0070D1, so these panels read as system interface
// over a game rather than as a theme of their own.
//
// Note the byte order: PPSSPP stores a colour as 0xAABBGGRR, not 0xAARRGGBB,
// so the red and blue bytes are the other way round from the hex above. Writing
// one as it reads is how the accent came out yellow-green.
constexpr uint32_t kPrimary = 0xFFD17000;    // on, and anything that acts
constexpr uint32_t kAccent = 0xFFD17000;     // focus
constexpr uint32_t kOutline = 0x2EFFFFFF;    // hairlines
// The panel itself is solid black; this is what lies over the rest of the
// screen. Dark enough to read against and no darker - fully opaque hid the game
// completely, which made opening the menu look like the game had crashed.
constexpr uint32_t kShade = 0xB8090705;      // the game, behind the bar

constexpr float kRadius = 10.0f;

// A rounded rectangle, built from the one primitive that draws cleanly.
inline void FillRound(UIContext *ui, const Bounds &b, float radius, uint32_t color) {
	if (b.w <= 0.0f || b.h <= 0.0f) {
		return;
	}
	const float r = std::max(0.0f, std::min(radius, std::min(b.w, b.h) * 0.5f));
	if (r < 1.0f) {
		ui->FillRect(UI::Drawable(color), b);
		return;
	}
	// The middle, full width, between the two corner bands.
	ui->FillRect(UI::Drawable(color), Bounds(b.x, b.y + r, b.w, b.h - r * 2.0f));
	// Then one strip per row of each band, inset by the circle it follows.
	//
	// The strips abut exactly and never overlap, and none of them overlaps the
	// middle. They used to run half a pixel long to close seams, which is fine
	// for an opaque fill and wrong for a translucent one: every overlap drew the
	// colour twice, so a 10% white highlight came out banded, with a darker
	// stripe across the middle where only one rectangle had been laid down.
	const int steps = std::max(1, (int)r);
	const float h = r / (float)steps;
	for (int i = 0; i < steps; ++i) {
		const float dy = r - ((float)i + 0.5f) * h;
		const float inset = r - std::sqrt(std::max(0.0f, r * r - dy * dy));
		const float x = b.x + inset;
		const float w = b.w - inset * 2.0f;
		ui->FillRect(UI::Drawable(color), Bounds(x, b.y + (float)i * h, w, h));
		ui->FillRect(UI::Drawable(color),
		             Bounds(x, b.y + b.h - (float)(i + 1) * h, w, h));
	}
}

// A hairline around a rounded rectangle: the border drawn, then the fill inset
// inside it. Cheaper and more exact than stroking a path.
inline void FillRoundOutlined(UIContext *ui, const Bounds &b, float radius, uint32_t fill,
                              uint32_t border, float width = 1.0f) {
	FillRound(ui, b, radius, border);
	FillRound(ui, Bounds(b.x + width, b.y + width, b.w - width * 2.0f, b.h - width * 2.0f),
	          radius - width, fill);
}

// A switch: a pill with the knob at one end, green when on. The shape says
// which way it is without needing to read anything.
inline void DrawToggle(UIContext *ui, float x, float y, bool on) {
	constexpr float kW = 50.0f;
	constexpr float kH = 26.0f;
	const float r = kH * 0.5f;
	if (on) {
		FillRound(ui, Bounds(x, y, kW, kH), r, kPrimary);
	} else {
		FillRoundOutlined(ui, Bounds(x, y, kW, kH), r, kPage, kOutline);
	}
	const float knobR = r - 5.0f;
	const float cx = on ? x + kW - r : x + r;
	FillRound(ui, Bounds(cx - knobR, y + 5.0f, knobR * 2.0f, knobR * 2.0f), knobR,
	          on ? kInk : kInkDim);
}

// A bar that fills to show how far along something is.
inline void DrawMeter(UIContext *ui, const Bounds &b, float fraction, uint32_t fill) {
	FillRound(ui, b, b.h * 0.5f, kOutline);
	const float w = b.w * std::clamp(fraction, 0.0f, 1.0f);
	if (w >= b.h) {
		FillRound(ui, Bounds(b.x, b.y, w, b.h), b.h * 0.5f, fill);
	}
}

}  // namespace psp5
