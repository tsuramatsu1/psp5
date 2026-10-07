// psp5 - a trace that survives the run.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// klog is the console's own record and it is the right place for a title's
// output, but reading it means being connected while the title runs. A crash
// during bring-up is then only visible to whoever happened to be capturing, and
// a missed window costs another launch.
//
// So the same lines also go to /app0/psp5.log, which can be read over FTP long
// afterwards. Truncated at start-up, so the file is one run and not every run
// since the title was deployed, and flushed on every line, because a buffered
// line is lost exactly when it matters.

#include "PS5Log.h"

#include <cstdarg>
#include <cstdio>
#include <sys/stat.h>

#include "PS5Paths.h"
#include "platform/platform.h"

namespace psp5 {
namespace {

FILE *g_log = nullptr;

}  // namespace

void OpenTrace() {
	if (g_log) {
		return;
	}
	g_log = fopen(PS5Paths::Log().c_str(), "wb");
	if (g_log) {
		// Reachable over FTP, like everything else the title writes.
		chmod(PS5Paths::Log().c_str(), 0666);
		Trace("psp5 trace opened");
	}
}

void Trace(const char *format, ...) {
	char line[512];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);

	say("%s", line);
	if (g_log) {
		fprintf(g_log, "%s\n", line);
		fflush(g_log);
	}
}

void CloseTrace() {
	if (g_log) {
		fclose(g_log);
		g_log = nullptr;
	}
}

}  // namespace psp5
