// psp5 - how much room is left in the direct pool.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The port patch has PPSSPP ask these two questions (Common/PS5Memory.h), and the
// title answers them. In PS5_RetroArch the frontend does; psp5 is its own title, so
// it does here.
//
//   ps5_memory_pressure  the GPU's buffers, the texture cache and the framebuffer
//                        cache all come out of the 12 GiB direct pool, and a
//                        framebuffer that cannot be allocated is a visible fault
//                        rather than a slow frame. When the pool is low PPSSPP
//                        collects garbage sooner (FBO_OLD_AGE drops to 2) and
//                        retries a failed framebuffer after reclaiming.
//   ps5_memory_report    a line in the log naming what happened, so a run that
//                        went wrong says why.
//
// The query is throttled: DecimateFBOs calls this every frame, and asking the
// kernel to walk its direct-memory map that often would cost more than it saves.

#include "Common/PS5Memory.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

#include <ps5platform/kernel.h>

#include "platform/platform.h"

namespace {

// Below this much free direct memory, psp5 is under pressure whatever was asked
// for. A PSP framebuffer at a high internal resolution is a few tens of MiB, and
// the texture cache holds many; 256 MiB is room for several without being so large
// that an ordinary run spends its time collecting garbage.
constexpr uint64_t kLowWaterBytes = 256ull << 20;

// How often the kernel is actually asked, in seconds.
constexpr double kQueryInterval = 0.25;

// Both are atomic because PPSSPP asks from more than one thread: DecimateFBOs from
// the GPU thread, CreateFramebuffer from whichever thread is building the frame.
// Two threads may still query at once, which costs an extra kernel call and
// nothing else.
std::atomic<uint64_t> g_freeBytes{~0ull};
std::atomic<double> g_lastQuery{-1.0};

uint64_t FreeDirectMemory() {
	const double now = now_seconds();
	const double last = g_lastQuery.load(std::memory_order_relaxed);
	if (last >= 0.0 && now - last < kQueryInterval) {
		return g_freeBytes.load(std::memory_order_relaxed);
	}
	g_lastQuery.store(now, std::memory_order_relaxed);

	int64_t start = 0;
	size_t size = 0;
	const int64_t total = sceKernelGetDirectMemorySize();
	if (sceKernelAvailableDirectMemorySize(0, total, PS5_KERNEL_DIRECT_ALIGNMENT,
	                                       &start, &size) != 0) {
		// The kernel refused to answer. Reporting "no pressure" keeps the emulator
		// on its normal path rather than making it collect garbage every frame on
		// the strength of a failed query.
		g_freeBytes.store(~0ull, std::memory_order_relaxed);
		return ~0ull;
	}
	g_freeBytes.store((uint64_t)size, std::memory_order_relaxed);
	return (uint64_t)size;
}

}  // namespace

extern "C" int ps5_memory_pressure(size_t request) {
	const uint64_t free_bytes = FreeDirectMemory();
	if (free_bytes == ~0ull) {
		return 0;
	}
	// sceKernelAvailableDirectMemorySize reports the largest free run, not the sum,
	// so this is the size of the biggest allocation that could still succeed.
	if (free_bytes < kLowWaterBytes) {
		return 1;
	}
	return request != 0 && free_bytes < (uint64_t)request + kLowWaterBytes;
}

extern "C" void ps5_memory_report(const char *event, size_t request, int result) {
	say("memory: %s request=%zu result=%d free=%llu MiB", event ? event : "?", request,
	    result, (unsigned long long)(FreeDirectMemory() >> 20));
}
