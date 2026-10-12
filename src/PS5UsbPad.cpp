// psp5 - USB controllers the console does not take as its own.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The approach, the device routes and the report decoding follow PS5SX2's
// orbis-shims/ProsperoUsbPad.cpp and OrbisUsbPadDecode.h (Copyright (C) 2026
// swordpdf, GPL-3.0-or-later). Two differences, both deliberate:
//
//   - No guitars. A PSP has four face buttons and two shoulders; a five-fret
//     neck has nowhere to go. Pads only, which removes about half of it.
//   - No blind probing. PS5SX2 learned, from two testers, that opening the raw
//     endpoints of a device the kernel would not identify can take a wired
//     DualSense away from the whole system - the PS button dies until the
//     console is power-cycled - and can stall the USB drive a game is reading
//     from. They gated that behind a flag for guitar games. psp5 does not do it
//     at all: a device it cannot identify is a device it leaves alone.
//
// So there are two routes here, both of which name the device before they read
// it:
//
//   1. /dev/uhidN - the kernel's generic HID driver, where it claimed the
//      device. read() gives its reports and an ioctl its report descriptor.
//   2. /dev/ugenB.A - FreeBSD's generic USB access, when USB_GET_DEVICEINFO
//      works, which is how the descriptors are read and the endpoint found.
//
// Everything goes into psp5.log as "usbpad:" lines - every node looked at, and
// why each was taken or left - because the only way to learn what an unknown
// pad sends is to read a log from someone who has one.

#include "PS5UsbPad.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "PS5Log.h"
#include "PS5Paths.h"
#include "platform/platform.h"

