# DKC1Recomp — complete tool reference

Companion to `SKILL.md`. Everything documented here lives in the repo;
paths are repo-relative. `<rom>` = headerless DKC1 USA v1.0.

## Build scripts (repo root)

| script | purpose |
|---|---|
| `build_host_tools.bat` | Isolated tool-session build (own obj dir/exe names, never contends with the primary session): `build/dkc1_headless_tools.exe`, `dkc1_desktop_tools.exe`, `dkc1_layer_capture.exe` |
| `build_host.bat` | Primary build: `dkc1_snesrecomp_headless.exe`, `dkc1_desktop.exe` |
| `build_host_noadapt.bat` | Builds from a generated tree WITHOUT the widescreen adapters (`build/gen_noadapt`) — the no-adapter oracle used to prove adapter inertness |
| `build_phaseguard_headless.bat` | Prefetch-phase-guard instrumented headless |
| `rebuild_widescreen_runtime.bat` | Regenerate + rebuild after recomp/cfg changes |
| `link_desktop_candidate.bat` / `rebuild_diagnostics_candidate.bat` | Link/rebuild under candidate exe names while a running visible exe holds the standard name |
| `build/link_desktop_retry.bat` | Link tool-session desktop to `_new` name, then retry the standard name |

All inject build identity (`git commit(+dirty) / config / timestamp`) shown
in the window title, debug panel, and written to `<state>.buildinfo.json`
sidecars; loading a state from a different build warns.

## Hosts

- **`dkc1_desktop_tools.exe <rom>`** — visible debugger. Dark-themed,
  menu bar (File: quick/save/load state via dialogs, export bundle;
  Emulation: pause/step; View: fullscreen, panel, provenance, FPS,
  layers). Keys: arrows+Z/X/S/A/Q/W game input, Enter=Start,
  F1 provenance overlay, F2–F6 layer isolation, F7 pause, F8 step,
  F9 export repro bundle, F11/F12 quick save/load, Alt+Enter fullscreen
  (Esc returns), Esc quit. **Click any pixel** → provenance report
  (world/tile/entry/writer/OAM/actor) in the panel.
- **`dkc1_headless_tools.exe <rom> [frames]`** — deterministic runner;
  prints end-of-run frame/WRAM/VRAM/CGRAM/OAM sha256 + audio fnv1a.
- **`dkc1_layer_capture.exe <rom> <state> <outdir>`** — same-frame layer
  isolation: reloads the snapshot per mask → backdrop/composite/BG1/BG2/
  BG3/OBJ PPMs + occupancy masks; aborts if frames differ.

## Environment variables (all default-off)

**Run control:** `DKC1_WIDESCREEN` (default 1; 0=native 4:3),
`DKC1_WIDESCREEN_EDGE` (level-wall presentation: `glide` default = the
inward clamp released over eight margins of travel, `reflect`, `bars`,
`shift` = pre-policy inward clamp, the A/B reference for wall reports),
`DKC1_SCRIPT` (route .dks), `SNESRECOMP_INPUT_PLAY` (raw input replay),
`DKC1_STARTUP_SCRIPT` (macOS input/wait-only .dks route run unthrottled before
the first interactive frame; state and checkpoint directives fail closed),
`DKC1_ALLOW_ROM_SHA256` (development-only exact 4 MB modified-ROM pin;
retail verification remains the default and malformed/non-matching pins fail),
`DKC1_SAVESTATE_INPUT` (load state at boot), `DKC1_SAVESTATE_OUTPUT` /
`DKC1_SAVESTATE_SAVE_AT` (save at frame), `DKC1_SRAM_INPUT`,
`DKC1_SUPERZSNES_STATE` (import emulator state bundle),
`DKC1_ROUTE_FRAME_LIMIT` / `DKC1_ROUTE_AUTOCLOSE_MS` /
`DKC1_ROUTE_RESULT` (visible-host automation), `SNESRECOMP_FPS`,
`DKC1_PRESENT_HZ` (optional 30-240 Hz presentation-cadence override;
normally the host chooses an exact 60 Hz display divisor when available),
`DKC1_USE_DISPLAY_LINK_PACING` (macOS A/B: opt into window-bound display-link
cadence), `DKC1_KEEP_RENDERER_VSYNC` (macOS A/B: restore blocking SDL Metal
vsync), `DKC1_DISABLE_DISPLAY_LINK` / `DKC1_DISABLE_VSYNC` (explicit negative
overrides), `DKC1_DISABLE_METAL_PRESENTER` (macOS A/B: restore SDL
presentation). The macOS release default keeps emulation on one fixed 60 Hz
Mach authority while a host-only Metal display link presents immutable frames
independently at the requested 120 Hz panel cadence.

