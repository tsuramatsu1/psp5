# psp5 - generates patches/ppsspp/ps5-standalone.patch.
#
# Copyright (C) 2026 the psp5 authors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The patch in patches/ is what tools/build.sh applies, and it is the thing the
# build depends on - but a 240-line diff is not where these edits can be read or
# argued with. This is: each one anchored to the exact text it replaces, with the
# reason beside it, and an assert so a change in the pinned PPSSPP fails loudly
# here instead of applying somewhere unintended.
#
# Run it against a checkout of the pinned PPSSPP, then diff that checkout:
#
#   python3 tools/mkpatch.py <path-to-ppsspp-src>
#   git -C <path-to-ppsspp-src> diff > patches/ppsspp/ps5-standalone.patch
#
# tools/build.sh creates that checkout at ${PSP5_WORK}/.deps/ppsspp-src.

import pathlib
import sys

D = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else '/root/psp5/.deps/ppsspp-src')
if not (D / 'CMakeLists.txt').exists():
	raise SystemExit('not a PPSSPP checkout: %s' % D)


def write(path, text):
	# Explicit LF. A CRLF line in a patched source becomes a CRLF line in the
	# patch, and the shell scripts this build generates fail on the first of
	# them with "pipefail: invalid option name".
	path.write_bytes(text.replace('\n', '\n').encode('utf-8'))

# ---- VulkanLoader.cpp (unchanged from the first version) -----------------
p = D / 'Common/GPU/Vulkan/VulkanLoader.cpp'
t = p.read_text()

t = t.replace("""#elif PPSSPP_PLATFORM(WINDOWS)
typedef HINSTANCE VulkanLibraryHandle;
static VulkanLibraryHandle vulkanLibrary;
#define dlsym(x, y) GetProcAddress(x, y)
#else""", """#elif PPSSPP_PLATFORM(WINDOWS)
typedef HINSTANCE VulkanLibraryHandle;
static VulkanLibraryHandle vulkanLibrary;
#define dlsym(x, y) GetProcAddress(x, y)
#elif PPSSPP_PLATFORM(PS5)
// The console has no Vulkan loader, and no dlopen for a title's own code. RADV is
// linked into the title instead and reached through the one entry point its
// archive exports, so the "library" here is only a non-null sentinel and every
// global function is resolved by psp5's src/PS5VulkanLoader.cpp.
typedef void *VulkanLibraryHandle;
static VulkanLibraryHandle vulkanLibrary;
extern "C" void *ps5_vk_global_proc(const char *name);
#define dlsym(x, y) ps5_vk_global_proc(y)
#else""", 1)

t = t.replace("""static VulkanLibraryHandle VulkanLoadLibrary(std::string *errorString) {
#if PPSSPP_PLATFORM(SWITCH)""", """static VulkanLibraryHandle VulkanLoadLibrary(std::string *errorString) {
#if PPSSPP_PLATFORM(PS5)
	// Statically linked: there is nothing to open, but the handle must be non-null
	// for VulkanLoad to go on and resolve the functions through the driver.
	return (VulkanLibraryHandle)1;
#elif PPSSPP_PLATFORM(SWITCH)""", 1)

t = t.replace("""	if (h) {
#if PPSSPP_PLATFORM(SWITCH)
		// Can't load, and can't free.
#elif PPSSPP_PLATFORM(WINDOWS)""", """	if (h) {
#if PPSSPP_PLATFORM(SWITCH)
		// Can't load, and can't free.
#elif PPSSPP_PLATFORM(PS5)
		// A sentinel, not a mapping: there is nothing to close.
#elif PPSSPP_PLATFORM(WINDOWS)""", 1)

t = t.replace("""bool VulkanMayBeAvailable() {
#if PPSSPP_PLATFORM(IOS)""", """bool VulkanMayBeAvailable() {
#if PPSSPP_PLATFORM(PS5)
	// RADV is linked into the title: if this build exists, Vulkan is there. The
	// probe below creates a throwaway instance and device to find out, which on the
	// console is a second device creation that buys nothing.
	g_vulkanAvailabilityChecked = true;
	g_vulkanMayBeAvailable = true;
	return true;
#elif PPSSPP_PLATFORM(IOS)""", 1)
write(p, t)