namespace {

// ---- the report descriptor ------------------------------------------------
//
// A HID device describes its own reports: which bits are buttons, which are
// axes, and what each axis ranges over. Reading that is what makes this work
// for a pad nobody has seen, instead of only for the handful with a known
// layout.

struct HidField {
	std::uint16_t page = 0;
	std::uint16_t usage = 0;
	std::uint8_t reportId = 0;
	std::uint32_t bit = 0;   // after the report ID byte
	std::uint32_t size = 0;  // in bits
	std::int32_t min = 0;
	std::int32_t max = 0;
};

struct HidLayout {
	bool ok = false;
	bool hasIds = false;
	bool gamepad = false;   // a Generic Desktop Joystick or Game Pad collection
	bool keyboard = false;  // ... Keyboard or Mouse: not ours
	std::vector<HidField> fields;
};

std::uint32_t Unsigned(const std::uint8_t *at, int count) {
	std::uint32_t value = 0;
	for (int i = 0; i < count; ++i) {
		value |= (std::uint32_t)at[i] << (8 * i);
	}
	return value;
}

std::int32_t Signed(const std::uint8_t *at, int count) {
	const std::uint32_t value = Unsigned(at, count);
	if (count == 1) {
		return (std::int8_t)value;
	}
	if (count == 2) {
		return (std::int16_t)value;
	}
	return (std::int32_t)value;
}

// The input items of a report descriptor (HID 1.11 section 6.2.2). Output and
// feature items advance nothing: they belong to other reports. Push and pop are
// followed; long items are skipped.
HidLayout ParseHid(const std::uint8_t *data, std::size_t length) {
	HidLayout out;
	struct Globals {
		std::uint16_t page = 0;
		std::int32_t min = 0;
		std::int32_t max = 0;
		std::uint32_t size = 0;
		std::uint32_t count = 0;
		std::uint8_t id = 0;
	} globals;
	std::vector<Globals> stack;
	std::vector<std::uint32_t> usages;
	std::uint32_t usageMin = 0;
	std::uint32_t usageMax = 0;
	bool haveRange = false;
	std::uint32_t offset[256] = {};
	int depth = 0;

	for (std::size_t i = 0; i < length;) {
		const std::uint8_t prefix = data[i];
		if (prefix == 0xFE) {
			if (i + 2 >= length) {
				break;
			}
			i += 3 + data[i + 1];
			continue;
		}
		int size = prefix & 3;
		if (size == 3) {
			size = 4;
		}
		if (i + 1 + (std::size_t)size > length) {
			break;
		}
		const std::uint8_t *item = data + i + 1;
		const std::uint8_t tag = prefix & 0xFC;
		const std::uint32_t value = Unsigned(item, size);
		switch (tag) {
			case 0x80: {  // Input
				const bool constant = (value & 1) != 0;
				const bool variable = (value & 2) != 0;
				std::uint32_t &at = offset[globals.id];
				// A report is at most a kilobyte here and a field at most 32
				// bits. A descriptor claiming more is broken, and trusting it
				// would have us index far outside the report.
				if (globals.size == 0 || globals.size > 32 || globals.count > 1024 ||
				    at + globals.size * globals.count > 8192) {
					usages.clear();
					haveRange = false;
					break;
				}
				for (std::uint32_t k = 0; k < globals.count; ++k) {
					std::uint32_t usage = 0;
					if (!usages.empty()) {
						usage = usages[std::min<std::size_t>(k, usages.size() - 1)];
					} else if (haveRange) {
						usage = std::min(usageMin + k, usageMax);
					}
					if (!constant && variable && (usage != 0 || haveRange)) {
						HidField field;
						field.page = (usage >> 16) ? (std::uint16_t)(usage >> 16) : globals.page;
						field.usage = (std::uint16_t)(usage & 0xFFFF);
						field.reportId = globals.id;
						field.bit = at;
						field.size = globals.size;
						field.min = globals.min;
						field.max = globals.max;
						out.fields.push_back(field);
					}
					at += globals.size;
				}
				usages.clear();
				haveRange = false;
				break;
			}
			case 0x90:  // Output
			case 0xB0:  // Feature
				usages.clear();
				haveRange = false;
				break;
			case 0xA0:  // Collection
				if (depth == 0 && value == 1) {
					const std::uint32_t usage = usages.empty() ? 0 : (usages[0] & 0xFFFF);
					if (globals.page == 1 && (usage == 4 || usage == 5)) {
						out.gamepad = true;
					}
					if (globals.page == 1 && (usage == 2 || usage == 6)) {
						out.keyboard = true;
					}
				}
				++depth;
				usages.clear();
				haveRange = false;
				break;
			case 0xC0:  // End Collection
				if (depth > 0) {
					--depth;
				}
				break;
			case 0x04: globals.page = (std::uint16_t)value; break;
			case 0x14: globals.min = Signed(item, size); break;
			case 0x24: globals.max = size ? Signed(item, size) : 0; break;
			case 0x74: globals.size = value; break;
			case 0x84:
				globals.id = (std::uint8_t)value;
				out.hasIds = true;
				break;
			case 0x94: globals.count = value; break;
			case 0xA4: stack.push_back(globals); break;
			case 0xB4:
				if (!stack.empty()) {
					globals = stack.back();
					stack.pop_back();
				}
				break;
			case 0x08: usages.push_back(size == 4 ? value : (value & 0xFFFF)); break;
			case 0x18:
				usageMin = size == 4 ? (value & 0xFFFF) : value;
				haveRange = true;
				break;
			case 0x28: usageMax = size == 4 ? (value & 0xFFFF) : value; break;
			default: break;
		}
		i += 1 + (std::size_t)size;
	}

	// A logical maximum that reads negative as a signed byte: 0x26 0xFF 0x00 is
	// 255, but 0x25 0xFF alone is -1. Taken as unsigned when the minimum is not
	// negative, which is what every other parser does and what devices mean.
	for (HidField &field : out.fields) {
		if (field.min >= 0 && field.max < field.min && field.size > 0 && field.size < 32) {
			field.max = (std::int32_t)((1u << field.size) - 1);
		}
	}
	out.ok = !out.fields.empty();
	return out;
}

std::uint32_t Bits(const std::uint8_t *report, std::size_t length, std::uint32_t bit,
                   std::uint32_t size) {
	std::uint32_t value = 0;
	for (std::uint32_t k = 0; k < size && k < 32; ++k) {
		const std::uint32_t at = bit + k;
		if (at / 8 >= length) {
			break;
		}
		value |= (std::uint32_t)((report[at / 8] >> (at % 8)) & 1u) << k;
	}
	return value;
}

std::int32_t FieldValue(const HidField &field, const std::uint8_t *report, std::size_t length) {
	const std::uint32_t raw = Bits(report, length, field.bit, field.size);
	if (field.min < 0 && field.size > 0 && field.size < 32 && ((raw >> (field.size - 1)) & 1u)) {
		return (std::int32_t)(raw | ~((1u << field.size) - 1u));
	}
	return (std::int32_t)raw;
}

std::uint8_t Scale(std::int32_t value, std::int32_t min, std::int32_t max) {
	if (max <= min) {
		return 128;
	}
	const std::int64_t scaled =
	    ((std::int64_t)std::clamp(value, min, max) - min) * 255 / ((std::int64_t)max - min);
	return (std::uint8_t)scaled;
}

// ---- one state, in PS3 terms ----------------------------------------------
//
// Every device here is decoded into this before it becomes psp5's buttons. PS3
// numbering because that is what the HID pads use: button 1 is Square, 2 Cross,
// and so on up to 13 for the PS button.

enum Ps3Button : std::uint16_t {
	kSquare = 1u << 0,
	kCross = 1u << 1,
	kCircle = 1u << 2,
	kTriangle = 1u << 3,
	kL1 = 1u << 4,
	kR1 = 1u << 5,
	kL2 = 1u << 6,
	kR2 = 1u << 7,
	kSelect = 1u << 8,
	kStart = 1u << 9,
	kL3 = 1u << 10,
	kR3 = 1u << 11,
	kPs = 1u << 12,
};

struct Common {
	std::uint16_t buttons = 0;
	std::uint8_t hat = 8;  // 0 up, 1 up-right, 2 right .. 7 up-left, 8 none
	std::uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
	std::uint8_t l2 = 0, r2 = 0;
};

enum class Format : std::uint8_t {
	none,
	hid,       // read through its own report descriptor
	ds3,       // Sony's DualShock 3 (054c:0268), report 1
	xinput,    // the Xbox 360 wired protocol (vendor class 0xFF, subclass 0x5D)
};

const char *FormatName(Format format) {
	switch (format) {
		case Format::hid: return "HID gamepad";
		case Format::ds3: return "DualShock 3";
		case Format::xinput: return "XInput";
		default: return "none";
	}
}

bool DecodeHid(const HidLayout &layout, const std::uint8_t *report, std::size_t length,
               Common *out) {
	if (!layout.ok || length == 0) {
		return false;
	}
	std::uint8_t id = 0;
	const std::uint8_t *body = report;
	std::size_t bodyLength = length;
	if (layout.hasIds) {
		id = report[0];
		body = report + 1;
		bodyLength = length - 1;
	}

	Common state;
	bool any = false;
	bool haveZ = false, haveRz = false, haveRx = false, haveRy = false;
	std::uint8_t z = 128, rz = 128, rx = 128, ry = 128;
	for (const HidField &field : layout.fields) {
		if (field.reportId != id || field.bit + field.size > bodyLength * 8) {
			continue;
		}
		const std::int32_t value = FieldValue(field, body, bodyLength);
		if (field.page == 9 && field.usage >= 1 && field.usage <= 13) {
			if (value != 0) {
				state.buttons |= (std::uint16_t)(1u << (field.usage - 1));
			}
			any = true;
		} else if (field.page == 1) {
			switch (field.usage) {
				case 0x30: state.lx = Scale(value, field.min, field.max); any = true; break;
				case 0x31: state.ly = Scale(value, field.min, field.max); any = true; break;
				case 0x32: z = Scale(value, field.min, field.max); haveZ = true; break;
				case 0x35: rz = Scale(value, field.min, field.max); haveRz = true; break;
				case 0x33: rx = Scale(value, field.min, field.max); haveRx = true; break;
				case 0x34: ry = Scale(value, field.min, field.max); haveRy = true; break;
				case 0x39: {
					const std::int32_t at = value - field.min;
					const std::int32_t steps = field.max - field.min + 1;
					state.hat = (value < field.min || value > field.max) ? 8
					            : steps == 8                             ? (std::uint8_t)at
					            : steps == 4                             ? (std::uint8_t)(at * 2)
					                                                     : 8;
					any = true;
					break;
				}
				default: break;
			}
		}
	}

	// PS3 pads put the right stick on Z and Rz; most others on Rx and Ry.
	bool analogTriggers = false;
	if (haveZ || haveRz) {
		state.rx = haveZ ? z : 128;
		state.ry = haveRz ? rz : 128;
		// With the right stick on Z and Rz, Rx and Ry are the analog triggers:
		// that is what a DualShock 4 sends, and without this its L2 and R2 were
		// on or off with nothing in between.
		analogTriggers = haveRx || haveRy;
	} else {
		state.rx = haveRx ? rx : 128;
		state.ry = haveRy ? ry : 128;
	}
	if (!any && !haveZ && !haveRz && !haveRx && !haveRy) {
		return false;
	}
	state.l2 = (state.buttons & kL2) ? 255 : 0;
	state.r2 = (state.buttons & kR2) ? 255 : 0;
	if (analogTriggers) {
		// Scale(...) centres an unsigned axis on 128; a trigger rests at zero,
		// so the raw reading is what is wanted here.
		if (haveRx) {
			state.l2 = std::max(state.l2, rx);
			if (rx >= 64) {
				state.buttons |= kL2;
			}
		}
		if (haveRy) {
			state.r2 = std::max(state.r2, ry);
			if (ry >= 64) {
				state.buttons |= kR2;
			}
		}
	}
	*out = state;
	return true;
}

// The DualShock 3 over USB, report 1, 49 bytes: byte 2 Select, L3, R3, Start
// and the d-pad; byte 3 the shoulders and face buttons; byte 4 the PS button;
// the sticks in 6..9, the triggers' pressure in 18 and 19.
bool DecodeDs3(const std::uint8_t *report, std::size_t length, Common *out) {
	if (length < 20 || report[0] != 0x01) {
		return false;
	}
	Common state;
	const std::uint8_t a = report[2];
	const std::uint8_t b = report[3];
	if (a & 0x01) { state.buttons |= kSelect; }
	if (a & 0x02) { state.buttons |= kL3; }
	if (a & 0x04) { state.buttons |= kR3; }
	if (a & 0x08) { state.buttons |= kStart; }
	if (b & 0x01) { state.buttons |= kL2; }
	if (b & 0x02) { state.buttons |= kR2; }
	if (b & 0x04) { state.buttons |= kL1; }
	if (b & 0x08) { state.buttons |= kR1; }
	if (b & 0x10) { state.buttons |= kTriangle; }
	if (b & 0x20) { state.buttons |= kCircle; }
	if (b & 0x40) { state.buttons |= kCross; }
	if (b & 0x80) { state.buttons |= kSquare; }
	if (report[4] & 0x01) { state.buttons |= kPs; }

	const bool up = (a & 0x10) != 0, right = (a & 0x20) != 0;
	const bool down = (a & 0x40) != 0, left = (a & 0x80) != 0;
	state.hat = up     ? (right ? 1 : left ? 7 : 0)
	            : down ? (right ? 3 : left ? 5 : 4)
	            : right ? 2
	            : left  ? 6
	                    : 8;
	state.lx = report[6];
	state.ly = report[7];
	state.rx = report[8];
	state.ry = report[9];
	state.l2 = std::max<std::uint8_t>(report[18], (state.buttons & kL2) ? 255 : 0);
	state.r2 = std::max<std::uint8_t>(report[19], (state.buttons & kR2) ? 255 : 0);
	*out = state;
	return true;
}

std::uint8_t XInputStick(std::int16_t value, bool invert) {
	std::int32_t scaled = ((std::int32_t)value + 32768) >> 8;
	if (invert) {
		scaled = 255 - scaled;
	}
	return (std::uint8_t)std::clamp(scaled, 0, 255);
}

// The Xbox 360 wired report: type 0, length 0x14, the buttons as a little
// endian 16 in bytes 2 and 3, the triggers in 4 and 5, the sticks as signed
// 16s in 6..13.
bool DecodeXInput(const std::uint8_t *report, std::size_t length, Common *out) {
	if (length < 14 || report[0] != 0x00 || report[1] < 0x14) {
		return false;
	}
	Common state;
	const std::uint16_t w = (std::uint16_t)(report[2] | (report[3] << 8));
	const bool up = (w & 0x0001) != 0, down = (w & 0x0002) != 0;
	const bool left = (w & 0x0004) != 0, right = (w & 0x0008) != 0;
	state.hat = up     ? (right ? 1 : left ? 7 : 0)
	            : down ? (right ? 3 : left ? 5 : 4)
	            : right ? 2
	            : left  ? 6
	                    : 8;
	if (w & 0x0010) { state.buttons |= kStart; }
	if (w & 0x0020) { state.buttons |= kSelect; }
	if (w & 0x0040) { state.buttons |= kL3; }
	if (w & 0x0080) { state.buttons |= kR3; }
	if (w & 0x0100) { state.buttons |= kL1; }
	if (w & 0x0200) { state.buttons |= kR1; }
	if (w & 0x0400) { state.buttons |= kPs; }
	// The face buttons by position, not by name: Xbox's A is where the PSP's
	// Cross is, and a player pressing the bottom button means the bottom one.
	if (w & 0x1000) { state.buttons |= kCross; }
	if (w & 0x2000) { state.buttons |= kCircle; }
	if (w & 0x4000) { state.buttons |= kSquare; }
	if (w & 0x8000) { state.buttons |= kTriangle; }

	state.l2 = report[4];
	state.r2 = report[5];
	if (state.l2 >= 64) { state.buttons |= kL2; }
	if (state.r2 >= 64) { state.buttons |= kR2; }

	std::int16_t lx, ly, rx, ry;
	std::memcpy(&lx, report + 6, 2);
	std::memcpy(&ly, report + 8, 2);
	std::memcpy(&rx, report + 10, 2);
	std::memcpy(&ry, report + 12, 2);
	state.lx = XInputStick(lx, false);
	state.ly = XInputStick(ly, true);  // XInput's Y is positive upwards
	state.rx = XInputStick(rx, false);
	state.ry = XInputStick(ry, true);
	*out = state;
	return true;
}

std::uint32_t HatBits(std::uint8_t hat) {
	switch (hat) {
		case 0: return PAD_UP;
		case 1: return PAD_UP | PAD_RIGHT;
		case 2: return PAD_RIGHT;
		case 3: return PAD_DOWN | PAD_RIGHT;
		case 4: return PAD_DOWN;
		case 5: return PAD_DOWN | PAD_LEFT;
		case 6: return PAD_LEFT;
		case 7: return PAD_UP | PAD_LEFT;
		default: return 0;
	}
}

// Into the bits platform.c already speaks. Select becomes the touchpad press,
// because that is what PS5Main maps to the PSP's Select: a pad with a real
// Select button should reach the same place the DualSense's touchpad does.
usb_pad_state ToPad(const Common &state) {
	usb_pad_state out = {};
	if (state.buttons & kSquare) { out.buttons |= PAD_SQUARE; }
	if (state.buttons & kCross) { out.buttons |= PAD_CROSS; }
	if (state.buttons & kCircle) { out.buttons |= PAD_CIRCLE; }
	if (state.buttons & kTriangle) { out.buttons |= PAD_TRIANGLE; }
	if (state.buttons & kL1) { out.buttons |= PAD_L1; }
	if (state.buttons & kR1) { out.buttons |= PAD_R1; }
	if (state.buttons & kL2) { out.buttons |= PAD_L2; }
	if (state.buttons & kR2) { out.buttons |= PAD_R2; }
	if (state.buttons & kStart) { out.buttons |= PAD_OPTIONS; }
	if (state.buttons & kSelect) { out.buttons |= PAD_TOUCH_PAD; }
	if (state.buttons & kL3) { out.buttons |= PAD_L3; }
	if (state.buttons & kR3) { out.buttons |= PAD_R3; }
	out.buttons |= HatBits(state.hat);
	out.l2 = state.l2;
	out.r2 = state.r2;
	out.lx = state.lx;
	out.ly = state.ly;
	out.rx = state.rx;
	out.ry = state.ry;
	return out;
}

// A DualSense belongs to the system: ScePad reads it already, and detaching
// its interface would take the controller away from the whole console, not
// just from psp5.
//
// A DualShock 4 does not. The PS5 gives it power and pairs it, but only PS4
// titles are allowed to see it - a PS5 title like psp5 is handed nothing for
// it by ScePad - so it is ours to read, and a tester's DS4 did nothing at all
// while this function answered for the whole of Sony's vendor id.
bool SystemOwned(std::uint16_t vendor, std::uint16_t product) {
	if (vendor != 0x054C) {
		return false;
	}
	switch (product) {
		case 0x0268:  // DualShock 3
		case 0x05C4:  // DualShock 4, first revision
		case 0x09CC:  // DualShock 4, second revision
		case 0x0BA0:  // the DualShock 4 USB wireless adaptor
			return false;
		default:
			// The DualSense (0CE6), the Edge (0DF2) and anything of Sony's that
			// psp5 has not been taught about: left to the console, because
			// taking a controller the system is using is not an experiment to
			// run on someone else's console.
			return true;
	}
}

// ---- the device -----------------------------------------------------------

struct Target {
	std::uint8_t iface = 0;
	std::uint8_t endpoint = 0;
	std::uint16_t packet = 64;
	bool xinput = false;
};

struct Device {
	std::string node;
	int fd = -1;
	int control = -1;
	std::uint16_t vendor = 0, product = 0;
	std::string name;
	Format format = Format::none;
	HidLayout layout;
	Target target;
};

std::mutex g_lock;
usb_pad_state g_state;         // under g_lock
bool g_connected = false;      // under g_lock
std::string g_name;            // under g_lock
std::atomic<bool> g_started{false};
std::atomic<bool> g_stop{false};
pthread_t g_thread;

void Log(const char *format, ...) __attribute__((format(printf, 1, 2)));
void Log(const char *format, ...) {
	char line[512];
	va_list args;
	va_start(args, format);
	std::vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	psp5::Trace("usbpad: %s", line);
}

void CloseDevice(Device *device) {
	if (device->fd >= 0) {
		close(device->fd);
	}
	if (device->control >= 0 && device->control != device->fd) {
		close(device->control);
	}
	*device = Device();
}

// Which decoding a device gets, from what is known of it. False means it is not
// ours and nothing more is done to it.
bool Classify(Device *device, const std::uint8_t *descriptor, std::size_t length) {
	if (SystemOwned(device->vendor, device->product)) {
		Log("%s: %04x:%04x is one of Sony's own controllers, so the system has it",
		    device->node.c_str(), device->vendor, device->product);
		return false;
	}
	if (device->target.xinput) {
		device->format = Format::xinput;
		return true;
	}
	if (device->vendor == 0x054C && device->product == 0x0268) {
		device->format = Format::ds3;
		return true;
	}
	if (length > 0) {
		device->layout = ParseHid(descriptor, length);
	}
	if (device->layout.keyboard && !device->layout.gamepad) {
		Log("%s: a keyboard or a mouse, not a pad", device->node.c_str());
		return false;
	}
	if (device->layout.ok && device->layout.gamepad) {
		device->format = Format::hid;
		return true;
	}
	Log("%s: %04x:%04x \"%s\": no gamepad in its %zu byte report descriptor, left alone",
	    device->node.c_str(), device->vendor, device->product, device->name.c_str(), length);
	return false;
}

void LogTaken(const Device &device, std::size_t descriptorLength) {
	Log("taken: %s, %04x:%04x \"%s\", as %s", device.node.c_str(), device.vendor, device.product,
	    device.name.c_str(), FormatName(device.format));
	if (device.format != Format::hid) {
		return;
	}
	int buttons = 0, axes = 0, hats = 0;
	for (const HidField &field : device.layout.fields) {
		buttons += field.page == 9;
		axes += field.page == 1 && field.usage >= 0x30 && field.usage <= 0x35;
		hats += field.page == 1 && field.usage == 0x39;
	}
	Log("  %zu byte descriptor: %d buttons, %d axes, %d hat%s, %s", descriptorLength, buttons, axes,
	    hats, hats == 1 ? "" : "s", device.layout.hasIds ? "report IDs" : "no report IDs");
}

// ---- route 1: /dev/uhidN --------------------------------------------------

bool TryUhid(const std::string &node, Device *device) {
	const std::string path = "/dev/" + node;
	const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		Log("%s: open: errno %d", path.c_str(), errno);
		return false;
	}
	*device = Device();
	device->node = path;

