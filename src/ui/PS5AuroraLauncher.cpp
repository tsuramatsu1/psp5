// psp5 - the Aurora Shelf home screen.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The kit (PS5_VKHomebrewUI) draws through gfx::VkRenderer, which wants a device,
// a graphics queue and a render pass, and nothing else - no window system, and
// not the template's VulkanExampleBase. psp5 already has the first two from
// PS5VulkanContext, so what this file adds is the pass, the framebuffers over the
// swapchain, the command buffers and the sync, and the frame loop that uses them.
//
// It runs before PPSSPP's draw context exists and tears everything down before it
// returns, because the kit's renderer and PPSSPP's render manager each assume they
// own the frame.

#include "PS5AuroraLauncher.h"

#include <cstdio>
#include <string>
#include <vector>

#include <volk.h>

#include "app/concept.hpp"
#include "concepts/concepts.hpp"
#include "core/input.hpp"
#include "core/settings.hpp"
#include "ui/feedback.hpp"
#include "demo/catalog.hpp"
#include "gfx/font.hpp"
#include "gfx/vk/vk_renderer.hpp"
#include "ui/fonts.hpp"

#include "PS5Paths.h"
#include "platform/platform.h"

// RADV's own entry point, from libvulkan_radeon.ps5.a. A linkage specification
// cannot appear inside a function, so it is declared here.
extern "C" PFN_vkVoidFunction radv_GetInstanceProcAddr(VkInstance instance, const char *name);

namespace psp5 {
namespace {

// The kit's fonts, staged beside the title's other assets by tools/link-title.sh.
const char *const kFontDir = "/app0/ui/fonts";

// Leaving: OPTIONS held, as every kit program does it.
constexpr double kHoldToLeaveSeconds = 1.0;

bool ReadWholeFile(const std::string &path, std::string *out) {
	FILE *fh = fopen(path.c_str(), "rb");
	if (!fh) {
		return false;
	}
	fseek(fh, 0, SEEK_END);
	const long size = ftell(fh);
	fseek(fh, 0, SEEK_SET);
	if (size <= 0) {
		fclose(fh);
		return false;
	}
	out->resize((size_t)size);
	const size_t got = fread(out->data(), 1, (size_t)size, fh);
	fclose(fh);
	return got == (size_t)size;
}

bool LoadFont(hui::gfx::VkRenderer &renderer, const char *name, hui::gfx::Font *font,
              hui::ui::FontRef *ref) {
	std::string data;
	const std::string path = std::string(kFontDir) + "/" + name;
	if (!ReadWholeFile(path, &data) || !font->load(data)) {
		say("ui: font %s failed (%s)", name, font->error().c_str());
		return false;
	}
	ref->font = font;
	ref->texture = renderer.create_font_texture(*font);
	return true;
}

// Everything Vulkan the launcher owns. Created in Begin, released in End, in the
// reverse order - the launcher must leave the device as it found it.
struct Frame {
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkSemaphore acquired = VK_NULL_HANDLE;
	VkSemaphore rendered = VK_NULL_HANDLE;
	VkFence done = VK_NULL_HANDLE;
};

class Surface {
public:
	bool Begin(const AuroraDevice &gpu);
	void End();

	VkRenderPass pass() const { return pass_; }
	int width() const { return width_; }
	int height() const { return height_; }
	uint32_t framesInFlight() const { return (uint32_t)frames_.size(); }
	// The framebuffer over the swapchain image BeginFrame just acquired.
	VkFramebuffer framebuffer(uint32_t imageIndex) const { return framebuffers_[imageIndex]; }

