# Status

What works on hardware, what does not, and what is known to be unfinished. Kept
out of the README because it goes stale faster than anything else there.

Last reviewed: 2026-10-08.

## Working

| | |
| --- | --- |
| Launching, drawing, running a commercial ISO | boots and plays with sound |
| Home screen: the memory stick's games, with their own icons and key art | read out of the ISO or PBP |
| Cheats, per game | the switch is psp5's own, in `config/psp5.txt`, so it does not drag PPSSPP's per-game settings on with it |
| Importing cheats from `PSP/Cheats/cheat.db` | from the shelf and from inside a game |
| Save states | the in-game menu, and *Resume* on the shelf into the newest one |
| Settings, written back to PPSSPP's configuration | the list scrolls, so adding one cannot overrun the screen |
| Per-game setting overrides | **Square** on the shelf |
| Sound on the home screen | five sets under `/app0/ui/sfx`, and the selected game's own `SND0.AT3` |
| RetroAchievements | sign-in, badges, and a locked/unlocked/all filter |
| Achievements for a game before it is played | **R2** on the shelf: the disc is hashed on a worker thread and the list read from rcheevos |
| Achievements over a running game | **R1 + R3** |
| The in-game menu | **L2 + R2** - cheats, save states, exit |
| Hours played | PPSSPP's own time tracker, per disc id |

## Not done

| | |
| --- | --- |
| Texture replacement | `bReplaceTextures` / `bSaveNewTextures` and `memstick/PSP/TEXTURES/<GAMEID>/` |

## Blocked

**The console's own on-screen keyboard.** `libSceImeDialog` is present on the
system, but the SDK ships no headers for it and no verified `SceImeDialogParam`
layout was found to write against. Guessing at the ABI of a system call that
takes a struct by pointer is how you get a crash that looks like something else,
so psp5 does not. Text entry is psp5's own panel until a known-good definition
turns up.

## Rough edges

**Hardcore mode is off by default.** RetroAchievements' hardcore mode forbids
save states, and PPSSPP enforces that by dropping the operation without a word -
no message, nothing in the log. psp5 defaults it off so save states work, and
offers it in the settings. Turning it on turns save states off.

**Cheat switches set before 2026-10-08 do not carry over.** They lived in
PPSSPP's per-game ini; psp5 keeps its own now, so a game that had cheats on reads
as off until it is set once more. The codes inside the `.ini` files are
untouched.

**The right trigger can fast-forward for a moment after the in-game menu
closes.** While the menu is up the game is shown no input at all, and buttons
still held when it closes are withheld until released - but the triggers also
travel as axes, and those resume at once. PPSSPP binds the right trigger to
`AnalogFpsLimit` by default.

**Achievement badges arrive over a second or two.** They are fetched four at a
time; asking for all of a game's at once exhausted the console's network stack
and every request failed with an SSL error. A row draws a plain mark until its
own image lands.

## Notes

The console port itself is not psp5's work: `patches/ppsspp/ps5-port.patch` is
taken unmodified from PS5_RetroArch, where it is proven on hardware. What psp5
adds is what a title needs and a libretro core does not.