	usb_device_info info = {};
	if (ioctl(fd, USB_GET_DEVICEINFO, &info) == 0) {
		device->vendor = info.udi_vendorNo;
		device->product = info.udi_productNo;
		device->name = info.udi_product;
	}

	std::vector<std::uint8_t> descriptor(4096);
	usb_gen_descriptor request = {};
	request.ugd_data = descriptor.data();
	request.ugd_maxlen = (std::uint16_t)descriptor.size();
	const int rc = ioctl(fd, USB_GET_REPORT_DESC, &request);
	const std::size_t length =
	    rc == 0 ? std::min<std::size_t>(request.ugd_actlen, descriptor.size()) : 0;
	Log("%s: %04x:%04x \"%s\", report descriptor %s (%zu bytes)", path.c_str(), device->vendor,
	    device->product, device->name.c_str(), rc == 0 ? "read" : "not read", length);

	if (!Classify(device, descriptor.data(), length)) {
		close(fd);
		*device = Device();
		return false;
	}
	device->fd = fd;
	LogTaken(*device, length);
	return true;
}

// ---- route 2: /dev/ugenB.A ------------------------------------------------

bool ControlRequest(int fd, std::uint8_t type, std::uint8_t request, std::uint16_t value,
                    std::uint16_t index, void *data, std::uint16_t length,
                    std::uint16_t *actual) {
	usb_ctl_request r = {};
	r.ucr_data = data;
	r.ucr_flags = 0x0004;  // USB_SHORT_XFER_OK
	r.ucr_request.bmRequestType = type;
	r.ucr_request.bRequest = request;
	r.ucr_request.wValue[0] = value & 0xFF;
	r.ucr_request.wValue[1] = value >> 8;
	r.ucr_request.wIndex[0] = index & 0xFF;
	r.ucr_request.wIndex[1] = index >> 8;
	r.ucr_request.wLength[0] = length & 0xFF;
	r.ucr_request.wLength[1] = length >> 8;
	if (ioctl(fd, USB_DO_REQUEST, &r) != 0) {
		return false;
	}
	if (actual != nullptr) {
		*actual = r.ucr_actlen;
	}
	return true;
}