	// Acquires the next image and gives back the command buffer to record into,
	// already begun. Returns false when the swapchain is gone, which ends the run.
	bool BeginFrame(uint32_t slot, VkCommandBuffer *cmd, uint32_t *imageIndex);
	bool EndFrame(uint32_t slot, uint32_t imageIndex);

private:
	AuroraDevice gpu_{};
	VkDevice device_ = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
	VkRenderPass pass_ = VK_NULL_HANDLE;
	VkCommandPool pool_ = VK_NULL_HANDLE;
	std::vector<VkImageView> views_;
	std::vector<VkFramebuffer> framebuffers_;
	std::vector<Frame> frames_;
	int width_ = 0;
	int height_ = 0;
};

bool Surface::Begin(const AuroraDevice &gpu) {
	gpu_ = gpu;
	device_ = (VkDevice)gpu.device;
	swapchain_ = (VkSwapchainKHR)gpu.swapchain;
	width_ = gpu.width;
	height_ = gpu.height;

	// One colour attachment, cleared each frame and left ready to present. The UI
	// neither tests nor writes depth, so there is no depth attachment.
	VkAttachmentDescription colour{};
	colour.format = (VkFormat)gpu.swapchainFormat;
	colour.samples = VK_SAMPLE_COUNT_1_BIT;
	colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colour.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	colour.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkAttachmentReference colourRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colourRef;

	// The acquire semaphore is waited at COLOR_ATTACHMENT_OUTPUT, so the pass must
	// not write before then.
	VkSubpassDependency dependency{};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.srcAccessMask = 0;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	passInfo.attachmentCount = 1;
	passInfo.pAttachments = &colour;
	passInfo.subpassCount = 1;
	passInfo.pSubpasses = &subpass;
	passInfo.dependencyCount = 1;
	passInfo.pDependencies = &dependency;
	if (vkCreateRenderPass(device_, &passInfo, nullptr, &pass_) != VK_SUCCESS) {
		say("ui: no render pass");
		return false;
	}

	// The swapchain's images, which VulkanContext owns; the views and framebuffers
	// over them are the launcher's.
	uint32_t imageCount = 0;
	vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
	std::vector<VkImage> images(imageCount);
	vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, images.data());

	views_.resize(imageCount);
	framebuffers_.resize(imageCount);
	for (uint32_t i = 0; i < imageCount; i++) {
		VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		viewInfo.image = images[i];
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = (VkFormat)gpu.swapchainFormat;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;
		if (vkCreateImageView(device_, &viewInfo, nullptr, &views_[i]) != VK_SUCCESS) {
			say("ui: no image view %u", i);
			return false;
		}
		VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		fbInfo.renderPass = pass_;
		fbInfo.attachmentCount = 1;
		fbInfo.pAttachments = &views_[i];
		fbInfo.width = (uint32_t)width_;
		fbInfo.height = (uint32_t)height_;
		fbInfo.layers = 1;
		if (vkCreateFramebuffer(device_, &fbInfo, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
			say("ui: no framebuffer %u", i);
			return false;
		}
	}

	VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = gpu.queueFamily;
	if (vkCreateCommandPool(device_, &poolInfo, nullptr, &pool_) != VK_SUCCESS) {
		say("ui: no command pool");
		return false;
	}

	// Two frames in flight, which is what the renderer's config says and what its
	// per-slot instance buffers are sized for.
	frames_.resize(2);
	for (Frame &frame : frames_) {
		VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		alloc.commandPool = pool_;
		alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		alloc.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(device_, &alloc, &frame.cmd) != VK_SUCCESS) {
			return false;
		}
		VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // so the first wait returns
		if (vkCreateSemaphore(device_, &sem, nullptr, &frame.acquired) != VK_SUCCESS ||
		    vkCreateSemaphore(device_, &sem, nullptr, &frame.rendered) != VK_SUCCESS ||
		    vkCreateFence(device_, &fence, nullptr, &frame.done) != VK_SUCCESS) {
			return false;
		}
	}
	return true;
}

bool Surface::BeginFrame(uint32_t slot, VkCommandBuffer *cmd, uint32_t *imageIndex) {
	Frame &frame = frames_[slot];
	// The slot's previous submission must be done: prepare() rewrites its buffers.
	vkWaitForFences(device_, 1, &frame.done, VK_TRUE, UINT64_MAX);

	const VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
	                                                frame.acquired, VK_NULL_HANDLE, imageIndex);
	if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
		return false;
	}
	vkResetFences(device_, 1, &frame.done);
	vkResetCommandBuffer(frame.cmd, 0);

	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(frame.cmd, &begin);
	*cmd = frame.cmd;
	return true;
}

