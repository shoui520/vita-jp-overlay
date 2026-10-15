# Where an OCR job's time goes: the stage benchmark

The recognizer is not the only cost of a job. Between a settled screen and a word on the
overlay, the game worker waits for a poll, reads the region a strip per display frame (on main,
again until the text has held still), finds and recognizes the lines (on main, with a beam + LM
decoder and a larger model reading doubtful lines again), and the Shell waits for another poll,
writes about 29 log lines and looks the first word up. `tools/bench_stages.py` measures each stage
on the real sources (host time, and Cortex-A9 instruction counts of the Vita compiler's code under
qemu-arm) and combines them with the source's polling periods and with the console's own timings
from #81's `--collect` logs into a model of one job. It is the baseline for the speed fixes that
follow it; each fix reruns it.

A9 figures are estimates from vita-vn-ocr's cost model. The console figures are #81's logs of
v0.8.0-test.1 (API 19's job; [console calibration](#console-calibration)); nothing of main's
API 20 job has run on a console yet, so its total is an estimate built from both.

## The job, end to end (estimate)

The baseline: one Auto OCR job on main's defaults at bcb780e (the stable-text wait of 150 ms, beam
+ LM decoding with the 1000k LM, the VOCR4-W20 cascade; boxes refined in the PSP regions),
VOCR3-H15 and the LM resident, at the linefind dev regions' means (9.4 strips, 2.4 lines, 93 CTC
frames and 35 cascade frames per job; 372 regions of vita-vn-ocr's `eval/linefind/manifests`
`vita_dev`/`psp_dev`, no protected set). [After #100 and #102](#after-100-and-102) below is the
same job on main since:

| part | 444 MHz, 60 fps | 444 MHz, 30 fps | 333 MHz, 60 fps | 333 MHz, 30 fps | estimate (333 MHz, 60 fps) | what sets it |
|---|---:|---:|---:|---:|---:|---|
| take (game worker poll) | 0-100 ms | 0-100 ms | 0-100 ms | 0-100 ms | 50 ms | `game_ocr/main.c` (bcb780e), 100 ms between `vjoOcrTake` calls |
| capture + finder + wait | 212-250 ms | 324-361 ms | 212-262 ms | 324-374 ms | 262 ms | 9.4 strips, a display frame each, and the wait's sleep (simulated: 212 ms at 60 fps); the finder's 25-51 ms (A9, 333 MHz) inside or after them |
| recognition (VOCR3-H15, beam + LM) | 197-375 ms | 197-375 ms | 263-499 ms | 263-499 ms | 505 ms | 93 frames; console 5.06 ms a frame, + the LM's 0.06-0.34 ms (A9, 333 MHz) |
| cascade (VOCR4-W20 re-reads) | 104-190 ms | 104-190 ms | 139-253 ms | 139-253 ms | 256 ms | 35 frames a job read again (43% of the lines), 3.9-6.9 ms a frame (A9, 333 MHz) |
| result (Shell poll) | 0-50 ms | 0-50 ms | 0-50 ms | 0-50 ms | 25 ms | `shell/game_ocr_client.c` (bcb780e), 50 ms between `vjoOcrRead` calls |
| log.txt | 273-1365 ms | 273-1365 ms | 273-1365 ms | 273-1365 ms | 456 ms | 29 lines, each an open/append/close of `log.txt`; console 9.4-47 ms an append (estimate 15.7) |
| text post-processing | < 0.1 ms | < 0.1 ms | < 0.1 ms | < 0.1 ms | 0 ms | A9 estimate |
| first-word lookup | 59-81 ms | 59-81 ms | 59-81 ms | 59-81 ms | 70 ms | console: 15.3 ms + 3.82 ms a read, 14.3 reads |
| **total** | **845-2411 ms** | **957-2523 ms** | **946-2610 ms** | **1057-2722 ms** | **1624 ms** | + what the model does not explain: 194 ms in #81's jobs (below) |
| + while `status.txt` is under 64 KiB | +273-1365 ms | +273-1365 ms | +273-1365 ms | +273-1365 ms | +456 ms | a boot's first jobs (est. 10-20; #81's boot filled it at about the 15th OCR job): every line is an append of `status.txt` too |
| a game's first job | 1249-2839 ms | 1361-2951 ms | 1350-3039 ms | 1462-3150 ms | 2040 ms | + the weights' and the LM's read and SHA-256: console 218-231 ms for the weights, the LM scaled by its bytes |