# ---- CMakeLists.txt ------------------------------------------------------
p = D / 'CMakeLists.txt'
t = p.read_text()

old = """elseif(WIN32)
	# Don't care about SDL.
	set(TargetBin PPSSPPWindows)
elseif(LIBRETRO)
else()"""
new = """elseif(WIN32)
	# Don't care about SDL.
	set(TargetBin PPSSPPWindows)
elseif(LIBRETRO)
elseif(PPSSPP_PS5)
	# psp5: a standalone title for the PlayStation 5. There is no window system and
	# no SDL in the SDK sysroot, so the platform layer that would be SDL/ lives
	# outside this tree, in the psp5 repository; PSP5_SRC_DIR points at it. PPSSPP's
	# own UI is the frontend, so NativeAppSource is used unchanged.
	#
	# TargetBin is deliberately left unset: CMake does not link this title. The
	# console's link is prospero-lld with a linker script, RADV whole-archived and a
	# native tool that turns the ELF into the console's format, so psp5's
	# tools/link-title.sh does it and CMake's job is to produce the archives. They
	# are added below, once NativeAppSource is complete.
	if(NOT PSP5_SRC_DIR)
		message(FATAL_ERROR "PPSSPP_PS5 is set but PSP5_SRC_DIR is not")
	endif()
	include_directories(${PSP5_SRC_DIR})
else()"""
assert t.count(old) == 1
t = t.replace(old, new, 1)

old = """if(TargetBin)
	if(APPLE)"""
# Raw: the CMake regexes below contain \. , which Python would read as an escape.
new = r"""if(PPSSPP_PS5)
	# The two archives tools/link-title.sh links. They are separate because only the
	# platform one is whole-archived: main() is reached from the CRT and the System_*
	# definitions from PPSSPP's own code, and an archive member that nothing
	# references strongly is dropped by the linker.
	add_library(psp5_app STATIC ${NativeAppSource})
	target_link_libraries(psp5_app ${LinkCommon} Common)

	add_library(psp5_platform STATIC
		${PSP5_SRC_DIR}/PS5Main.cpp
		${PSP5_SRC_DIR}/PS5Audio.cpp
		${PSP5_SRC_DIR}/PS5Achievements.cpp
		${PSP5_SRC_DIR}/net/PS5HttpRequest.cpp
		${PSP5_SRC_DIR}/net/console_curl.c
		${PSP5_SRC_DIR}/PS5Overlay.cpp
		${PSP5_SRC_DIR}/PS5Paths.cpp
		${PSP5_SRC_DIR}/PS5Log.cpp
		${PSP5_SRC_DIR}/PS5VulkanContext.cpp
		${PSP5_SRC_DIR}/PS5VulkanLoader.cpp
		${PSP5_SRC_DIR}/PS5Memory.cpp
		${PSP5_SRC_DIR}/PS5GLStubs.cpp
		${PSP5_SRC_DIR}/PS5LibcShims.cpp
		${PSP5_SRC_DIR}/platform/platform.c
	)
	# Only here: see the note where PSP5_CURL_PREFIX is set.
	target_include_directories(psp5_platform PRIVATE ${PSP5_CURL_PREFIX}/include)
	target_link_libraries(psp5_platform psp5_app)

	# The Aurora Shelf launcher, from PS5_VKHomebrewUI's kit (tools/setup-kit.sh).
	# Optional: without PSP5_UI_KIT the title is PPSSPP's own UI alone.
	if(PSP5_UI_KIT)
		file(GLOB_RECURSE psp5_kit_sources ${PSP5_UI_KIT}/src/*.cpp ${PSP5_UI_KIT}/src/*.c)
		# Not the kit's own console entry point, platform layer or heap - psp5 brings
		# up the console itself. Not its OpenGL backend either: there is no GL here,
		# and those files want GL/glcorearb.h.
		list(FILTER psp5_kit_sources EXCLUDE REGEX "/src/(main\.cpp|platform/|runtime/)")
		list(FILTER psp5_kit_sources EXCLUDE REGEX "/src/gfx/(gl_[a-z_]+|backdrop|canvas)\.cpp$")
		# Nor the kit's Aurora: psp5 ships its own copy of that one design, in
		# src/ui/kit/aurora.cpp, reading the memory stick in place of the kit's
		# sample catalogue. Both files define hui::concepts::make_aurora.
		list(FILTER psp5_kit_sources EXCLUDE REGEX "/src/concepts/aurora\.cpp$")
		# Third-party code inside the kit, built as it stands.
		set_source_files_properties(${psp5_kit_sources} PROPERTIES COMPILE_OPTIONS "-w")
		# volk supplies the vkXxx entry points as function pointers. The kit calls
		# them directly, and there is no libvulkan to link on the console - RADV is
		# an archive reached through one symbol - so without this every Vulkan call
		# in the kit is an undefined symbol at link time.
		add_library(psp5_ui STATIC
			${psp5_kit_sources}
			${PSP5_SRC_DIR}/ui/volk/volk.c
			${PSP5_SRC_DIR}/ui/HuiPlatform.cpp
			${PSP5_SRC_DIR}/ui/PS5AuroraLauncher.cpp
			${PSP5_SRC_DIR}/ui/PS5GameArt.cpp
			${PSP5_SRC_DIR}/ui/PS5GameLibrary.cpp
			${PSP5_SRC_DIR}/ui/PS5GameSound.cpp
			${PSP5_SRC_DIR}/ui/PS5Keyboard.cpp
			${PSP5_SRC_DIR}/ui/PS5Cheats.cpp
			${PSP5_SRC_DIR}/ui/PS5Settings.cpp
			${PSP5_SRC_DIR}/ui/kit/aurora.cpp
		)
		target_include_directories(psp5_ui PUBLIC ${PSP5_UI_KIT}/src ${PSP5_SRC_DIR}
			${PSP5_SRC_DIR}/ui/volk)
		target_compile_features(psp5_ui PUBLIC cxx_std_20)
		target_compile_definitions(psp5_ui PUBLIC PS5_UI VK_NO_PROTOTYPES)
		target_compile_options(psp5_ui PRIVATE -include volk.h)
		target_link_libraries(psp5_platform psp5_ui)
	endif()
endif()

if(TargetBin)
	if(APPLE)"""
