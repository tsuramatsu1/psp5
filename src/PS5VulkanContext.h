// psp5 - PPSSPP's GraphicsContext on the console's display.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "Common/GraphicsContext.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/GPU/thin3d.h"

class VulkanRenderManager;

// There is no window system on the console: the title presents straight to the
// display through VK_KHR_display, which PPSSPP already supports as
// WINDOWSYSTEM_DISPLAY (it is the path the KMSDRM build takes). So this context is
// the SDL Vulkan one with the window removed.
class PS5VulkanContext : public GraphicsContext {
public:
	~PS5VulkanContext() override { delete draw_; }

	bool Init(std::string *errorMessage);

	void Shutdown() override;
	void Resize() override;
	void Poll() override;

	void *GetAPIContext() override { return vulkan_; }
	Draw::DrawContext *GetDrawContext() override { return draw_; }

	int Width() const { return vulkan_ ? vulkan_->GetBackbufferWidth() : 0; }
	int Height() const { return vulkan_ ? vulkan_->GetBackbufferHeight() : 0; }

private:
	Draw::DrawContext *draw_ = nullptr;
	VulkanContext *vulkan_ = nullptr;
	VulkanRenderManager *renderManager_ = nullptr;
};