Ranges: low is the A9 model's lower bound with no polling wait and 9.4 ms an append (the lowest
of #81's jobs' residuals, which carry the polls' variance; the lowest measured append is 12.1 ms),
high its upper bound with both polls at their longest and 47 ms an append. The estimate column takes the
compute at the A9 upper bound at 333 MHz (the bound the console matched, below), the polls at
their mean, 15.7 ms an append (the self-test's median) and the lookup's console fit. The totals
are for a `status.txt` already full (64 KiB, one append a line); before that, in a boot's first
jobs (est. 10-20; #81's boot filled it at about the 15th OCR job), each line is two appends (the
row below them). The game worker runs at the
lowest user priority, so a busy game stretches its share; that is not modelled. The kernel's settle
detection before a job (a signature every 8th game frame, `kernel/vjo_kernel.h`
`VJO_CHECK_EVERY_FRAMES`, and 300 ms without change, `kernel/scene.h` `SCENE_STABLE_US`) is policy
and not in the table. The cascade's own weights (1,558,016 B) are read and checked once, at the
first line a game's jobs read again; that load is not in the first-job row. The model takes the
worker's CPU as the finder and the recognition: the rest of `job_h15_greedy` (row checksums, the
workspace's zeroing, the text's normalisation; 4.1 M of its 146.9 M instructions, 2.8%) is about
6-12 ms at 333 MHz and left out.

The same model for API 19's job (v0.8.0-test.1: greedy decoding, no wait, no cascade, 22 log
lines) gives 625-1868 ms at 333 MHz and 60 fps (estimate 1098 ms) at the dev means. At the 11
console jobs' own means (0.9 lines and 60 frames a job, 5.5 strips, 19.1 log lines and 35.9
appends inside the Shell's job time, 61 ms of weight reads, an 883x112 region) it gives
712-2404 ms, estimate 1198 ms; those jobs took 1393 ms on average (median 1368). The model leaves
194 ms of them, 14%, unexplained (5.4 ms per append): the game worker runs at the lowest user
priority, so the game's threads delay its wakeups and its job, and the game's own file I/O may
slow the Shell's appends; the control thread's "subtitle text ready" line falls inside every job
(about 1.6 appends, 26 ms est.), and the kernel flushes `kernel.txt` about 3 times a job; none of
this is measured. This check is not independent: recognition's rate
and the lookup's fit come from the same logs (the append's cost does not: it is the self-test's).
So main's job is estimated at about 0.5 s more than test.1's (1.6 s against 1.1 s at the dev
means, `status.txt` full): the cascade, the wait and the log's 29 lines instead of 22 (the API 20
steps and the recognized text, #88).

### After #100 and #102

The same job on main with #100 (a job's result lines and its text's written as one append each:
29 lines in 8 appends) and #102 (the take and the read wait in the kernel for the submission and
the completion instead of 100 ms and 50 ms polls); every other part as above:

| part | 444 MHz, 60 fps | 444 MHz, 30 fps | 333 MHz, 60 fps | 333 MHz, 30 fps | estimate (333 MHz, 60 fps) |
|---|---:|---:|---:|---:|---:|
| take (the submission wakes the worker) | 0 ms | 0 ms | 0 ms | 0 ms | 0 ms |
| result (the completion wakes the Shell) | 0 ms | 0 ms | 0 ms | 0 ms | 0 ms |
| log.txt (8 appends) | 75-376 ms | 75-376 ms | 75-376 ms | 75-376 ms | 126 ms |
| **total** | **647-1272 ms** | **759-1384 ms** | **748-1472 ms** | **860-1583 ms** | **1219 ms** |
| + while `status.txt` is under 64 KiB | +75-376 ms | +75-376 ms | +75-376 ms | +75-376 ms | +126 ms |
| a game's first job | 1052-1701 ms | 1163-1812 ms | 1152-1900 ms | 1264-2012 ms | 1635 ms |

