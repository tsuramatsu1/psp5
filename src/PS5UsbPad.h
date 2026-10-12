// psp5 - USB controllers the console does not take as its own.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The approach, the device routes and the report decoding follow PS5SX2's
// orbis-shims/ProsperoUsbPad.cpp and OrbisUsbPadDecode.h (Copyright (C) 2026
// swordpdf, GPL-3.0-or-later), narrowed to what a PSP needs: pads only, no
// guitars, and none of the blind probing that cost that project two testers
// their controller.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ScePad already reports every controller the system supports - a DualSense, a
// DualShock 4, the licensed pads - and platform.c reads those. It reports
// nothing at all for a wired Xbox 360 pad, a PS3 pad or a DualShock 3: the
// console powers them and ignores them. This reads such a device itself, over
// USB, and hands its state to pad_poll to fold into player one's.
//
// What is never touched: Sony's own PS4 and PS5 controllers (taking their
// interface would take the controller away from the whole console), keyboards
// and mice, and anything whose report descriptor holds no gamepad.

struct usb_pad_state {
	uint32_t buttons;  // psp5's PAD_* bits, as ScePad numbers them
	uint8_t l2, r2;
	uint8_t lx, ly, rx, ry;  // 0..255, 128 at rest
};

// Starts the reader thread. Does nothing after the first call, and nothing at
// all when the file `nousbpad` exists beside the memory stick.
void usb_pad_start(void);

// The device's state now. False when none is connected, leaving *out untouched.
bool usb_pad_read(struct usb_pad_state *out);

// What was found, for the settings panel and the log: "Xbox 360 Controller",
// or NULL with nothing connected. The string belongs to this module and stays
// valid until the device goes away.
const char *usb_pad_name(void);

// Stops the thread and closes the device. Called at shutdown; safe when the
// thread was never started.
void usb_pad_stop(void);

#ifdef __cplusplus
}  // extern "C"
#endif
