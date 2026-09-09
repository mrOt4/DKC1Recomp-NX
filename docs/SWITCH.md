# Nintendo Switch port (libnx + SDL2 + OpenGL ES2)

Homebrew target for DKC1Recomp. Boots straight into the game — no
menus, pickers, or launchers.

## Build

Prerequisites: devkitPro with devkitA64, libnx, and switch portlibs SDL2.

```sh
# 1. Generate the private recomp units once (needs the USA v1.0 ROM;
#    never committed — generated/ is gitignored):
python scripts/generate_snesrecomp.py --rom <path>/dkc1.sfc

# 2. Build the homebrew:
make -f Makefile.switch            # -> dkc1.nro + dkc1.nacp
#    make -f Makefile.switch DEBUG=1  # + L3/R3 quick savestates
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
| L3 / R3 (`DEBUG=1` builds only) | Quick-save / quick-load |

## Presentation

- Fixed SDL window on the GLES2-accelerated renderer; the framebuffer
  fills the window. Aspect follows the desktop default (16:9).
- Resolution follows the console: 1280x720 handheld, 1920x1080 docked
  (`appletGetOperationMode` at boot plus a 12 Hz check that resizes on
  dock/undock mid-session).
- VSync is on; frame deadlines stay on the host 60 Hz clock.

## Deliberately excluded on Switch

Native menus, ROM/music/file pickers, MSU-1 packs, Baby Kong ROM,
pause menu, overlay equivalents, TCP debug server, co-sim, oracle,
mods, GLSL/Metal/GL presenters, post-mortem minidumps, tier-2 JSON
manifests. `log.flag` file logging and the `[saves]` SRAM log lines
stay.

## Files

| Path | Role |
|---|---|
| `Makefile.switch` | devkitA64 makefile (includes the framework fragment, forces SDL2/no-launcher defines, `--gc-sections`) |
| `runner/sdl_host.c` (`__SWITCH__`) | SD bring-up, `sdmc:/switch/dkc1/rom.smc` resolution, fixed 720p window, SDL-clock frame pacer, raw-joystick input, applet tick, SRAM seed + 30 s writer, L3/R3 debug |
| `runner/switch_platform.c` | `Dkc1Mac*` surface: fixed handheld settings/controls, null pickers, no-op menus/links/presenters |
| `runner/switch_gamepad.c` | HID-order raw joystick reader (A/B/X/Y, ZL/ZR triggers, +/-/Start/Select, stick-as-DPad) + native 2-pad controller applet |
| `runner/switch_msu1_stub.c` | no-op MSU-1 (needs mmap + pack UX) |
| `snesrecomp/runner/src/switch/` | framework: SD bring-up, applet tick, exit/focus hooks, ROM resolver (new files in the pin) |
| `snesrecomp/runner/switch.mk` | framework source fragment for make builds |
