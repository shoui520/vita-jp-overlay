# The memory ledger

Every byte our three plugins take from a Vita memory pool is accounted for in one place, checked
in CI, and printed at build time and at run time. The pools themselves, their sizes and who else
uses them are in [docs/vita-memory.md](vita-memory.md); why this exists is
[issue #59's root cause](issue-59-root-cause.md).

## The parts

| Part | Where | What it does |
|---|---|---|
| budget table | [`include/vjo_ledger_budget.h`](../include/vjo_ledger_budget.h) | one `#define` per item (its worst case in bytes) and per pool (its limit), with the source of each number; plain integer arithmetic so the C code and the checker read the same values |
| counters | [`core/ledger.c`](../core/ledger.c) | current and peak bytes per item and per pool, atomics only, no libc; one `VjoLedger` per process |
| wrappers | `shell/memory.cpp`, `shell/ledger.c`, `kernel/buffers.c`, `kernel/main.c`, `game_ocr/main.c` | the only places that call an allocator or create a thread; each names its item and counts the bytes in and out |
| CI check | [`tools/check_memory_ledger.py`](../tools/check_memory_ledger.py) | fails the build on an allocation outside the wrappers, a worst case over its limit, or a packaged module with more pages than declared; prints the table |
| run time | `vjo_ledger_shell_log` (Shell), `kledger_log` (kernel) | the pools and items, current and peak, in `status.txt` and `kernel.txt` |

## The table

Printed by `tools/check_memory_ledger.py` (CI runs it in the host job against the sources and in
the Vita job against the packaged modules; `ctest -R memory_ledger` runs it locally):

```
  pool                              permanent   worst case        limit     headroom
  shell pages                         389,120      458,752    1,048,576      589,824
  shell paf                             8,192      225,280      985,088      759,808
  kernel                              135,168    2,224,128    2,224,128            0
  game                                106,496    3,043,328    3,145,728      102,400
  shell paf (network dictionary)     (opt-in)      731,632      985,088      253,456
  packaged modules (loaded pages vs declared budget)
      VitaJPOverlay_Kernel      118,784 B of    118,784 B declared
      VitaJPOverlay_Shell       299,008 B of    299,008 B declared
      VitaJPOverlay_OCR          69,632 B of     69,632 B declared
```

The last row is the opt-in network dictionary with an Anki card and the overlay all at their
peaks. Since Anki cards are text-only (128 KiB arena; `shell/anki.c` `VJO_ANKI_SCREENSHOT 0`)
it is inside the limit; with a 384 KiB picture card it was 8,688 B over, which the checker
reports as a **warning** with the bytes, not a failure, because the runtime reserve guard
refuses the last of the three (the code survives a refusal, host tests). The shipped default
(local JMdict) has 759,808 B of headroom. The kernel row keeps the Lens JPEG capture's
2,088,960 B: Lens still uses it; Anki and OCR take none of it.

*Permanent* is held from boot (or from the game's start) until exit; *worst case* adds the largest
transient set that can be live at once.

### Pool "shell pages": SceShell's partition (`ScePhyMemPartShell`, 50 MiB)

| item | bytes | what | source |
|---|---|---|---|
| Shell module pages | 299,008 | `VitaJPOverlay_Shell.suprx`: 217,088 text + 81,920 data/bss, permanent (405,504 B at `e759187`: the cold files are built with `-Os`, and the overlay's 96 KiB text arena left `.bss` for the PAF heap) | packaged SELF segments [measured] |
| OCR worker pages in SceShell | 0 | `VitaJPOverlay_OCR.suprx` is registered per game (one `*TITLEID` section each, the installer's `--title`), never under `*ALL`, so taiHEN does not load it into SceShell. Were it loaded there (an older install's `*ALL` line, a hand edit), its `module_start` returns `NO_RESIDENT`, which unloads it at once (it held 45,056 B there before PR #68) | `tools/patch_tai_config.py`; `game_ocr/main.c`; [SDK: Kernel-Overview, `module_start`] |
| VjoNet stack+TLS | 69,632 | 65,536 + 4,096, permanent | `shell/worker.c`; TLS per thread [SDK: Kernel-Reference, `sceKernelCreateThread`] |
| VjoControl stack+TLS | 20,480 | 16,384 + 4,096, permanent | same |
| VjoAnki stack+TLS | 69,632 | only while an Anki job runs: with `anki_mining = on`, a count of the queue on every overlay open, a save (×) or a send (△); a refusal is logged with its rc, these bytes and SceShell's free USER (`shell/ledger.c`) | `shell/anki.c` |
| VjoShellMem USER_RW | 0 | the host tests' result memory; the Vita build uses PAF (`VJO_PAF_ALLOC`) | `shell/worker.c` |
| **limit** | **1,048,576** | half of the "genuinely under 2 MiB" a Shell plugin may have at most [expert]; SceShell had 28,672 and 98,304 B free beside a game [measured], so the limit is a ceiling on our footprint, not a promise that it fits: module pages may only shrink, and the checker fails when a packaged module has more pages than declared | AGENTS.md rule 1; [vita-memory.md 5.1](vita-memory.md) |

### Pool "shell paf": SceShell's PAF global heap (5,242,880 B, mapped once)

| item | bytes | what | source |
|---|---|---|---|
| Paf plugin+RCO | 8,192 | the RCO (6,368 B file, measured; 5,632 B before the region selector's cursor) and the Paf `Plugin` object | **[estimate]**; the self-test measures it |
| result arenas | 65,536 (local dictionary) or 571,888 (network: 2 × 262,144 + 47,600 connection pool) | held while a game runs | `shell/worker.c` `LOCAL_RESULT_ARENA_SIZE`, `MEM_SIZE`; asserted equal in `test_shell_worker.c` |
| Anki job arena | 131,072 | per job: a text-only card; the Picture field's screenshot is off in the build (`VJO_ANKI_SCREENSHOT 0`), so neither its 393,216 B arena nor the kernel's 2,088,960 B full-frame capture is ever taken for Anki | `shell/anki.c` `ANKI_TEXT_MEM`; checked against the budget at compile time and in `test_shell_anki.c` |
| overlay text arena | 20,480 | the header and body texts and their spans (17,952 B needed, `test_ledger.c`), only while the overlay page is open; a refusal keeps the page closed | `shell/overlay.cpp` |
| **limit** | **985,088** | the measured free heap, 2,557,952 B (2,498 KiB beside CLANNAD, 2026-10-10), minus the 1.5 MiB reserve the runtime guard keeps for firmware; the reserve is the headroom | `docs/meiki-ocr.md` on `experiment/meiki-ocr` [measured]; `shell/memory.cpp` |

Allocations here take no new pages from the partition; a failure is a `NULL`, never `0x80024302`
(vita-memory.md 3.2). Later sessions saw 355-441 KiB and 190 KiB free [measured], which would
refuse everything above; the code survives a refusal (host tests), so this limit is the best
measured case, not a guarantee. The worst case of the shipped default (one 64 KiB arena, the
overlay open, an Anki card being saved) is 225,280 B; the opt-in network dictionary's is
731,632 B, inside the limit too (with a 384 KiB picture card it was 993,776 B, 8,688 B over,
which the checker reports as a warning).

The word lookup's ranking (`core/local_dict.c`) takes no allocation and no item of its own: its
common-readings table (70 pairs, 924 B of `const` strings) is in the Shell module's text pages, which
stay at 299,008 B because `shell/worker.c` is now built with `-Os` (the build log's module row:
216,904 B of text, 81,232 B of data and `.bss` [measured]). Its state is on the VjoNet thread's
64 KiB stack: the cursor lookup's frame grows from 576 to 1,328 B (two 8-entry match arrays and the
span ends) and its deepest call chain from 1,008 to 2,464 B [measured: `arm-vita-eabi-gcc -O2
-fstack-usage` of `core/local_dict.c`, main and this change]. The lookup's 48 KiB scratch stays
inside the 64 KiB result arena (compile-time check `WorkspaceFits`). The lookup log line uses a
256-byte stack line, as every `vjo_log` call does.

Drawing the region with the stick (#81, `core/region_select.c`) takes no allocation either. Its
code (1,108 B of `region_select.c`, and `shell/overlay.cpp` at 8,822 B where the old file built
for size is 8,237 B) is paid by building `shell/overlay.cpp` at `-Os` (11,608 B at `-O2`); it
runs only while the overlay is open. The Shell's text goes from 217,048 to 215,312 B and its
data and `.bss` from 81,232 to 81,336 B (the selector's state, 104 B), so the module stays at
299,008 B [measured: the build log's module row, main `4a7ae4d` and this change]. The RCO grows
from 5,632 to 6,368 B for the cursor's five planes and one style; the 8,192 B Paf plugin
estimate is kept, which leaves 1,824 B (was 2,560 B) for the Paf `Plugin` object, a size still
unmeasured. On the Paf main thread's stack the region hint's frame is 248 B with its 192-byte
line, under `render`'s 312 B, so the overlay's deepest frame is unchanged [measured:
`arm-vita-eabi-g++ -Os -fstack-usage`].

### Pool "kernel": `ScePhyMemPartKD` (36 MiB, shared by every driver)

| item | bytes | what | source |
|---|---|---|---|
| kernel module pages | 118,784 | `VitaJPOverlay_Kernel.skprx`: 40,960 text + 77,824 data/bss (the 32 KiB model staging buffer and the 4 KiB log ring are in it); one text page more than API 20 for the frame-pacing step (API 21: 37,044 B of text) | packaged SELF segments [measured, API 21 build] |
| VjoWorker stack | 16,384 | permanent | `kernel/main.c` |
| VjoCapture | 2,088,960 at most (960 × 544 × 4) | the Lens/Anki JPEG capture only since API 19: region-sized, `KERNEL_RW`, per capture, freed when the reader has the last row; 458,752 B at 917 × 125 [measured]; #74's console refused 2,088,960 B five times (`0x80024302`) [measured] | `kernel/buffers.c` |
| VjoOcrStrip | 65,536 at most | the OCR ladder's last rung: one strip for one `vjoOcrCaptureRows` call, only when the display hook could not write the game's memory; never held with `VjoCapture` (refused while that is), so it adds nothing to the worst case | `kernel/buffers.c` |
| **limit** | **2,224,128** | today's worst case (API 21: one kernel text page more, the frame-pacing step): a **ratchet**, not a cited safe size. The pool's free space on a retail console is unmeasured; a 2,088,960 B capture failed there once (#14) and has succeeded since. The self-test reads the partition kernel-side and replaces this number | [vita-memory.md 5.3](vita-memory.md) |

### Pool "game": `ScePhyMemPartGame` (256 MiB minus the game's own use)

| item | bytes | what | source |
|---|---|---|---|
| OCR worker pages | 69,632 | the module, in each game it is registered for: 57,344 text + 12,288 data/bss; raised from 45,056 B for the in-game capture (strips, ladder, probe, step log), then from 53,248 B by one text page for API 20's job policy (the stable-text wait and the blank-line check), then from 57,344 B by two text pages and one data page for API 21 (ocr-speed: the faster recognizer kernels, parallel lines, the line cache's 3,152 B and the 1,312 B line queue in .bss, the helpers and the clock), in the game's partition, never SceShell's | packaged SELF segments [measured, API 21 build] |
| VjoGameVocr stack+TLS | 36,864 | 32,768 + 4,096 | `game_ocr/main.c` |
| VjoGameVocrWeights | 1,560,576 at most | F20, the largest accepted model, page-rounded; resident between jobs | `core/vocr_ocr.c`; asserted in `test_ledger.c` |
| VjoGameVocr workspace | 860,160 at most | F20 with two helpers, page-rounded, per job, at any region (245,808 B without helpers): per helper a recognizer arena and context, a line's pixels and its text, 304,496 B for F20 and 246,384 B for VOCR3-H15; helpers only when the job's memory check passes with them. Since API 19 it holds no grey copy of the region (770,048 B with one at 960 × 544) | `vjo_vocr_job_bytes_helpers`; asserted in `test_ledger.c` |
| VjoGameVocrHelper stacks+TLS | 40,960 at most | API 21: 16,384 + 4,096 per helper thread, at most two, each created for one line and deleted when it is placed; none between jobs | `game_ocr/main.c` `pool_run` |
| VjoGameVocrCapture | 65,536 at most | one strip of grey rows and, per region row, its checksum, its recent states (16, or as many as fit) and two bits: at most 65,536 B at any region (`VJO_VOCR_CAPTURE_LIMIT`; 65,480 B at 960 × 544, 61,388 B at 917 × 125), per job; `USER_RW`, else CDRAM, else phycont | `vjo_vocr_capture_bytes` |
| VjoGameVocrLine | 516,096 at most | a line taller than a strip (a finder band), kept whole at its own width × height, only when one occurs (41 of 372 linefind dev regions; 78,884 B at most there); what fits the job limit with F20, the workspace and the capture block | `core/vocr_ocr.h` `VJO_VOCR_LINE_MAX_BYTES` |
| **limits** | weights 1,601,536; workspace 1,048,576; job 3,145,728 | weights: the recognizer's ceiling tier [vita-vn-ocr [`docs/vita-memory-budget.md`](https://github.com/bee-san/vita-vn-ocr/blob/main/docs/vita-memory-budget.md)]; workspace and job raised for API 21's helpers (from 790,528 and 2,392,064): every job's preflight (free `USER` minus the 4 MiB reserve) covers its own need, helpers included, and takes fewer helpers when short; CLANNAD had 85.6 MB above the reserve beside every job [measured, #81] | `include/vjo_ledger_budget.h` |

## The CI check

`tools/check_memory_ledger.py` runs in both CI jobs and as `ctest -R memory_ledger`:

1. **Allocation sites.** It strips comments and string literals from every file under `shell/`,
   `kernel/`, `game_ocr/`, `core/`, `vita/` and `include/`, and looks for `sceKernelAllocMemBlock`,
   `ksceKernelAllocMemBlock`, `sce_paf_memalign`/`malloc`, `sceKernelCreateThread`,
   `ksceKernelCreateThread`, libc `malloc`/`calloc`/`realloc`, C++ `new` and the other allocators
   listed in the script. Each (file, call) pair must be in `ALLOWED`, with at most the listed
   count; those are the wrappers. Anything else fails: "route it through a ledger wrapper". A
   wrapper that disappears fails too.
2. **Worst cases.** It evaluates the budget header and fails when a pool's worst case exceeds its
   limit, or a game item exceeds its tier limit.
3. **Packaged modules.** Given `--kernel`, `--shell` and `--worker`, it sums each module's loaded
   segments rounded to 4 KiB pages and fails when a module has more pages than its declared
   budget. Growing a module is therefore a deliberate act: raise the number in the header, cite
   where the bytes come from in the PR, and expect the question why they cannot live elsewhere.

`tools/tests/test_memory_ledger.py` proves each failure mode on a copy of the tree (an added raw
allocation in C and C++, a call in a comment or string that must not count, a removed wrapper, a
worst case over its limit, an item over its tier, a module one byte into the next page, a file
that is not a SELF).

## At run time

The Shell writes its ledger to `ux0:data/VitaJPOverlay/status.txt` at boot, at each game start and
at each game exit (`tools/install_ftp.sh --status VITA_IP` fetches it):

```
ledger boot pages: current=389120 peak=389120 limit=1048576
ledger boot paf: current=8192 peak=8192 limit=985088 free=2557952
ledger   Shell pages: current=299008 peak=299008
ledger   VjoNet stack: current=69632 peak=69632
ledger   VjoControl stack: current=20480 peak=20480
ledger   Paf plugin (est.): current=8192 peak=8192
```

`free=` is the PAF heap's free size as `GetFreeSize()` reports it (0 before Paf is ready). Stacks
include their 4 KiB of TLS. The kernel writes `ledger kernel: current=… peak=… limit=…` and its
items to `kernel.txt` at start and at every capture. The game worker keeps its ledger in the
game's process, which has no file access; its numbers reach the Shell log through the job result
(`game vocr memory: free USER=… need …`, `workspace_bytes`, `resident_bytes`).

Module pages and the Paf plugin are counted from the declared budget (the CI check ties the
declaration to the binary); stacks, blocks and arenas are counted as they are made and freed.

## What is still an estimate

- The Paf plugin and RCO (8,192 B) and whatever the overlay's widgets take from the PAF heap
  while a page is open: unmeasured. The self-test (plan item 5) records `free=` before and after
  opening the overlay.
- The kernel pool's free space, hence its real limit.
- The PAF limit is one console's best sample; the self-test records the free heap at boot and at
  game launch on every run.
