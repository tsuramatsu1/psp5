// psp5 - PPSSPP as a PlayStation 5 title: the entry point, the host contract and
// the run loop.
//
// Copyright (C) 2026 the psp5 authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is to the console what SDL/SDLMain.cpp is to a desktop: it brings the
// platform up, answers the System_* calls PPSSPP makes of its host, turns the
// console's pad into PPSSPP's input events, and runs NativeFrame. PPSSPP's own UI
// is the frontend - psp5 does not add one.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "ppsspp_config.h"

#include "Common/Audio/AudioBackend.h"
#include "Common/CommonFuncs.h"
#include "Common/Input/InputState.h"
#include "Common/Input/KeyCodes.h"
#include "Common/Log.h"
#include "Common/Log/LogManager.h"
#include "Common/System/NativeApp.h"
#include "Common/System/System.h"
#include "Common/TimeUtil.h"
#include "Core/Config.h"
#include "Core/KeyMap.h"
#include "Core/System.h"

#include "PS5Audio.h"
#include "PS5Log.h"
#include "PS5Paths.h"
#include "PS5VulkanContext.h"
#include "ui/PS5AuroraLauncher.h"
#include "platform/platform.h"

namespace {

PS5VulkanContext *g_graphics = nullptr;
bool g_quit = false;

// ---------------------------------------------------------------------------
// The pad
// ---------------------------------------------------------------------------
//
// The console reports buttons as a bitfield; PPSSPP wants an event per edge. The
// table is the console's DualSense in the arrangement PPSSPP expects of a generic
// pad, which is what its default mapping is written against: cross is the "A"
// button, circle "B", square "X", triangle "Y".
struct ButtonMap {
	uint32_t pad;
	InputKeyCode code;
};

// These are the codes PPSSPP's own defaultPadMap binds for DEVICE_ID_PAD_0
// (Core/KeyMapDefaults.cpp), not the names they read like. Sending the
// obvious-looking NKCODE_BUTTON_A/B/X/Y instead leaves every face button
// unmapped, because nothing in the default map mentions them - only the d-pad
// worked, since NKCODE_DPAD_* is what the map really wants.
//
//   cross  BUTTON_2    triangle BUTTON_1    L1 BUTTON_7   options   BUTTON_10
//   circle BUTTON_3    square   BUTTON_4    R1 BUTTON_8   touch pad BUTTON_9
//
// L3 and R3 are deliberately not in PPSSPP's default map; they are sent anyway so
// they can be bound in Controls without psp5 changing.
constexpr ButtonMap kButtons[] = {
    {PAD_CROSS, NKCODE_BUTTON_2},     // CTRL_CROSS
    {PAD_CIRCLE, NKCODE_BUTTON_3},    // CTRL_CIRCLE
    {PAD_SQUARE, NKCODE_BUTTON_4},    // CTRL_SQUARE
    {PAD_TRIANGLE, NKCODE_BUTTON_1},  // CTRL_TRIANGLE
    {PAD_L1, NKCODE_BUTTON_7},        // CTRL_LTRIGGER
    {PAD_R1, NKCODE_BUTTON_8},        // CTRL_RTRIGGER
    {PAD_OPTIONS, NKCODE_BUTTON_10},  // CTRL_START
    {PAD_TOUCH_PAD, NKCODE_BUTTON_9}, // CTRL_SELECT
    {PAD_UP, NKCODE_DPAD_UP},
    {PAD_DOWN, NKCODE_DPAD_DOWN},
    {PAD_LEFT, NKCODE_DPAD_LEFT},
    {PAD_RIGHT, NKCODE_DPAD_RIGHT},
    {PAD_L3, NKCODE_BUTTON_THUMBL},
    {PAD_R3, NKCODE_BUTTON_THUMBR},
};

// What each player's buttons were at the previous poll, so only edges are sent.
uint32_t g_previousButtons[PAD_PLAYERS] = {};

// Which players PPSSPP has been told about, one bit each.
//
// Announcing a pad is not cosmetic: KeyMap::NotifyPadConnected is what makes
// PPSSPP run AutoConfForPad and install DEFAULT_MAPPING_PAD for that device. Until
// it does, the only bindings that answer are the device-independent ones - which
// is the d-pad and nothing else, so every face button does nothing. Every other
// port announces its pad (SDL, Windows, Android); psp5 did not.
uint32_t g_announcedPlayers = 0;

void AnnouncePad(int player) {
	if (g_announcedPlayers & (1u << player)) {
		return;
	}
	g_announcedPlayers |= 1u << player;
	// The name picks the default mapping and is what the controls screen shows.
	char name[32];
	snprintf(name, sizeof(name), "DualSense %d", player + 1);
	KeyMap::NotifyPadConnected((InputDeviceID)(DEVICE_ID_PAD_0 + player), name);
	say("pad: player %d announced to PPSSPP as \"%s\"", player, name);
}

void SendButtonEdges(int player, uint32_t held, uint32_t previous) {
	const InputDeviceID device = (InputDeviceID)(DEVICE_ID_PAD_0 + player);
	for (const ButtonMap &entry : kButtons) {
		const bool now = (held & entry.pad) != 0;
		const bool before = (previous & entry.pad) != 0;
		if (now == before) {
			continue;
		}
		KeyInput key{};
		key.deviceId = device;
		key.keyCode = entry.code;
		key.flags = now ? KeyInputFlags::DOWN : KeyInputFlags::UP;
		NativeKey(key);
	}
}

void SendAxes(int player, const pad &state) {
	const InputDeviceID device = (InputDeviceID)(DEVICE_ID_PAD_0 + player);
	// PPSSPP's sticks are y-up; the platform layer reports y-down, as the console
	// does, so Y is negated here rather than in the vendored platform layer.
	const AxisInput axes[] = {
	    {device, JOYSTICK_AXIS_X, state.left_x},
	    {device, JOYSTICK_AXIS_Y, -state.left_y},
	    {device, JOYSTICK_AXIS_Z, state.right_x},
	    {device, JOYSTICK_AXIS_RZ, -state.right_y},
	    {device, JOYSTICK_AXIS_LTRIGGER, state.l2},
	    {device, JOYSTICK_AXIS_RTRIGGER, state.r2},
	};
	NativeAxis(axes, ARRAY_SIZE(axes));
}

void PollInput() {
	pad state{};
	pad_poll(&state);

	const uint32_t players = pad_players();
	for (int player = 0; player < PAD_PLAYERS; player++) {
		if (!(players & (1u << player))) {
			// A controller that went away releases whatever it was holding, so a
			// button does not stay down forever in the emulator.
			if (g_previousButtons[player]) {
				SendButtonEdges(player, 0, g_previousButtons[player]);
				g_previousButtons[player] = 0;
			}
			continue;
		}
		pad current{};
		if (player == 0) {
			current = state;
		} else if (!pad_player(player, &current)) {
			continue;
		}
		// First time this player's controller is seen, so PPSSPP maps it.
		AnnouncePad(player);
		// A reading taken while the shell holds the pad (the home screen, a system
		// dialog) is not input for the title.
		if (current.held & PAD_INTERCEPTED) {
			continue;
		}
		SendButtonEdges(player, current.held, g_previousButtons[player]);
		g_previousButtons[player] = current.held;
		SendAxes(player, current);
	}
}

}  // namespace

