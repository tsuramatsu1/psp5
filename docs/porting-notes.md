# Porting notes

What is known about this port, and what is guessed. The guesses are the point of
this file: psp5 has never run on a console, so the first run will find several of
them, and knowing which is which saves the time of debugging the wrong layer.

## What is proven, and by whom

The console port of PPSSPP is not psp5's work. `patches/ppsspp/ps5-port.patch` is
PS5_RetroArch's, unmodified, and the things most likely to be hard are the things
it already settled:

- **The JIT.** `AllocateExecutableMemory` goes to `ps5_exec_allocate`, which hands
  out direct memory that is read, write and execute at once. `mmap(PROT_EXEC)` is
  refused by the console, and flipping protections per emitted block costs about
  26 µs, which a busy JIT cannot afford.
- **Guest memory.** `MemArenaPosix` uses `ps5_shm`: one direct-memory object mapped
  at several addresses, for the PSP's mirrors and fastmem. POSIX shared memory also
  works but charges every view to the title's small flexible budget.
- **Thread names.** `pthread_set_name_np` faults inside libkernel on a fresh worker
  thread and takes the process down, so the port keeps the name in TLS instead.
- **RADV's quirks** in the Vulkan framebuffer, texture cache and thin3d paths.

Treat a fault in any of those as a sign that the SDK or RADV pin has moved, not as
something to re-solve.

## What psp5 added, and has not run

### The Vulkan loader against static RADV

`src/PS5VulkanLoader.cpp`. The libretro core never did this: RetroArch owned the
Vulkan device and handed it over, so the core had no loader at all. A title has to
find the driver itself, and there is no loader and no `dlopen`.

RADV's archive exports exactly two symbols — `radv_GetInstanceProcAddr` and
`vk_icdGetInstanceProcAddr` — and everything else is reached through them.

Two functions needed care, and either could be wrong:

- **`vkGetDeviceProcAddr`.** An ICD returns only *global* entry points for a null
  instance, so this is null before an instance exists — but PPSSPP checks it is
  non-null before creating one. psp5 answers with a trampoline that resolves the
  real function on first call. If device-level calls come back null, this is why.
- **`vkCreateInstance`.** Wrapped only to record the instance the trampoline needs.
  If instance creation misbehaves, check the wrapper before suspecting RADV.

### Presentation

`src/PS5VulkanContext.cpp` uses `WINDOWSYSTEM_DISPLAY`, which is PPSSPP's own
`VK_KHR_display` path — the one its KMSDRM build takes. That path is real code
that works elsewhere, but it has never selected a *console* display mode, and the
mode it picks decides the resolution psp5 reports to the UI. **Watch the
`display: WxH` line in klog on the first run.** A guess that is wrong here shows up
as a correct-looking picture at the wrong size.

The libretro build compiled this path out (`#if !defined(__LIBRETRO__)`), so it is
specifically what the standalone build turned back on and nobody has run.

### The title's paths

`UI/NativeApp.cpp` picks the memory stick, `flash0` and the current directory from
a chain of platform branches. With no PS5 branch, the console falls into the
generic one, which reads `$XDG_CONFIG_HOME`, then `$HOME`, and settles on a
**relative** `./config/ppsspp` when neither exists. Neither exists on the console,
and there is no current directory worth trusting — `getcwd` is not provided — so
PPSSPP would have ignored everything `PS5Paths` set up and written saves nowhere
findable.

`ps5-standalone.patch` adds a PS5 branch that takes the paths the title passed to
`NativeInit` instead. This is the general shape of the problem the porting
reference warns about: give the program its base paths at start-up rather than
letting it derive them. Anything else in PPSSPP that reads the environment for a
path is likely wrong here too, and this was found by reading, not by running.

### Input

`PollInput` in `src/PS5Main.cpp` maps the DualSense to what PPSSPP expects of a
generic pad: cross is "A", circle "B", square "X", triangle "Y". That is the
arrangement PPSSPP's default mapping is written against, so the defaults should
land on the PSP's buttons without the user remapping anything — but *should* is the
operative word, and it is one line per button to change.

Known gaps:

- The touch pad click stands in for SELECT, because nothing else is left.
- Readings taken while the shell holds the pad carry `PAD_INTERCEPTED` and are
  dropped, so the home screen does not leak input into a game.
- Sticks are negated on Y: the console reports y-down, PPSSPP wants y-up.

### The run loop

`main` in `src/PS5Main.cpp` polls the pad, calls `NativeFrame`, then
`graphicsContext->Poll()`. That is PPSSPP's **single-threaded** shape: the SDL port
takes the same path whenever its emu thread is disabled, and the Android and iOS
ports call `NativeFrame` the same way.