// The configuration descriptor, walked for an interface psp5 can read: a HID
// one, or the Xbox 360's vendor class. Returns false when the device has
// neither, which is how a USB drive or a hub is left alone.
bool FindInterface(const std::uint8_t *config, std::size_t length, Target *target,
                   bool *hid) {
	std::size_t at = 0;
	bool inside = false;
	bool wantedHid = false;
	bool wantedXInput = false;
	std::uint8_t iface = 0;
	while (at + 2 <= length) {
		const std::uint8_t size = config[at];
		const std::uint8_t type = config[at + 1];
		if (size < 2 || at + size > length) {
			break;
		}
		if (type == 0x04 && size >= 9) {  // interface
			const std::uint8_t number = config[at + 2];
			const std::uint8_t klass = config[at + 5];
			const std::uint8_t subclass = config[at + 6];
			inside = false;
			wantedHid = klass == 0x03;
			wantedXInput = klass == 0xFF && subclass == 0x5D;
			if (wantedHid || wantedXInput) {
				inside = true;
				iface = number;
			}
		} else if (type == 0x05 && size >= 7 && inside) {  // endpoint
			const std::uint8_t address = config[at + 2];
			const std::uint8_t attributes = config[at + 3];
			if ((address & 0x80) != 0 && (attributes & 0x03) == 0x03) {
				target->iface = iface;
				target->endpoint = address;
				target->packet = (std::uint16_t)(config[at + 4] | (config[at + 5] << 8));
				if (target->packet == 0 || target->packet > 64) {
					target->packet = 64;
				}
				target->xinput = wantedXInput;
				*hid = wantedHid;
				return true;
			}
		}
		at += size;
	}
	return false;
}