assert t.count(old) == 1
t = t.replace(old, new, 1)
write(p, t)

# ---- UI/NativeApp.cpp: the title's paths, not the environment's -----------
p = D / 'UI/NativeApp.cpp'
t = p.read_text()
old = """	g_Config.flash0Directory = g_Config.internalDataDirectory / "assets/flash0";
#elif !PPSSPP_PLATFORM(WINDOWS)"""
new = """	g_Config.flash0Directory = g_Config.internalDataDirectory / "assets/flash0";
#elif PPSSPP_PLATFORM(PS5)
	// The console has no $HOME, no $XDG_CONFIG_HOME, and no current directory worth
	// trusting: getcwd is not provided. So these are the paths the title passed to
	// NativeInit, not anything derived from the environment. Without this branch the
	// generic one below puts the memory stick at a relative ./config/ppsspp, which on
	// the console resolves nowhere, and saves are silently lost.
	g_Config.memStickDirectory = Path(savegame_dir);
	g_Config.flash0Directory = Path(external_dir) / "flash0";
	g_Config.defaultCurrentDirectory = Path(savegame_dir);
#elif !PPSSPP_PLATFORM(WINDOWS)"""
assert t.count(old) == 1
t = t.replace(old, new, 1)
write(p, t)

# ---- Core/Config.cpp: the default backend ---------------------------------
p = D / 'Core/Config.cpp'
t = p.read_text()
old = """static int DefaultGPUBackend() {
	if (IsVREnabled()) {
		return (int)GPUBackend::OPENGL;
	}
"""
new = """static int DefaultGPUBackend() {
#if PPSSPP_PLATFORM(PS5)
	// Vulkan is the only backend that exists here: PPSSPP_API_ANY_GL is 0 for the
	// console, so no GL backend is compiled in at all. Without this the chain below
	// falls through to its OPENGL default, PPSSPP brings up a backend that is not
	// there, and the run dies before the first frame - and because it records the
	// attempt in FailedGraphicsBackends.txt first, the next launch reads OPENGL as
	// "already failed", tries Vulkan, dies again, and then has both marked failed.
	// From there NextValidBackend falls back to this function forever.
	return (int)GPUBackend::VULKAN;
#else
	if (IsVREnabled()) {
		return (int)GPUBackend::OPENGL;
	}
"""
assert t.count(old) == 1, "DefaultGPUBackend head"
t = t.replace(old, new, 1)

