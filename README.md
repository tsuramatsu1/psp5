# psp5

PPSSPP as a standalone PlayStation 5 homebrew title: a PSP emulator that boots to
PPSSPP's own interface, renders through Vulkan on RADV, and runs the x86-64 JIT.

PPSSPP already runs on the PS5 as a libretro core inside
[PS5_RetroArch](https://github.com/mihawk-99/PS5_RetroArch). psp5 is the other
shape of the same work: its own title, with no frontend in front of it — the same
relationship [PS5SX2](https://github.com/Swordpdf/PS5SX2) has to PS5_RetroArch's
LRPS2 core.

## Status

The console port itself is not new work: `patches/ppsspp/ps5-port.patch` is taken
unmodified from PS5_RetroArch, where it is proven on hardware. What psp5 adds is
what a title needs and a core does not.

| | |
| --- | --- |
| PPSSPP's emulator core, JIT, memory arena and Vulkan backend cross-compile | **done** |
| PPSSPP's UI and psp5's platform layer cross-compile | **done** |
| FFmpeg cross-builds and links in (PSP video, Atrac3 audio) | **done** |
| The title links, converts and signs | **done** — a 47 MB `eboot.bin`, integrity valid |
| The title folder is complete | **done** — `eboot.bin`, `sce_module/libc.prx`, `sce_sys/{param.json,icon0.png}`, 190 asset files |
| Vulkan on RADV, `VK_KHR_display` presentation | **written**, not yet run |
| Pad, audio, title paths | **written**, not yet run |
| Booting on a console | **not done** |

**Nothing here has run on a PS5.** Everything above is a build result — the title
is well-formed, not working. Whether it boots, draws a frame or plays a game is
unknown, and `docs/porting-notes.md` lists what is most likely to be wrong first.

`sce_sys/icon0.png` is drawn by `tools/make-icon.py` (512x512, RGB, no alpha, as the
console wants) and `tools/link-title.sh` stages it. Replace either the file or the
script to change it.

## What it is made of

| Piece | Where it comes from |
| --- | --- |
| The emulator | [PPSSPP](https://github.com/hrydgard/ppsspp) v1.20.4, pinned, unvendored |
| The console port | `patches/ppsspp/ps5-port.patch`, from PS5_RetroArch |
| The Vulkan driver | [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)'s RADV, linked statically |
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
through a temporary name, so the mirror is never a half-written title. An ordinary
relink moves the eboot alone; the 22 MB of assets go only when they change.

`tools/build.sh` checks the SDK and RADV before it starts and says which one is
wrong. It also needs zlib to be the one RADV was built with, which it finds beside
`PS5_RADV` by default; `PS5_ZLIB` overrides it.

FFmpeg is a separate script because it is a slow cross-build that never changes,
while `tools/build.sh` is run over and over. Without it `tools/build.sh` still
works and the link says so, but the PSP's video and its Atrac3 audio are missing —
game intros and menu backgrounds included — so a release build has it.

To work on the port itself: edit `.deps/ppsspp-src`, build with `PSP5_DEV=1` to
skip the reset, and write the patch back when it works.

## How it is put together

PPSSPP's platform ports are a `main` plus a graphics context; `SDL/SDLMain.cpp` is
the desktop one. `src/` is the console's:

| File | What it does |
| --- | --- |
| `PS5Main.cpp` | the entry point, the `System_*` host contract, pad to PPSSPP input, the run loop |
| `PS5VulkanContext.cpp` | PPSSPP's `GraphicsContext` on `VK_KHR_display` — no window system |
| `PS5VulkanLoader.cpp` | the global Vulkan functions, resolved against RADV's own entry point |
| `PS5Audio.cpp` | PPSSPP's `AudioBackend` on the console's 48 kHz output |
| `PS5Paths.cpp` | the title's folders under `/app0`, created 0777 so FTP can reach them |
| `platform/` | klog, splash, pad, audio and the shell exit (vendored, MIT) |

PPSSPP's own UI is the frontend. psp5 does not add a game browser or a settings
screen, because PPSSPP has both and they are better than a new one.

### Decisions worth knowing

**The two patches are kept apart.** `ps5-port.patch` is upstream's, unmodified, so
it can be replaced wholesale when PS5_RetroArch's moves. `ps5-standalone.patch` is
91 lines across two files: the Vulkan loader, and a PS5 branch in the platform
selection.

**Flexible memory stays at the console default (448 MiB).** The 1 GiB a title can
ask for in `param.json` is taken out of direct memory, and psp5's large
allocations — the JIT's code cache, the guest memory arena, the heap, the GPU's
buffers — are all in direct memory already. Raising it would cost 576 MiB of the
pool that actually matters here. `sce_sys/param.json` is where to change it.

**System dialogs are refused, not ignored.** Every `System_MakeRequest` psp5 does
not implement returns false. A request that is neither answered nor refused leaves
PPSSPP's UI waiting for a callback that never arrives.

## Licence

GPL-3.0-or-later. PPSSPP is GPL-2.0-**or-later**, which permits combining it with
the GPL-3.0-or-later platform layer and RADV build; the combined work is GPL-3.0.
`src/platform/` is MIT, from PS5_VulkanTemplate, and keeps its notice.

## Credits

The hard parts of this are other people's: **Henrik Rydgård** and PPSSPP's
contributors for the emulator, **mihawk-99** for the PS5 Vulkan driver, the payload
SDK fork, the platform layer and the PPSSPP port patch, and **Swordpdf** for
PS5SX2, which is what a standalone emulator title on this stack looks like.