bool TryUgen(const std::string &node, Device *device) {
	const std::string path = "/dev/" + node;
	const int fd = open(path.c_str(), O_RDWR);
	if (fd < 0) {
		return false;
	}
	*device = Device();
	device->node = path;

	usb_device_info info = {};
	if (ioctl(fd, USB_GET_DEVICEINFO, &info) != 0) {
		// Sony's kernel answers ENOTTY here on some firmware. PS5SX2 then opens
		// the endpoint nodes blind and reads whatever answers; that is what
		// killed a tester's DualSense, so psp5 stops at a device it cannot name.
		close(fd);
		return false;
	}
	device->vendor = info.udi_vendorNo;
	device->product = info.udi_productNo;
	device->name = info.udi_product;
	if (SystemOwned(device->vendor, device->product)) {
		close(fd);
		return false;
	}

	std::uint8_t config[512] = {};
	std::uint16_t actual = 0;
	// GET_DESCRIPTOR, configuration 0: the header first for its total length,
	// then as much of it as fits.
	if (!ControlRequest(fd, 0x80, 0x06, 0x0200, 0, config, sizeof(config), &actual) ||
	    actual < 9) {
		Log("%s: %04x:%04x: no configuration descriptor", path.c_str(), device->vendor,
		    device->product);
		close(fd);
		return false;
	}

	bool hid = false;
	if (!FindInterface(config, actual, &device->target, &hid)) {
		Log("%s: %04x:%04x \"%s\": no HID or XInput interface, left alone", path.c_str(),
		    device->vendor, device->product, device->name.c_str());
		close(fd);
		return false;
	}

	std::vector<std::uint8_t> descriptor(4096);
	std::size_t length = 0;
	if (hid) {
		std::uint16_t got = 0;
		// GET_DESCRIPTOR, report (0x22), to the interface.
		if (ControlRequest(fd, 0x81, 0x06, 0x2200, device->target.iface, descriptor.data(),
		                   (std::uint16_t)descriptor.size(), &got)) {
			length = std::min<std::size_t>(got, descriptor.size());
		}
	}
	if (!Classify(device, descriptor.data(), length)) {
		close(fd);
		*device = Device();
		return false;
	}

	// The endpoint has its own node: /dev/usb/B.A.E, where E is its number.
	unsigned bus = 0, address = 0;
	if (std::sscanf(node.c_str(), "ugen%u.%u", &bus, &address) != 2) {
		close(fd);
		*device = Device();
		return false;
	}
	char endpoint[64];
	std::snprintf(endpoint, sizeof(endpoint), "/dev/usb/%u.%u.%u", bus, address,
	              device->target.endpoint & 0x0F);
	const int read_fd = open(endpoint, O_RDONLY | O_NONBLOCK);
	if (read_fd < 0) {
		Log("%s: %s: open: errno %d", path.c_str(), endpoint, errno);
		close(fd);
		*device = Device();
		return false;
	}
	device->control = fd;
	device->fd = read_fd;
	LogTaken(*device, length);
	return true;
}