# Close the #else that was opened above, at the end of the function.
old = """	// TODO: On some additional Linux platforms, we should also default to Vulkan.
	return (int)GPUBackend::OPENGL;
}"""
new = """	// TODO: On some additional Linux platforms, we should also default to Vulkan.
	return (int)GPUBackend::OPENGL;
#endif
}"""
assert t.count(old) == 1, "DefaultGPUBackend tail"
t = t.replace(old, new, 1)
write(p, t)

# ---- VulkanContext.cpp: adopt the console display's own mode ---------------
p = D / 'Common/GPU/Vulkan/VulkanContext.cpp'
t = p.read_text()
old = """		// Free the mode list now.
		delete [] mode_props;"""
new = """#if PPSSPP_PLATFORM(PS5)
		// The console's display decides the mode; the title does not choose one.
		// Nothing has told PPSSPP a resolution by this point, so g_display is still
		// zero, the exact-match loop above can never hit, and the surface extent
		// further down (image_size, from g_display) would be 0x0 even if it did.
		// Take the first mode the display reports - Vulkan lists the preferred mode
		// first - and adopt it as the display resolution.
		if (display_mode == VK_NULL_HANDLE && mode_count > 0) {
			const VkDisplayModeParametersKHR &chosen = mode_props[0].parameters;
			display_mode = mode_props[0].displayMode;
			mode_found = true;
			g_display.pixel_xres = chosen.visibleRegion.width;
			g_display.pixel_yres = chosen.visibleRegion.height;
			INFO_LOG(Log::G3D, "PS5: adopting the display's preferred mode, %dx%d",
				g_display.pixel_xres, g_display.pixel_yres);
		}
#endif

		// Free the mode list now.
		delete [] mode_props;"""
assert t.count(old) == 1, "mode list anchor"
t = t.replace(old, new, 1)
write(p, t)

# ---- UI/NativeApp.cpp: register the title's assets with the VFS -------------
p = D / 'UI/NativeApp.cpp'
t = p.read_text()
old = """#if PPSSPP_PLATFORM(IOS) || PPSSPP_PLATFORM(MAC)
	// Packed assets are included in app
	g_VFS.Register("", new DirectoryReader(Path(external_dir)));
#endif"""
new = """#if PPSSPP_PLATFORM(IOS) || PPSSPP_PLATFORM(MAC)
	// Packed assets are included in app
	g_VFS.Register("", new DirectoryReader(Path(external_dir)));
#endif
#if PPSSPP_PLATFORM(PS5)
	// The title's own assets, at the absolute path main() passed in. Nothing else
	// below finds them on a console: ASSETS_DIR and the /usr/share entries are host
	// paths, there is no executable directory to resolve against, and the bare
	// "assets" entry is relative to a working directory the console does not
	// provide. Without this the VFS is empty - every asset read fails, starting
	// with the UI atlas, and the first unchecked getImage() crashes.
	g_VFS.Register("", new DirectoryReader(Path(external_dir)));
#endif"""
assert t.count(old) == 1, "VFS external_dir anchor"
t = t.replace(old, new, 1)

# The host-path block is not just useless here - File::GetExeDirectory() has no
# meaning for a title, and resolving it can reach getcwd, which the console does
# not provide.
old = """#if !defined(MOBILE_DEVICE) && !defined(_WIN32) && !PPSSPP_PLATFORM(SWITCH)
	g_VFS.Register("", new DirectoryReader(File::GetExeDirectory() / "assets"));"""
new = """#if !defined(MOBILE_DEVICE) && !defined(_WIN32) && !PPSSPP_PLATFORM(SWITCH) && !PPSSPP_PLATFORM(PS5)
	g_VFS.Register("", new DirectoryReader(File::GetExeDirectory() / "assets"));"""
assert t.count(old) == 1, "host paths anchor"
t = t.replace(old, new, 1)
write(p, t)

# ---- PPSSPP's own screens never reach the display -------------------------
# psp5 has its own home screen. PPSSPP is the emulator underneath it, and none
# of its interface - the logo, the game browser, the pause menu - is meant to be
# seen. Three edits are enough, because every route into that interface passes
# through one of them.

