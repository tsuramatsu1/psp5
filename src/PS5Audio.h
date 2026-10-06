// psp5 - PPSSPP's audio on the console's output.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Common/Audio/AudioBackend.h"

class PS5AudioBackend : public AudioBackend {
public:
	~PS5AudioBackend() override { Shutdown(); }

	void EnumerateDevices(std::vector<AudioDeviceDesc> *outputDevices, bool captureDevices = false) override;
	void SetRenderCallback(RenderCallback callback, void *userdata) override;
	bool InitOutputDevice(std::string_view uniqueId, LatencyMode latencyMode, bool *revertedToDefault) override;
	int SampleRate() const override;
	int BufferSize() const override;
	int PeriodFrames() const override;
	std::string GetCurrentDeviceName() const override { return "PlayStation 5"; }
	std::string GetErrorString() const override { return error_; }

	void Shutdown();

private:
	static void FillTrampoline(int16_t *frames, int count, void *user);
	void Fill(int16_t *frames, int count);

	RenderCallback callback_ = nullptr;
	void *userdata_ = nullptr;
	std::vector<float> mix_;
	std::string error_;
	bool started_ = false;
};
