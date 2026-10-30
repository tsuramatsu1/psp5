// psp5 - frame generation, as an experiment.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PS5_FrameGen (tools/setup-framegen.sh, GPL-3.0-or-later) makes a frame
// between two the emulator has drawn, from their colour alone, with a small
// network as Vulkan compute passes. FG_METHOD_NEURAL is the only method psp5
// can use: the others want motion vectors, and a PSP game has none - PPSSPP's
// depth buffer is the emulated console's and does not describe the picture on
// screen.
//
// Why there is no Vulkan in this header. PPSSPP keeps its own entry points in
// namespace PPSSPP_VK; psp5's launcher and the UI kit reach Vulkan through
// volk. A translation unit that sees both finds every vkXxx ambiguous, and the
// generator's own header asks for volk's. So the generator and everything that
// touches it are compiled on the volk side (psp5_ui), and this is the line
// between: handles cross it as void *, formats as the uint32_t they already
// are. The cost is a cast at each end; the alternative was a header no caller
// could include.
#pragma once

#include <cstdint>

namespace psp5 {
namespace framegen {

// Whether a generator was compiled into this title at all. False in a build
// made with PSP5_NO_FRAMEGEN, or on a machine that could not fetch it.
bool BuiltIn();

// ... and whether it started and has its weights. Everything below is a no-op
// until this is true.
bool Ready();

// Why it is not ready, for the settings row and the log. Empty when it is.
const char *Problem();

// ---- the player's switch --------------------------------------------------
//
// Off unless asked for, and described as an experiment wherever it appears.
// Nothing about this has been shown to improve a game on this console; what
// can be said is that it runs, and what it costs.
bool Enabled();
void setEnabled(bool on);

// ---- the readout ----------------------------------------------------------
//
// This exists before the feature does, on purpose. The last attempt at frame
// generation was removed because nothing could show whether it was doing
// anything: PPSSPP's frame rate readout reports the emulated vblank rate,
// which is about 60 for any game running at full speed whether or not a
// generator is running, so it answers the same way in both cases.
//
// Counting the two kinds of present apart is the only thing that can tell
// them apart, so it is built first and is useful on its own.
struct Readout {
	std::uint64_t real = 0;      // frames the emulator drew and presented
	std::uint64_t generated = 0; // frames the generator made and presented
	float realPerSecond = 0.0f;
	float generatedPerSecond = 0.0f;
	// What the generator's passes cost, averaged over the last second. The
	// honest figure to judge it by: below about 8 ms it is buying frames,
	// above that it is spending more than the frame it saves.
	float millisecondsPerFrame = 0.0f;
};
Readout Counts();

// Counted once per present, on the thread that presents.
//
// The per-second figures roll on their own clock rather than on a frame time
// handed in: the only caller that knows when a real frame went out is deep in
// the emulator, where there is no frame time to pass and no reason to invent
// a second call just to carry one.
void NoteReal();
// A frame was generated. Counted here, where it is known to have happened.
void NoteGenerated();
// ... and what it cost, which arrives later and separately: the GPU writes
// the timestamps and they are read a few frames on, when that slot comes
// round and its fence has been waited. Counting the frame only once its
// timing could be read conflated two different things, and a reader that was
// never ready made a generator that was running look like one that was not.
void NoteCost(float milliseconds);

// ---- lifecycle ------------------------------------------------------------
//
// Started once the device and the swapchain are known, and stopped after the
// render manager is destroyed - never before. PPSSPP joins its render thread
// when the render manager goes, so tearing the generator down first is a race
// against a thread that is still presenting.
bool Start(void *device, void *physicalDevice, std::uint32_t width, std::uint32_t height,
           std::uint32_t slots, std::uint32_t outputFormat);
void Stop();

// Whether to make a frame after the one just drawn.
//
// `gameFps` is the rate the game is really drawing at - PPSSPP's flip count,
// not its vblank rate. Generating for a game already at the display's rate
// buys nothing and costs a pass, so there is a ceiling; generating for a game
// that is crawling interpolates between two frames too far apart to be
// related, so there is a floor as well.
bool WantFrame(float gameFps);

// Records the generator's passes into a command buffer the caller owns.
//
// The caller owns every image, every barrier and the submission: before this,
// the two colour views are in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL and the
// output is in VK_IMAGE_LAYOUT_GENERAL, with their writes visible to the
// compute stage; after it, the caller makes the output's writes visible to
// whatever reads it. `slot` is the frame-in-flight index whose fence the
// caller waited on - a slot must not be reused while the command buffer that
// last used it is still running.
//
// `t` is where the generated frame sits between the two, 0 < t < 1.
bool Record(void *commandBuffer, std::uint32_t slot, void *prevColorView, void *nextColorView,
            void *outputView, float t);

}  // namespace framegen
}  // namespace psp5