# 0. A switch that cannot be refused. ScreenManager::switchScreen keeps the first
# pending switch and silently deletes any later one, and PPSSPP always has one
# pending here: NativeInit queues its game browser at start-up, and EmuScreen
# queues it again when a game ends - neither is ever consumed, because psp5 only
# runs frames while a game is running. Without this the EmuScreen psp5 asks for
# is dropped and the browser it was replacing is what reaches the display.
p = D / 'Common/UI/Screen.h'
t = p.read_text()
old = """	void switchScreen(Screen *screen);
	void update();"""
new = """	void switchScreen(Screen *screen);
#if PPSSPP_PLATFORM(PS5)
	// As switchScreen, but replacing a pending switch rather than losing to it.
	void switchScreenNow(Screen *screen);
#endif
	void update();"""
assert t.count(old) == 1, "ScreenManager declaration anchor"
t = t.replace(old, new, 1)
write(p, t)

p = D / 'Common/UI/Screen.cpp'
t = p.read_text()
old = """void ScreenManager::cancelScreensAbove(Screen *screen) {"""
new = """#if PPSSPP_PLATFORM(PS5)
void ScreenManager::switchScreenNow(Screen *screen) {
	// The queued screen has only been constructed - it has never been made
	// current, so it has no views and nothing to unwind.
	for (Layer &layer : nextStack_) {
		delete layer.screen;
	}
	nextStack_.clear();
	switchScreen(screen);
}
#endif

void ScreenManager::cancelScreensAbove(Screen *screen) {"""
assert t.count(old) == 1, "cancelScreensAbove anchor"
t = t.replace(old, new, 1)
write(p, t)

# 1. A way in. PPSSPP boots a game named in argv, but psp5 does not know which
# game until its home screen has run, which is long after NativeInit. This hands
# PPSSPP an EmuScreen directly, so the first frame it ever draws is the game and
# neither the logo nor the browser is drawn even once.
p = D / 'UI/NativeApp.cpp'
t = p.read_text()
old = """void NativeFrame(GraphicsContext *graphicsContext) {"""
new = """#if PPSSPP_PLATFORM(PS5)
// psp5 drives the screen stack itself: its home screen picks the game and hands
// it straight to an EmuScreen. Going through REQUEST_GAME_BOOT instead would
// work, but a posted message is delivered after g_screenManager->update(), so
// switch would land a frame late and whatever PPSSPP had on screen - its logo,
// or its game browser - would be drawn once before the game appeared.
extern "C" void PS5_BootGame(const char *path) {
	g_screenManager->switchScreenNow(new EmuScreen(Path(path)));
}
#endif

void NativeFrame(GraphicsContext *graphicsContext) {"""
assert t.count(old) == 1, "NativeFrame anchor"
t = t.replace(old, new, 1)
write(p, t)

# 2. A way out. Every path that leaves a game - finishing, an error, the pause
# menu's "back to menu", a denied permission - ends in `new MainScreen()`, and
# MainScreen has exactly one constructor. Catching it there covers all of them,
# including any psp5 has not gone looking for. The screen is still constructed
# and still switched to, because unpicking that is PPSSPP's business; it simply
# never gets drawn: the switch takes effect in the next frame's update(), and
# psp5 leaves the frame loop before then.
p = D / 'UI/MainScreen.cpp'
t = p.read_text()
old = """MainScreen::MainScreen() {
	g_BackgroundAudio.SetGame(Path());
}"""
new = """#if PPSSPP_PLATFORM(PS5)
// Defined by psp5: PPSSPP is about to show its game browser, which on a console
// means the game has ended and psp5's home screen should come back.
extern "C" void PS5_NotifyGameEnded();
#endif

MainScreen::MainScreen() {
	g_BackgroundAudio.SetGame(Path());
#if PPSSPP_PLATFORM(PS5)
	PS5_NotifyGameEnded();
#endif
}"""
assert t.count(old) == 1, "MainScreen constructor anchor"
t = t.replace(old, new, 1)
write(p, t)

# 3. No pause menu. The one remaining screen a player can reach from inside a
# game. psp5 offers its own way out instead - OPTIONS held - which stops the
# game and returns to the shelf.
p = D / 'UI/EmuScreen.cpp'
t = p.read_text()
old = """	if (pauseTrigger_) {
		pauseTrigger_ = false;
		screenManager()->push(new GamePauseScreen(gamePath_, bootPending_));
	}"""