// ---------------------------------------------------------------------------
// The host contract
// ---------------------------------------------------------------------------

void System_Toast(std::string_view text) {
	say("toast: %.*s", (int)text.size(), text.data());
}

void System_ShowKeyboard() {
	// The console's on-screen keyboard is a system dialog psp5 does not yet drive.
	// SYSPROP_HAS_KEYBOARD is false, so PPSSPP uses its own instead of waiting.
}

void System_Vibrate(int length_ms) {
	if (length_ms <= 0) {
		pad_vibrate(0.0f, 0.0f);
		return;
	}
	pad_vibrate(0.6f, 0.6f);
}

void System_LaunchUrl(LaunchUrlType urlType, std::string_view url) {
	// No browser is reachable from a title.
	say("launch url ignored: %.*s", (int)url.size(), url.data());
}

void System_AskForPermission(SystemPermission permission) {}

PermissionStatus System_GetPermissionStatus(SystemPermission permission) {
	return PERMISSION_STATUS_GRANTED;
}

std::string System_GetProperty(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_NAME:
		return "PlayStation 5";
	case SYSPROP_LANGREGION:
		return "en_US";
	case SYSPROP_BUILD_VERSION:
		return PPSSPP_GIT_VERSION;
	case SYSPROP_GPUDRIVER_VERSION:
		return "RADV";
	default:
		return "";
	}
}

