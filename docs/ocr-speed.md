# OCR speed (API 21): faster recognizer, parallel lines, the line cache, the clock

The owner's v0.8.0-test.1 console logs (#81) are the ground truth: VOCR3-H15 ran **4,941-5,238 µs per
CTC frame** (mean about 5.07 ms; 70-96-frame CLANNAD lines, 0.2-0.46 s each), the finder and the capture
strips took 67-136 ms per 883×112 textbox, a 5-line full screen 1.84 s of recognition. This build makes
the recognition 2-3 times faster in four steps; every one keeps the text exactly what it was.

| step | what | effect (estimate unless marked) |
|---|---|---|
| 1 | vita-vn-ocr's faster W8I kernels (vita-vn-ocr #84, `docs/ocr-speed.md`; the runtime pin) | 5.07 → 2.88 ms per frame for H15 at 333 MHz (1.76x; 1.5x if the A9's prefetch does not help); bit-identical logits |
| 2 | **parallel lines**: up to 3 lines at once (`ocr_threads = 3`, the default), each on its own thread at the lowest priority on the game's cores | a 3-line box takes about its longest line instead of the sum, when the game leaves cores free |
| 3 | **the line cache**: a line whose pixels are unchanged since a job read it gets its text without the recognizer | a box read again (auto mode re-runs jobs on the same text: #81's log has the same 742×39 line recognized three times, 382 ms each) costs a hash, not a recognition |
| 4 | **`ocr_boost_clock = on`** (off by default): 444 MHz while a job recognizes, the clock before restored after | 1.33x on the recognition |

## A 3-line textbox (3 × 80 frames, VOCR3-H15), before → after

| stage | before (test.1) | after, clock off | after, clock on | source |
|---|---:|---:|---:|---|
| capture strips + finder | 67-136 ms | same | same | console (#81) |
| weights | resident 1-6 ms (first job 218-231 ms) | same | same | console (#81) |
| recognition, 1 core | 1.22 s | 0.69 s | 0.52 s | console 5.07 ms/frame; after: vita-vn-ocr's A9 estimate calibrated to it |
| recognition, 2 cores free | - | 0.46 s | 0.35 s | estimate: lines 1-2 at once, then 3 |
| recognition, 3 cores free | - | 0.23 s | 0.17 s | estimate: the 3 lines at once |
| the same box again | 1.22 s | 0 (cache), plus about 1-2 ms of hashing (estimate) | same | host: 0 recognitions on the 2nd pass of 367 regions |

How many cores a game leaves to the overlay is unmeasured; the lines run at the lowest user priority,
so every game thread preempts them. This build logs it (below), per line and per job.

## What the log shows (API 21)

`log.txt`, per job, from the worker's step log:

- `gstep job par kind=- arg=H rc=0xD bytes=B sum=0xFFCCLLMM ... us=R`: H helpers the job could use, D lines
  they recognized, B bytes their workspace and stacks took, R the recognition in µs; `sum` packs the
  helpers that did not start (FF), the lines from the cache (CC) and the recognized lines' **lowest and
  mean share of a core** (LL, MM: the thread's run time against the wall time, percent;
  `sceKernelGetThreadInfo` runClocks). A share well under 100 means game threads had that core.
- `gstep job clock kind=- arg=BEFORE rc=0x... bytes=DURING sum=AFTER`: the ARM clock (MHz) before,
  during and after the recognition, and the set call's result (1: already at 444 MHz, left alone).
  Only with `ocr_boost_clock = on`.
- `gstep job pace kind=- arg=F rc=MAX bytes=SUM sum=SLOW main=SUM0 cdram=MAX0 phycont=SLOW0 us=F0`: added
  by the kernel, the **game's frame pacing**: F frames during the job (from its claim to its
  completion), their longest gap MAX (µs), their sum, SLOW gaps over 25 ms; and the same for the F0
  frames since the job before. A job that costs the game frames shows a larger MAX and more SLOW
  than the window before it.
- `game vocr lines:` is unchanged (crop, frames, ms per line); `game vocr speed:` is the recognition's
  ms per frame (cached lines run no frames and are not in it).

## Parallel lines

`core/vocr_ocr.c` (`VjoVocrPool`): the job's thread fetches each line from the strip in line order (every
row check, the blank-line check, the cache), copies a line it hands to a helper out of the strip (the
next fetch rewrites it), and places every line's text, stats and cache entry in line order. A helper
runs `vocr_run` on its own arena and context over the shared, read-only weights; the last line, and any
line when no helper is free, runs on the job's thread. `vocr_run` is deterministic and shares nothing
else, so the text is the serial job's: on the 367 linefind development regions (856 lines, 31,057
frames; the 5 rows of v1267, reserved as a shared release since vita-vn-ocr #81, are refused) with the
real VOCR3-H15 weights, serial, 1 helper, 2 helpers and the cache gave **0 different texts** (`host/vocr_parallel_bench.c`, `tools/vocr_regions_pack.py`).

In the game (`game_ocr/main.c`) each helper is a thread created for its line and deleted once the line
is placed (`VjoGameVocrHelper`, 16 KiB stack, the worker's priority and cores), so no thread or stack
outlives a job. A thread that cannot be created runs its line on the worker's thread. The job's memory
check counts the helpers' workspace (per helper a recognizer arena and context, a strip-sized line and a
text: 246,384 B for H15) and stacks, and takes one helper fewer at a time down to none when the game's
free memory minus the 4 MiB reserve is short. A cancelled job stops its helpers at their next 4-frame
tile and joins them before it returns; a helper that cannot be joined keeps the workspace allocated
and the worker takes no more jobs.

Host timing (x86, 5 threads of a shared box; only the division of the work, not Vita numbers): 52.7 ms
per region serially, 39.7 ms with one helper, 32.0 ms with two (the regions average 2.3 lines; a
one-line region gains nothing); the same regions again from the cache: 0.79 ms.

## The line cache

`VjoVocrCache`, 8 lines in the worker (3,152 B of .bss): the key is a 64-bit FNV-1a of the line's grey rows, its size, its
polarity and the job's `cache_variant`; a hit gives the text the recognizer gave for those exact pixels
with the same model (and the line's `VjoVocrLineStat.reserved`, which the decoder work in #89 uses for
its flags). `cache_variant` is for whatever else decides a line's shown text (#89's LM tier and
cascade): jobs that differ in it share no entry. Entries of another model never match, and a job that
reads the weights from the file again (another file, the first job) empties it; a job adds its lines
only once it has succeeded.

