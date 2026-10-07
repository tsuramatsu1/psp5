// psp5 - the Aurora Shelf home screen.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace psp5 {

// The device the title already created, as plain handles.
//
// Deliberately no Vulkan headers here. PPSSPP keeps its entry points in namespace
// PPSSPP_VK and the kit calls the global ones through volk; a translation unit
// that sees both finds every vkXxx ambiguous. Passing the handles across as
// integers keeps those two worlds in separate files, which is also the honest
// description of what they are: the launcher borrows a device, nothing more.
struct AuroraDevice {
	std::uint64_t instance = 0;
	std::uint64_t physicalDevice = 0;
	std::uint64_t device = 0;
	std::uint64_t queue = 0;
	std::uint64_t swapchain = 0;
	std::uint32_t queueFamily = 0;
	std::uint32_t swapchainFormat = 0;  // VkFormat
	int width = 0;
	int height = 0;
};

// Runs PS5_VKHomebrewUI's Aurora Shelf design as psp5's home screen and returns
// when the player leaves it.
//
// It drives its own frame loop - acquire, record, submit, present - because the
// kit's renderer and PPSSPP's render manager each assume they own one. So the
// launcher runs first, with PPSSPP's draw context not yet created, and every
// Vulkan object it makes is destroyed before it returns; PPSSPP then has the
// device to itself.
//
// Returns false if it could not start (no fonts, no render pass), in which case
// the title should go on to PPSSPP's own interface rather than show nothing.
bool RunAuroraLauncher(const AuroraDevice &gpu);

}  // namespace psp5
