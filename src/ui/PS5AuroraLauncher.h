// psp5 - the Aurora Shelf home screen.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace psp5 {

// Closing the title. OPTIONS opens the home screen's settings, so there is no
// gesture left to hold for this - it is a row at the foot of that panel, which
// is also where someone would look for it.
void RequestQuit();
bool QuitRequested();

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

// The game the player chose, or empty if they left the home screen without
// choosing one. The home screen calls the first; main() reads the second once
// RunAuroraLauncher has returned, and boots what it names.
//
// A value rather than a callback because the two sides never run at once: the
// launcher owns the device while it is up, PPSSPP owns it afterwards, and the
// only thing that has to cross between them is a path.
void RequestLaunch(const std::string &path, const std::string &discId);
const std::string &PendingLaunch();
// The chosen game's PARAM.SFO DISC_ID, which names its cheat file. Empty when
// the game carries none, in which case it has no cheats to show either.
const std::string &PendingLaunchDiscId();
void ClearPendingLaunch();

}  // namespace psp5

// Called by PPSSPP, from the one constructor of the game browser it would
// otherwise have shown. psp5 has its own home screen, so this is simply the
// signal that the game has ended; main() leaves the frame loop before PPSSPP
// draws that screen, and the shelf comes back instead.
//
// extern "C", because the call sites are inside PPSSPP's own translation units.
extern "C" void PS5_NotifyGameEnded();

// Defined by PPSSPP (see patches/ppsspp/ps5-standalone.patch): hands it the game
// to run, as an EmuScreen, without any of its own screens being drawn first.
extern "C" void PS5_BootGame(const char *path);

namespace psp5 {

// Whether PPSSPP has left the game since the flag was last cleared.
bool GameEnded();
void ClearGameEnded();

}  // namespace psp5