new = """	if (pauseTrigger_) {
		pauseTrigger_ = false;
#if !PPSSPP_PLATFORM(PS5)
		screenManager()->push(new GamePauseScreen(gamePath_, bootPending_));
#endif
	}"""
assert t.count(old) == 1, "pause trigger anchor"
t = t.replace(old, new, 1)
write(p, t)

# ---- the in-game cheat panel ----------------------------------------------
# psp5 draws its own panel over a running game (src/PS5Overlay.cpp). Two
# things have to come from inside PPSSPP: somewhere to draw at the end of the
# frame, and a way to make the engine re-read a cheat file psp5 has just edited.

p = D / 'Core/CwCheat.cpp'
t = p.read_text()
old = """void __CheatShutdown() {
	__CheatStop();
}"""
new = """void __CheatShutdown() {
	__CheatStop();
}

#if PPSSPP_PLATFORM(PS5)
// The engine keeps the codes it parsed when the game booted, so a cheat file
// edited underneath it changes nothing until it is read again. psp5s in-game
// panel edits that file, and calls this.
//
// __CheatStart is what reads it; stopping first is what makes it re-read rather
// than add to what it already has. Doing nothing when cheats are off is not a
// shortcut: __CheatStart would start the engine that the off switch just
// stopped.
extern "C" void PS5_ReloadCheats() {
	__CheatStop();
	if (g_Config.bEnableCheats) {
		__CheatStart();
	}
}
#endif"""
assert t.count(old) == 1, "CheatShutdown anchor"
t = t.replace(old, new, 1)
write(p, t)

p = D / 'UI/EmuScreen.cpp'
t = p.read_text()
old = """		if (g_Config.iShowStatusFlags) {
			DrawFPS(ctx, GetLayoutBounds(*ctx));
		}
	}"""
new = """		if (g_Config.iShowStatusFlags) {
			DrawFPS(ctx, GetLayoutBounds(*ctx));
		}
	}

#if PPSSPP_PLATFORM(PS5)
	// psp5s own panel, drawn over the game. Here rather than as a screen of its
	// own because a screen would be PPSSPP interface - this is a few rectangles
	// and some text, and it takes no part in the screen stack.
	PS5_DrawOverlays(ctx);
#endif"""
assert t.count(old) == 1, "renderUI anchor"
t = t.replace(old, new, 1)

# Before hasVisibleUI as well as renderUI, since both call into psp5.
old = """void EmuScreen::renderImDebugger() {"""
new = """#if PPSSPP_PLATFORM(PS5)
extern "C" void PS5_DrawOverlays(UIContext *ui);
#endif

void EmuScreen::renderImDebugger() {"""
assert t.count(old) == 1, "overlay declaration anchor"
t = t.replace(old, new, 1)

# renderUI is only called from render() inside `if (hasVisibleUI())`, and with no
# on-screen controls and no messages that is false on most frames. psp5s panels
# would then be drawn on the frames PPSSPP happened to have something to show and
# left off the rest, which on screen is a flicker.
old = """bool EmuScreen::hasVisibleUI() {
	// Regular but uncommon UI."""
new = """bool EmuScreen::hasVisibleUI() {
#if PPSSPP_PLATFORM(PS5)
	// psp5 draws its own panels at the end of renderUI. Nothing below knows
	// about them, so they would not keep renderUI running by themselves.
	if (PS5_WantsOverlay()) {
		return true;
	}
#endif
	// Regular but uncommon UI."""
assert t.count(old) == 1, "hasVisibleUI anchor"
t = t.replace(old, new, 1)

old = """#if PPSSPP_PLATFORM(PS5)
extern "C" void PS5_DrawOverlays(UIContext *ui);
#endif"""
new = """#if PPSSPP_PLATFORM(PS5)
extern "C" void PS5_DrawOverlays(UIContext *ui);
extern "C" bool PS5_WantsOverlay();
#endif"""
assert t.count(old) == 1, "overlay declarations anchor"
t = t.replace(old, new, 1)
write(p, t)