**Evidence taps:** `DKC1_PACING_LOG` (desktop-host frame work/wait/submit/
GDI timing jsonl; summarize with `tools/analyze_pacing.py`),
`DKC1_SCANOUT_LOG` (macOS physical drawable `presentedTime`, source-frame
repeat, queue, camera, and PPU-scroll jsonl; summarize with
`tools/analyze_scanout.py` while the app is visibly unobscured),
`DKC1_WS_TRACE` (per-frame widescreen decision/hash
jsonl), `DKC1_WRAM_DUMP`=first-last + `DKC1_WRAM_DUMP_PATH` (+optional
`_RANGES`) raw WRAM frames, `DKC1_WRAM_HASH_LOG` (per-frame WRAM
fingerprint), `DKC1_WRAM_OUTPUT`/`DKC1_VRAM_OUTPUT` (final memory),
`DKC1_OAM_LOG` (shadow+PPU OAM per frame), `DKC1_LIFECYCLE_TRACE`
(+`DKC1_LIFECYCLE_SAMPLE_EVERY_FRAME`), `DKC1_INPUT_RECORD`,
`DKC1_SESSION_DIR` (checkpoint output), `DKC1_FRAME_PPM*` (frame images),
`DKC1_AUDIO_PCM`, `DKC1_STATE_TRACE`, `DKC1_STREAM_DEBUG`,
`DKC1_TRACE_PC` (PC probes on interpreter-tier execution),
`DKC1_HD_PPM_SEQUENCE=1` (with `DKC1_FRAME_PPM_PREFIX`: the HD frame too, as
`<prefix>_hd_N.ppm`), `DKC1_DRAW_TIMES`=path (per frame: PPU draw and HD
part, microseconds).

**Performance switches (on by default where noted):** `DKC1_PPU_BANDS`=0/1
(scanline bands on the HD worker pool; default on Switch only, byte-identical
to the sequential loop), `DKC1_HD_PBO=0` (direct texture uploads instead of
the pixel-buffer ring), `SNESRECOMP_INTERP_LEAN=0` (full interpreter step
for APU-port poll loops too). Switch bench mode: `bench.flag` +
`bench/cases.txt` in the app folder (see `docs/SWITCH.md`).

**Detectors (integrity, all counted + logged):**
`DKC1_INVARIANT_MONITOR`=jsonl|1 — 9-verdict cross-subsystem monitor;
`DKC1_BLANK_SCAN`=jsonl — rendered-blank margin columns;
`SNESRECOMP_WS_CACHE_LOG`=jsonl — scene-local cache out-of-range events;
`SNESRECOMP_WS_RETRODICT`=jsonl — served-margin vs stream-truth
mismatches; `SNESRECOMP_WS_WRITE_TRACE`=1 — per-cell last-writer
attribution (powers click-to-provenance); `DKC1_WS_PROVENANCE`,
`DKC1_MARGIN_PROXIES`/`_LOG`/`_RENDER`, `DKC1_PREFETCH_PHASE_GUARD`,
`DKC1_PREFETCH_TRANSACTION_DEBUG`, `DKC1_WS_COLD_STATE_LOAD`,
`DKC1_WS_FORCE_FALLBACK_FRAME`, `SNESRECOMP_WS_YLOG`.

**Capture:** `DKC1_FLIGHT_RECORDER`=1 (+`_DIR`) — rolling ~60s inputs +
periodic state anchors; F9/auto exports a bundle (states, inputs, WRAM/
VRAM/CGRAM/dual OAM, PPU regs, manifest w/ hashes + build id, layer
captures, post-failure tail). `DKC1_AUTO_EXPORT`=1 — any detector hit
triggers the export. `DKC1_DESKTOP_DEBUG_PANEL`=0 hides the panel.

