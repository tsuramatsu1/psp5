# PSP5

**A PSP emulator for the PlayStation 5, based on [PPSSPP](https://github.com/hrydgard/ppsspp).**

PPSSPP is the emulator — its interpreter and x86-64 JIT, its HLE of the PSP's
operating system, its Vulkan renderer. PSP5 is that emulator built as a
standalone PS5 homebrew title: it boots to its own console home screen, reads
the games off the memory stick, and runs them. None of PPSSPP's own interface is
shown — not its logo, not its game browser, not its pause menu, and none of its
on-screen messages.

The title installs as **PSP5**; *PSP5* is the project.

The console port itself is not PSP5's work: `patches/ppsspp/ps5-port.patch` is
taken unmodified from PS5_RetroArch, where it is proven on hardware. What PSP5
adds is what a title needs and a libretro core does not.

PPSSPP already runs on the PS5 as a libretro core inside
[PS5_RetroArch](https://github.com/mihawk-99/PS5_RetroArch). PSP5 is the other
shape of the same work: its own title, with no frontend in front of it — the same
relationship [PS5SX2](https://github.com/Swordpdf/PS5SX2) has to PS5_RetroArch's
LRPS2 core.

## Using it

Copy `dist/PPSA99131/` to `/mnt/usb0/PPSA99131` on the console and install it
with ShadowMountPlus. Games go on the emulated memory stick, which is inside the
title's own folder:

```
PPSA99131/memstick/PSP/GAME/<your game>.iso     # also .cso, .chd, EBOOT.PBP
PPSA99131/memstick/PSP/Cheats/<DISC_ID>.ini     # CWCheat files
PPSA99131/memstick/PSP/Cheats/cheat.db          # the CWCheat database, to import from
PPSA99131/memstick/PSP/TEXTURES/<DISC_ID>/      # a texture pack, with its textures.ini
```

The disc id is shown under the title on the home screen.

### RetroAchievements

Sign in from **OPTIONS → System → RetroAchievements** and games award
achievements as you play. The list opens two ways, and looks the same in both:
**R2** on the home screen, for a game before it is played, and **R1 + R3** over a
running game. Left and right filter it — all, locked, unlocked — and the badges
are the real ones.

Reading a game's achievements before it runs means identifying it: the disc
image is hashed on a worker thread, which takes a moment and is why the bar
opens on a wait. Nothing is asked of the server twice.

The account is the player's rather than the game's, so PSP5 keeps the answer in
its own `config/psp5.txt` instead of PPSSPP's `bAchievementsEnable`, which is one
of its per-game settings — a game configured before signing in would otherwise
carry "off" for ever. The login token is PPSSPP's, under `PSP/SYSTEM`.

**Hardcore mode is off by default.** RetroAchievements forbids save states while
it is on, and PPSSPP enforces that by dropping the operation silently — no
message, nothing in the log. It is in the settings for anyone who wants it, and
it says what it costs.

This needs HTTPS, which the console has no system libcurl for. See
**Building** — without it the title still builds and runs, and says so instead of
offering to sign in.

### Texture packs

**Settings → System → Texture replacement** loads a pack from
`memstick/PSP/TEXTURES/<DISC_ID>/`. A pack needs a `textures.ini` beside its
images, even a bare one - without it the replacer stays off and says nothing.

**Save new textures** writes what a game draws to
`memstick/PSP/TEXTURES/<DISC_ID>/new/`, named by hash, which is how a pack is
started. A file put back in the folder above under the same name replaces that
texture; there is no need to list it anywhere.

### Controls

On the home screen:

| | |
| --- | --- |
| **L1 / R1** | Recent, A–Z, Favorites |
| **Cross** | the game's details: Play, Resume, cheats |
| **Square** | settings for the game under the cursor |
| **Triangle** | keep a game in Favorites |
| **R2** | this game's achievements |
| **OPTIONS** | settings for the title |

*Resume* appears in place of the cheats entry when the game has a save state,
and boots it straight into the newest one.

In the settings, **L1 / R1** turn the page — Picture, Sound, System — and left
and right change the focused row. *Close PSP5* is at the foot of System.

In a game:

| | |
| --- | --- |
| **L2 + R2** | the menu: cheats, save states, and the way out |
| **R1 + R3** | the achievements for this game |

The triggers are used for the menu because the PSP has no L2 or R2 of its own -
its shoulder buttons are L1 and R1, which a game needs.

### Typing

PSP5 draws its own keyboard, with the console's own shortcuts: **Square**
deletes, **Triangle** is a space, **L1 / R1** move the text cursor, **L2**
shifts, **R2** is done, **Circle** gives up.

## What it is made of

| Piece | Where it comes from |
| --- | --- |
| The emulator | [PPSSPP](https://github.com/hrydgard/ppsspp) v1.20.4, pinned, unvendored |
| The console port | `patches/ppsspp/ps5-port.patch`, from PS5_RetroArch |
| The Vulkan driver | [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)'s RADV, linked statically |
| The home screen | [PS5_VKHomebrewUI](https://github.com/mihawk-99/PS5_VKHomebrewUI)'s kit, Aurora Shelf design |
| The platform layer | [PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate)'s `ps5/src/platform.c`, vendored in `src/platform/` (MIT) |
| The SDK | the [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) fork, for `ps5platform/{exec,shm,heap,fp}.h` |
| HTTPS | PacBrew's libcurl and OpenSSL, cross-built for the console |
| Networking | `console_curl.c`, vendored in `src/net/` from [ps5-native-app-boilerplate](https://github.com/BlackBearReloaded/ps5-native-app-boilerplate) (GPL-3.0-or-later) |

PPSSPP is not vendored. `tools/build.sh` fetches the pinned revision and applies
the two patches, so tracking upstream is a rebase of those patches rather than a
merge of a fork.

## Building

The build runs on Linux (or WSL). It needs the **payload SDK fork** — the upstream
SDK has no `ps5platform/` headers, and the port's JIT and guest memory are built on
them.

```bash
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk   # the fork
export PS5_RADV=/path/to/radv-release             # PS5_Vulkan's output
export PS5_VULKAN=/path/to/PS5_Vulkan             # built, for the link

./tools/build-ffmpeg.sh                           # once; slow, and worth it
PSP5_FFMPEG_PREFIX=$PWD/build/ffmpeg ./tools/build.sh
./tools/link-title.sh                             # -> dist/PPSA99131/
```

Building under WSL from a working copy on the Windows filesystem? Build on the
Linux side — object files on `/mnt/c` are far too slow — and set
`PSP5_DIST_MIRROR` so the finished title lands back in the working copy:

```bash
export PSP5_DIST_MIRROR=/mnt/c/Users/<you>/Documents/Repos/psp5/dist
```

It copies only after the title folder is complete, and replaces `eboot.bin`
through a temporary name, so the mirror is never a half-written title.

RetroAchievements needs HTTPS, and the console has no system libcurl.
`tools/link-title.sh` looks for PacBrew's libcurl and the OpenSSL it was built
against under `PSP5_CURL_PREFIX` (default
`/opt/ps5-payload-sdk/target/user/homebrew`). Without them the link says so and
the title is built without HTTPS: everything else works, and PSP5 reports that
signing in is unavailable rather than failing at it.

`tools/build.sh` checks the SDK and RADV before it starts and says which one is
wrong. It also needs zlib to be the one RADV was built with, which it finds beside
`PS5_RADV` by default; `PS5_ZLIB` overrides it.

FFmpeg is a separate script because it is a slow cross-build that never changes,
while `tools/build.sh` is run over and over. Without it `tools/build.sh` still
works and the link says so, but the PSP's video and its Atrac3 audio are missing —
game intros and menu backgrounds included — so a release build has it.

## How it is put together

PPSSPP's platform ports are a `main` plus a graphics context; `SDL/SDLMain.cpp` is
the desktop one. `src/` is the console's:

| File | What it does |
| --- | --- |
| `PS5Main.cpp` | the entry point, the `System_*` host contract, pad to PPSSPP input, the loop |
| `PS5VulkanContext.cpp` | PPSSPP's `GraphicsContext` on `VK_KHR_display` — no window system |
| `PS5VulkanLoader.cpp` | the global Vulkan functions, resolved against RADV's own entry point |
| `PS5Audio.cpp` | PPSSPP's `AudioBackend` on the console's 48 kHz output |
| `PS5Paths.cpp` | the title's folders under `/app0`, created 0777 so FTP can reach them |
| `PS5Overlay.cpp` | the in-game menu, drawn over the game |
| `PS5Achievements.cpp` | the achievements bar, over the game, on **R1 + R3** |
| `PS5OverlayDraw.h` | what both of those are drawn with: palette, rounded shapes, toggles |
| `net/PS5HttpRequest.cpp` | PPSSPP's `http::Request` on libcurl |
| `net/console_curl.c` | the console's own socket and resolver calls (vendored) |
| `ui/PS5AuroraLauncher.cpp` | the home screen's own frame loop, borrowing the device |
| `ui/PS5GameLibrary.cpp` | the memory stick's games, and a cover for each |
| `ui/PS5GameArt.cpp` | `ICON0.PNG`, `PIC1.PNG` and `PARAM.SFO`, read out of an ISO or PBP |
| `ui/PS5Cheats.cpp`, `ui/PS5Settings.cpp` | the cheat file, and PPSSPP's configuration |
| `ui/PS5Prefs.cpp` | the few settings that are PSP5's own, in `config/psp5.txt` |
| `ui/PS5GameAchievements.cpp` | a game's achievements, read on the home screen before it is played |
| `ui/PS5Keyboard.cpp` | typing on a controller, with the console's own shortcuts |
| `ui/PS5GameSound.cpp` | the selected game's `SND0.AT3`, under the shelf |
| `ui/kit/aurora.cpp` | PSP5's copy of the kit's Aurora Shelf design |
| `platform/` | klog, splash, pad, audio and the shell exit (vendored, MIT) |

### Decisions worth knowing

**The home screen and the emulator take turns with the device.** The kit's
renderer and PPSSPP's render manager each assume they own the frame loop, so they
cannot both be up: the shelf runs, hands the device back exactly as it found it,
and PPSSPP takes it for the game. Each turn ends in `PS5VulkanContext::ShutdownDraw`.

**The Aurora design is forked, not patched.** The kit's designs read a
`demo::Catalog` that fills itself in its own constructor, so there is no seam to
push real content through — and the kit is a pinned checkout that
`tools/setup-kit.sh` re-fetches, so an edit in place would not survive. PSP5 keeps
its own copy of the one design it ships and drops the kit's from the build.

**None of PPSSPP's interface is drawn.** Four edits in the patch do it: PSP5
hands PPSSPP an `EmuScreen` directly, so neither the logo nor the game browser is
drawn even once; `MainScreen`'s constructor tells PSP5 the game ended, and PSP5
leaves the frame loop before the switch to it takes effect; the pause menu is
compiled out; and `ScreenManager::switchScreenNow` makes a screen switch that
cannot be refused by one already queued.

**PPSSPP's on-screen messages are cut at the view, not at each caller.**
`g_OSD.Show` is called from all over the core - "Game controller connected",
save-state notices, achievement popups - and silencing them one at a time leaves
the next one to surface later. `OnScreenMessagesView::Draw` returns immediately
instead, so a new call site cannot put a toast on screen.

**The home screen pumps the network queue itself.** PPSSPP's request callbacks
only run from `RequestManager::Update`, which `NativeFrame` calls - and
`NativeFrame` only runs while a game is up. The shelf has its own frame loop, so
it calls `PS5_PumpNetwork` each frame; without it a sign-in is posted and waits
for ever.

**The achievements client is built from PSP5's own setting, every start.**
PPSSPP's `bAchievementsEnable` records what the player wants, not that a client
exists, and nothing on the home screen creates one. PSP5 calls
`Achievements::UpdateSettings` unconditionally when its own setting is on, which
builds the client and logs back in from the saved token.

**PPSSPP's per-game settings are not where PSP5's answers live.** Six of them
now: achievements, hardcore mode, cheats, the control mapping, and both texture
replacement switches. Each is marked `CfgFlag::PER_GAME`, which means
`Config::LoadGameConfig` - called from `Load_PSP_ISO` partway through a boot -
overwrites whatever was set at start-up with whatever that game's second ini
happens to hold. A setting changed on the home screen was simply undone on the
way into the game, silently.

So PSP5 keeps its own answers in `config/psp5.txt` and asserts them again at one
anchor in `EmuScreen`, after `LoadGameConfig` has had its say. Anything new that
turns out to be `PER_GAME` belongs there too. It is the single most repeated
trap in this codebase.

**The patches are kept apart.** `ps5-port.patch` is upstream's, unmodified, so it
can be replaced wholesale when PS5_RetroArch's moves. `ps5-standalone.patch` is
PSP5's, generated by `tools/mkpatch.py` — each edit anchored to the text it
replaces, with the reason beside it, so a change in the pinned PPSSPP fails loudly
there rather than applying somewhere unintended.

**Undefined weak symbols are bound to zero explicitly.** Mesa leaves ~6,700
`radv_*` and `annotate_*` dispatch entries undefined on purpose, and the title
converter requires a stub for every entry in `.dynsym`. `--no-dynamic-linker` used
to drop them, but whether it does depends on the host LLVM, so `tools/link-title.sh`
links, collects what is left, binds each with `--defsym`, and links again.

**Flexible memory stays at the console default (448 MiB).** The 1 GiB a title can
ask for in `param.json` is taken out of direct memory, and PSP5's large
allocations — the JIT's code cache, the guest memory arena, the heap, the GPU's
buffers — are all in direct memory already. Raising it would cost 576 MiB of the
pool that actually matters here. `sce_sys/param.json` is where to change it.

**System dialogs are refused, not ignored.** Every `System_MakeRequest` PSP5 does
not implement returns false. A request that is neither answered nor refused leaves
PPSSPP waiting for a callback that never arrives.

`sce_sys/icon0.png` is drawn by `tools/make-icon.py` (512x512, RGB, no alpha, as
the console wants) and `tools/link-title.sh` stages it. Replace either the file or
the script to change it. The wordmark is drawn rather than set in a typeface:
the PSP logo's letters are single-weight strokes that no ordinary face has.

The home screen's sound sets are drawn too. `tools/make-sounds.py` synthesises
`assets/sfx/<set>/<cue>_NN.wav` - nothing sampled, nothing licensed - and the
link stages them beside the kit's two. A set is a folder, so adding one is
adding a folder: PSP5 lists what it finds under `/app0/ui/sfx` and offers them
all in the settings.

## Licence

GPL-3.0-or-later. PPSSPP is GPL-2.0-**or-later**, which permits combining it with
the GPL-3.0-or-later platform layer and RADV build; the combined work is GPL-3.0.
`src/platform/` is MIT, from PS5_VulkanTemplate, and keeps its notice.
`src/ui/volk/` is MIT. `src/ui/kit/aurora.cpp` is forked from PS5_VKHomebrewUI
(GPL-3.0-or-later) and keeps its copyright line. `src/net/console_curl.{c,h}` is
GPL-3.0-or-later, from BlackBearReloaded's ps5-native-app-boilerplate, and keeps
its notice.

## Credits

The hard parts of this are other people's: **Henrik Rydgård** and PPSSPP's
contributors for the emulator this is built on, **mihawk-99** for the PS5 Vulkan
driver, the payload SDK fork, the platform layer, the homebrew UI kit and the
PPSSPP port patch, **BlackBearReloaded** for the console networking in
ps5-native-app-boilerplate, and **Swordpdf** for PS5SX2, which is what a
standalone emulator title on this stack looks like.

RetroAchievements support is rcheevos, which PPSSPP already carries, pointed at
[retroachievements.org](https://retroachievements.org) — the achievements
themselves are the work of its community.