# ---- a game's SND0.AT3, for psp5's home screen ----------------------------
# PPSSPP plays this itself when a game is highlighted in its game grid, but only
# through its own audio path, and on psp5's home screen that is not running -
# the kit's mixer owns the device until a game starts. The RIFF parsing and the
# ATRAC decoder set-up are the fiddly part and they are already right here, so
# this exposes them; psp5 does the resampling and the playing.
p = D / 'UI/BackgroundAudio.cpp'
t = p.read_text()
old = """bool BackgroundAudio::Play() {"""
new = """#if PPSSPP_PLATFORM(PS5)
// Decodes a whole SND0.AT3 to interleaved stereo s16. The caller frees *out
// with free(). Returns false for anything that is not ATRAC psp5 can decode.
//
// Whole rather than streamed: a games jingle is a few seconds, psp5 wants to
// loop it under a menu, and decoding once means the audio thread only ever
// reads from memory.
extern "C" bool PS5_DecodeAtrac(const char *bytes, int size, short **out, int *outFrames,
								int *outRate) {
	*out = nullptr;
	*outFrames = 0;
	*outRate = 0;
	if (!bytes || size <= 0) {
		return false;
	}

	RIFFReader riff((const uint8_t *)bytes, size);
	WavData wave;
	if (!wave.Read(riff) || !wave.raw_data || wave.raw_bytes_per_frame <= 0) {
		return false;
	}
	if (wave.codec != PSP_CODEC_AT3 && wave.codec != PSP_CODEC_AT3PLUS) {
		return false;
	}

	const uint8_t *extraData = wave.codec == PSP_CODEC_AT3 ? &wave.at3_extradata[2] : nullptr;
	const size_t extraDataSize = wave.codec == PSP_CODEC_AT3 ? 14 : 0;
	AudioDecoder *decoder =
		CreateAudioDecoder((PSPAudioType)wave.codec, wave.sample_rate, wave.num_channels,
						   wave.raw_bytes_per_frame, extraData, extraDataSize);
	if (!decoder) {
		return false;
	}

	std::vector<int16_t> pcm;
	std::vector<int16_t> block(32 * 1024);
	for (int offset = 0; offset + wave.raw_bytes_per_frame <= wave.raw_data_size;
		 offset += wave.raw_bytes_per_frame) {
		int consumed = 0;
		int samples = 0;
		if (!decoder->Decode(wave.raw_data + offset, wave.raw_bytes_per_frame, &consumed, 2,
							 block.data(), &samples) ||
			samples <= 0) {
			break;
		}
		pcm.insert(pcm.end(), block.begin(), block.begin() + (size_t)samples * 2);
	}
	delete decoder;

	if (pcm.empty()) {
		return false;
	}
	*outFrames = (int)(pcm.size() / 2);
	*outRate = wave.sample_rate;
	*out = (short *)malloc(pcm.size() * sizeof(int16_t));
	if (!*out) {
		*outFrames = 0;
		return false;
	}
	memcpy(*out, pcm.data(), pcm.size() * sizeof(int16_t));
	return true;
}
#endif

bool BackgroundAudio::Play() {"""
assert t.count(old) == 1, "BackgroundAudio::Play anchor"
t = t.replace(old, new, 1)
write(p, t)

# ---- HTTPS on the console -------------------------------------------------
# PPSSPP turns HTTPS off for every platform that is not Windows, Apple or
# Android, and with it an https request returns a null handle and is never sent.
# That is why RetroAchievements did nothing at all: its API is https only.
#
# Its own HTTPS is naett, which cannot be used here for two reasons: it is a git
# submodule, so this patch cannot reach it, and it chooses its backend from
# platform macros the console does not set. psp5 drives PacBrew's libcurl
# directly instead (src/net/), which is built for this target already and is
# what ProsperoEden uses in a native title.
p = D / 'CMakeLists.txt'
t = p.read_text()
old = """if(NOT ANDROID AND NOT WIN32 AND (NOT APPLE OR IOS))
	set(HTTPS_NOT_AVAILABLE ON)
endif()"""
new = """if(NOT ANDROID AND NOT WIN32 AND (NOT APPLE OR IOS))
	set(HTTPS_NOT_AVAILABLE ON)
endif()
if(PPSSPP_PS5)
	# The prefix PacBrew builds into: headers and archives for libcurl and the
	# OpenSSL it was built against. HTTPS_NOT_AVAILABLE stays set, so naett is
	# never built; the factory below takes a PS5 branch instead.
	if(NOT PSP5_CURL_PREFIX)
		set(PSP5_CURL_PREFIX "/opt/ps5-payload-sdk/target/user/homebrew")
	endif()
	if(NOT EXISTS "${PSP5_CURL_PREFIX}/include/curl/curl.h")
		message(FATAL_ERROR "no curl headers under ${PSP5_CURL_PREFIX}; set PSP5_CURL_PREFIX")
	endif()
	# Deliberately not include_directories(): that prefix also carries a libpng,
	# and putting it in front of everything made PPSSPP compile against libpng16
	# while linking against its own bundled libpng17 - which only showed up as
	# five png_set_* symbols that suddenly did not exist. It goes on the one
	# target that asks for curl, below.
endif()"""
assert t.count(old) == 1, "HTTPS_NOT_AVAILABLE anchor"
t = t.replace(old, new, 1)
write(p, t)

