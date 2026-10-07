// psp5 - the UI kit's system calls, on psp5's platform layer.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The kit reaches the system through six functions of its own
// (src/platform/ps5/system.hpp in PS5_VKHomebrewUI). psp5's platform layer
// already has all of them, so this is the whole of the kit's platform port.
//
// The kit's own src/platform/ and src/runtime/ are not built (see the PS5 branch
// in patches/ppsspp/ps5-standalone.patch): psp5 brings up the console itself, and
// its heap is the payload SDK's.

#include "platform/platform.h"
#include "platform/ps5/system.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <unistd.h>

namespace hui::sys {

std::int64_t monotonic_us() {
	return static_cast<std::int64_t>(now_seconds() * 1e6);
}

void log(const char *format, ...) {
	char line[512];
	va_list args;
	va_start(args, format);
	std::vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	say("ui: %s", line);
}

bool hide_splash_screen() {
	return true;  // platform_init already did, before anything drew.
}

void sleep_us(std::uint32_t microseconds) {
	usleep(microseconds);
}

void park() {
	for (;;) {
		usleep(100000);
	}
}

void quit() {
	// psp5 never leaves this way: the launcher returns from its own loop and the
	// title goes on to run PPSSPP. Kept because the kit's contract has it, and
	// because parking is a better end than returning into a torn-down renderer.
	park();
}

}  // namespace hui::sys