bool Surface::EndFrame(uint32_t slot, uint32_t imageIndex) {
	Frame &frame = frames_[slot];
	vkEndCommandBuffer(frame.cmd);

	const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = &frame.acquired;
	submit.pWaitDstStageMask = &wait;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &frame.cmd;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &frame.rendered;
	if (vkQueueSubmit((VkQueue)gpu_.queue, 1, &submit, frame.done) != VK_SUCCESS) {
		return false;
	}

	VkSwapchainKHR swapchain = swapchain_;
	VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &frame.rendered;
	present.swapchainCount = 1;
	present.pSwapchains = &swapchain;
	present.pImageIndices = &imageIndex;
	const VkResult presented = vkQueuePresentKHR((VkQueue)gpu_.queue, &present);
	return presented == VK_SUCCESS || presented == VK_SUBOPTIMAL_KHR;
}

void Surface::End() {
	if (!device_) {
		return;
	}
	// Nothing may be in flight when these go.
	vkDeviceWaitIdle(device_);
	for (Frame &frame : frames_) {
		if (frame.done) vkDestroyFence(device_, frame.done, nullptr);
		if (frame.rendered) vkDestroySemaphore(device_, frame.rendered, nullptr);
		if (frame.acquired) vkDestroySemaphore(device_, frame.acquired, nullptr);
	}
	frames_.clear();
	if (pool_) {
		vkDestroyCommandPool(device_, pool_, nullptr);
		pool_ = VK_NULL_HANDLE;
	}
	for (VkFramebuffer fb : framebuffers_) {
		if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
	}
	framebuffers_.clear();
	for (VkImageView view : views_) {
		if (view) vkDestroyImageView(device_, view, nullptr);
	}
	views_.clear();
	if (pass_) {
		vkDestroyRenderPass(device_, pass_, nullptr);
		pass_ = VK_NULL_HANDLE;
	}
	device_ = VK_NULL_HANDLE;
}

// The console's pad readings, in the shape the kit's tracker takes. Both are
// "buttons plus four axes as bytes", so this is a copy, not a conversion.
int GatherInput(hui::PadSample *samples, int capacity) {
	const pad_reading *readings = nullptr;
	const int count = pad_readings(&readings);
	int used = 0;
	for (int i = 0; i < count && used < capacity; i++) {
		if (readings[i].buttons & PAD_INTERCEPTED) {
			continue;  // the shell has the pad; not input for the title
		}
		hui::PadSample &sample = samples[used++];
		sample.buttons = readings[i].buttons;
		sample.left_x = readings[i].left_x;
		sample.left_y = readings[i].left_y;
		sample.right_x = readings[i].right_x;
		sample.right_y = readings[i].right_y;
		sample.l2 = readings[i].l2;
		sample.r2 = readings[i].r2;
	}
	return used;
}

}  // namespace