p = D / 'Common/Net/HTTPRequest.cpp'
t = p.read_text()
old = """#ifndef HTTPS_NOT_AVAILABLE
		return std::make_shared<HTTPSRequest>(method, url, postdata, postMime, outfile, flags, name);
#else
		return std::shared_ptr<Request>();
#endif"""
new = """#ifndef HTTPS_NOT_AVAILABLE
		return std::make_shared<HTTPSRequest>(method, url, postdata, postMime, outfile, flags, name);
#elif PPSSPP_PLATFORM(PS5)
		// Fully qualified: this call is inside namespace http, where a bare
		// psp5 would be looked up as http::psp5 first.
		return ::psp5::CreateHttpsRequest(method, url, postdata, postMime, outfile, flags, name);
#else
		return std::shared_ptr<Request>();
#endif"""
assert t.count(old) == 1, "https factory anchor"
t = t.replace(old, new, 1)

# At the top of the file, not beside the factory: that sits inside namespace
# http, and an include there would declare psp5 as http::psp5.
old = """#include "Common/Net/HTTPRequest.h"
#include "Common/Net/HTTPClient.h"
"""
new = """#include "Common/Net/HTTPRequest.h"
#include "Common/Net/HTTPClient.h"
#if PPSSPP_PLATFORM(PS5)
#include "net/PS5HttpRequest.h"
#endif
"""
assert t.count(old) == 1, "factory declaration anchor"
t = t.replace(old, new, 1)
write(p, t)

# The home screen runs its own loop, so nothing was pumping the queue these
# requests travel on: a sign-in would be posted and then wait for ever.
p = D / 'UI/NativeApp.cpp'
t = p.read_text()
old = """extern "C" void PS5_BootGame(const char *path) {"""
new = """extern "C" void PS5_PumpNetwork() {
	g_DownloadManager.Update();
	Achievements::Idle();
}

extern "C" void PS5_BootGame(const char *path) {"""
assert t.count(old) == 1, "pump anchor"
t = t.replace(old, new, 1)
write(p, t)

# OpenSSL comes with libcurl, and its libcrypto exports AES_encrypt, AES_decrypt
# and AES_cbc_encrypt as well - with a different key structure and a different
# signature. Two definitions of each is a duplicate symbol at link time, and
# letting either win would hand one library the other's idea of a key: kirk's
# takes an AES_ctx, OpenSSL's an AES_KEY, and they are not the same shape. So
# kirk's say whose they are. Renamed in the header, which both its own source
# and every caller include.
p = D / 'ext/libkirk/AES.h'
t = p.read_text()
old = """#ifndef __RIJNDAEL_H
#define __RIJNDAEL_H

#include "kirk_common.h"
"""
new = """#ifndef __RIJNDAEL_H
#define __RIJNDAEL_H

#include "kirk_common.h"

/* psp5: these names belong to OpenSSL too, which is linked in for HTTPS. */
#define AES_set_key kirk_AES_set_key
#define AES_encrypt kirk_AES_encrypt
#define AES_decrypt kirk_AES_decrypt
#define AES_cbc_encrypt kirk_AES_cbc_encrypt
#define AES_cbc_decrypt kirk_AES_cbc_decrypt
#define AES_CMAC kirk_AES_CMAC
"""
assert t.count(old) == 1, "kirk AES anchor"
t = t.replace(old, new, 1)
write(p, t)

print("edits applied")





