// psp5 - a trace that survives the run.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace psp5 {

// Opens /app0/psp5.log, truncated. Call once, after the paths exist.
void OpenTrace();

// One line, to klog and to that file, flushed. Use it for anything that must be
// readable after a crash rather than only while someone is capturing klog.
void Trace(const char *format, ...) __attribute__((format(printf, 1, 2)));

void CloseTrace();

}  // namespace psp5