So the estimate goes from 1624 to 1219 ms (−405 ms, −25%), and from 2080 to 1345 ms (−735 ms) in
a boot's first jobs, at 333 MHz and 60 fps. The woken threads' scheduling (the game worker runs at
the lowest user priority) is not modelled, and neither is what #81's jobs leave unexplained.

## Console calibration

#81's `--collect` zip (v0.8.0-test.1 on the owner's console; private, so only aggregates are
quoted) has 11 Auto OCR jobs with VOCR3-H15 in one game's 883x112 region outside the self-test's
stages (10 found a line and 1 none; 3 read the weights, the other 8 had them resident), 30
lookups and the self-test's 5 stages:

- **Recognition: 5.06 ms per CTC frame** (664 frames in 10 jobs' `recognize=`; the owner's figure
  is 5.0-5.2 ms). The A9 model gives 2.75-5.00 ms at 333 MHz, the console's clock: the console sits
  at the upper bound (the lower bound assumes dual issue, no cache stall).
- **A file append costs 9-47 ms (median 15.7 measured, 20.9 inferred).** The self-test's
  timestamps measure it: each stage's closing report lines, written after its job (a report, a
  `status.txt` and a `log.txt` append each: an open, a write and a close on ux0, with the game
  running), took 36-138 ms a line over its 5 stages, 12.1-46.0 ms per append, median 15.7.
- **The Shell's job took 1393 ms on average** (`job N done ... in T ms`; median 1368), of which the
  game's own job (its last step's time, the 3 weight reads included) was 489 ms and the first
  word's lookup 80 ms (none in the job without a line). The jobs logged 12-24 lines inside that
  time (19.1 on average), and `status.txt`, aligned line by line with `log.txt`, was still under
  64 KiB for 9 of them and part of a 10th, so a line was two appends: 35.9 appends a job. After the
  two polls at their mean (75 ms), the rest is 20.9 ms per append (median; 9.4-47.0), in line with
  the self-test's. At 15.7 ms an append, logging is 560 ms of those jobs and about 190 ms is
  something else (the console check above).
- **Lookups: 15.3 ms + 3.82 ms per dictionary read** (a fit over the 30 lookups: first word mean
  81 ms with 15.1 reads, cursor mean 78 ms with 17.0). The fixed part is the per-lookup open, size
  seek, header read and close (`core/local_dict.c` `open_dictionaries`, `check_header`;
  `shell/platform_vita.c` `local_open`), the per-read part one 4 KiB `sceIoLseek` + `sceIoRead` at
  a random offset of the 134 MiB file. The CPU is 0.19-0.64 ms of it (A9).
- **Capture: 16 ms a strip** (5.5 strips in 88 ms: the game ran at 60 fps), and `find=` 105 ms
  mean, strip waits included.
- **First job: `load=` 218-231 ms** (the self-test's first stage and 3 of the 11 jobs) to read and
  SHA-256 check the 1,155,968 B weights (SHA-256 alone 66-139 ms, A9, 333 MHz).

These replace the earlier assumptions (0.5-3 ms a log line, 0.2-1 ms a read), which were 5-30
times too low: the file system, not the CPU, is the Shell's cost. Main's job (API 20) has not run
on a console; test.2's logs will check its estimate, line by line (`game vocr cascade speed`,
`gstep job settle`, `gstep job lm`).

## The stages