// ---- the thread -----------------------------------------------------------

void ListNodes(std::vector<std::string> *uhid, std::vector<std::string> *ugen) {
	DIR *dir = opendir("/dev");
	if (dir == nullptr) {
		return;
	}
	while (const dirent *entry = readdir(dir)) {
		const std::string name = entry->d_name;
		if (name.rfind("uhid", 0) == 0) {
			uhid->push_back(name);
		} else if (name.rfind("ugen", 0) == 0) {
			ugen->push_back(name);
		}
	}
	closedir(dir);
	std::sort(uhid->begin(), uhid->end());
	std::sort(ugen->begin(), ugen->end());
}

bool Find(Device *device, bool first) {
	std::vector<std::string> uhid, ugen;
	ListNodes(&uhid, &ugen);
	if (first) {
		Log("looking: %zu uhid node(s), %zu ugen node(s)", uhid.size(), ugen.size());
	}
	for (const std::string &node : uhid) {
		if (TryUhid(node, device)) {
			return true;
		}
	}
	for (const std::string &node : ugen) {
		if (TryUgen(node, device)) {
			return true;
		}
	}
	return false;
}

void Publish(const usb_pad_state &state, bool connected, const std::string &name) {
	std::lock_guard<std::mutex> hold(g_lock);
	g_state = state;
	g_connected = connected;
	g_name = name;
}

