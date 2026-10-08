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
constexpr uint32_t kPage = 0xFF0D1117;       // the bar itself
constexpr uint32_t kSurface = 0xFF161B22;    // a row at rest
constexpr uint32_t kSurfaceHigh = 0xFF21262D; // ... and under the cursor
constexpr uint32_t kInk = 0xFFE6EDF3;
constexpr uint32_t kInkDim = 0xFF8B949E;
constexpr uint32_t kInkFaint = 0xFF6E7681;
constexpr uint32_t kPrimary = 0xFF238636;    // on, and anything that acts
constexpr uint32_t kAccent = 0xFF1F6FEB;     // focus
constexpr uint32_t kOutline = 0xFF30363D;    // hairlines
constexpr uint32_t kShade = 0xC00D1117;      // the game, behind the bar

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
	const int steps = std::max(1, (int)r);
	for (int i = 0; i < steps; ++i) {
		const float dy = r - ((float)i + 0.5f);
		const float inset = r - std::sqrt(std::max(0.0f, r * r - dy * dy));
		const float h = r / (float)steps;
		const float x = b.x + inset;
		const float w = b.w - inset * 2.0f;
		ui->FillRect(UI::Drawable(color), Bounds(x, b.y + (float)i * h, w, h + 0.5f));
		ui->FillRect(UI::Drawable(color),
		             Bounds(x, b.y + b.h - (float)(i + 1) * h - 0.5f, w, h + 0.5f));
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
