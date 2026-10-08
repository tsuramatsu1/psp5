# psp5

**A PSP emulator for the PlayStation 5, based on [PPSSPP](https://github.com/hrydgard/ppsspp).**

PPSSPP is the emulator — its interpreter and x86-64 JIT, its HLE of the PSP's
operating system, its Vulkan renderer. psp5 is that emulator built as a
standalone PS5 homebrew title: it boots to its own console home screen, reads
the games off the memory stick, and runs them. None of PPSSPP's own interface is
shown.

PPSSPP already runs on the PS5 as a libretro core inside
[PS5_RetroArch](https://github.com/mihawk-99/PS5_RetroArch). psp5 is the other
shape of the same work: its own title, with no frontend in front of it — the same
relationship [PS5SX2](https://github.com/Swordpdf/PS5SX2) has to PS5_RetroArch's
LRPS2 core.

## Status

It runs on hardware: the title launches, the home screen draws, and games boot
and play with sound.

| | |
| --- | --- |
| Launching, drawing, running a commercial ISO | **working** |
| Home screen: the memory stick's games, with their own icons and key art | **working** |
| Cheats, per-game, edited from the shelf or from inside a game | **working** |
| Save states, from the in-game menu | **working** |
| Settings, written back to PPSSPP's configuration | **working** |
| Sound on the home screen | **not done** — the kit's cues are collected and dropped |
| Per-game setting overrides | **not done** |

The console port itself is not psp5's work: `patches/ppsspp/ps5-port.patch` is
taken unmodified from PS5_RetroArch, where it is proven on hardware. What psp5
adds is what a title needs and a libretro core does not.

## Using it

Copy `dist/PPSA99131/` to `/mnt/usb0/PPSA99131` on the console and install it
with ShadowMountPlus. Games go on the emulated memory stick, which is inside the
title's own folder:

```
PPSA99131/memstick/PSP/GAME/<your game>.iso     # also .cso, .chd, EBOOT.PBP
PPSA99131/memstick/PSP/Cheats/<DISC_ID>.ini     # CWCheat files
```

### Controls

On the home screen:

| | |
| --- | --- |
| **L1 / R1** | Recent, A–Z, Favorites |
| **Cross** | the game's details, and Play |
| **Triangle** | keep a game in Favorites |
| **OPTIONS** | settings, and *Close psp5* at the foot of them |

In a game:

| | |
| --- | --- |
| **L1 + L3** | the menu: cheats, save states, and the way out |

## What it is made of

| Piece | Where it comes from |
| --- | --- |
| The emulator | [PPSSPP](https://github.com/hrydgard/ppsspp) v1.20.4, pinned, unvendored |
| The console port | `patches/ppsspp/ps5-port.patch`, from PS5_RetroArch |
| The Vulkan driver | [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)'s RADV, linked statically |
| The home screen | [PS5_VKHomebrewUI](https://github.com/mihawk-99/PS5_VKHomebrewUI)'s kit, Aurora Shelf design |
| The platform layer | [PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate)'s `ps5/src/platform.c`, vendored in `src/platform/` (MIT) |
| The SDK | the [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) fork, for `ps5platform/{exec,shm,heap,fp}.h` |

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
| `ui/PS5AuroraLauncher.cpp` | the home screen's own frame loop, borrowing the device |
| `ui/PS5GameLibrary.cpp` | the memory stick's games, and a cover for each |
| `ui/PS5GameArt.cpp` | `ICON0.PNG`, `PIC1.PNG` and `PARAM.SFO`, read out of an ISO or PBP |
| `ui/PS5Cheats.cpp`, `ui/PS5Settings.cpp` | the cheat file, and PPSSPP's configuration |
| `ui/kit/aurora.cpp` | psp5's copy of the kit's Aurora Shelf design |
| `platform/` | klog, splash, pad, audio and the shell exit (vendored, MIT) |

### Decisions worth knowing

**The home screen and the emulator take turns with the device.** The kit's
renderer and PPSSPP's render manager each assume they own the frame loop, so they
cannot both be up: the shelf runs, hands the device back exactly as it found it,
and PPSSPP takes it for the game. Each turn ends in `PS5VulkanContext::ShutdownDraw`.

**The Aurora design is forked, not patched.** The kit's designs read a
`demo::Catalog` that fills itself in its own constructor, so there is no seam to
push real content through — and the kit is a pinned checkout that
`tools/setup-kit.sh` re-fetches, so an edit in place would not survive. psp5 keeps
its own copy of the one design it ships and drops the kit's from the build.

**None of PPSSPP's interface is drawn.** Four edits in the patch do it: psp5
hands PPSSPP an `EmuScreen` directly, so neither the logo nor the game browser is
drawn even once; `MainScreen`'s constructor tells psp5 the game ended, and psp5
leaves the frame loop before the switch to it takes effect; the pause menu is
compiled out; and `ScreenManager::switchScreenNow` makes a screen switch that
cannot be refused by one already queued.

**The patches are kept apart.** `ps5-port.patch` is upstream's, unmodified, so it
can be replaced wholesale when PS5_RetroArch's moves. `ps5-standalone.patch` is
psp5's, generated by `tools/mkpatch.py` — each edit anchored to the text it
replaces, with the reason beside it, so a change in the pinned PPSSPP fails loudly
there rather than applying somewhere unintended.

**Undefined weak symbols are bound to zero explicitly.** Mesa leaves ~6,700
`radv_*` and `annotate_*` dispatch entries undefined on purpose, and the title
converter requires a stub for every entry in `.dynsym`. `--no-dynamic-linker` used
to drop them, but whether it does depends on the host LLVM, so `tools/link-title.sh`
links, collects what is left, binds each with `--defsym`, and links again.

**Flexible memory stays at the console default (448 MiB).** The 1 GiB a title can
ask for in `param.json` is taken out of direct memory, and psp5's large
allocations — the JIT's code cache, the guest memory arena, the heap, the GPU's
buffers — are all in direct memory already. Raising it would cost 576 MiB of the
pool that actually matters here. `sce_sys/param.json` is where to change it.

**System dialogs are refused, not ignored.** Every `System_MakeRequest` psp5 does
not implement returns false. A request that is neither answered nor refused leaves
PPSSPP waiting for a callback that never arrives.

`sce_sys/icon0.png` is drawn by `tools/make-icon.py` (512x512, RGB, no alpha, as
the console wants) and `tools/link-title.sh` stages it. Replace either the file or
the script to change it.

## Licence

GPL-3.0-or-later. PPSSPP is GPL-2.0-**or-later**, which permits combining it with
the GPL-3.0-or-later platform layer and RADV build; the combined work is GPL-3.0.
`src/platform/` is MIT, from PS5_VulkanTemplate, and keeps its notice.
`src/ui/volk/` is MIT. `src/ui/kit/aurora.cpp` is forked from PS5_VKHomebrewUI
(GPL-3.0-or-later) and keeps its copyright line.

## Credits

The hard parts of this are other people's: **Henrik Rydgård** and PPSSPP's
contributors for the emulator this is built on, **mihawk-99** for the PS5 Vulkan
driver, the payload SDK fork, the platform layer, the homebrew UI kit and the
PPSSPP port patch, and **Swordpdf** for PS5SX2, which is what a standalone
emulator title on this stack looks like.