void *Run(void *) {
	Device device;
	bool first = true;
	double nextScan = 0.0;
	std::vector<std::uint8_t> report(64);
	unsigned reads = 0;

	while (!g_stop.load(std::memory_order_relaxed)) {
		if (device.fd < 0) {
			// A pad plugged in later should still work, but a scan opens device
			// nodes, so it is once every two seconds and not every pass.
			const double now = now_seconds();
			if (now >= nextScan) {
				nextScan = now + 2.0;
				if (Find(&device, first)) {
					reads = 0;
					Publish(usb_pad_state{}, true, device.name.empty() ? "USB controller"
					                                                   : device.name);
				}
				first = false;
			}
			if (device.fd < 0) {
				usleep(100 * 1000);
				continue;
			}
		}

		const ssize_t got = read(device.fd, report.data(), report.size());
		if (got > 0) {
			Common state;
			bool ok = false;
			switch (device.format) {
				case Format::hid:
					ok = DecodeHid(device.layout, report.data(), (std::size_t)got, &state);
					break;
				case Format::ds3:
					ok = DecodeDs3(report.data(), (std::size_t)got, &state);
					break;
				case Format::xinput:
					ok = DecodeXInput(report.data(), (std::size_t)got, &state);
					break;
				default: break;
			}
			if (ok) {
				// The first few reports go into the log: when a pad maps wrongly
				// this is the only record of what it actually sent.
				if (reads < 8) {
					Log("report %u: buttons %04x hat %u sticks %u,%u %u,%u triggers %u,%u", reads,
					    state.buttons, state.hat, state.lx, state.ly, state.rx, state.ry, state.l2,
					    state.r2);
				}
				++reads;
				Publish(ToPad(state), true, device.name);
			}
			continue;
		}
		if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			usleep(2 * 1000);
			continue;
		}
		// Unplugged, or the node went away: back to looking.
		Log("%s: read: errno %d, letting go", device.node.c_str(), errno);
		CloseDevice(&device);
		Publish(usb_pad_state{}, false, std::string());
	}

	CloseDevice(&device);
	Publish(usb_pad_state{}, false, std::string());
	return nullptr;
}

}  // namespace