std::vector<std::string> System_GetPropertyStringVec(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_TEMP_DIRS:
		return {PS5Paths::Cache()};
	default:
		return {};
	}
}

int64_t System_GetPropertyInt(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_DISPLAY_XRES:
		return g_graphics ? g_graphics->Width() : 1920;
	case SYSPROP_DISPLAY_YRES:
		return g_graphics ? g_graphics->Height() : 1080;
	case SYSPROP_DISPLAY_COUNT:
		return 1;
	case SYSPROP_AUDIO_SAMPLE_RATE:
		return 48000;
	case SYSPROP_DEVICE_TYPE:
		return DEVICE_TYPE_TV;
	default:
		return -1;
	}
}

float System_GetPropertyFloat(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_DISPLAY_REFRESH_RATE:
		return 60.0f;
	case SYSPROP_DISPLAY_SAFE_INSET_LEFT:
	case SYSPROP_DISPLAY_SAFE_INSET_RIGHT:
	case SYSPROP_DISPLAY_SAFE_INSET_TOP:
	case SYSPROP_DISPLAY_SAFE_INSET_BOTTOM:
		return 0.0f;
	default:
		return -1.0f;
	}
}

bool System_GetPropertyBool(SystemProperty prop) {
	switch (prop) {
	// PPSSPP's own file browser and on-screen keyboard are used instead of system
	// dialogs: a title cannot open the console's pickers, and a request it cannot
	// answer would leave the UI waiting on a callback that never comes.
	case SYSPROP_HAS_FILE_BROWSER:
	case SYSPROP_HAS_FOLDER_BROWSER:
	case SYSPROP_HAS_IMAGE_BROWSER:
	case SYSPROP_HAS_TEXT_INPUT_DIALOG:
	case SYSPROP_HAS_KEYBOARD:
	case SYSPROP_HAS_ACCELEROMETER:
	case SYSPROP_HAS_BACK_BUTTON:
	case SYSPROP_HAS_TEXT_CLIPBOARD:
	case SYSPROP_HAS_LOGIN_DIALOG:
	case SYSPROP_HAS_OPEN_DIRECTORY:
	case SYSPROP_CAN_CREATE_SHORTCUT:
	case SYSPROP_CAN_SHOW_FILE:
	case SYSPROP_SUPPORTS_HTTPS:
	case SYSPROP_DEBUGGER_PRESENT:
		return false;
	default:
		return false;
	}
}

void System_Notify(SystemNotification notification) {}

bool System_MakeRequest(SystemRequestType type, int requestId, const std::string &param1,
                        const std::string &param2, int64_t param3, int64_t param4) {
	switch (type) {
	case SystemRequestType::EXIT_APP:
	case SystemRequestType::RESTART_APP:
		// A title cannot restart itself in place, so both end the run and let the
		// shell take over.
		g_quit = true;
		return true;
	default:
		// Anything psp5 does not implement must be refused rather than ignored: a
		// request neither answered nor refused leaves PPSSPP's UI waiting.
		return false;
	}
}

// System_PostUIMessage and System_RunOnMainThread are deliberately not here:
// UI/NativeApp.cpp defines both for any build that includes PPSSPP's UI, which a
// standalone title does. Only the libretro and headless builds, which leave the UI
// out, supply their own.

std::vector<std::string> System_GetCameraDeviceList() { return {}; }

bool System_AudioRecordingIsAvailable() { return false; }

bool System_AudioRecordingState() { return false; }

