# Nintendo Switch port (libnx + SDL2 + OpenGL)

Homebrew target for DKC1Recomp. Boots straight into the game — no
pickers or launchers; settings live in the in-game menu.

## Build

Prerequisites: devkitPro with devkitA64, libnx, and switch portlibs SDL2.

```sh
# 1. Generate the private recomp units once (needs the USA v1.0 ROM;
#    never committed — generated/ is gitignored):
python scripts/generate_snesrecomp.py --rom <path>/dkc1.sfc

# 2. Build the homebrew:
make -f Makefile.switch            # -> dkc1.nro + dkc1.nacp
```

Without step 1 the link stops at undefined `g_dispatch_table` /
`g_ram_routine_guards` symbols; everything else compiles and links.

## Install

- Copy `dkc1.nro` to `sdmc:/switch/dkc1/`.
- Copy the legally obtained ROM to `sdmc:/switch/dkc1/rom.smc`
  (`rom.sfc` also accepted; a 512-byte copier header is stripped and the
  payload is verified as USA v1.0 before boot).
- First boot writes defaults; `saves/` holds SRAM and savestates.
  A missing ROM writes `MISSING_ROM.txt` next to the NRO.
- Debugging without nxlink: create an empty `sdmc:/switch/dkc1/log.flag`
  and stderr is redirected, unbuffered, to `sdmc:/switch/dkc1/debug.log`.

## Controls (fixed, seamless)

Player 1 and 2 autodetect pads in order (a lone pad drives player 1).

| Switch | SNES |
|---|---|
| A / B / X / Y | A / B / X / Y |
| ZL / ZR | L / R |
| Plus (+) | Start |
| Minus (-) | Select |
| DPad, left stick | DPad |
| R3 (click the right stick), or Plus + Minus together | In-game menu |
| L (hold) / R (hold), when rewind is on | Rewind / 3x fast-forward |

## In-game menu

Opening the menu pauses the game; B, R3 or Plus closes it. Settings are
saved to `sdmc:/switch/dkc1/menu.cfg`.

| Item | Effect |
|---|---|
| Ranura 1–5 | Save-state slot (`quicksave.state`, `slot2.state` … in `sdmc:/switch/dkc1/`) |
| Guardar / Cargar estado | Save or load that slot (loading closes the menu) |
| Rebobinar L / Avanzar R | Hold L to rewind (snapshot every 3 frames, up to 128 MiB), R for 3x speed |
| Música MSU-1 | Use the pack in `msu1/`; takes effect at the next launch |
| Texturas HD | HD pack on or off (16:9/16:10 only) |
| Vidas infinitas | Lives (`$7E0575`) never drop below 5 |
| Códigos (cheats.txt) | Pro Action Replay codes from `sdmc:/switch/dkc1/cheats.txt` |
| Rendimiento en pantalla | Frame-time HUD (see below) |
| Salir del juego | Save SRAM and quit |

`cheats.txt` holds one 8-digit Pro Action Replay code per line
(`7E0575 09`, `7E0575-09` and `7E0575:09` all work; `#` starts a comment).
Only WRAM codes (`7E`/`7F` banks) are accepted; the ROM is never patched,
so Game Genie codes are not supported. Codes are written before every frame.

## Performance

60 fps on the A57 needs a few things together:

- **CPU boost.** Horizon runs titles at 1020 MHz; the port always raises
  the CPU to 1785 MHz (the console's own boost rate) through `clkrst`
  (`pcv` before 8.0.0). The GPU clock is untouched. The rate is re-checked
  every 5 s, because docking and the HOME menu reset it.
- **Threads on their own cores.** Horizon starts every thread on the
  process's default core. The emulation thread is pinned to the first
  usable core and each HD worker to another (`switch_clock.c`).
- **HD on the GPU.** With an HD pack, the PPU records which tile and
  texel made every pixel. Worker threads encode that into two G-buffer
  textures, and an OpenGL pass composes the HD frame
  (`dkc1_hd_gpu.c`, bit-exact with the CPU compositor). The Switch default
  is 2x scale.
- **HUD.** "Rendimiento en pantalla" shows, averaged per second: the total
  frame cost, emulation (`cpu`), PPU + HD + upload (`ppu`), the worst frame,
  and how many frames went over 16.7 ms (`lento`, red when non-zero). The
  same line goes to stderr (`log.flag` → `debug.log`).

Sound breaking up means frames are over budget: the audio queue only
absorbs ±0.5 % of rate drift. The HUD shows which part is slow.

Measured on device (HD v2 pack at 2x and MSU-1 on, 168 s of play,
`log.flag`):

| | HD v1 pack | HD v2 pack |
|---|---|---|
| Average frame in a level | 18–22 ms | about 12 ms |
| Frames over 16.7 ms in a level | 20–45 per second | 0 |
| GPU composition (`gpu.flag`) | 15–28 ms | no longer the limit |
| GL upload (`submit`) | 2.4 ms | 1.3–1.5 ms |

What is left: when the game uploads music or samples to the SPC (logos,
level transitions), that code runs interpreted and emulation costs
8–13 ms for a moment. In 168 s, 10 seconds (363 frames) went over budget,
all of them at those moments, with 13 audio gaps.

## Files written to the SD card

`menu.cfg`, the save states, `saves/` (SRAM) and, only with `log.flag`,
`debug.log`. The runtime's tier-2 discovery journals (`tier2_*.json*`,
a developer tool that records interpreted dispatch sites) are not written
on Switch unless a `SNESRECOMP_TIER2_*` path is set; any platform can turn
them off with `SNESRECOMP_TIER2_DISABLE=1`. Files left by older builds can
be deleted.