`host/bench/stage_bench.c` runs each stage with the real code: the display hook's grey rows and
signature (`kernel/capture.c`, `kernel/scene.c`), the game worker's job (`core/vocr_ocr.c`
`vjo_vocr_ocr_policy` with the game worker's policy and vita-vn-ocr's finder, recognizer and LM
decoder at the pinned commit, a540027), the weights' SHA-256 (BearSSL), the Shell's text handling,
word lookup (`core/client.c`, `core/local_dict.c` with the bundled `plaintext.vjdict`), config
parsing, the Anki card record, furigana, and a replica of the Shell's log formatting. Inputs are
committed: the synthetic screen `host/samples/vn_textbox.jpg` (its 896x156 dialogue box, two lines)
and the 50 real VOCR lines of `host/fixtures/word_lookup/vocr_lines.tsv`. The job's clock is
simulated: each strip is one display frame, the worker's sleep passes that much time, recognition
none, so the wait reads exactly the passes it would at that rate (and the cascade's time ceiling,
2 s, never binds).

| workload | per | host (x86-64) | A9 insns | A9 at 444 MHz | A9 at 333 MHz | libc share | parity |
|---|---|---:|---:|---:|---:|---:|:-:|
| `capture_abgr` | strip (mean 52 rows) | 66.3 µs | 627,170 | 0.71-1.90 ms | 0.94-2.54 ms | 10% | yes |
| `capture_abgr_sig` | strip (mean 52 rows) | 68.0 µs | 642,163 | 0.72-1.94 ms | 0.96-2.59 ms | 10% | yes |
| `capture_a2b10` | strip (mean 52 rows) | 111.1 µs | 1,187,418 | 1.34-3.17 ms | 1.78-4.22 ms | 6% | yes |
| `capture_a1b5` | strip (mean 52 rows) | 110.7 µs | 1,209,573 | 1.36-3.02 ms | 1.82-4.03 ms | 3% | yes |
| `capture_full_abgr` | strip (mean 54.4 rows) | 75.0 µs | 703,333 | 0.79-2.35 ms | 1.06-3.09 ms | 11% | yes |
| `scene_sig` | signature | 4.2 µs | 67,766 | 0.08-0.33 ms | 0.10-0.44 ms | 34% | yes |
| `scene_update` | update | 0.5 µs | 9,421 | 11-21 µs | 14-28 µs | 0% | yes |
| `scene_update_typing` | update | 1.1 µs | 14,201 | 16-32 µs | 21-43 µs | 1% | yes |
| `find` | region | 887.1 µs | 16,492,224 | 18.57-37.94 ms | 24.76-50.58 ms | 0% | yes |
| `recognize_h15` | line | 33.05 ms | 63,151,996 | 111.57-202.51 ms | 148.76-270.02 ms | 3% | yes |
| `recognize_fl10` | line | 17.67 ms | 37,241,876 | 63.33-118.80 ms | 84.43-158.40 ms | 3% | yes |
| `recognize_w20` | line | 48.92 ms | 86,255,224 | 158.14-278.81 ms | 210.85-371.75 ms | 2% | yes |
| `job_h15` | job | 68.65 ms | 156,233,396 | 228.23-488.47 ms | 304.30-649.68 ms | 2% | yes |
| `job_h15_first` | job | 76.19 ms | 238,341,790 | 269.60-712.22 ms | 359.46-943.91 ms | 2% | yes |
| `job_h15_lm` | job | 68.43 ms | 154,967,672 | 228.13-484.51 ms | 304.17-644.41 ms | 2% | yes |
| `job_h15_greedy` | job | 67.61 ms | 146,857,280 | 223.44-455.68 ms | 297.92-607.57 ms | 2% | yes |
| `sha256_h15` | model | 3.99 ms | 43,966,006 | 49.76-104.44 ms | 66.34-139.25 ms | 2% | yes |
| `postproc` | text | 0.4 µs | 5,320 | 6-12 µs | 8-16 µs | 17% | yes |
| `lookup_first` | lookup | 46.3 µs | 127,156 | 0.14-0.48 ms | 0.19-0.64 ms | 70% | yes |
| `lookup_every` | lookup | 46.3 µs | 116,185 | 0.13-0.38 ms | 0.18-0.51 ms | 65% | yes |
| `config` | parse | 7.0 µs | 128,214 | 0.14-0.29 ms | 0.19-0.39 ms | 9% | yes |
| `log` | line | 0.3 µs | 2,792 | 3-6 µs | 4-8 µs | 100% | yes |
| `anki` | card | 3.6 µs | 58,578 | 0.07-0.13 ms | 0.09-0.18 ms | 35% | yes |
| `furigana` | word | 0.1 µs | 1,715 | 2-4 µs | 3-5 µs | 51% | yes |

