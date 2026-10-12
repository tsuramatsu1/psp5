// psp5 - asking whatever is running as root to let the title list folders.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The protocol, the request names and the crash guard follow SwanStationPS5's
// src/platform/ps5_unlock.c (Copyright (C) 2026 darkxex, GPL-3.0-or-later) and
// PS5SX2's ProsperoHenJailbreak.cpp (Copyright (C) 2026 swordpdf, same
// licence). Neither escapes anything and nor does this.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// A sandboxed title can open and write a file under /mnt or /data by its full
// path, but it cannot list a folder: opendir fails. Games on a USB drive are
// found by listing, so without this psp5 sees only the memory stick inside its
// own folder.
//
// There is no sandbox escape here. The console is already running something as
// root - etaHEN, OnionHEN, LegacyJB, the PS5SX2 Helper - and each of them
// watches for a title that asks to be let out. psp5 asks; if nothing answers,
// psp5 carries on with its own memory stick and says so.
//
// Three things make this delicate, and all three are why it is a separate file
// called before anything else:
//
//   - It must run while the process has one thread. The daemons refuse a
//     multi-threaded target and take the request anyway, so asking late loses
//     the request silently. platform_init starts a thread to pump stderr into
//     klog, so this runs before even that.
//   - The process needs its own credential first (seteuid of its own euid), or
//     there is nothing for the daemon to change.
//   - The request can take the title down with it. A marker written before
//     asking and removed after means a launch that died asking is followed by
//     one that does not ask, instead of a console that cannot start psp5.

typedef enum {
	PS5_UNLOCK_ALREADY,      // folders already list: nothing was asked
	PS5_UNLOCK_GRANTED,      // something answered
	PS5_UNLOCK_NO_ANSWER,    // nothing is listening; psp5 stays sandboxed
	PS5_UNLOCK_CANNOT_ASK,   // the request could not even be written
	PS5_UNLOCK_AFTER_CRASH,  // the last launch died asking, so this one did not
} PS5UnlockResult;

// Call first thing in main(), before any other thread exists.
PS5UnlockResult PS5_Unlock(void);

// Whether folders can be listed now. The library asks before it looks at the
// drives, and the settings panel shows the answer.
int PS5_FoldersListable(void);

// "granted", "nothing answered", and so on, for the log and the panel.
const char *PS5_UnlockDescribe(PS5UnlockResult result);

#ifdef __cplusplus
}  // extern "C"
#endif