**Tier2 discovery captures:** default-named `tier2_*.json(l)` files land
in `build/tier2/` (hosts set `SNESRECOMP_TIER2_DIR` automatically);
`SNESRECOMP_TIER2_MANIFEST`/`_JOURNAL` override paths explicitly;
`SNESRECOMP_TIER2_VERBOSE` for detail.

**Trace-hook build** (`build_host_trace.bat` -> `dkc1_headless_trace.exe`,
lean `SNESRECOMP_FUNC_ENTRY_HOOK` mode): `SNESRECOMP_FUNC_PROFILE`=jsonl
per-function call counts/frames/contexts (+`SNESRECOMP_PROFILE_CONTEXT_ADDR`,
DKC1 uses 0032); `SNESRECOMP_WATCH`=addr:len[,...] +
`SNESRECOMP_WATCH_LOG` — bounded WRAM watchpoints reporting net byte changes
at matched generated-function entry/exit boundaries. Parent tails after a
callee return remain attributed to the parent; changes observed with no AOT
window active (including top-level interpreter execution) are explicitly
`host/outside-function-window`, never assigned to the stale previous entry.
Multiple writes that cancel inside one function
window are below this lean mode's resolution; use force_lle + `DKC1_TRACE_PC`
for instruction/store-level attribution. Invalid/overlapping/out-of-WRAM specs
fail closed, and `reverse_watch.py` refuses conclusions from truncated logs.

**Engine diagnostics:** `SNESRECOMP_PPU_PROFILE` (tool build only; aggregate
per-stage renderer CPU time at exit), `SNESRECOMP_DSPOUT`, `SNESRECOMP_*_TRACE_FILE`,
`SNESRECOMP_OFFRAILS_STDERR`, `SNESRECOMP_APU_TOUCH_CYCLES`.
The tool build also retains `SNESRECOMP_STACKBAL_AUDIT`; normal player builds
compile out its per-return hash-table update while preserving the semantic
recomp call stack and interpreter/AOT return handling.

## Route DSL (`recipes/*.dks`)

```
MASK [* N]                 # input mask (hex, snes9x bit order) for N frames
wait ADDR OP VALUE [width|mask|shift|signed|timeout N]
pulse MASK ON OFF [base HEX] ADDR OP VALUE [timeout N]   # press until predicate
checkpoint NAME            # WRAM/VRAM/OAM hashes + wram dump into session dir
state_save PATH / state_load PATH
```
Bits: 1=B 2=Y 4=Select 8=Start 10=Up 20=Down 40=Left 80=Right 100=A
200=X 400=L 800=R. Level-entry edge = frame counter `$0028` reset.
Routes whose first op is `state_load` are dependent legs (sweep skips
them). Contracts (`contracts/*.json`) bind a route to checkpoint
expectations, ws-trace assertions, integrity `budgets` (ratchets), and an
optional `quickload` leg seeded by a state the entry route itself saves.

## Tool catalog (`tools/`)

**Understanding / naming**
- `reverse_watch.py --rom R --route S --address HEX[:len] --before-frame F`
  — who last changed this address before frame F, function-attributed,
  with context and escalation hints (one deterministic forward pass).
- `impact.py ADDR|NAME` — change blast radius: structured callers from the
  exported IDA call graph (exact instruction operands if unavailable),
  dispatch membership, routes that executed it, and required regression
  gates. Pseudocode substring matches are not caller evidence.
- `build_profile_corpus.py --rom R` — all-or-nothing per-route function
  profiles into build/profiles/ (feeds impact/profile_diff). Existing profiles
  are removed before rebuilding; any failed route or missing/invalid profile
  returns nonzero and publishes no partial corpus.
- `capability_manifest.py` — docs/CAPABILITIES.json: per-scene
  host-widescreen status (proven/degraded/centered/unproven), strictly
  evidence-based from successful sweep routes. `proven` requires complete
  calibration and zero raw fallback, aggregate blank serves, gameplay
  pillarbox frames, or unstable margins; blockers are emitted explicitly.
