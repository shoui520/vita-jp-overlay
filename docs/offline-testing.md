# Testing without the console: what runs where, and what cannot

The owner's words: "we have no way to properly test this without a live device". This page
says exactly what is testable off the console, what CI runs, what Vita3K can and cannot tell us,
and what only the console can answer. The two things the console kept failing on, boot and
memory, are the two things no emulator models: a plugin's pages inside SceShell's 50 MiB
partition, and taiHEN loading our modules into firmware processes
([vita-memory.md](vita-memory.md), AGENTS.md rule 5).

## 1. The matrix

| Our code | host x86_64 (CI `host` job) | ARMv7 under qemu-arm (`tools/arm_host_tests.sh`) | Vita3K | the console |
|---|---|---|---|---|
| kernel: scene detection, triggers, foreground state machine, capture buffers, the game-OCR broker, the boot guard, the ledger | real code, SDK calls stubbed (`host/tests/vita_stubs`) | real code, same stubs, 32-bit, LDREX/STREX atomics | not loadable: the kernel plugin needs `SceDisplay`/`SceCtrl`/`SceTouch`/`SceProcessmgr` ForDriver exports to hook and `SceSysmem`/`SceIofilemgr` ForDriver calls; Vita3K has no firmware kernel to export them (its taiHEN HLE implements "all functions taiHEN itself relies on", nothing of a kernel plugin's imports) | the only place its hooks, its partition and its pool exist |
| kernel: the hooks themselves (display, pad, touch, process events) | no | no | no | only here |
| Shell plugin: control policy, memory lifetime, Anki miner and queue, strip, cursor, dictionary paging | real `shell/worker.c`, `anki.c`, `anki_queue.c`, `capture.c`, `strip_view.c`, `files.c` with stubs (16 suites) | not built (needs BearSSL and the core library; vita-vn-ocr's ARM lane covers the recognizer) | not loadable in its real context: Vita3K has no SceShell ("Shell support is pretty spotty" is about its own replacement UI); ScePaf, SceShell's partition and the LiveArea daily job do not exist there | only here |
| Shell plugin: the Paf UI (`overlay.cpp`, `strip.cpp`) | header-level tests (`test_header_highlight.cpp`, `test_word_navigation.cpp`); the arrows' row walk (`core/text_rows.c`) against a layout double (`test_text_rows.c`) | `test_text_rows.c` only | no | only here |
| game worker: memory check, the capture ladder and probe, resident weights, model I/O through the broker, cancellation, the safe-boot exit | real `game_ocr/main.c` with stubs, real FL10/H15 when the models are present; with the real kernel (rows syscall, display-hook copy) in `test_game_model_broker.c` and under #74's measured pools in `test_alloc_sim.c` | the capture parity test (API 18's capture against API 19's, `test_capture_parity.c`) | as a plugin it could load into a game through Vita3K's taiHEN HLE, but its `vjoOcr*` syscalls come from our kernel plugin, which cannot load. `tools/vita3k_smoke.sh` runs its job code as a homebrew app instead, with an in-process kernel stand-in: a crash check, nothing more | only here |
| packaged modules: syscall ABI, firmware-independent kernel imports, ARM-only instructions, page budgets | `check_vita_syscall_abi.py`, `check_kernel_instructions.py`, `check_memory_ledger.py` in the `vita` job | | | |
| SceShell's memory under our footprint | the replay model (section 4), from console samples | | not modelled: no budgets, no SceShell | the self-test ([self-test.md](self-test.md)) |

## 2. What CI runs (and why it is real code)

The host suites compile the plugin sources themselves (`#include "../../shell/worker.c"`,
`kernel/boot.c`, `game_ocr/main.c`), not copies, and replace only the SDK boundary: memory
blocks, threads, mutexes, files, the clock, and the kernel syscalls. Allocation failures,
failed joins, cancelled jobs and crashes between steps are injected there. 34 ctest suites
plus the Python tool suites run in the `host` job (with the frozen dictionary checked out, the
word lookup on real recognized lines and the step that unpacks the dictionary for it); `test_jpegsw.py` and the Hachidori relay
contract test complete it.

## 3. ARM under qemu-arm: the same C on the Vita's CPU

`tools/arm_host_tests.sh` builds the pure-C kernel and core suites with a Linux ARM toolchain
(`-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -static`, the flags vita-vn-ocr's runtime lane
uses) and runs them with `qemu-arm -cpu cortex-a9`:

| suite | cases | what the ARM build adds over x86_64 |
|---|---|---|
| boot_guard | 11 | 32-bit `uint32_t`/`int` arithmetic of the record, byte-identical record text |
| ledger | 2 | `__sync` atomics compile to LDREX/STREX; 32-bit counters |
| fbgrey | 4 | `core/fbgrey.h`'s word-at-a-time grey (two multiplies a pixel, the A1B5G5R5 expansion folded into the weights) against the per-channel code it replaced, for every R, G, B, alpha pattern and A1B5G5R5 value: its carries and wraps in 32-bit arithmetic |
| kernel_capture | 11 | 32-bit stride and capacity arithmetic; the OCR grey rows in every pixel format; the first pass's region signature in every format, strip height and 400 random regions (the A1B5G5R5 one-pass rows never skip a sampled row); every box the region selector can save is cropped as drawn |
| kernel_input | 14 | the trigger state machine |
| kernel_game_ocr | 35 | the broker's 32-bit offsets and `SceOff` (64-bit on a 32-bit CPU); the rows syscall and its state machine |
| kernel_meminfo | 6 | the three kernel memory routes with the firmware's objects at 32-bit kernel-range, upper-kernel and user-range addresses (the user-half ones unreadable, at 0x5... and 0x8...), and the guard at each of its edges |
| region_select | 14 | the region selector's stick cursor (`core/region_select.c`): its 1/256-pixel position, speed and timer arithmetic in 32 bits |
| text_rows | 11 | the overlay arrows' row walk (`core/text_rows.c`, #81): float cell edges and 32-bit offsets on the Vita's FPU |
| settings_layout | 12 | the Overlay app's text layouts, icon PNG check and title letters (`settings/text_layout.c`, `settings/icons.c`, `settings/letter.c`): 32-bit `size_t` in the chunk walk, chunk lengths that would wrap it, each check against an unmapped page |
| shell_log | 11 | the Shell's logger (`shell/log_vita.c`) on an in-memory file system: a batch of lines gives the files the same bytes as line by line through `status.txt`'s 64 KiB cap and `log.txt`'s 1 MiB rotation, with 32-bit sizes |
| capture_parity | 2 | with `VJO_VOCR_SOURCE_DIR`: API 18's and API 19's capture give the recognizer identical bytes on the Vita's CPU (procedural frames; `tools/capture_parity.py` adds the eval crops) |
| word_lookup | 1,321 lookups | with `VJO_PLAINTEXT_DIR`: `core/local_dict.c` with the bundled dictionary at every Japanese letter of the real VOCR lines in `host/fixtures/word_lookup` gives output identical to the host build's (entries, spans, definitions; `host/tests/word_lookup_parity.c`) |

Measured on the fleet box (Arm GNU Toolchain 14.2.Rel1, qemu-arm 10.0.13): all 133 cases pass,
and the eval-crop parity (372 regions × 3 formats) has no difference there either.
In CI it is the `arm` step of the host job (`gcc-arm-linux-gnueabihf`, `qemu-user-static`). It
is not a Vita: no firmware, no taiHEN, no SceShell, no budgets; a pass here says the C is
32-bit clean, nothing more.

## 4. The Shell budget replay model

`tools/shell_budget_model.py` replays the boot and game-launch allocation sequence of the
current build against the console's measured SceShell memory:

- every item's size comes from `include/vjo_ledger_budget.h`, which CI ties to the packaged
  modules ([memory-ledger.md](memory-ledger.md));
- every sample comes from `docs/benchmarks/shell-budget-samples.json`, with its date, source,
  and the overlay build that was loaded when it was taken, measured from that release's SELF
  segments. A sample taken with build B loaded means SceShell had `free + pages(B)` without
  us; the headroom the current build leaves at each step is that minus the pages it holds.

Current build (`--anki` adds a card save), against the 11 October session (v0.7.4, CLANNAD in
front, 98,304 B free, the build holding 462,848 B):

```
step                                        held      headroom
Shell module loaded into SceShell        299,008       262,144
OCR worker pages in SceShell             299,008       262,144   (0: registered per game, not loaded there)
VjoNet thread                            368,640       192,512
VjoControl thread (boot complete)        389,120       172,032
game start: result memory (local)        389,120       172,032
Anki card saved: job thread              458,752       102,400
```

What this says, and what it does not:

- Before the fix (main at `ae94879`) the build held 532,480 B at the end of boot and would have
  left 28,672 B in that session: seven pages, less than the 65,544 B change that broke the boot
  in #42; an Anki save would have found no pages for its thread. The fix (worker `NO_RESIDENT`,
  the overlay's text arena out of `.bss`) holds 389,120 B, 73,728 B below the v0.7.4 build that
  ran beside a game, and leaves 172,032 B at the end of boot in the same session.
- `e759187`, the build that failed in #59, would show 20,480 B of headroom in the same
  scenario, positive. The model cannot prove that a build fits: SceShell's own use differs
  between sessions (a second sample, ownership unverified, had 28,672 B free), the daily
  LiveArea job's request size is unknown, and a private testing agent was resident in SceShell
  in both sampled sessions. It ranks builds and changes, and it says which step would fail
  first. The console decides.
- The PAF heap scenarios (2,538,568 B and 2,557,952 B free after a reboot; 355,256 B with a
  leaked 2 MiB pool) show every allocation of ours refused at the low-water mark and all of
  them fitting after a reboot, which is what the console showed.

`tools/tests/test_shell_budget_model.py` checks the arithmetic on a synthetic budget, that the
real tree's boot completes in the v0.7.4 scenario (the regression this guards), and that the
pre-fix footprint (`--shell-pages 397312 --worker-pages 45056`) ranks worse than the tree's.

## 5. Vita3K, precisely

Vita3K gained a taiHEN HLE in its PR #3885 (wiki page "Using taiHEN plugins", revised May
2026): `config.txt` is read from `ux0:tai`/`ur0:tai`, `*KERNEL`, `*TITLEID`, `*ALL` and `*main`
are parsed, and "all functions taiHEN itself relies on are implemented. Plugins that use
functions Vita3K does not implement likely will not work." The page's own note on `*main`:
"Shell support is pretty spotty". For us:

- the kernel plugin imports `SceDisplayForDriver`, `SceCtrlForDriver`, `SceTouchForDriver`,
  `SceProcessmgrForDriver` (hook targets), `SceSysmemForDriver`, `SceIofilemgrForDriver`,
  `SceThreadmgrForDriver` and `taihenModuleUtils`; none is a function "taiHEN itself relies
  on", and there is no firmware kernel behind them: it does not load;
- the Shell plugin needs SceShell and ScePaf, which do not exist in Vita3K; its memory
  question (pages in a 50 MiB partition shared with firmware) is the one Vita3K does not
  model at all;
- the game worker could be injected into a game, but without the kernel plugin its weak
  `vjoOcr*` imports stay unresolved and `module_start` starts nothing (by design: that is also
  what happens on a console whose kernel plugin failed).

So Vita3K can run none of the three plugins as they are, and it would say nothing about boot
or memory if it could: no `ScePhyMemPartShell`, no `ScePhyMemPartKD`, no `0x80024302`
[expert; AGENTS.md rule 5]. It is not wired into CI. `tools/vita3k_smoke.sh` (#74 preflight)
builds `tools/vita3k_smoke/` as a homebrew app: the real game worker's five self-test jobs and
the two other pixel formats in a Vita user process, on a 32 KiB thread stack, with the release
weights and an in-process stand-in for the kernel's syscalls. It passes when every job returns
and the app and the emulator exit 0: it catches crashes in that code, and is not evidence of
anything on a console. vita-vn-ocr uses qemu-arm, not Vita3K,
for the recognizer's ARM parity and cycle estimates, for the same reason.

The Overlay app is an ordinary homebrew app, so Vita3K can run it as it ships.
`tools/vita3k_settings_smoke.sh` builds the app's sources unchanged with a scripted controller
and counted textures (`tools/vita3k_settings_smoke.c`, linked by `-Wl,--wrap`; the
`VJO_SETTINGS_SMOKE` target of `vita/CMakeLists.txt`, never in the release), installs 120
synthetic games (`tools/make_settings_smoke_games.py`: generated titles and icons, some
missing, damaged, oversized or not PNG) and runs the script: the Games list scrolled down and
back, paged with L/R pressed and held, letters with Left/Right, toggles, the filters, a Start
rescan, the other pages, Circle. It fails on a crash, a hang, an icon texture freed after a
draw with no GPU wait in between, or one left alive. Given `RELEASE_VPK`, the release
`JPOverlaySettings.vpk` must also keep running for 30 s over the same games. The app draws
text with the firmware's PVF library, which this Vita3K does not emulate (`scePvfNewLib` is
unimplemented): it needs Sony's 3.74 system software and font package
(`VITA3K_FIRMWARE`, `VITA3K_FONTS`), which Vita3K then loads as the real modules. A pass
says the app's own code survives that script under Vita3K's GPU emulation; its timings are
the host's dynarec and are not used. (Vita3K does not fault on a texture freed while the GPU
reads it, so that is counted, not observed: main `7ed16d8`'s app freed 365 of its 366 icon
textures so, with 66 failed reloads of broken icons, and exited 0.) If `vita-elf-create` refuses
the smoke app's ELF ("segment 1 overlaps": its code segment ends too close to the data
segment), the script rebuilds it with 4 KiB of padding (`VJO_SETTINGS_SMOKE_PAD`).

## 6. What only the console answers

1. Whether the boot completes with our pages present (the self-test's `boot`/`ok` steps and
   the boot guard's record).
2. SceShell's free partition at boot and at game launch with the current build, and the
   kernel pool's free size (the self-test's `mem`/`kernel` lines). These replace the model's
   samples and the ledger's ratchet.
3. The game's free `USER` per title (the self-test's `game` line; the fleet's model-limit
   work asks for every title tried).
4. The hooks: input filtering, capture, process events; and the PAF UI.
5. CPU3 load, by PSVshell readings with matched clocks, plugin set and workload (unmeasured
   until then; AGENTS.md rule 8).
