# Status

What works on hardware, what does not, and what is known to be unfinished. Kept
out of the README because it goes stale faster than anything else there.

Last reviewed: 2026-10-08.

## Working

| | |
| --- | --- |
| Launching, drawing, running a commercial ISO | boots and plays with sound |
| Home screen: the memory stick's games, with their own icons and key art | read out of the ISO or PBP |
| Cheats, per-game, edited from the shelf or from inside a game | |
| Save states, from the in-game menu | and *Resume* on the shelf, into the newest one |
| Settings, written back to PPSSPP's configuration | |
| Per-game setting overrides | **Square** on the shelf |
| Sound on the home screen | the kit's cues, and the selected game's own `SND0.AT3` |
| RetroAchievements | sign-in, and the achievements bar on **R1 + R3** |
| Hours played | PPSSPP's own time tracker, per disc id |

## Not done

| | |
| --- | --- |
| Texture replacement | `bReplaceTextures` / `bSaveNewTextures` and `memstick/PSP/TEXTURES/<GAMEID>/` |
| Viewing a game's achievements from the home screen | needs the game loaded into the rcheevos client without booting it |

## Blocked

**The console's own on-screen keyboard.** `libSceImeDialog` is present on the
system, but the SDK ships no headers for it and no verified `SceImeDialogParam`
layout was found to write against. Guessing at the ABI of a system call that
takes a struct by pointer is how you get a crash that looks like something else,
so psp5 does not. Text entry is psp5's own panel until a known-good definition
turns up.

## Notes

The console port itself is not psp5's work: `patches/ppsspp/ps5-port.patch` is
taken unmodified from PS5_RetroArch, where it is proven on hardware. What psp5
adds is what a title needs and a libretro core does not.