## Faster interpreted stretches

Music and sample uploads to the SPC run in the interpreter (logos, level
transitions). Two exact optimizations cut their emulation cost by 36 % on
the desktop (the worst second went from 3.5 to 2.2 ms):
- The quiescence detector compares the epochs first, which differ on every
  live register poll.
- `cpu_read8` reads plain ROM through a table of 4 KiB page pointers built
  per cartridge. The table is rebuilt when the cart, its image or the SRAM
  changes; carts with coprocessors keep the full routing.

Video, WRAM, OAM and audio hashes are unchanged in 16:9 and 4:3.

## Widescreen HUD

In widescreen levels the banana counter and the lives counter sit at the
screen edges, not inside the centered 256 columns. DKC1 draws them into
the first OAM slots, ahead of every object, and only while they are shown.
`Dkc1HudOamPrefix` (`dkc1_game.c`) counts that leading run of HUD sprites
by their tiles: the spinning banana, the digits and the Kong heads, all in
the top band. The PPU's HUD OAM shifter then moves the left half to the
left edge and the right half to the right edge. This is presentation only:
WRAM, OAM and audio hashes are unchanged.

## Audio

**Output.** The Switch build feeds audren itself (`runner/switch_audio.c`)
instead of using SDL's queued device. SDL 2.28's Switch driver gives its
feeder thread priority 0x3B (it maps every request except
`SDL_THREAD_PRIORITY_HIGH`, including the `TIME_CRITICAL` its audio thread
asks for, to that lowest level) on the process's default core, the
emulation core, and gives audren only two 21 ms buffers. That thread ran
only while the emulation thread slept, so a long frame left audren with
nothing to play. That crackle existed on Switch only, the same mix was clean
on the desktop. The replacement feeder:
- runs at 0x2B, above the emulation thread;
- is pinned to the last usable core;
- wakes every audren frame (5 ms) and keeps eight 5 ms buffers queued.

The host's queue and rate servo are unchanged. The HUD's `aud` field counts
the buffers per second that had to play silence because the host fell
behind.

**Frame cadence.** The host sizes each frame's audio for 60 Hz, so the loop
must average exactly 60 Hz. The frame pacer (written for macOS) re-anchored
its schedule whenever a frame was over 2 ms late. On Switch, a GL swap
sometimes takes 8–9 ms, so the loop ran at 58.6–58.9 Hz. The audio was then
0.3–0.5 % short every second, both rate servos sat at their limit with an
empty queue, and audren played about five silent gaps a second: the
remaining crackle. On Switch the pacer now keeps a fixed cadence and makes
up a lateness of up to three frames over the following frames; only a
longer stall re-anchors and restarts the audio.

Measured on device, MSU-1 and HD off, 60 s:

| | gaps/s | loop rate | queue (target 3328) |
|---|---|---|---|
| SDL audio driver | crackle, not measured | — | — |
| own feeder, re-anchoring pacer | ~5 | 58.6–58.9 Hz | 0–600, servo pinned at 1.005 |
| own feeder, fixed cadence | 1 in the whole minute | 60.000 Hz | ~3000–3600, servo 0.996–1.003 |

Two other approaches were tried and dropped:
- Sizing the audio from a measured loop rate: slow stretches skewed it, and
  the queue overflowed afterwards.
- Vblank pacing (swap interval 1): the driver then blocks the texture upload
  for most of a frame and misses vblanks, so the loop ran near 57 Hz.

**Diagnostics.** An empty `audio_dump.flag` in the app folder records 60 s
of the mix: `audio_render.raw` (after rate conversion), `audio_queued.raw`
(after the servo) and `audio_played.raw` (as handed to audren, gaps
included), all 16-bit stereo 48 kHz. Each second it also logs the queue
depth, the servo ratio, the underflow counters and the loop rate to stderr;
add `log.flag` to send stderr to `debug.log`.

