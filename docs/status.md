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
| Typing | psp5's own keyboard, with the console's shortcuts: Square deletes, Triangle spaces, L1/R1 move the cursor, L2 shifts, R2 is done |
| Texture replacement | a pack from `PSP/TEXTURES/<DISC_ID>/`, and dumping to `new/` to build one |
| Settings | paged - Picture, Sound, System - on **L1 / R1** |
| Recent | ordered by when a game was last played, then by when its file arrived |

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

## Not done yet

### Frame generation (PS5_MihawkGen)

[PS5_MihawkGen](https://github.com/mihawk-99/PS5_MihawkGen) interpolates frames
with a small neural network. Planned, not present: it was built into psp5 once,
on 2026-10-28, and taken back out the same day because nothing could show it was
doing anything. The notes below are what that attempt cost, so the next one does
not pay twice.

**The frames are not already there to be replaced.** PPSSPP presents only when
the framebuffer really changed - `__DisplayFlip` checks `fbReallyDirty` - so a
game running at 30 fps presents thirty times a second and sends no duplicates.
There is nothing to substitute. A generator here has to *insert* a frame between
two real ones and present it itself, which means it owns part of the present
loop rather than filtering it.

**The frame rate readout cannot be used as evidence.** `__DisplayGetFPS` hands
back two different numbers: `fps` is the emulated vblank rate, which is about 60
for any PSP game running at full speed whatever it is drawing, and the flip
count is the game's own picture rate. "Framegen is off and it still says 60" was
that, not a fault. Before the next attempt is worth starting, there needs to be
a readout that separates real flips from generated presents - otherwise the
feature cannot be judged on or off, which is why this one was removed.

**`CalculateFPS` was the only usable gate signal.** Everything else that looked
like a frame rate latches or is smoothed, so it cannot say "this game is below
the threshold *now*".

**Keep Vulkan out of the boundary.** The kit and psp5's UI reach Vulkan through
volk; PPSSPP keeps its own entry points in namespace `PPSSPP_VK`. A translation
unit that sees both finds every `vkXxx` ambiguous. The way through was to give
the entry point scalar `void *` and `uint32_t` arguments, so the patch needed
one `extern "C"` line, no struct and no Vulkan header on PPSSPP's side.

**Shut it down after `delete draw_`, not before.** PPSSPP's render thread is
joined only when the render manager is destroyed, so tearing the generator down
first is a race against a thread that is still presenting.

**`tools/link-title.sh` does not remove what it staged.** It creates the
feature's folder under `dist/<TITLE_ID>/` and never deletes it, so taking the
feature out again leaves 1.7 MB of stale weights that still ship. Check `dist/`
after any removal.

## Notes

The console port itself is not psp5's work: `patches/ppsspp/ps5-port.patch` is
taken unmodified from PS5_RetroArch, where it is proven on hardware. What psp5
adds is what a title needs and a libretro core does not.
