# Dependencies

psp5 builds against three things it does not contain. All three come from the same
stack, and the versions have to agree with each other — the link fails when they do
not, which is the intended behaviour.

## 1. The payload SDK — the *fork*, not upstream

```
PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
```

From [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK). The upstream
public SDK does **not** work: psp5 needs `target/include/ps5platform/`, which is
where the JIT's executable memory (`exec.h`), the guest memory arena (`shm.h`), the
heap (`heap.h`) and the IEEE floating-point state (`fp.h`) live. The port patch is
written directly against those headers.

`tools/build.sh` checks for `ps5platform/exec.h` and stops with that explanation if
the SDK is the wrong one. The quickest check by hand:

```bash
ls "$PS5_PAYLOAD_SDK/target/include/ps5platform/"      # exec.h shm.h heap.h fp.h ...
ls "$PS5_PAYLOAD_SDK/target/lib/libps5platform.a"
```

## 2. RADV — the Vulkan driver

```
PS5_RADV=/path/to/PS5_Vulkan/.deps/native/radv-release
```

From [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan), which builds Mesa's
RADV for the console out of [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa).
psp5 needs the built archive and the headers:

```bash
ls "$PS5_RADV/lib/libvulkan_radeon.ps5.a"   # ~258 MB
ls "$PS5_RADV/include/vulkan/"
```

The archive exports exactly two symbols — `radv_GetInstanceProcAddr` and
`vk_icdGetInstanceProcAddr` — and `src/PS5VulkanLoader.cpp` reaches the whole driver
through them. There is no loader on the console, and a title cannot `dlopen` one.

## 3. A built PS5_Vulkan checkout — for the link

```
PS5_VULKAN=/path/to/PS5_Vulkan
```

Only `tools/link-title.sh` needs this, and it needs the checkout *built*, not just
cloned. The console's link is not something psp5 reimplements:

| What `link-title.sh` uses | Why |
| --- | --- |
| `tools/radv-link.sh` | the recipe: linker script, RADV whole-archived, the heap and thread wraps, libc's names bound to the platform layer's `ps5_*` |
| `build/host/ps5-native-tool` | turns the linked ELF into the console's format, and signs it |
| `tooling/native/app_crt.cpp` | the CRT, including the exit that returns to the shell |
| `tooling/native/app-symbols.map` | the version script |
| `runtime/libc.prx` | shipped in the title's `sce_module/` |
| `vendor/ps5/sdk/stubs/agc_*.c` | RADV calls AGC, which the SDK ships no stubs for |

## Keeping them in step

The SDK pin and the RADV archive have to match: the archive was linked against a
particular platform layer, and a mismatch shows up as unresolved symbols at link
time rather than as a fault on the console. PS5_Vulkan pins its own SDK revision,
and a title may pin a later one as long as the platform layer keeps what the
archive was built against.

`tools/link-title.sh` also refuses two specific mistakes before they reach a
console:

- **Imports nothing a title loads exports.** A symbol defined only by
  `libkernel_sys` or `libScePosixForWebKit` links fine but is null at run time, so
  the first call jumps to address 0. The script lists any it finds and says to bind
  them in PS5_Vulkan's recipe.
- **An unsigned or malformed eboot**, by inspecting it after signing.

## The build host

Linux, or WSL. 16 GB of disk for the PPSSPP checkout with submodules, RADV's
archive and the object tree; the PPSSPP clone alone fetches its own FFmpeg,
glslang, armips and the rest.
