// psp5 - the music a game plays while it is under the cursor.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "audio/mixer.hpp"
#include "audio/stream_ring.hpp"

namespace psp5 {

// A PSP disc carries SND0.AT3, a short loop its own menu plays while the game
// is highlighted. psp5's shelf does the same thing with the same file.
//
// It plays on the mixer's music bus, through a ring the frame loop keeps fed -
// the kit's own arrangement for music, and what gives the fade when the cursor
// moves on.
class GameSound {
public:
	// Attaches to a mixer for as long as the home screen is up.
	void Attach(hui::audio::Mixer &mixer);
	void Detach(hui::audio::Mixer &mixer);

	// The cursor moved. Nothing happens straight away: the PSP waits for the
	// selection to settle before it starts, and a shelf scrolled past at speed
	// would otherwise start and stop a dozen clips.
	void Focus(std::size_t index);

	// Called once a frame, from the loop that owns the mixer. Decodes a game's
	// sound when its moment arrives and keeps the ring fed.
	void Update(hui::audio::Mixer &mixer, float dt);

private:
	void Start(std::size_t index);
	void Stop();

	// Small on purpose: about a sixth of a second. The ring is refilled every
	// frame, so there is no need for more - and when the cursor moves on, what
	// is already buffered has to play out before the next game can be heard.
	// A larger ring would carry the last game into the new one.
	hui::audio::StreamRing ring_{1u << 13};
	// The decoded loop, stereo at the mixer's rate. One at a time: the shelf
	// only ever plays what is under the cursor.
	std::vector<float> pcm_;
	std::size_t at_ = 0;

	std::size_t playing_ = (std::size_t)-1;
	std::size_t pending_ = (std::size_t)-1;
	float rest_ = 0.0f;
	bool attached_ = false;
};

GameSound &GameSoundPlayer();

}  // namespace psp5