AudioBackend *System_CreateAudioBackend() { return new PS5AudioBackend(); }

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
	// Standard error into klog and the shell's splash dismissed, before anything
	// that might log or fail.
	platform_init("psp5");
	say("psp5 - PPSSPP %s on the PlayStation 5", PPSSPP_GIT_VERSION);

	if (!PS5Paths::Prepare()) {
		psp5::Trace("fatal: cannot create the memory stick under %s", PS5Paths::kRoot);
		return 1;
	}

	psp5::OpenTrace();
	PS5Paths::Probe();

	if (!pad_open()) {
		// Not fatal: the title still boots and its UI says there is no controller.
		say("warning: no controller");
	}

	const std::string memstick = PS5Paths::Memstick();
	const std::string assets = PS5Paths::Assets();
	const std::string cache = PS5Paths::Cache();

	// There is no getcwd and no $HOME on the console, so every base path is handed
	// to PPSSPP explicitly rather than derived.
	const char *args[] = {"psp5"};
	NativeInit(ARRAY_SIZE(args), args, memstick.c_str(), assets.c_str(), cache.c_str());

	// PPSSPP's own logs into klog, through the stderr capture platform_init set up.
	// Without this a release build keeps them to itself: the first console runs
	// showed psp5's own lines and nothing from the emulator, so a failure inside
	// PPSSPP - "Failed to generate UI atlas!" among them - was invisible and had to
	// be inferred from where it crashed. Noisy, and worth it during bring-up.
	g_logManager.SetAllLogLevels(LogLevel::LINFO);
	g_logManager.SetAllLogEnable(true);
	g_logManager.SetOutputsEnabled(LogOutput::Stdio);

	g_graphics = new PS5VulkanContext();
	std::string error;
	if (!g_graphics->InitDevice(&error)) {
		psp5::Trace("fatal: graphics: %s", error.c_str());
		delete g_graphics;
		g_graphics = nullptr;
		NativeShutdown();
		return 1;
	}

	// The home screen, before PPSSPP's render manager exists: the kit and the
	// render manager each assume they own the frame loop, so they take turns. The
	// launcher gives the device back exactly as it found it; if it cannot start,
	// the title simply goes on to PPSSPP's own interface.
	{
		VulkanContext *vk = g_graphics->vulkan();
		psp5::AuroraDevice gpu;
		gpu.instance = (std::uint64_t)vk->GetInstance();
		gpu.physicalDevice = (std::uint64_t)vk->GetCurrentPhysicalDevice();
		gpu.device = (std::uint64_t)vk->GetDevice();
		gpu.queue = (std::uint64_t)vk->GetGraphicsQueue();
		gpu.swapchain = (std::uint64_t)vk->GetSwapchain();
		gpu.queueFamily = (std::uint32_t)vk->GetGraphicsQueueFamilyIndex();
		gpu.swapchainFormat = (std::uint32_t)vk->GetSwapchainFormat();
		gpu.width = vk->GetBackbufferWidth();
		gpu.height = vk->GetBackbufferHeight();
		psp5::RunAuroraLauncher(gpu);
	}

	if (!g_graphics->InitDraw(&error)) {
		psp5::Trace("fatal: graphics: %s", error.c_str());
		g_graphics->Shutdown();
		delete g_graphics;
		g_graphics = nullptr;
		NativeShutdown();
		return 1;
	}

	if (!NativeInitGraphics(g_graphics)) {
		psp5::Trace("fatal: NativeInitGraphics failed");
		g_graphics->Shutdown();
		delete g_graphics;
		g_graphics = nullptr;
		NativeShutdown();
		return 1;
	}

	psp5::Trace("running");
	while (!g_quit) {
		PollInput();
		NativeFrame(g_graphics);
		g_graphics->Poll();
	}

	say("shutting down");
	NativeShutdownGraphics();
	g_graphics->Shutdown();
	delete g_graphics;
	g_graphics = nullptr;
	NativeShutdown();

	// Returning is the exit: the platform layer's catchReturnFromMain asks the
	// shell to close the title. Calling exit() or letting _start return would be
	// reported as a crash.
	return 0;
}
