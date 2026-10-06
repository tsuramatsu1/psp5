# Working in psp5

## What this repository is

PPSSPP, unvendored and pinned, plus the few files that make it a PlayStation 5
title. `tools/build.sh` fetches PPSSPP v1.20.4, applies two patches and builds.

Keep that shape. The value of this repository is that it is *small*: two patches
and six source files against an emulator nobody here maintains.

## The rules that matter

**PPSSPP is never vendored.** If a change belongs in PPSSPP, it goes in a patch
under `patches/ppsspp/`. If it belongs upstream, send it upstream.

**The two patches stay apart, and do different jobs.**

| Patch | What may go in it |
| --- | --- |
| `ps5-port.patch` | nothing. It is PS5_RetroArch's, unmodified, and is replaced wholesale when theirs moves. |
| `ps5-standalone.patch` | what a title needs and a libretro core does not. |

If a fix belongs in `ps5-port.patch`, it belongs in PS5_RetroArch first. Taking a
local copy of their patch forks it, and the next update silently drops the fix.

**Platform code, not program code.** PPSSPP's own logic is not the place for a
console workaround. A gap that is the console's — libc, memory, threads — belongs
in the platform layer; a gap that is PPSSPP's belongs behind
`PPSSPP_PLATFORM(PS5)` in as few files as possible, the way upstream keeps its
other platforms.

**Do not add a frontend.** PPSSPP's UI is the frontend. A game browser or settings
screen here would be a second thing to maintain and worse than what already exists.

**Refuse, do not ignore.** A `System_MakeRequest` psp5 cannot answer returns false.
A request that is neither answered nor refused leaves PPSSPP's UI waiting for a
callback that never comes, which looks like a hang with no error.

**Accuracy is not traded silently.** A setting that trades accuracy for speed is
named as such and is not a default. When a game falls short of full speed, make the
emulator faster rather than making it do less: underclocking the emulated CPU or
skipping work changes the game's behaviour.

## Editing the port

Set the tree up once, with `ps5-port.patch` committed so it stays out of the diff:

```bash
cd .deps/ppsspp-src
git checkout --force fa50bb1976065c4f8b1b47af227d367fe9771555
git clean -qfd -e build
git apply --whitespace=nowarn ../../patches/ppsspp/ps5-port.patch
git add -A && git commit -m "base: PS5 port patch"
git apply --whitespace=nowarn ../../patches/ppsspp/ps5-standalone.patch
```

`git add -A`, not `-a`: the port patch *adds* `Common/PS5Memory.h`, and `-a` stages
only tracked files, so the new file is left untracked and the next `git clean`
deletes it. The build then fails on a missing header that the patch plainly
contains.

Then edit, and build the tree as it stands:

```bash
PSP5_DEV=1 ./tools/build.sh
```

When it works, write the patch back:

```bash
git -C .deps/ppsspp-src diff > patches/ppsspp/ps5-standalone.patch
```

`tools/build.sh` without `PSP5_DEV` always resets the tree and reapplies both
patches from the pinned revision, so an edit that is not written back is lost on
the next ordinary build.

## Claims about behaviour

Nothing in this repository has run on a console. Say "builds" when something
builds, and do not write "works" until a console run says so — `README.md`'s status
table is the record, and it is a list of build results.

When a console run does happen, keep its numbers: what the klog said, what the
resolution was, what the frame rate was in which game. A measurement nobody wrote
down has to be taken again.

## Reference

The five repositories this is built on, and what each answers:

| | |
| --- | --- |
| [PS5_RetroArch](https://github.com/mihawk-99/PS5_RetroArch) | the PPSSPP port patch, and how a JIT emulator behaves on this console |
| [PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate) | the platform layer, the link recipe, and `skills/` — the written-down answers to most porting questions |
| [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) | RADV, and the link recipe psp5's `tools/link-title.sh` calls |
| [PS5SX2](https://github.com/Swordpdf/PS5SX2) | what a standalone emulator title on this stack looks like |
| [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) | where RADV itself comes from |

`PS5_VulkanTemplate/skills/ps5-porting/` is worth reading before any non-obvious
change: memory and JIT, files and I/O, and forks each have a reference file.