## The request (API 21)

The helpers and the clock ride in a new word, `VjoGameOcrRequest.speed` (`VJO_GAME_OCR_S_*`: bits 0-1
the helpers, bit 2 the clock), not in `flags`, whose free bits the dialogue PRs use; the kernel and
the worker refuse unknown bits of either word before anything is allocated.

## The ARM clock (`ocr_boost_clock`, off)

`scePowerSetArmClockFrequency(444)` just before the first line is recognized and back to the clock read
before once the last one is, unless the game changed it meanwhile; nothing for a job whose lines all
come from the cache. ScePower keeps the clock each process asked for and applies it while that process
is in front, so the request is the game's own and ends with it (PSVshell reads the same per-process
value, Electry/PSVshell 9a47b0e `src/oc.c:119-147`, and replaces a game's request in its kernel hook,
`src/main.c:106-123`). 444 MHz is the highest the API accepts (500 needs PSVshell's kernel patch,
`main.c:113-117`). Battery and heat at 444 MHz during a recognition are unmeasured.

## Memory (ledger)

| item | before | after | note |
|---|---:|---:|---|
| OCR worker pages | 57,344 | 69,632 | +2 text pages (the runtime's kernels +1.5 KB, the scheduler, cache and pool) and +1 data page (the cache, the job's line queue); the game's partition |
| VjoGameVocr workspace, worst | 249,856 | 860,160 | F20 at 960×544 with two helpers; only when the job's check passes with them |
| VjoGameVocrHelper stacks+TLS | 0 | 40,960 at most | per line, never between jobs |
| game job limit | 2,392,064 | 3,145,728 | worst case 3,043,328 B; CLANNAD had 85.6 MB above the reserve beside every job (#81) |
| kernel pages | 114,688 | 118,784 | one text page: the frame-pacing step (37,044 B of text); .bss unchanged |
| Shell pages | 299,008 | 299,008 | unchanged (text 216,800 of 217,088 B) |

Each block still goes through the ledger wrappers (`tools/check_memory_ledger.py`: pass). CPU3: unmeasured.

## What these speeds allow for the next models (VOCR5)

The largest int8 weights whose 3-line box (3 x 640-px lines = 240 CTC frames, plus 0.1 s for the finder
and the strips) stays under each tier's time: random-init blobs of vita-vn-ocr's `vita/model_tiers.py`
width family (5,631 classes, bottleneck 96, 1.6-6.2 MB) and the released FL10/H15, each profiled with
`vita/a9_layers.py` on 24 development lines of 60-100 frames, on `main`'s runtime and on #85's,
calibrated to test.1's 5.07 ms per frame (H15 on `main`'s code). Each cell: the low end without credit
for #85's weight prefetch (the new instructions, `main`'s cache misses), the high end with it.
Estimates.

