// psp5 - frame generation, as an experiment.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// See PS5FrameGen.h for why no Vulkan type appears in the header. This file is
// the volk side of that line, compiled into psp5_ui with the generator itself.

#include "ui/PS5FrameGen.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "PS5Log.h"
#include "PS5Paths.h"
#include "ui/PS5Prefs.h"

#ifdef PSP5_FRAMEGEN
#include <framegen.h>
#endif

namespace psp5 {
namespace framegen {
namespace {

// The window the generator is worth running in, in frames a second of the
// game's own drawing.
//
// Above the ceiling there is nothing to buy: a game already presenting at the
// display's rate gains no smoothness from a frame inserted between two that
// are 16 ms apart, and the pass is pure cost. Below the floor the two frames
// are far enough apart - a third of a second at 3 fps - that whatever is
// between them is invention rather than interpolation, and the network was
// trained on neighbours, not strangers.
constexpr float kCeilingFps = 50.0f;
constexpr float kFloorFps = 12.0f;

// One generated frame for each real one, placed halfway. The generator takes
// any t in (0, 1); halfway is the only one that makes sense for a single
// inserted frame, and more than one would need the real frame held back
// further still.
constexpr float kMidpoint = 0.5f;

// Rolled once a second, so the readout is a rate and not a total that only
// ever climbs.
constexpr float kWindowSeconds = 1.0f;

struct State {
	bool started = false;
	std::string problem;

	// The readout. Written on the thread that presents and read on the frame
	// loop; both are the same thread here, and a torn count would be a wrong
	// number on a diagnostic rather than a fault.
	std::uint64_t real = 0;
	std::uint64_t generated = 0;
	std::uint64_t realAtWindow = 0;
	std::uint64_t generatedAtWindow = 0;
	float realPerSecond = 0.0f;
	float generatedPerSecond = 0.0f;
	std::chrono::steady_clock::time_point windowOpened{};
	float millisecondsSum = 0.0f;
	std::uint32_t millisecondsCount = 0;
	float millisecondsPerFrame = 0.0f;

#ifdef PSP5_FRAMEGEN
	fg_context *context = nullptr;

