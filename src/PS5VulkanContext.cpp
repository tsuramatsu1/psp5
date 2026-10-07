// psp5 - PPSSPP's GraphicsContext on the console's display.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PS5VulkanContext.h"

#include "Common/GPU/Vulkan/VulkanDebug.h"
#include "Common/GPU/Vulkan/VulkanRenderManager.h"
#include "Common/GPU/thin3d_create.h"
#include "Common/System/Display.h"
#include "Common/System/NativeApp.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "GPU/Vulkan/VulkanUtil.h"

#include "platform/platform.h"

bool PS5VulkanContext::Init(std::string *errorMessage) {
	return InitDevice(errorMessage) && InitDraw(errorMessage);
}

bool PS5VulkanContext::InitDevice(std::string *errorMessage) {
	init_glslang();

	// The console has no message box and no debugger attached to break into, and a
	// validation break would take the title down in front of the user.
	g_LogOptions.breakOnError = false;
	g_LogOptions.breakOnWarning = false;
	g_LogOptions.msgBoxOnError = false;

	std::string loadError;
	if (!VulkanLoad(&loadError)) {
		*errorMessage = "Failed to reach the RADV driver: " + loadError;
		return false;
	}

	vulkan_ = new VulkanContext();

	VulkanContext::CreateInfo info{};
	InitVulkanCreateInfoFromConfig(&info);
	if (vulkan_->CreateInstance(info) != VK_SUCCESS) {
		*errorMessage = vulkan_->InitError();
		delete vulkan_;
		vulkan_ = nullptr;
		return false;
	}

	// One GPU, so the configured device name is not consulted.
	int deviceNum = vulkan_->GetBestPhysicalDevice();
	if (deviceNum < 0 || vulkan_->CreateDevice(deviceNum) != VK_SUCCESS) {
		*errorMessage = vulkan_->InitError();
		delete vulkan_;
		vulkan_ = nullptr;
		return false;
	}
	say("vulkan: %s", vulkan_->GetPhysicalDeviceProperties(deviceNum).properties.deviceName);

	// VK_KHR_display: no window system, no surface extent to ask a window for.
	// VulkanContext reads the extent from the display mode it selects, and
	// SetCbGetDrawSize is what it falls back on when a resize is signalled, so it
	// reports the swapchain it already has.
	vulkan_->SetCbGetDrawSize([this]() {
		return VkExtent2D{(uint32_t)vulkan_->GetBackbufferWidth(),
		                  (uint32_t)vulkan_->GetBackbufferHeight()};
	});

	if (vulkan_->InitSurface(WINDOWSYSTEM_DISPLAY, nullptr, nullptr) != VK_SUCCESS) {
		*errorMessage = vulkan_->InitError();
		Shutdown();
		return false;
	}

	// InitSurface has just adopted the display's own mode and written its size into
	// g_display (the standalone patch does this; nothing else would have, because a
	// title is never told a resolution). That sets the pixel size only, so derive
	// the rest - the dp size and the DPI scales the UI lays itself out against -
	// before any of PPSSPP's UI is built. Without this the UI measures itself
	// against a zero-sized display.
	Native_UpdateScreenScale(g_display.pixel_xres, g_display.pixel_yres, 1.0f);

	// The swapchain, here rather than in InitDraw, so the Aurora launcher has
	// something to present to: it runs between the two halves, and a null
	// swapchain took RADV's wsi_GetSwapchainImagesKHR straight through a null
	// pointer on the first console run.
	//
	// FIFO because it is the only mode the console offers (klog: "Supported
	// present modes: FIFO"), which is what ConfigPresentModeToVulkan settles on
	// too - so InitDraw's own call is a rebuild with the same mode, not a change.
	if (!vulkan_->InitSwapchain(VK_PRESENT_MODE_FIFO_KHR)) {
		*errorMessage = vulkan_->InitError();
		Shutdown();
		return false;
	}

	return true;
}

bool PS5VulkanContext::InitDraw(std::string *errorMessage) {
	bool useMultiThreading = g_Config.bRenderMultiThreading;
	if (g_Config.iInflightFrames == 1) {
		useMultiThreading = false;
	}
	draw_ = Draw::T3DCreateVulkanContext(vulkan_, useMultiThreading);

	// The launcher presented to the swapchain InitDevice made. Replace it now that
	// draw_ can say which present mode the configuration wants.
	vulkan_->DestroySwapchain();
	if (!vulkan_->InitSwapchain(ConfigPresentModeToVulkan(draw_))) {
		*errorMessage = vulkan_->InitError();
		Shutdown();
		return false;
	}

	SetGPUBackend(GPUBackend::VULKAN);
	if (!draw_->CreatePresets()) {
		*errorMessage = "Failed to create the shader presets";
		Shutdown();
		return false;
	}
	draw_->HandleEvent(Draw::Event::GOT_BACKBUFFER, vulkan_->GetBackbufferWidth(),
	                   vulkan_->GetBackbufferHeight());

	renderManager_ = (VulkanRenderManager *)draw_->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);
	renderManager_->SetInflightFrames(g_Config.iInflightFrames);

	say("display: %dx%d", vulkan_->GetBackbufferWidth(), vulkan_->GetBackbufferHeight());
	return true;
}

void PS5VulkanContext::Shutdown() {
	if (!vulkan_) {
		return;
	}
	if (draw_) {
		draw_->HandleEvent(Draw::Event::LOST_BACKBUFFER, vulkan_->GetBackbufferWidth(),
		                   vulkan_->GetBackbufferHeight());
	}
	delete draw_;
	draw_ = nullptr;
	renderManager_ = nullptr;

	vulkan_->WaitUntilQueueIdle();
	vulkan_->DestroySwapchain();
	vulkan_->DestroySurface();
	vulkan_->DestroyDevice();
	vulkan_->DestroyInstance();
	delete vulkan_;
	vulkan_ = nullptr;
	finalize_glslang();
}

void PS5VulkanContext::Resize() {
	// The display mode does not change under a title, so this is only reached when
	// the render manager asks for a swapchain rebuild (a lost or out-of-date one).
	draw_->HandleEvent(Draw::Event::LOST_BACKBUFFER, vulkan_->GetBackbufferWidth(),
	                   vulkan_->GetBackbufferHeight());
	vulkan_->DestroySwapchain();
	vulkan_->InitSwapchain(ConfigPresentModeToVulkan(draw_));
	draw_->HandleEvent(Draw::Event::GOT_BACKBUFFER, vulkan_->GetBackbufferWidth(),
	                   vulkan_->GetBackbufferHeight());
}

void PS5VulkanContext::Poll() {
	if (vulkan_ && vulkan_->GetSwapchain() && renderManager_->NeedsSwapchainRecreate()) {
		Resize();
	}
}