bool RunAuroraLauncher(const AuroraDevice &gpu) {
	// Before anything calls a vkXxx, including Surface::Begin below: every entry
	// point in this file and in the kit is a volk pointer, and an uninitialised one
	// is null. Calling it jumps to address 0, which is exactly what the first
	// console run did - SIGSEGV with rip 0 and the fault address equal to it.
	//
	// RADV is linked in as an archive and reached through its own
	// GetInstanceProcAddr, which is what volk is aimed at.
	volkInitializeCustom((PFN_vkGetInstanceProcAddr)radv_GetInstanceProcAddr);
	volkLoadInstance((VkInstance)gpu.instance);
	volkLoadDevice((VkDevice)gpu.device);

	say("ui: volk loaded");

	Surface surface;
	if (!surface.Begin(gpu)) {
		surface.End();
		return false;
	}

	say("ui: surface ready, %d frames in flight", (int)surface.framesInFlight());

	hui::gfx::VkRenderer renderer;
	hui::gfx::VkRendererConfig config;
	config.physical_device = (VkPhysicalDevice)gpu.physicalDevice;
	config.device = (VkDevice)gpu.device;
	config.queue = (VkQueue)gpu.queue;
	config.queue_family = gpu.queueFamily;
	config.frames_in_flight = surface.framesInFlight();
	config.render_pass = surface.pass();
	config.subpass = 0;
	config.samples = VK_SAMPLE_COUNT_1_BIT;
	if (!renderer.init(config)) {
		say("ui: the kit's renderer would not start");
		surface.End();
		return false;
	}

	// The six faces the designs use. Without them nothing can be drawn, so a
	// missing font is the one reason to give up and let PPSSPP's UI take over.
	say("ui: renderer started");

	hui::gfx::Font regular, semibold, display, mono, pixel, hand;
	hui::ui::Fonts fonts;
	if (!LoadFont(renderer, "inter-regular.huifont", &regular, &fonts.regular) ||
	    !LoadFont(renderer, "inter-semibold.huifont", &semibold, &fonts.semibold) ||
	    !LoadFont(renderer, "montserrat-medium.huifont", &display, &fonts.display) ||
	    !LoadFont(renderer, "dejavu-sans-mono.huifont", &mono, &fonts.mono) ||
	    !LoadFont(renderer, "press-start-2p.huifont", &pixel, &fonts.pixel) ||
	    !LoadFont(renderer, "patrick-hand.huifont", &hand, &fonts.hand)) {
		say("ui: fonts missing from %s - falling back to PPSSPP's interface", kFontDir);
		renderer.release();
		surface.End();
		return false;
	}

	say("ui: fonts loaded");

	hui::demo::Catalog catalog;
	catalog.build_covers(renderer, fonts);
	say("ui: covers built (%d items)", (int)catalog.size());

	hui::Settings settings;
	hui::app::Telemetry telemetry;
	hui::app::Context context{fonts, catalog, telemetry, settings};
	std::unique_ptr<hui::app::Concept> aurora = hui::concepts::make_aurora(context);
	aurora->enter();
	say("ui: aurora created");

	say("ui: Aurora Shelf at %dx%d", surface.width(), surface.height());

	hui::InputTracker tracker;
	hui::PadSample samples[64];
	double previous = now_seconds();
	double holding = 0.0;
	uint32_t slot = 0;
	uint64_t frames = 0;

	for (;;) {
		pad state{};
		pad_poll(&state);
		const int count = GatherInput(samples, (int)(sizeof(samples) / sizeof(samples[0])));

		const double now = now_seconds();
		float dt = (float)(now - previous);
		previous = now;
		if (dt > 0.05f) {
			dt = 0.05f;  // a long frame must not fling the springs
		}

		hui::InputFrame input =
		    tracker.update(std::span<const hui::PadSample>(samples, (size_t)count),
		                   (uint64_t)(now * 1e6));

		// Leaving, the way every kit program does it: OPTIONS held.
		holding = (state.held & PAD_OPTIONS) ? holding + dt : 0.0;
		if (holding >= kHoldToLeaveSeconds) {
			break;
		}

		hui::ui::Feedback feedback;
		aurora->update(input, dt, feedback);

		hui::app::Frame frame;
		aurora->draw(frame);

		renderer.begin();
		renderer.backdrop(frame.backdrop);
		renderer.draw(frame.scene);
		if (frame.glass) {
			renderer.glass();
		}
		renderer.draw(frame.overlay);
		renderer.backdrop(frame.post);

		VkCommandBuffer cmd = VK_NULL_HANDLE;
		uint32_t imageIndex = 0;
		if (!surface.BeginFrame(slot, &cmd, &imageIndex)) {
			break;
		}
		// Uploads and the glass copies, outside any pass.
		renderer.prepare(cmd, slot);

		VkClearValue clear{};
		VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		pass.renderPass = surface.pass();
		pass.framebuffer = surface.framebuffer(imageIndex);
		pass.renderArea.extent.width = (uint32_t)surface.width();
		pass.renderArea.extent.height = (uint32_t)surface.height();
		pass.clearValueCount = 1;
		pass.pClearValues = &clear;
		vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
		renderer.draw(cmd, surface.width(), surface.height());
		vkCmdEndRenderPass(cmd);

		if (!surface.EndFrame(slot, imageIndex)) {
			break;
		}
		if (frames == 0) {
			say("ui: first frame presented");
		}
		frames++;
		slot = (slot + 1) % surface.framesInFlight();
	}

	say("ui: leaving the home screen");
	aurora.reset();
	renderer.release();
	surface.End();
	return true;
}

}  // namespace psp5
