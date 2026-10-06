// psp5 - PPSSPP's audio on the console's output.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The platform layer owns one 48 kHz stereo output and a thread of its own that
// asks for 256 frames at a time and blocks while the console plays them, so the
// fill paces itself. PPSSPP renders float samples, the console takes interleaved
// 16-bit, so the only work here is the conversion.

#include "PS5Audio.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "platform/platform.h"

namespace {

constexpr int kSampleRate = 48000;
// What the platform layer asks for in one call. Kept as the reported period so
// PPSSPP's latency estimate matches what actually happens.
constexpr int kPeriodFrames = 256;

}  // namespace

void PS5AudioBackend::EnumerateDevices(std::vector<AudioDeviceDesc> *outputDevices, bool captureDevices) {
	if (captureDevices) {
		return;  // No recording.
	}
	// The console picks the output (TV, headset); a title is given one and cannot
	// choose, so a single entry stands for it.
	outputDevices->push_back(AudioDeviceDesc{"PlayStation 5", ""});
}

void PS5AudioBackend::SetRenderCallback(RenderCallback callback, void *userdata) {
	callback_ = callback;
	userdata_ = userdata;
}

bool PS5AudioBackend::InitOutputDevice(std::string_view uniqueId, LatencyMode latencyMode,
                                       bool *revertedToDefault) {
	if (revertedToDefault) {
		*revertedToDefault = false;
	}
	if (started_) {
		return true;
	}
	mix_.resize((size_t)kPeriodFrames * 2);
	started_ = audio_start(&PS5AudioBackend::FillTrampoline, this);
	if (!started_) {
		error_ = "no audio output";
	}
	return started_;
}

void PS5AudioBackend::Shutdown() {
	if (started_) {
		// Stops the thread before the buffers the fill uses go away.
		audio_stop();
		started_ = false;
	}
}

int PS5AudioBackend::SampleRate() const { return kSampleRate; }
int PS5AudioBackend::BufferSize() const { return kPeriodFrames; }
int PS5AudioBackend::PeriodFrames() const { return kPeriodFrames; }

void PS5AudioBackend::FillTrampoline(int16_t *frames, int count, void *user) {
	static_cast<PS5AudioBackend *>(user)->Fill(frames, count);
}

void PS5AudioBackend::Fill(int16_t *frames, int count) {
	const size_t samples = (size_t)count * 2;
	if (!callback_) {
		std::fill_n(frames, samples, (int16_t)0);
		return;
	}
	if (mix_.size() < samples) {
		mix_.resize(samples);
	}
	std::fill_n(mix_.data(), samples, 0.0f);
	callback_(mix_.data(), count, kSampleRate, userdata_);

	for (size_t i = 0; i < samples; i++) {
		// PPSSPP renders in -1..1 but does not promise to stay inside it, so the
		// conversion clamps rather than wrapping into noise.
		float value = std::clamp(mix_[i], -1.0f, 1.0f);
		frames[i] = (int16_t)std::lrintf(value * 32767.0f);
	}
}