**Rate conversion.** Every stage that changes the sample rate uses a Kaiser-windowed sinc
filter (16 taps) instead of linear interpolation:

| Stage | Where | Before → after |
|---|---|---|
| MSU-1 track 44.1 kHz → device rate | `runner/dkc1_msu1.c` | linear: intermodulation 28 dB above the true 16–20 kHz content on tonal tracks → matches a soxr reference (−79 vs −78 dB) |
| SPC 32.04 kHz → 48 kHz (only when the device rate differs from native) | `snesrecomp/.../common_rtl.c` | images above 16 kHz −49 → −62 dB; treble no longer dulled |
| Queue rate servo (±0.5 %) | `runner/desktop_audio_rate.c` | a 12 kHz tone carried −18 dB of modulation products → −83 dB |

At the native rate, and at an exact stretch ratio of 1, the filters pass
samples through unchanged, so desktop hosts and the headless runner keep
their audio hashes.

Compressed MSU-1 tracks are decoded on their own thread (pinned off the
emulation core on Switch). The thread reads the whole `.ogg` into memory
when the track starts (at most about 6 MB) and keeps half a second decoded
ahead. The frame-critical mixer only copies decoded frames. Before, it
decoded 46 ms chunks from the SD card in place, which made 0.4–1 ms spikes
on the desktop and several milliseconds on the A57: enough to push a frame
over budget and starve the queue. A track now starts once its file is
loaded, a few tens of milliseconds after the game asks for it.

`DKC1_AUDIO_RATE=48000` makes the headless runner render at 48 kHz, so the
Switch conversion path can be checked off-device (`DKC1_AUDIO_PCM` writes
the output).

## MSU-1 music and HD textures

- Compress a PCM MSU-1 pack with `tools/msu1_compress.py --in <pack>
  --out <ogg-pack>` (about 13 % of the original size) and copy the result
  to `sdmc:/switch/dkc1/msu1/` (`dkc_msu-N.ogg` or `track-N.ogg`). Raw `.pcm`
  packs are not supported on Switch (no mmap).
- An HD texture pack (`tools/hd_pack.py`) in `sdmc:/switch/dkc1/hd/`
  (`tiles.bin`) enables HD textures in 16:9/16:10.

## Presentation

- OpenGL 3.3 core (GLES 3.0 fallback) presenter that also runs the HD
  compositor; SDL_Renderer when neither context is available. Aspect
  follows the desktop default (16:9).
- Resolution follows the console: 1280x720 handheld, 1920x1080 docked
  (`appletGetOperationMode` at boot plus a 12 Hz check that resizes on
  dock/undock mid-session).
- Frame deadlines stay on the host 60 Hz clock (swap interval 0, so the
  GL swap never adds a second wait).

## Deliberately excluded on Switch

Native (OS) menus, ROM/music/file pickers, raw (.pcm) MSU-1 packs,
Baby Kong ROM, TCP debug server, co-sim, oracle, Metal presenter,
post-mortem minidumps, tier-2 JSON manifests. `log.flag` file logging and the `[saves]` SRAM log lines
stay.

## Files

| Path | Role |
|---|---|
| `Makefile.switch` | devkitA64 makefile (includes the framework fragment, forces SDL2/no-launcher defines, `--gc-sections`) |
| `runner/sdl_host.c` (`__SWITCH__`) | SD bring-up, `sdmc:/switch/dkc1/rom.smc` resolution, GL presenter, SDL-clock frame pacer, raw-joystick input, applet tick, SRAM seed + 5 s dirty-check writer, in-game menu loop, cheats, perf HUD |
| `runner/dkc1_overlay_menu.c` | In-game menu: items, `menu.cfg`, PAR cheat parser, 8x8 text (public-domain `third_party/font8x8`) into an alpha overlay |
| `runner/switch_clock.c` | CPU boost (`clkrst`/`pcv`) and core pinning |
| `runner/dkc1_hd.c` + `runner/dkc1_hd_gpu.c` | HD capture/encode and the GL compositor |
| `runner/switch_platform.c` | `Dkc1Mac*` surface: fixed handheld settings/controls, null pickers, no-op menus/links/presenters |
| `runner/switch_gamepad.c` | HID-order raw joystick reader (A/B/X/Y, ZL/ZR triggers, +/-/Start/Select, stick-as-DPad) + native 2-pad controller applet |
| `runner/dkc1_msu1.c` + `runner/dkc1_stb_vorbis.c` | MSU-1 music from a compressed (Ogg Vorbis) pack in `sdmc:/switch/dkc1/msu1/`, streamed from the SD card |
| `snesrecomp/runner/src/switch/` | framework: SD bring-up, applet tick, exit/focus hooks, ROM resolver (new files in the pin) |
| `snesrecomp/runner/switch.mk` | framework source fragment for make builds |
