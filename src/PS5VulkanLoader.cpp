// psp5 - resolving Vulkan against the statically linked RADV driver.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console has no Vulkan loader and no dlopen for a title's own code, so
// PPSSPP's VulkanLoader cannot find a libvulkan.so to dlsym. RADV is linked into
// the title instead (PS5_Vulkan's libvulkan_radeon.ps5.a), and the whole driver is
// reachable through the two symbols that archive exports:
// radv_GetInstanceProcAddr and vk_icdGetInstanceProcAddr. The standalone patch
// points VulkanLoader's LOAD_GLOBAL_FUNC at ps5_vk_global_proc below instead of
// dlsym; everything after that is ordinary Vulkan.
//
// Two of the three functions PPSSPP loads globally cannot simply be forwarded:
//
//   vkGetDeviceProcAddr  An ICD's GetInstanceProcAddr returns only the global
//                        entry points for a null instance, so this one is null
//                        until an instance exists. PPSSPP requires it to be
//                        non-null before it creates one, so it gets a trampoline
//                        that resolves the real function on first use.
//   vkCreateInstance     Wrapped only to record the instance that trampoline
//                        needs.

#include <cstring>

#include "Common/GPU/Vulkan/VulkanLoader.h"

using namespace PPSSPP_VK;

// RADV's own entry point, from libvulkan_radeon.ps5.a.
extern "C" PFN_vkVoidFunction radv_GetInstanceProcAddr(VkInstance instance, const char *name);

namespace {

VkInstance g_instance = VK_NULL_HANDLE;

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *createInfo,
                                              const VkAllocationCallbacks *allocator,
                                              VkInstance *instance) {
	auto real = (PFN_vkCreateInstance)radv_GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
	if (!real) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	VkResult result = real(createInfo, allocator, instance);
	if (result == VK_SUCCESS) {
		g_instance = *instance;
	}
	return result;
}

// Resolved on first call, by which time CreateInstance has recorded the instance.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char *name) {
	static PFN_vkGetDeviceProcAddr real = nullptr;
	if (!real && g_instance != VK_NULL_HANDLE) {
		real = (PFN_vkGetDeviceProcAddr)radv_GetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
	}
	return real ? real(device, name) : nullptr;
}

}  // namespace

extern "C" void *ps5_vk_global_proc(const char *name) {
	if (!std::strcmp(name, "vkCreateInstance")) {
		return (void *)CreateInstance;
	}
	if (!std::strcmp(name, "vkGetDeviceProcAddr")) {
		return (void *)GetDeviceProcAddr;
	}
	if (!std::strcmp(name, "vkGetInstanceProcAddr")) {
		return (void *)radv_GetInstanceProcAddr;
	}
	return (void *)radv_GetInstanceProcAddr(VK_NULL_HANDLE, name);
}