| tier (box) | before (`main`), 1 core, 333 MHz | after, 1 core, 333 MHz (clock off) | after, 1 core, 444 MHz (clock on) | after, 2 cores free, 333 MHz | after, 2 cores free, 444 MHz |
|---|---:|---:|---:|---:|---:|
| Small (< 1 s) | 0.88 MB | 1.27-1.46 MB: **the < 1 MB weight cap binds** | 1.60-1.87 MB: the cap binds | 1.73-2.07 MB | 2.06-2.56 MB |
| Normal (< 2 s) | 1.73 MB | **2.14-2.69 MB** (3.4-4.5 M MACs/frame) | 2.40-3.35 MB (3.9-5.8 M) | 2.61-3.73 MB | 3.03-4.80 MB |
| Large (< 3 s) | 2.30 MB | **2.63-3.79 MB** (4.4-6.7 M MACs/frame) | 3.07-4.88 MB (5.3-8.8 M) | 3.57-5.51 MB | 4.38 MB to over 6.2 MB |

The "before" column is the same method on `main`'s runtime: it matches the owner's 18:09Z ceilings
(Small < 1 MB, Normal ≤ 1.6 MB, Large ≤ 2.5 MB) to within the interpolation. FL10's box: 0.82 s →
0.50-0.56 s; a 0.82 MB 4,987-class Small (`s_w100v4987`): 0.93 → 0.55-0.62 s; H15: 1.32 → 0.79-0.91 s
(all 1 core, 333 MHz).

The safe numbers for VOCR5 are the low ends of the 1-core, clock-off column: **Normal ≤ 2.1 MB, Large
≤ 2.6 MB** of int8 weights (Small stays at its < 1 MB cap). They hold whatever the game does with its
cores and with `ocr_boost_clock` off. The high ends need the prefetch to work as modelled; the 444 MHz
and 2-core columns need the clock flag or free cores, all three unmeasured on the console until test.2's
`speed`, `par` and `pace` lines are read. Shapes over 2 MB miss the 2 MiB L2 (shared with the game),
where the calibration on H15 (which fits) is least certain.

## Not measured

- The console has run none of this: the times are the A9 cost model calibrated to test.1's 5.07 ms
  per frame. The next test build's `par`, `pace` and `speed` lines measure it.
- How many cores a game leaves free, and whether two or three NEON threads on the shared L2 slow the
  game's frames: the `pace` step's job window against the window before it answers it per job.
- CPU3, battery, heat.