PPSSPP's other shape runs `NativeFrame` on an emu thread while the main thread
drives `graphicsContext->ThreadFrame(true)`. psp5 does not, yet. The simple loop is
the one to get working first, and the threaded one is a change to this file alone —
but note that the render manager is already multi-threaded underneath
(`g_Config.bRenderMultiThreading`), so this is not the same question as whether
rendering is threaded.

### Audio

`src/PS5Audio.cpp` converts PPSSPP's float render to the interleaved 16-bit the
console takes, at 48 kHz in 256-frame periods. The platform layer's own thread
paces it. The conversion clamps rather than wrapping, because PPSSPP does not
promise to stay inside -1..1 and a wrap turns a loud moment into noise.

Untested, and the likeliest symptom of a mistake here is latency rather than
silence.

## What the link taught

Four problems showed up between "everything compiles" and "a signed eboot", and
each is written into `tools/link-title.sh` so it does not have to be found again.

**zlib twice.** RADV's archive is whole-archived into the title and carries the
zlib it was built with, so PPSSPP's bundled copy duplicated every `deflate*` and
`inflate*` symbol. The fix is not to drop one at link time but to build PPSSPP
against the very library RADV used — `PS5_ZLIB`, which `tools/build.sh` defaults to
PS5_Vulkan's copy. Anything else RADV bundles would behave the same way; zlib is
the only one that collides today, checked by looking for `ZSTD_*`, `XXH*`,
`png_*`, `expat_*` and `LLVM*` in the archive and finding none.

**Symbols the SDK declares but nothing exports.** `swab`, `tmpfile` and `tmpnam`
were simply undefined. `fork`, `link`, `symlink`, `readlink`, `pathconf`,
`mkstemp`, `isatty` and `gai_strerror` were worse: the stubs for `libkernel_sys`
and `libScePosixForWebKit` declare them, so they *link*, but a title loads neither
module, so each would be null at run time and its first call would jump to address
0. `src/PS5LibcShims.cpp` defines them, and `link-title.sh` refuses any that
reappear.

**Weak undefined Vulkan entry points.** Mesa names every entry point in its
dispatch tables through a weak reference and leaves the unimplemented ones
undefined, expecting null. About 6,400 `radv_*` and `annotate_*` names arrive that
way, and the converter demands a stub for every entry in `.dynsym`, stopping at
`radv_EnumeratePhysicalDevices`. The answer is **`--no-dynamic-linker`**: there is
no dynamic linker, so lld binds those references to zero at link time and leaves
them out of the dynamic table — exactly the null Mesa wants.

Two things that do not work: a version script cannot localise an undefined symbol
(lld refuses, and it refuses 6,400 times), and `-z nodynamic-undefined-weak` is not
in lld 18.1.3 — it warns "unknown -z value" and carries on, which looks like
success until the converter fails identically.

**main() in an archive.** `libpsp5_platform.a` is whole-archived because `main` is
referenced only from the CRT and the `System_*` definitions only from PPSSPP's own
code; an archive member nothing references strongly is dropped, silently.

## Things deliberately not done

- **No frontend.** PPSSPP's UI is the frontend. A separate game browser would be a
  second thing to maintain and worse than what PPSSPP already has.
- **No system dialogs.** `SYSPROP_HAS_FILE_BROWSER` and the rest are false, so
  PPSSPP uses its own browser and on-screen keyboard. A title cannot open the
  console's pickers, and claiming otherwise would leave the UI waiting.
- **No networking.** Sockets exist on the console but little beyond them is proven.
  `SYSPROP_SUPPORTS_HTTPS` is false, so RetroAchievements and the update check stay
  off rather than stalling a frame on a retry loop.
- **FFmpeg is off by default.** `tools/build.sh` builds without it until
  `PSP5_FFMPEG_PREFIX` is set. The PSP's video and some of its audio need it —
  without it, game intros and menu backgrounds are missing — so a release build
  must have it. It is off first only because it is a long cross-build that is
  irrelevant to whether the emulator itself compiles.

## Before the first console run

The porting skill's order of work asks for each step to be proven on the console
rather than assumed. In that order:

1. Link the title and package it. **Not done** — this is the next task, and the
   link recipe (SDK libraries and RADV inside one `--start-group`, the heap wrap
   around `malloc`) is the part most likely to need several attempts.
2. Capture klog and check the title starts: the `psp5 - PPSSPP ...` line, then
   `vulkan: ...`, then `display: WxH`, then `running`.
3. Reach PPSSPP's UI. A black screen with `running` in the log is a presentation
   problem; no `running` at all is an init problem, and the last line says which.
4. Load a game and hold full speed. Only then are the defaults worth choosing, and
   per the porting rules they are the best picture that holds 100%, recorded with
   the measurement behind them.

Shader compilation must not stall a frame: PPSSPP's asynchronous pipeline
compilation stays on, and the first launch of a game is someone's first impression.
