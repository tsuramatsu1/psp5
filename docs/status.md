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

### Frame generation (PS5_MihawkGen) - experimental, half built

[PS5_MihawkGen](https://github.com/mihawk-99/PS5_MihawkGen) makes a frame
between two the emulator drew, with a small network as Vulkan compute passes.
`tools/setup-framegen.sh` pins v1.0.0 (network d5) and exports it into
`.deps/framegen`, the way the UI kit is exported.

**What is in the title.** The generator compiles into `psp5_ui`, its weights are
staged at `/app0/framegen/models/network.fgw`, `src/ui/PS5FrameGen.cpp` owns the
context and `Settings > System > Frame generation (experimental)` turns it on.
It is off by default and nothing else behaves differently when it is on.

**What is not.** No generated frame reaches the screen. The present loop is
untouched: PPSSPP still renders once and presents once per game frame, so a
30 fps game is still 30 fps with the switch on.

**The measuring run.** With the switch on, the network does run - every frame,
on the real pictures - and is timed; the frame it makes is written to an image
that is never read. `FrameData::Submit` hands psp5 the finished picture inside
the frame's own command buffer, where the backbuffer render pass has already
left the swapchain image in `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`; psp5 blits it
into one of two history images, restores the layout, and records the
generator's passes between two GPU timestamps. No extra semaphore, no extra
submit, no extra present, and nothing on screen changes - so a mistake here
draws wrongly rather than hanging the console.

That buys the number the rest of this depends on, shown live under the settings
row: what the passes cost on this GPU at this resolution. Under about 8 ms and
inserting frames is worth building; near 16 ms it costs more than the frame it
would save, and the honest answer is to stop.

**Still to write: the present loop.** To show more frames the title has to
present more often - a second acquire, a second submit and a second present per
game frame, and the real frame held back one so the generated one lands between
its parents. That is where the latency is traded for smoothness, and it is the
part that can hang the console, which is why the cost is measured first.

**FG_METHOD_NEURAL is the only method available here.** The others want motion
vectors, and a PSP game has none: PPSSPP's depth buffer is the emulated
console's and does not describe the picture on screen. The neural method reads
the two colour frames and nothing else, which is exactly what there is.

**The readout came first, deliberately.** The previous attempt (2026-10-28) was
removed because nothing could show whether it was doing anything.
`__DisplayGetFPS` reports the emulated vblank rate - about 60 for any game
running at full speed, whatever it is drawing - so it answers the same way with
a generator and without one. The counter now sits on `DisplayFireActualFlip`,
under `fbReallyDirty`, which is PPSSPP's own decision that the picture changed
and so the only honest definition of a real frame. Generated frames are counted
where they are made. The settings row shows both rates and the cost per frame,
and it is worth having before anything is generated: it says what a game is
really drawing rather than what it is being clocked at.

**The frames are not already there to be replaced.** PPSSPP presents only when
the framebuffer really changed, so a game at 30 fps presents thirty times a
second and sends no duplicates. A generator here has to *insert* a frame and
present it itself, which means owning part of the present loop - and holding the
real frame back by one so the generated one can go first, which is latency
traded for smoothness. That is the piece still to write.

**Keep Vulkan out of the boundary.** `framegen.h` includes `<vulkan/vulkan.h>`
and wants volk's declarations; PPSSPP keeps its own in namespace `PPSSPP_VK`,
and a unit seeing both finds every `vkXxx` ambiguous. So the generator lives in
`psp5_ui` (the volk target) and `ui/PS5FrameGen.h` has no Vulkan type in it at
all - handles cross as `void *`, formats as `uint32_t`.

**Shut it down after `delete draw_`, not before.** PPSSPP joins its render
thread only when the render manager is destroyed.

**`tools/link-title.sh` now unstages as well as stages.** It used to create
`dist/<TITLE_ID>/framegen/` and never remove it, so a build made after the
feature was taken out still shipped 1.7 MB of its weights. The `elif` that
deletes the folder is the fix, and the mirror does the same.

## Notes

The console port itself is not psp5's work: `patches/ppsspp/ps5-port.patch` is
taken unmodified from PS5_RetroArch, where it is proven on hardware. What psp5
adds is what a title needs and a libretro core does not.