	// The measuring run owns three images at the swapchain's size: the last two
	// finished pictures, and somewhere for the generated one to go. The last is
	// written and never read - this run changes nothing on screen. It exists so
	// the passes do the work they would really do, and so their cost is the cost
	// they would really have.
	VkDevice device = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkImage history[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
	VkDeviceMemory historyMemory[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
	VkImageView historyView[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
	VkImage output = VK_NULL_HANDLE;
	VkDeviceMemory outputMemory = VK_NULL_HANDLE;
	VkImageView outputView = VK_NULL_HANDLE;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t slots = 1;
	// Which half of `history` the next picture goes into, and how many have been
	// captured at all - the first frame has no partner to be generated against.
	std::uint32_t next = 0;
	std::uint64_t captured = 0;

	// Timestamps, two per slot. Written by the GPU, read a few frames later when
	// that slot comes round again and its work is known to be done.
	VkQueryPool timestamps = VK_NULL_HANDLE;
	float nanosecondsPerTick = 1.0f;
	std::vector<bool> timed;
	bool saidGenerating = false;
	bool saidNoTiming = false;
	bool saidEntered = false;
	bool saidRefused = false;
#endif
};

State &state() {
	static State it;
	return it;
}

#ifdef PSP5_FRAMEGEN
// The weights, which are the one part of the generator that is not compiled
// into the title. tools/link-title.sh stages them beside it.
std::string NetworkPath() {
	return std::string(PS5Paths::kRoot) + "/framegen/models/network.fgw";
}

// A memory type that will do for `requirements` with these properties, or
// UINT32_MAX. The console has the usual heaps; nothing clever is wanted here.
std::uint32_t MemoryTypeFor(VkPhysicalDevice physical, std::uint32_t bits,
                            VkMemoryPropertyFlags want) {
	VkPhysicalDeviceMemoryProperties memory{};
	vkGetPhysicalDeviceMemoryProperties(physical, &memory);
	for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
		if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	return UINT32_MAX;
}

bool MakeImage(State &it, VkImageUsageFlags usage, VkImage *image, VkDeviceMemory *memory,
               VkImageView *view) {
	VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	info.imageType = VK_IMAGE_TYPE_2D;
	// The format the generator's shaders read and write. The swapchain's may
	// differ; vkCmdBlitImage converts on the way in, which is the whole reason
	// the capture is a blit and not a copy.
	info.format = VK_FORMAT_R8G8B8A8_UNORM;
	info.extent = {it.width, it.height, 1};
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(it.device, &info, nullptr, image) != VK_SUCCESS) {
		return false;
	}

	VkMemoryRequirements need{};
	vkGetImageMemoryRequirements(it.device, *image, &need);
	const std::uint32_t type =
	    MemoryTypeFor(it.physical, need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type == UINT32_MAX) {
		return false;
	}
	VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	allocate.allocationSize = need.size;
	allocate.memoryTypeIndex = type;
	if (vkAllocateMemory(it.device, &allocate, nullptr, memory) != VK_SUCCESS) {
		return false;
	}
	if (vkBindImageMemory(it.device, *image, *memory, 0) != VK_SUCCESS) {
		return false;
	}

	VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	viewInfo.image = *image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = info.format;
	viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	return vkCreateImageView(it.device, &viewInfo, nullptr, view) == VK_SUCCESS;
}

// One image barrier, written out rather than wrapped: there are six of them in
// this file and each one is three lines of what it actually means.
void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
             VkAccessFlags before, VkAccessFlags after, VkPipelineStageFlags fromStage,
             VkPipelineStageFlags toStage) {
	VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	barrier.oldLayout = from;
	barrier.newLayout = to;
	barrier.srcAccessMask = before;
	barrier.dstAccessMask = after;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, fromStage, toStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool ReadNetwork(std::vector<char> *blob, std::string *problem) {
	const std::string path = NetworkPath();
	FILE *fh = fopen(path.c_str(), "rb");
	if (!fh) {
		*problem = "no network beside the title";
		psp5::Trace("framegen: cannot open %s", path.c_str());
		return false;
	}
	fseek(fh, 0, SEEK_END);
	const long size = ftell(fh);
	fseek(fh, 0, SEEK_SET);
	if (size <= 0) {
		fclose(fh);
		*problem = "the network is empty";
		return false;
	}
	blob->resize(static_cast<std::size_t>(size));
	const std::size_t got = fread(blob->data(), 1, blob->size(), fh);
	fclose(fh);
	if (got != blob->size()) {
		*problem = "the network is short";
		psp5::Trace("framegen: read %zu of %zu bytes from %s", got, blob->size(), path.c_str());
		return false;
	}
	return true;
}
#endif

}  // namespace

bool BuiltIn() {
#ifdef PSP5_FRAMEGEN
	return true;
#else
	return false;
#endif
}

bool Ready() {
#ifdef PSP5_FRAMEGEN
	return state().started && state().context != nullptr;
#else
	return false;
#endif
}

const char *Problem() {
#ifdef PSP5_FRAMEGEN
	return state().problem.c_str();
#else
	return "not built into this title";
#endif
}

bool Enabled() {
	return prefs::frameGen();
}

void setEnabled(bool on) {
	prefs::setFrameGen(on);
}

Readout Counts() {
	const State &it = state();
	Readout out;
	out.real = it.real;
	out.generated = it.generated;
	out.realPerSecond = it.realPerSecond;
	out.generatedPerSecond = it.generatedPerSecond;
	out.millisecondsPerFrame = it.millisecondsPerFrame;
	return out;
}

// Rolls the per-second figures when a second has gone by. Called from both
// counters so the window advances whichever kind of frame is arriving - a
// generator that stopped producing should show zero, not its last rate.
void Roll() {
	State &it = state();
	const auto now = std::chrono::steady_clock::now();
	if (it.windowOpened.time_since_epoch().count() == 0) {
		it.windowOpened = now;
		return;
	}
	const float elapsed =
	    std::chrono::duration<float>(now - it.windowOpened).count();
	if (elapsed < kWindowSeconds) {
		return;
	}
	it.realPerSecond = static_cast<float>(it.real - it.realAtWindow) / elapsed;
	it.generatedPerSecond =
	    static_cast<float>(it.generated - it.generatedAtWindow) / elapsed;
	it.millisecondsPerFrame =
	    it.millisecondsCount
	        ? it.millisecondsSum / static_cast<float>(it.millisecondsCount)
	        : 0.0f;
	// Into the log as well as the settings row. The generator only runs while
	// a game is up, and the settings panel is on the home screen, which is
	// after it has stopped - so the row shows a frozen last reading and the
	// only live view of this is here. One line a second, and only while
	// frames are actually being made.
	if (it.generated != it.generatedAtWindow) {
		psp5::Trace("framegen: %.0f drawn + %.0f made a second, %.2f ms each",
		            it.realPerSecond, it.generatedPerSecond, it.millisecondsPerFrame);
	}
	it.realAtWindow = it.real;
	it.generatedAtWindow = it.generated;
	it.millisecondsSum = 0.0f;
	it.millisecondsCount = 0;
	it.windowOpened = now;
}

void NoteReal() {
	++state().real;
	Roll();
}

void NoteGenerated() {
	++state().generated;
	Roll();
}

void NoteCost(float milliseconds) {
	State &it = state();
	it.millisecondsSum += milliseconds;
	++it.millisecondsCount;
}

#ifdef PSP5_FRAMEGEN

bool Start(void *device, void *physicalDevice, std::uint32_t width, std::uint32_t height,
           std::uint32_t slots, std::uint32_t outputFormat) {
	State &it = state();
	if (it.started) {
		return it.context != nullptr;
	}
	it.started = true;
	it.problem.clear();
	it.device = static_cast<VkDevice>(device);
	it.physical = static_cast<VkPhysicalDevice>(physicalDevice);
	it.width = width;
	it.height = height;
	it.slots = slots ? slots : 1;

	fg_create_info info{};
	info.device = static_cast<VkDevice>(device);
	info.physical_device = static_cast<VkPhysicalDevice>(physicalDevice);
	info.width = width;
	info.height = height;
	info.slots = slots;
	info.output_format = static_cast<VkFormat>(outputFormat);
	// source_width/source_height are the generator's pixel-art path, which
	// wants the frames to be a whole-number enlargement of a smaller image.
	// PPSSPP's output is not: it is scaled and filtered by a render resolution
	// the player chooses, so the relationship is not a whole number and
	// claiming it is would make the motion search look for blocks that are not
	// there. Left at zero, which is "not pixel art".
	info.source_width = 0;
	info.source_height = 0;

	if (const char *wrong = fg_check_create_info(&info)) {
		it.problem = wrong;
		psp5::Trace("framegen: %s", wrong);
		return false;
	}

	char error[256] = {};
	if (fg_context_create(&info, &it.context, error, sizeof(error)) != VK_SUCCESS) {
		it.problem = error[0] ? error : "the context could not be made";
		it.context = nullptr;
		psp5::Trace("framegen: %s", it.problem.c_str());
		return false;
	}

	std::vector<char> blob;
	if (!ReadNetwork(&blob, &it.problem)) {
		fg_context_destroy(it.context);
		it.context = nullptr;
		return false;
	}
	if (fg_context_load_network(it.context, blob.data(), blob.size(), error, sizeof(error)) !=
	    VK_SUCCESS) {
		it.problem = error[0] ? error : "the network would not load";
		psp5::Trace("framegen: %s", it.problem.c_str());
		fg_context_destroy(it.context);
		it.context = nullptr;
		return false;
	}
	if (!fg_method_available(it.context, FG_METHOD_NEURAL)) {
		it.problem = "the neural method is not available";
		psp5::Trace("framegen: %s", it.problem.c_str());
		fg_context_destroy(it.context);
		it.context = nullptr;
		return false;
	}

	// The measuring run's images. A failure here is not fatal: the generator is
	// still loaded and the readout still counts real frames, which is most of
	// what it is for.
	const bool images =
	    MakeImage(it, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &it.history[0],
	              &it.historyMemory[0], &it.historyView[0]) &&
	    MakeImage(it, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &it.history[1],
	              &it.historyMemory[1], &it.historyView[1]) &&
	    MakeImage(it, VK_IMAGE_USAGE_STORAGE_BIT, &it.output, &it.outputMemory, &it.outputView);
	if (!images) {
		psp5::Trace("framegen: the measuring images could not be made; counting only");
	} else {
		VkQueryPoolCreateInfo pool{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
		pool.queryType = VK_QUERY_TYPE_TIMESTAMP;
		// Two a slot: one before the generator's passes and one after.
		pool.queryCount = it.slots * 2;
		if (vkCreateQueryPool(it.device, &pool, nullptr, &it.timestamps) == VK_SUCCESS) {
			VkPhysicalDeviceProperties properties{};
			vkGetPhysicalDeviceProperties(it.physical, &properties);
			// How long a tick of the GPU's timestamp counter is. Zero means the
			// queue does not support timestamps at all, and then there is nothing
			// to measure with.
			it.nanosecondsPerTick = properties.limits.timestampPeriod;
			if (it.nanosecondsPerTick <= 0.0f) {
				vkDestroyQueryPool(it.device, it.timestamps, nullptr);
				it.timestamps = VK_NULL_HANDLE;
				psp5::Trace("framegen: this queue has no timestamps; the cost cannot be measured");
			} else {
				it.timed.assign(it.slots, false);
			}
		}
	}

	psp5::Trace("framegen: ready at %ux%u, %u slot(s), %zu bytes of network", width, height, slots,
	            blob.size());
	return true;
}

void Stop() {
	State &it = state();
	// The context first: it holds descriptor sets pointing at the views below.
	if (it.context) {
		fg_context_destroy(it.context);
		it.context = nullptr;
	}
	if (it.device) {
		if (it.timestamps) {
			vkDestroyQueryPool(it.device, it.timestamps, nullptr);
			it.timestamps = VK_NULL_HANDLE;
		}
		for (int i = 0; i < 2; ++i) {
			if (it.historyView[i]) {
				vkDestroyImageView(it.device, it.historyView[i], nullptr);
			}
			if (it.history[i]) {
				vkDestroyImage(it.device, it.history[i], nullptr);
			}
			if (it.historyMemory[i]) {
				vkFreeMemory(it.device, it.historyMemory[i], nullptr);
			}
			it.historyView[i] = VK_NULL_HANDLE;
			it.history[i] = VK_NULL_HANDLE;
			it.historyMemory[i] = VK_NULL_HANDLE;
		}
		if (it.outputView) {
			vkDestroyImageView(it.device, it.outputView, nullptr);
			it.outputView = VK_NULL_HANDLE;
		}
		if (it.output) {
			vkDestroyImage(it.device, it.output, nullptr);
			it.output = VK_NULL_HANDLE;
		}
		if (it.outputMemory) {
			vkFreeMemory(it.device, it.outputMemory, nullptr);
			it.outputMemory = VK_NULL_HANDLE;
		}
	}
	it.device = VK_NULL_HANDLE;
	it.captured = 0;
	it.next = 0;
	it.started = false;
}

// The measuring run.
//
// Called from inside the command buffer that finished the frame, with the
// swapchain image in VK_IMAGE_LAYOUT_PRESENT_SRC_KHR - the backbuffer render
// pass's finalLayout, so that is what it is and nothing after changes it. Every
// barrier here puts back what it found, because this command buffer is about to
// be submitted and presented by PPSSPP and nothing may be left moved.
//
// Nothing here changes what is displayed. The generated frame is written to an
// image that is never read. What it buys is the one number that decides whether
// the rest of this feature is worth building: what the passes cost on this GPU
// at this resolution.
void Capture(void *commandBufferHandle, void *swapchainImageHandle, std::uint32_t slot) {
	State &it = state();
	if (!it.saidEntered) {
		it.saidEntered = true;
		// Once, with every reason it might do nothing, so a capture that never
		// happens says which of them it was rather than leaving it to be guessed.
		psp5::Trace("framegen: capture offered a frame (context %d, output %d, switch %d, "
		            "slots %u, cmd %d, image %d)",
		            it.context != nullptr, it.output != VK_NULL_HANDLE, (int)Enabled(), it.slots,
		            commandBufferHandle != nullptr, swapchainImageHandle != nullptr);
	}
	if (!it.context || !it.output || !Enabled()) {
		if (!it.saidRefused) {
			it.saidRefused = true;
			psp5::Trace("framegen: capture refused the frame");
		}
		return;
	}
	VkCommandBuffer cmd = static_cast<VkCommandBuffer>(commandBufferHandle);
	VkImage shown = static_cast<VkImage>(swapchainImageHandle);
	if (!cmd || !shown) {
		return;
	}
	slot %= it.slots;

	// Last time round, did this slot leave a pair of timestamps behind? Its
	// work has completed - PPSSPP waited on that frame's fence before handing
	// the slot out again - so they can be read without blocking.
	if (it.timestamps && slot < it.timed.size() && it.timed[slot]) {
		std::uint64_t ticks[2] = {0, 0};
		const VkResult read =
		    vkGetQueryPoolResults(it.device, it.timestamps, slot * 2, 2, sizeof(ticks), ticks,
		                          sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
		if (read == VK_SUCCESS && ticks[1] > ticks[0]) {
			NoteCost(static_cast<float>(ticks[1] - ticks[0]) * it.nanosecondsPerTick /
			         1000000.0f);
		} else if (!it.saidNoTiming) {
			// Said once. The frames are still generated and still counted;
			// what is missing is only what they cost.
			it.saidNoTiming = true;
			psp5::Trace("framegen: the timestamps would not read back (%d); frames are "
			            "counted but not timed", (int)read);
		}
		it.timed[slot] = false;
	}

	VkImage into = it.history[it.next];

	// The finished picture, out of the swapchain and into our own format.
	Barrier(cmd, shown, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
	        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	        VK_PIPELINE_STAGE_TRANSFER_BIT);
	Barrier(cmd, into, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	        VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkImageBlit blit{};
	blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blit.dstSubresource = blit.srcSubresource;
	blit.srcOffsets[1] = {static_cast<std::int32_t>(it.width), static_cast<std::int32_t>(it.height),
	                      1};
	blit.dstOffsets[1] = blit.srcOffsets[1];
	vkCmdBlitImage(cmd, shown, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, into,
	               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

	// Put the swapchain image back before anything else can go wrong: PPSSPP
	// presents it the moment this command buffer completes.
	Barrier(cmd, shown, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	        VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
	        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	Barrier(cmd, into, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
	        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	const std::uint32_t previous = 1 - it.next;
	it.next = previous;
	++it.captured;

	// The first picture has no partner, and a generator run against an image
	// that has never been written reads memory nobody wrote.
	if (it.captured < 2) {
		return;
	}

	// Somewhere for the frame to go. It is never read; what matters is that the
	// passes run and are timed.
	Barrier(cmd, it.output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
	        VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	if (it.timestamps) {
		vkCmdResetQueryPool(cmd, it.timestamps, slot * 2, 2);
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, it.timestamps, slot * 2);
	}

	fg_frame_pair pair{};
	pair.prev_color = it.historyView[previous];
	pair.next_color = it.historyView[1 - previous];
	pair.prev_depth = VK_NULL_HANDLE;
	pair.next_depth = VK_NULL_HANDLE;
	pair.next_motion = VK_NULL_HANDLE;
	pair.t = kMidpoint;
	fg_record_generate(it.context, cmd, slot, FG_METHOD_NEURAL, &pair, it.outputView);
	NoteGenerated();
	if (!it.saidGenerating) {
		it.saidGenerating = true;
		psp5::Trace("framegen: generating at %ux%u (nothing is displayed yet)", it.width,
		            it.height);
	}

	if (it.timestamps) {
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, it.timestamps,
		                    slot * 2 + 1);
		if (slot < it.timed.size()) {
			it.timed[slot] = true;
		}
	}
}

bool WantFrame(float gameFps) {
	if (!Enabled() || !Ready()) {
		return false;
	}
	return gameFps > kFloorFps && gameFps < kCeilingFps;
}

bool Record(void *commandBuffer, std::uint32_t slot, void *prevColorView, void *nextColorView,
            void *outputView, float t) {
	State &it = state();
	if (!it.context || !commandBuffer || !prevColorView || !nextColorView || !outputView) {
		return false;
	}
	fg_frame_pair pair{};
	pair.prev_color = static_cast<VkImageView>(prevColorView);
	pair.next_color = static_cast<VkImageView>(nextColorView);
	// The neural method reads colour and nothing else, which is why it is the
	// one psp5 can use at all: there are no motion vectors here, and the depth
	// PPSSPP has is the emulated console's.
	pair.prev_depth = VK_NULL_HANDLE;
	pair.next_depth = VK_NULL_HANDLE;
	pair.next_motion = VK_NULL_HANDLE;
	pair.t = t > 0.0f && t < 1.0f ? t : kMidpoint;
	fg_record_generate(it.context, static_cast<VkCommandBuffer>(commandBuffer), slot,
	                   FG_METHOD_NEURAL, &pair, static_cast<VkImageView>(outputView));
	return true;
}

#else  // PSP5_FRAMEGEN

// No generator in this build. The calls stay so the graphics code reads the
// same either way; each one is a branch that is never taken.
bool Start(void *, void *, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {
	return false;
}
void Stop() {}
bool WantFrame(float) {
	return false;
}
bool Record(void *, std::uint32_t, void *, void *, void *, float) {
	return false;
}

#endif  // PSP5_FRAMEGEN

}  // namespace framegen
}  // namespace psp5

// What the emulator calls. A plain C name so the patched translation unit
// needs one declaration and no header of psp5's at all - the same shape as
// PS5_DrawOverlays and the rest of the hooks.
extern "C" void PS5_FrameGenNoteReal(void) {
	psp5::framegen::NoteReal();
}

// Called with the frame's command buffer still open and the swapchain image in
// PRESENT_SRC_KHR. A no-op in a build with no generator, and while the switch
// is off.
extern "C" void PS5_FrameGenCapture(void *commandBuffer, void *swapchainImage,
                                    unsigned int slot, int submitType, int flags) {
#ifdef PSP5_FRAMEGEN
	// The emulator submits more than once a frame and only one of those carries
	// the finished picture. Reported once for each kind of submit, so the log
	// says what was offered rather than only that nothing was taken: flags bit 0
	// is skipSwap, bit 1 is hasAcquired, and a null image means this submit was
	// not the one.
	static int saidFor = -1;
	if (saidFor != submitType) {
		saidFor = submitType;
		psp5::Trace("framegen: submit type %d, flags 0x%x, image %d, slot %u", submitType,
		            flags, swapchainImage != nullptr, slot);
	}
	if (swapchainImage) {
		psp5::framegen::Capture(commandBuffer, swapchainImage, slot);
	}
#else
	(void)commandBuffer;
	(void)swapchainImage;
	(void)slot;
	(void)submitType;
	(void)flags;
#endif
}