- `ir_validate.py --stage1|--stage2|--stage3|--all` — validation gates for
  the staged 65816 IR (lossless decode, CFG/SSA/width facts, typed-memory
  coverage). A ROM is required only for the stage-1 opcode-byte oracle.
- `irview.py ADDR|NAME [--ssa]` — structured view from the validated IR.
  Instruction-index function labels are seed boundaries, so the renderer
  explicitly marks external tail fallthroughs, unresolved indirect successors,
  CFG/SSA problems, width conflicts, and unreachable blocks. It never silently
  splices a neighboring seed into the selected routine.
- `slice.py --store HEX [--callers --readers]` — static complement of
  reverse_watch: every IR-proven write site covering a WRAM address,
  each with the SSA backward slice of the stored value (constants,
  loads, merges, entry params). `--value-of OPADDR` slices A at one op.
  Validated: `--store 1595` reproduces the damage chain (BFC745 #$0001,
  SteelKeg BFD005 #$0040) that reverse_watch proved at runtime.
- `oracle_spec.py NAME | --emit-all` — per-function differential-oracle
  capture/compare manifests from control-flow-closed IR effects. Proven
  external tail fallthroughs and direct tail jumps are followed; unresolved
  continuations fail closed to `needs-lle-shadow`
  (build/ir/oracle_specs.json). Honest eligibility: indirect writes,
  MMIO ordering, or deep calls mark a function needs-lle-shadow instead
  of pretending state-diff suffices.
- `ir/summarize.py` (run as module) — build/ir/summaries.json:
  per-function proven read/write sets with widths, indexed-ness and op
  sites; feeds atlas ("IR-proven writers" on WRAM view), impact.py
  (write set + data-coupled readers), slice.py, oracle_spec.py.
  Regenerate after any disassembly/rename_map update.
- `structure.py ADDR|NAME` — flat symbolized 1:1 listing (curated RAM
  names, context-qualified define annotations, local cross-reference
  labels); display aid, no reconstructed blocks or semantic claims.

**Mod layer (docs/MOD_LAYER.md)**
- `gen_symbols.py [--show ADDR]` — build/ir/symbols.json: ONE canonical
  generated record per function (names+provenance, proven entry/exit
  M/X, symbolic read/write sets, callers, dispatch roles, runtime-route
  evidence, oracle eligibility). rename_map.json stays the only
  hand-edited name source. Regenerate after summarize/oracle/profile
  updates.
- `gen_wram_header.py [--check]` — runner/dkc1_wram_gen.h: named WRAM
  offsets, little-endian view accessors over live WRAM (never copies),
  actor SoA per-field accessors, struct mirrors. `--check` = staleness
  gate + independent cross-parser address agreement.
- `mod_conflicts.py mods/*.json` — routine-replacement conflicts, WRAM
  write-set overlaps between mods, presentation-class violations
  (presentation mods may not replace gameplay-writing routines), and
  oracle-eligibility / no-runtime-evidence warnings.
- `oracle_run.py FN --rom R --route S --out LOG [--exe EXE]` — one
  differential-oracle capture leg: arms the trace host
  (`SNESRECOMP_ORACLE`/`_RANGES`/`_LOG`, ranges derived from the
  function's oracle spec) and replays a deterministic route; per
  outermost call it logs entry/exit registers, flags byte, WRAM
  ranges, and cycle delta. Zero captures = the route never ran the
  function (not equivalence).
- `oracle_diff.py A.jsonl B.jsonl` — byte-identical = proven-equivalent
  over that route; otherwise the FIRST divergent call with field-level
  breakdown and an upstream-vs-local verdict (entry states matching
  means the function itself diverged).
- `gen_replacements.py --rom R [--bless]` — fail-closed staging for
  DKC1_REPLACE: supported-ROM sha, blessed region-byte hash, proven
  entry-mode match, single defining TU; emits the build override that
  renames the generated variant to `*_original` and links
  `runner/replacements/`. Then `build_host_replace.bat` builds
  `build/dkc1_headless_replace_trace.exe` (`DKC1_REPLACE_DISABLE=1`
  falls back to originals at runtime). Validate stock-vs-replace with
  oracle_run/oracle_diff + end-of-run hashes; see docs/MOD_LAYER.md.
- `coverage_explorer.py` — docs/COVERAGE.md + build/coverage.json: the
  full 256-entrance universe joined against capabilities + sweep
  evidence, with a ranked next-evidence worklist (centered-only scenes
  first — a route already exists; then unobserved *_Main levels).
  Multiple scene variants for one entrance aggregate conservatively: an
  entrance is proven only when every observed variant is proven.
  never-observed = absence of evidence, never assumed-unreachable.
- `promote_bundle.py BUNDLE --rom R [--name N]` — flight-recorder
  capture -> LOCAL regression asset (recipes/promoted/ +
  contracts/promoted/, both gitignored: snapshots are never committed).
  Gates: manifest/ROM/every-declared-file hashes, path-safe promotion name,
  Nx byte-identical end-WRAM replay,
  and match against the bundle's own final.wram.bin
  (--allow-capture-drift records instead of refusing, for captures from
  older builds). Emits a state_load + run-length-MASK replay.dks and a
  contract with scene-identity checkpoints and zero budgets — promoted
  contracts inherit the full ratchet discipline immediately.
- `sync_names.py` — derive `<Base>_StateN` names for dispatch-contract
  targets in literal table-ordinal order (provenance-tagged, curated map
  always wins) -> safe generated `docs/derived_names.json`, consumed by
  the state catalog and structure display tools; never writes `reference/`.
- `state_catalog.py` — docs/STATE_MACHINES.md: per state machine, each
  state in literal dispatch order, with conservative static refs/immediate
  stores; `--lifecycle` marks matching observed NorSpr actor states.
- `profile_diff.py A [B]` — coverage + "functions exclusive to run A"
  behavioral isolation from trace-build profiles. Its coverage denominator is
  the exact address-bearing `CpuState` alias declaration set in `funcs.h`, not
  M/X variants or handwritten helpers.
- `poke_test.py --state S --set ADDR=HEX --run N --expect EXPR` — WRAM
  fault injection (proves downstream reaction, not natural production).

**Navigation / knowledge**
- `atlas.py ADDR|7EXXXX|name:TERM [--callers] [--json]` — unified query
  across IDA names+descriptions, disassembly+pseudocode, live recomp
  variant, dispatch contracts, WRAM labels, known issues.
- `export_ida_dispatch.py [--apply]` — push cfg dispatch contracts into
  the seeded IDA DB as user xrefs (idat headless).
- `ingest_dkc1_disasm.py` — disassembly ingestion used for seeding.

**Regression / sweeps**
- `analyze_pacing.py LOG [--warmup N] [--json]` — summarize desktop
  scheduler submit cadence separately from emulation/render work and GDI
  completion; v1 and v2 pacing logs are accepted.
- `run_regression.py CONTRACTS --rom R [--json-out]` — 3×-identical gate
  (checkpoints + end-of-run renderer/audio hashes + integrity budgets),
  entry + quickload legs.
- `level_sweep.py --rom R` — run every standalone recipe; grades
  calibration, raw-fallbacks, blank serves, pillarbox-in-gameplay, margin
  instability, cache OOB, OAM X-high-loss signature.
- `make_dashboard.py` — regenerate `docs/DASHBOARD.md` + `dashboard.html`
  from results + sweep + `KNOWN_ISSUES.json`.
- `fresh_entry_stress_sweep.py`, `grade_fresh_entry_sweep.py`,
  `world_map_fresh_entry_sweep.py`, `snapshot_widescreen_stress.py`,
  `run_imported_state_suite.py`, `triage_stress_lifecycle.py` —
  entry/state stress suites.
- `check_widescreen_capability_floor.py REPORT` — fail-closed release ratchet
  for the exact committed 40 gameplay entrances. It rejects a lost entrance,
  nondeterminism, a failed widescreen grade, raw margin pixels, terrain misses,
  and unexpected centered/4:3 gameplay.

**Divergence / differential**
- `first_divergence.py --rom R --script S --frames N [--profile]` —
  stock-vs-wide first-divergence locator (resolve-then-replay, full-WRAM
  fingerprints, intended-differences profile =
  `contracts/wide-intended-differences.json`).
- `compare_widescreen_regions.py` — region differ.
- `bisect_transition_contamination.py`,
  `transition_contamination_sentinel.py`, `detect_legacy_width_cull.py` —
  transition/contamination hunters.

**Widescreen margin analysis**
- `analyze_ws_trace.py` — grade a `DKC1_WS_TRACE` (policy violations,
  fallback/blank counts, decision stats).
- `analyze_retrodiction.py LOG` — cluster served-vs-stream mismatches
  (attribute-byte vs wrong-tile, worst columns).
- `verify_shadow_localization.py`, `verify_vertical_rope_margins.py`,
  `verify_prefetch_soft_fallback.py`, `verify_margin_proxy_ab.py`,
  `build_margin_proxy_manifest.py`, `verify_widescreen_savestate.py`,
  `verify_blank_scan_detector.bat` — targeted margin verifiers.

**Object lifecycle / OAM**
- `lifecycle_by_source.py TRACE [--html-out]` — re-key lifecycle events
  by authored source record; FREED-IN-VIEW/THRASH flags; swimlane HTML.
- `oam_inspect.py` — shadow-vs-PPU OAM with 9-bit X decode, lost-X-high
  windows, DMA-lag streak logic.
- `analyze_persists.py` — grade wide-persists-stock-culls from raw WRAM.
- `audit_prefetch_phases.py`, `audit_prefetch_wram.py`,
  `audit_prefetch_transaction.py`, `analyze_prefetch_write_sets.py` —
  stock-vs-wide object phase auditors (match by source, conservative
  verdicts).
- `export_timeline.py` — event timeline export.

**Dispatch**
- `resolve_dispatch.py`, `audit_animation_dispatch.py`,
  `audit_indirect_tables.py`, `audit_dispatch_contracts.ps1` — harvest/
  verify indirect-dispatch targets feeding `recomp/*.cfg` contracts.

**Repro minimization**
- `macro_minimize.py INPUTS --predicate '{...}' [--snapshot-input]` —
  ddmin input shrink with 3×-consistency soundness.
- `minimize_bundle.py BUNDLE --rom R --predicate '{...}'` — minimize
  straight from a flight-recorder bundle (replays from its anchor).
- `verify_flight_bundle.py`, `verify_wram_dump.py` — evidence validators.
- `run_route_recipe.py` — single-recipe runner.

**Visible-host automation (PowerShell)**
- `launch_visible_snapshot.ps1`, `capture_visible_snapshot_library.ps1`,
  `validate_visible_snapshot_library.ps1`, `capture_process_window.ps1`.

## Key WRAM addresses (opcode-verified; full dictionary via atlas)

`$0028` frame ctr (resets at level entry) · `$0032` mode · `$003E`
entrance · `$0500/$0504` joypad held/pressed (P1) · `$088B/$0895` camera ·
`$1B23/$1B25` camera bounds (in-level: upper ≥ $100) · scanner `$00A0/A2/
A4` + window `$00EF/$00F1` · actor arrays indexed by even slot `$02..$32`:
id `$0D45`, source `$15FD`, x `$0B19`, y `$0BC1`, state `$1029`, anim
`$10D1`, pose `$0AE5/$0D11` · events `$1595` (**consumed same-frame**;
$40 damage, $01/$20 death → `Player_HandleHitEvents $BFA0F7`) ·
bookkeeping `$192B` len $100 · collision flags `$12A5` · invuln `$11A1`.

## Knowledge sources (read-only; addresses join them — use the atlas)

- `reference/disassembly/` (in-repo consolidated copy; original at
  `D:\Downloads\DKLR\DKC1_Disassembly\`) — labeled disassembly + RAM map
  (semantic intent; its `Custom\Patches\*Widescreen*` and `ROM_Map_HACK_*`
  belong to the LEGACY hack — ignore for recomp work)
- `reference/disassembly/DKC1/Pseudocode/` — mechanical C lift +
  lossless listing + `instruction_index.csv` (a stale recomp snapshot;
  never diagnose recomp behavior from it)
- `reference/disassembly/Tools/IDA/DKC1_U1.i64` +
  `work\rename_map.json` — curated names/descriptions + our runtime
  dispatch xrefs; rebuildable headlessly
- `reference/legacy-widescreen/` — the retired SuperZSNES-era emulator
  hack (worklogs/tools; prior art only, never current workflow)

## Aquatic presentation A/B switches (2026-09-06)

`DKC1_WS_PIXEL_BOUNDARIES=1`, `DKC1_WS_LIVE_SCROLL=1`, and
`DKC1_WS_WALL_ADJACENCY=1` independently opt into the DKC2/DKC3 presentation
backports. All require the exact value `1` and default off pending the full
40-entrance gate. The first protects native pixels in straddling 4bpp chunks
and repeat bands; the second uses live scanline scroll for shadow X; the third
uses structurally constrained map adjacency for vertical wall continuation.
Trace additions: root `presentation_features` is a 1/2/4 bitmask;
`boundary_adjacency_tiles` counts successful adjacency-derived 8x8 entries and
is included in `boundary_continuation_tiles`. Failed/ambiguous chains leave
the original decoded tile intact. `DKC1_ASPECT=16:10` now selects the existing
308x224 mode in headless and layer-capture hosts when widescreen is enabled.
Use `DKC1_WIDESCREEN_EDGE=shift` for an unbiased native-center oracle.
See `docs/WIDESCREEN_AQUATIC_BACKPORTS.md` for commands, acceptance scope, and
the paused local candidate bundle.

## September 6 Mac host and cache-boundary additions

- `DKC1_WS_SCROLL_REBASE=1`: default-off, currently calibrated cache rebuild on the exhausted-window frame. It also captures the live native guard row on calibrated fine-Y=7 frames, including ordinary scrolling without a rebase. WS trace feature bit 8 identifies the option; `decision.cache_rebase` identifies actual origin rebuilds only. `verify_shadow_localization.py` requires a calibrated cold commit for any marked origin change. See `docs/WIDESCREEN_WATER_FLASH.md`, `docs/WIDESCREEN_CAMERA_AUDIT.md`, and the Croctopus/Coral regression recipes.
- Native **Game → Controls and Assist…** exposes source routing, remapping, analog deadzones, and opt-in rewind/3× fast-forward. Default Assist holds: Backspace/Tab or left/right triggers. Game time remains canonical. Rewind memory is capped at 128 MiB; actual history duration depends on serialized-state capacity. See `docs/HOST_ADOPTION_IMPLEMENTATION.md`.
- `DKC1_ASSIST_TEST_INPUT=<input file>`: default-off Mac-only host-action schedule using the existing hex/repetition format; masks 1=rewind, 2=fast-forward, 4=quick-save, 8=quick-load. It enables Assist only for that run. Save/load actions operate the normal quicksave path; do not use those bits against a tester's only state. `DKC1_ASSIST_TEST_LOG=<path>` optionally records host tick, host/guest frame, pops, and history depth. Gameplay playback stays independent and is abandoned after a rewind/load.
- `DKC1_PAUSE_AFTER_FRAME=<positive host frame>`: default-off Mac exact-frame pause. Clears gameplay and host-action schedules and stops sound/rumble; preserves the actual submitted pixels for window capture. `DKC1_SAVESTATE_OUTPUT` also works at graceful Mac shutdown. Use private paths, not the user's normal slots.
- `DKC1_PACING_LOG` adds `audio_ratio`, `audio_fill_average`, and `audio_target_frames`. Canonical production is unchanged; only mixed host PCM is resampled. `DKC1_SCANOUT_LOG` repeat goal 0 means unqualified/non-integer cadence using target timestamps; goals 1–4 are qualified divisors.
- Flight bundles and post-failure input tails preserve both controllers as six-digit masks. The verifier accepts both historical three-digit and new six-digit masks. Existing bundle schema and hashes remain valid.

## Mac graphics presentation checks (2026-09-06)

- **Escape** opens the native pause panel when windowed (exits fullscreen first). **View → Graphics Settings…** opens its graphics tab. Reconstruct, CRT, and color profiles affect only displayed pixels. Raw plane/WS traces remain the oracle. Settings persist under `GraphicsV1`; use a separate bundle identifier for preference-isolated QA.
- `DKC1_USER_DIR=<absolute existing directory>` overrides the Mac host's normal save/working directory. Use it with a separate QA bundle to protect user slots. It does not isolate NSUserDefaults by itself. Default behavior remains SDL's application support path.
- Optional Mac startup overrides: `DKC1_DISPLAY=flat|crt`, `DKC1_UPSCALER=nearest|bilinear|reconstruct|sharp-bilinear`, `DKC1_SCREEN=raw|crt|composite|trinitron`, `DKC1_CRT_PRESET=living-room|studio|soft`, `DKC1_RECONSTRUCT_MODE=0..4`, and `DKC1_RECONSTRUCT_STRENGTH`, `DKC1_RECONSTRUCT_SOFTNESS`, `DKC1_RECONSTRUCT_SHADING` in 0..100. Invalid numeric settings clamp; omitted variables retain saved/default settings.
- `cmake --build build/macos --target test_macos_graphics`, then `build/macos/test_macos_graphics runner/macos_graphics.metal [existing-output-directory] [input.ppm]` runs offscreen Metal native-pixel, shader, repeated-frame, and cache-invalidation checks. Input is binary P6; omitted input generates an edge/dither pattern. Output contains twelve PPMs. `DKC1_TEST_SCALE=1..16` (default 4) and `DKC1_TEST_PIXEL_ASPECT=1` optionally control output geometry. Exit 77 means no Metal device; exit 1 is failure. These test-only variables do not affect the app.
- Five slots use `quicksave.state`, `slot2.state` … `slot5.state`. Quick Save/Load and their keyboard/Assist actions operate the selected slot. See `docs/GRAPHICS_OPTIONS_PORT.md` for architecture, donor comparison, screenshots, and scope.

## Coral Capers authored seam capability (2026-09-06)

`DKC1_WS_WALL_SEAMS=1` is a separate default-off presentation opt-in. It continues two verified faces of one offscreen rock-wall junction and separately verified western-alcove and upper-shaft walls. The west face uses its authored three-row period; the east face and alcove use source-verified donor strips with an identical retained wall cell. Exact source-layout and full junction/donor-byte checks contain each capability; native pixels and guest memory are untouched. WS trace feature bit 16 and `wall_seam_tiles` identify its use. Preserve this environment setting explicitly when replaying its evidence. See `docs/WIDESCREEN_WALL_SEAM.md` for the exact saves, source proof, 16:10/16:9 A/B, Glide-aware original-viewport checks, and missing Coral Capers fresh-entry gate. This does not grant a general capability to replace populated margin art.

## Windows SDL host validation (v0.0.10)

`build_windows.ps1 -Rom <private-ROM>` builds the Windows SDL/GL host without
replacing the legacy Win32 or Mac hosts. CTest runs `windows_graphics`,
`windows_platform`, and `windows_music`. `DKC1Recomp.exe --graphics-test` needs
no ROM; `--platform-test <existing private directory>` creates a fresh synthetic
settings INI. The music test uses synthetic ZIP/PCM data. None creates game art.

`DKC1_SMOKE_TEST_HIDDEN=1` hides only the Windows SDL smoke window.
`DKC1_SMOKE_TEST_FRAMES=<positive count>` exits gracefully with
`[smoke] complete host_frame=N`. Existing graphics overrides, startup input-only
routes, exact-frame pause, final snapshot, and Assist schedules work on Windows
too. An isolated existing `DKC1_USER_DIR` redirects both Windows preferences and
states. Never use a tester's only slot. Windows pacing headers identify
`platform=windows` and `clock_source=QueryPerformanceCounter`, not Metal scanout.
See `docs/WINDOWS_RELEASE.md` for commands and verification limits.

## v0.0.9 Mac release opt-in

Escape → Settings → **Aquatic widescreen fixes** persists
`GraphicsV1.aquatic_fixes` (default 0). On the next launch it supplies all five
`DKC1_WS_*` presentation flags documented above, without replacing explicit
individual environment overrides. It does not apply while the pause panel is
open. Public bundles have no `LSEnvironment`; clean-user defaults remain off.
`DKC1_BUILD_DIR` optionally selects an isolated `build_macos.sh` output tree
so packaging does not remove a running playtest bundle.