*Strip*: the 896x156 region reads in three strips of 64, 64 and 28 rows (the full frame in ten:
nine of 59 and one of 13), so a full 64-row strip costs about 23% more than these per-strip means
(the work is the same for every row). *job_h15*: the job on main's defaults (`stage_job` mode 14:
the wait, beam + LM, the cascade); `job_h15_lm` the LM alone, `job_h15_greedy` API 19's job (the
previous baseline's `job_h15`: 145,456,441 instructions at 4a7ae4d, now 146,857,280 with the
pinned runtime a540027). On this sample no line is under the cascade's confidence (0.7), so
`job_h15` reads none again; the dev regions do. *Parity*: the Vita compiler's build gave the host
build's output checksum. *libc share*: of the counted instructions, the part in glibc's
`memcpy`/`memset`/`strlen`/`vsnprintf`..., which stand in for the firmware's (`sceClib*`,
`SceSysclibForDriver`); their real cost is unknown.

| workload | memory it touches (per op; the Vita compiler's 32-bit build) |
|---|---|
| capture (each format) | reads 186,368 B of the game's frame (93,184 B at A1B5G5R5), writes 46,592 B of grey into the worker's block per strip (mean 52 rows; a full 64-row strip: 229,376 and 57,344 B); 2 x 3,840 B of kernel scratch (static) |
| `scene_sig` | the sampled rows of the region (every 7th row and 4th pixel of the 896x156 box), copied a row at a time |
| `find` | 31,971 B arena; 382 row reads (two passes) |
| `recognize_h15` / `_fl10` / `_w20` | 182,944 / 124,832 / 241,056 B arena and the resident weights; 54 CTC frames per line here |
| `job_h15` | 538,624 B workspace (the job, the LM's and the cascade's extras; the game's block also holds the 4 KiB greedy text for the log: 544,768 B page-rounded) + 63,000 B capture block; 13 strips (one a settle pass) and 796 row reads for a 156-row region |
| `job_h15_greedy` | 186,704 B workspace + 63,000 B capture block; 10 strips and 604 row reads |
| `lookup_first` / `_every` | 1 open, 14.3 / 14.0 reads, 49.6 / 48.8 KiB per lookup; arena peak 53,552 B of the Shell's 64 KiB |
| `config`, `log`, `anki` | 4,283 B of config.ini into a 2,388 B `VjoConfig`; 111 B per log line; 997 B per card record |

Over the 372 dev regions (`bench_stages.py dev`), API 19's job reads 7.3 strips (median 8, p90
12, max 18), 2.4 lines (median 2, p90 4) and 86 CTC frames (median 78, p90 158); every region row
is read 3.99 times (the finder's two passes, the line fetches, the recheck of a region inside one
strip); 94 regions fit in one strip, 41 lines are taller than a strip. Main's default job reads
9.4 strips at 60 fps (median 10, p90 15; 9.1 at 30 fps), 0.99 of them settle passes, in 212 ms of
simulated display at 60 fps (p90 249) and 324 ms at 30 fps; 93 frames (the PSP regions' refined
boxes are wider); 43% of the LM-decoded lines (229 of 372 jobs) are read again by VOCR4-W20, 35
frames a job (median 22, p90 96); the LM changes 33% of the lines it decodes.

## Hot spots

In the order this repository can act on them, with what each costs a job on the console
(estimates at 333 MHz, 60 fps, main's defaults); recognition, the largest part (505 ms), is
vita-vn-ocr's runtime.

1. **Logging: 0.46 s of a 1.6 s job** (est.; 0.27-1.37 s; #100 brings it to 0.13 s), twice that
   in a boot's first jobs (est. 10-20). Every `vjo_log` line is an open/append/close of
   `log.txt`, and of `status.txt` while that is under 64 KiB (`shell/log_vita.c:46-66, 94-109`),
   so a crash keeps every line, and on the console each append costs 9-47 ms ([console
   calibration](#console-calibration)). Main's job logs 29 lines: 21 of them come from one
   result, back to back (`shell/worker.c` `vocr_run_one`: `game vocr rc=`, `capture`, then
   `log_vocr_measurements`' `memory`, `speed`, `cascade speed` (in 62% of the dev jobs) and
   `lines`, and `log_vocr_steps`' 15 steps), and one or two per recognized line
   (`log_text_lines`). Formatting is 4-8 µs a line (A9). ds4vita (aad83c9 `log.c:8-49`) and
   VitaGrafix (65b6ee3 `src/log.c:10-61`) keep a buffer and write it with one open/write/close.
   The kernel's `kernel.txt` is flushed with its own open/close on every 16 ms tick that has new
   lines (`kernel/main.c:131-160` `kernel_file_flush`, called at `:217`), about 3 per job. Fix:
   one result's lines written with one append, the same bytes in the same order (est. 0.33 s a
   job at 15.7 ms an append, 0.44 s at the jobs' 20.9 ms; twice that while `status.txt` is under
   64 KiB).
2. **Polled hand-offs: 75 ms per job on average, 150 ms at worst** (fixed by #102). The game worker sleeps 100 ms
   between `vjoOcrComplete`/`vjoOcrRegister` + `vjoOcrTake` rounds (`game_ocr/main.c:1080-1103`;
   50 ms while a claim is outstanding), so a submitted job waits 0-100 ms before the game claims
   it, and every candidate game wakes ten times a second for those calls. The Shell asks for the
   result every 50 ms (`shell/game_ocr_client.c:55`). The kernel already waits on an event flag
   for model I/O instead of polling (`kernel/game_ocr.c:664`, `MEV_DONE`), the pattern of ref00d's
   RSA engine thread (blocks on a request bit, signals a done bit: ref00d f3271bc
   `src/ref00d_rsa_engine.c:31-57, 94`); the SDK advises against polling loops
   ([vita-memory.md](vita-memory.md), "Don't busy-wait"). Fix: Submit and Complete signal event
   bits that Take and Read wait on, bounded as the model syscalls' waits are. The worker then
   still competes with the game's threads at the lowest priority: part of the 194 ms that #81's
   jobs leave unexplained may be that, and no poll removes it.
3. **The cascade: 256 ms a job** (est.; 139-253 ms A9 at 333 MHz) on these regions, where it reads
   43% of the lines again with VOCR4-W20 (1.37 times H15's cost a frame). That is the dialogue
   accuracy work's trade (vita-vn-ocr #82, `ocr_cascade_ms`), bounded by its 2 s ceiling; test.2's
   `cascade speed` lines will say what share of real dialogue it reads again. The bench's clock
   gives recognition no time, so that ceiling never binds here; at the console's rates it would in
   about 13 of the 372 dev regions, which hold 15.5% of the cascade frames (audit-overlay-perf-1b's
   estimate): 256 ms is an upper estimate.
4. **Recognition: 505 ms a job** (est.), vita-vn-ocr's runtime at 5.06 ms a frame on the console
   (112-203 ms per 54-frame line for VOCR3-H15 at 444 MHz, 63-119 ms for VOCR3-FL10), pinned here;
   its own speed work happens there. The LM adds 0.06-0.34 ms a frame (A9, 333 MHz; up to 7%).
5. **Strip waits and the stable-text wait: 212 ms a job at 60 fps, 324 ms at 30 fps** (simulated):
   9.4 display frames, 1 of them the wait's pass; under API 19, 1.8 of the 7.3 re-read rows for
   the finder's second pass while the recognizer's part of the workspace is idle. That is
   `core/vocr_ocr.c`'s strip stream and policy, the dialogue work's (a pass-2 cache was declined
   there).
6. **Lookups: 70-81 ms each, nearly all file I/O.** Console: 15.3 ms a lookup (an open, a size
   seek, the 64-byte header and a close, `core/local_dict.c` `open_dictionaries`/`check_header`,
   `shell/platform_vita.c` `local_open`) and 3.82 ms a read, 14.3 reads; the CPU is 0.19-0.64 ms
   (70% in libc: `strlen` over the preferred-readings table for every candidate,
   `core/local_dict.c:492`, and `memset` zeroing the 45 KB workspace, `:588`/`:718`). A page cache
   kept across the lookups of one text would save little: audit-overlay-perf-1 measured it
   directly, keeping `local_dict.c`'s own `cached_read` pages across one text's lookups (a scratch
   patch, not in this tree) on the every-letter workload: 0.0% fewer reads with the current 4
   pages, 1.5% with 8, 2.8% with 32 (128 KiB of SceShell memory), output unchanged. So no cache.
   Keeping the file open between lookups would save most of the fixed 15 ms, and `sceIoPread` one
   of the two syscalls per read (nonpdrm 8372225 `main.c:228` reads with `ksceIoPread`; VitaTweaks
   4233c54 `DownloadEnabler/main.c:50` with `sceIoPread`; the kernel's own model broker already
   does, `kernel/game_ocr.c:898`).
7. **The display hook's grey conversion: 12 instructions a pixel** (`core/fbgrey.h:62`,
   `kernel/capture.c:181`), 0.7-1.9 ms per strip (mean 52 rows) at 444 MHz (0.9-2.5 ms at 333)
   inside the game's `sceDisplaySetFrameBuf`, and twice that for A2B10G10R10 and A1B5G5R5 (an
   expansion pass to A8B8G8R8, then the grey pass, `core/fbgrey.h:38`). It adds no OCR latency
   (each strip waits for a frame anyway) but it is frame time the game loses during a capture,
   about 9 frames a job. Fix (#83): two multiplies a pixel, and A1B5G5R5's expansion folded into
   the grey pass. Integer only: the kernel has no FPU (`tools/check_kernel_instructions.py`).
8. **SHA-256 on a game's first job: 50-104 ms** at 444 MHz for the weights (BearSSL's compact
   `br_sha2small_round`, 35 instructions a byte), as much again for the LM on main, and once for
   the cascade's weights. Once per game session; resident afterwards. Not changed: third-party
   crypto, and a one-time cost.
9. **Wakeups.** SceShell's control thread wakes every 50 ms (`shell/worker.c:45`), with or
   without a game; the kernel worker every 16 ms (`kernel/main.c:21`); the game worker every
   100 ms in every candidate game. CPU3's share is unmeasured (AGENTS.md rule 8).

Not hot (444 MHz; 333 MHz in brackets): config parsing 0.14-0.29 ms per overlay open
(0.19-0.39), text post-processing 6-12 µs (8-16), the scene tracker 11-32 µs per 64 ms (14-43),
card records 0.07-0.13 ms (0.09-0.18), furigana 2-4 µs a word (3-5).

## Not covered

No stage measures these; the job model leaves them out or takes them from the console:

- the overlay's Paf layout and font rendering (ScePaf; they cannot run off the console);
- input polling: `kernel/main.c:212` runs `input_poll()` on every 16 ms tick, and
  `kernel/input.c` filters inside the controller hooks;
- startup: module start, the boot-time config load and the Shell's thread creation (only the
  per-open config parse is a stage);
- the game worker's request wrapper (`game_ocr/main.c` `vjo_game_ocr_run_request`: the free
  memory query, the check, two allocations and frees, the step records); the job stage calls
  `core/vocr_ocr.c` on static buffers;
- the kernel's side of a job: the `kernel/game_ocr.c` Submit, Take, rows, Complete and Read
  syscalls and `kernel/display.c`'s hook dispatch, and the firmware's user/kernel copies;
- the file system: the log's appends, the dictionary's reads, the model broker's reads and the
  Anki queue's write (`shell/anki_queue.c` `commit`: write, `sceIoSyncByFd`, rename, mount sync)
  are not timed off the console; the log and the dictionary are calibrated from #81's logs, the
  Anki write is not (its stage covers the note and the JSON record only).

## Plugin size budget

From the Vita build of `main` (bcb780e; [memory-ledger.md](memory-ledger.md)): the Shell module
holds 299,008 B, with 1,832 B left in its last code page (215,256 of 217,088) and 3,936 B in its
data pages; a Shell change that adds a page breaks AGENTS.md rule 1. The kernel module has
1,248 B (code) and 112 B (data) left, the game worker 452 B and 2,356 B.

## Reproduce

```sh
. .refs/env.sh   # VITASDK, ARM_CC, QEMU_PLUGINS_DIR (vita-vn-ocr's plugin qemu), VJO_VOCR_SOURCE_DIR
cmake --build build/host --target vjo-stage-bench
python3 tools/bench_stages.py smoke --binary build/host/vjo-stage-bench --models $VJO_VOCR_MODEL_DIR
python3 tools/bench_stages.py host --binary build/host/vjo-stage-bench --models $VJO_VOCR_MODEL_DIR \
    --dict build/host/plaintext -o host.json
python3 tools/bench_stages.py a9 --vita-build build/vita --models $VJO_VOCR_MODEL_DIR \
    --dict build/host/plaintext -o a9.json
python3 tools/bench_stages.py dev --binary build/host/vjo-stage-bench --models $VJO_VOCR_MODEL_DIR \
    --manifests $VJO_VOCR_SOURCE_DIR/eval/linefind/manifests --shots DIR [--shots DIR] -o dev.json
python3 tools/bench_stages.py report --host host.json --a9 a9.json --dev dev.json \
    -o docs/benchmarks/pipeline-stages-YYYYMMDD.json --md table.md
```

`--models` is the directory with the pinned weights (VOCR3-H15, VOCR3-FL10, VOCR4-W20) and the
1000k LM. `smoke` (ctest `stage_bench_smoke`) runs every workload once and compares each output
checksum with `tools/bench_stages_expected.json`: the checksums hash each stage's whole output (the
grey bytes and the signature, the lines, the text and the greedy text, the entries and their
spans, every parsed setting, the Anki record's bytes), so a speed change that alters a stage's
output fails it. The log stage is a replica of the Shell's formatting (its format strings, a
typical job's values), so for logging it checks the formatted bytes, not the Shell's code; the
Shell's log lines are checked by `host/tests/test_shell_worker.c` through its own `vjo_log` stub;
`shell/log_vita.c` itself has no host test here (the logging PR adds one). The `a9` lane links the Vita build's own libraries
(`libvjo_core_vita.a`, `libvjo_vocr*.a`, `libvjo_bearssl.a`) and the kernel's
`capture.c.obj`/`scene.c.obj`, so the counted instructions are the plugins' (Thumb-2, the modules'
own flags), runs each workload with 1 and 3 iterations and keeps the difference (one steady-state
iteration), and costs it with vita-vn-ocr's `vita/a9_cycles.py` at the pinned commit (NEON/VFP
issue, dual issue, load/store pipe, the 32 KiB L1 and 2 MiB L2 simulated by qemu's cache plugin).
The bench's own code is not counted; glibc's string and formatting functions are, as stand-ins,
reported as *libc share*. The baseline's raw data (main bcb780e, the console figures as
aggregates): [benchmarks/pipeline-stages-20261015.json](benchmarks/pipeline-stages-20261015.json).
