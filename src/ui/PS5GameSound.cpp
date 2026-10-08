// psp5 - the music a game plays while it is under the cursor.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The decoding is PPSSPP's: PS5_DecodeAtrac is a small function the standalone
// patch adds to UI/BackgroundAudio.cpp, where the RIFF parsing and the ATRAC
// decoder set-up already live. PPSSPP plays this file itself when a game is
// highlighted in its game grid, but only through its own audio path, and on
// psp5's home screen that is not running - the kit's mixer owns the device
// until a game starts.

#include "ui/PS5GameSound.h"

#include <algorithm>
#include <cstdlib>

#include "PS5Log.h"
#include "ui/PS5GameLibrary.h"

// Defined by PPSSPP (patches/ppsspp/ps5-standalone.patch).
extern "C" bool PS5_DecodeAtrac(const char *bytes, int size, short **out, int *outFrames,
                                int *outRate);

namespace psp5 {
namespace {

// How long the cursor has to rest before the sound starts. Long enough that
// running along a shelf is silent, short enough that stopping on a game feels
// like it answered.
constexpr float kRestSeconds = 0.45f;

// The music bus, under the interface sounds rather than over them.
constexpr float kGain = 0.55f;
constexpr float kFadeSeconds = 0.35f;

}  // namespace

GameSound &GameSoundPlayer() {
	static GameSound sound;
	return sound;
}

void GameSound::Attach(hui::audio::Mixer &mixer) {
	mixer.attach_stream(0, &ring_);
	mixer.set_stream_gain(0, 0.0f, 0.0f);
	attached_ = true;
}

void GameSound::Detach(hui::audio::Mixer &mixer) {
	if (!attached_) {
		return;
	}
	// The audio thread reads the ring until the stream is let go, and the ring
	// is a member of this.
	mixer.set_stream_gain(0, 0.0f, 0.0f);
	mixer.attach_stream(0, nullptr);
	attached_ = false;
	Stop();
}

void GameSound::Focus(std::size_t index) {
	if (index == pending_) {
		return;
	}
	pending_ = index;
	rest_ = 0.0f;
}

void GameSound::Stop() {
	// The ring is not emptied here: draining it is the audio thread's to do,
	// and this runs on the frame loop. Writing stops instead, and what is left
	// plays out under a gain that is already on its way down.
	pcm_.clear();
	at_ = 0;
	playing_ = (std::size_t)-1;
}

void GameSound::Start(std::size_t index) {
	Stop();
	playing_ = index;

	const std::string &bytes = Library().sound(index);
	if (bytes.empty()) {
		return;
	}

	short *pcm = nullptr;
	int frames = 0;
	int rate = 0;
	if (!PS5_DecodeAtrac(bytes.data(), (int)bytes.size(), &pcm, &frames, &rate) || frames <= 0) {
		psp5::Trace("sound: %s has no ATRAC psp5 can decode",
		            Library().entry(index).title.c_str());
		return;
	}

	// To the mixer's rate, which is not the disc's: SND0 is 44.1 kHz and the
	// console's output is 48. Linear between neighbours - this is a menu jingle
	// under a shelf, not the game's own audio.
	const double step = (double)rate / (double)hui::audio::kSampleRate;
	const std::size_t out = (std::size_t)((double)frames / step);
	pcm_.resize(out * 2);
	for (std::size_t i = 0; i < out; ++i) {
		const double source = (double)i * step;
		const std::size_t a = (std::size_t)source;
		const std::size_t b = std::min(a + 1, (std::size_t)frames - 1);
		const float t = (float)(source - (double)a);
		for (int channel = 0; channel < 2; ++channel) {
			const float first = pcm[a * 2 + (std::size_t)channel] / 32768.0f;
			const float second = pcm[b * 2 + (std::size_t)channel] / 32768.0f;
			pcm_[i * 2 + (std::size_t)channel] = first + (second - first) * t;
		}
	}
	free(pcm);

	psp5::Trace("sound: %s, %d frames at %d Hz", Library().entry(index).title.c_str(), frames,
	            rate);
}

void GameSound::Update(hui::audio::Mixer &mixer, float dt) {
	if (!attached_) {
		return;
	}

	if (pending_ != playing_) {
		rest_ += dt;
		if (rest_ >= kRestSeconds) {
			rest_ = 0.0f;
			Start(pending_);
			mixer.set_stream_gain(0, pcm_.empty() ? 0.0f : kGain, kFadeSeconds);
		} else if (playing_ != (std::size_t)-1) {
			// On the way out before the next one is even chosen, so the two
			// never overlap.
			mixer.set_stream_gain(0, 0.0f, kFadeSeconds);
		}
	}

	if (pcm_.empty()) {
		return;
	}

	// Keep the ring fed, looping as the PSP does. Writing only what fits means
	// this never blocks and never runs long.
	const std::size_t frames = pcm_.size() / 2;
	while (ring_.space() > 0) {
		const std::size_t run = std::min(ring_.space(), frames - at_);
		if (run == 0) {
			at_ = 0;
			continue;
		}
		ring_.write(&pcm_[at_ * 2], run);
		at_ += run;
		if (at_ >= frames) {
			at_ = 0;
		}
	}
}

}  // namespace psp5