extern "C" {

void usb_pad_start(void) {
	if (g_started.exchange(true)) {
		return;
	}
	// The way out without a rebuild. Reading a stranger's USB device is the
	// one thing here that can go wrong for the whole console rather than just
	// for psp5, so there is a file that stops it happening at all.
	const std::string off = std::string(PS5Paths::kRoot) + "/nousbpad";
	if (access(off.c_str(), F_OK) == 0) {
		psp5::Trace("usbpad: off (%s is there)", off.c_str());
		return;
	}
	g_stop.store(false);
	if (pthread_create(&g_thread, nullptr, Run, nullptr) != 0) {
		psp5::Trace("usbpad: could not start the reader thread");
		g_started.store(false);
	}
}

bool usb_pad_read(struct usb_pad_state *out) {
	std::lock_guard<std::mutex> hold(g_lock);
	if (!g_connected) {
		return false;
	}
	*out = g_state;
	return true;
}

const char *usb_pad_name(void) {
	// Copied out, not pointed at: g_name belongs to the reader thread, which
	// replaces it when a device goes away, and the caller would be reading the
	// freed buffer of the string that used to be there.
	static char name[128];
	std::lock_guard<std::mutex> hold(g_lock);
	if (!g_connected || g_name.empty()) {
		return nullptr;
	}
	std::snprintf(name, sizeof(name), "%s", g_name.c_str());
	return name;
}

void usb_pad_stop(void) {
	if (!g_started.load()) {
		return;
	}
	g_stop.store(true);
	pthread_join(g_thread, nullptr);
	g_started.store(false);
}

}  // extern "C"
