# The on-device self-test

Every console test so far ended with the same two questions: did it boot, and what ran out of
memory. The self-test answers both from one file, in the owner's two commands, and tells the
fleet exactly which step failed and with how much free memory in which pool.

Since API 19 (#74) one run also exercises the whole in-game capture: the capture probe, the
fallback model, the configured model, the full screen and a warm second recognition, each stage
with its own record whether it passed or not ([ingame-capture.md](ingame-capture.md)).

## Running it

1. Arm it (VitaShell FTP running): `bash tools/install_ftp.sh --self-test VITA_IP`. This uploads
   one empty file, `ux0:data/VitaJPOverlay/selftest.txt`.
2. Reboot. Wait for the LiveArea and leave the Vita alone for a minute (the boot guard's `ok`).
3. Start the game to test. Reach a screen with dialogue text and wait about 30 seconds without
   pressing anything: on the first settled screen with text in your region, one OCR job runs every
   stage below in turn. Screens before it with no line in your region, or that change under every
   read, are waited past: the game's logos and notices (#81's run began 2 s after the game
   started, on CLANNAD's opening notice, whose five lines lay above the region, and all three
   region stages found nothing), menus, black screens. Each closes the first stage's `begin` with
   a `stage fallback wait` record; after 30 such screens the stages run anyway, and the analyser
   says when they then found no text. OCR runs automatically while the test is armed, whatever
   `ocr_mode` says; the region is
   **yours** (draw it with Square first if you have not; with none, the full screen). The game
   needs the OCR worker registered for it (installed with `--title TITLEID`, README 'Install');
   otherwise the stages report `rc=-117` (`ocr unavailable`).
4. Back in VitaShell: `bash tools/install_ftp.sh --collect VITA_IP` fetches the report with every
   log and record into one zip ([below](#collecting-everything---collect)); `--status VITA_IP` fetches
   just `ux0:data/VitaJPOverlay/selftest-report.txt` (and `status.txt`, `kernel.txt`, `boot.txt`).
5. `python3 tools/analyse_selftest.py build/device/status/selftest-report.txt` prints the run
   and its OCR, one PASS/FAIL row per stage, then the diagnostics (PASS/WARN), the free memory
   every route read at each step, the game worker's memory around each allocation, the findings
   and a verdict. Only the OCR rows decide the verdict; a diagnostic WARN never does. Attach the
   report to the issue.

| stage | job | what it shows |
|---|---|---|
| `fallback` | your region, the fallback model forced | the fallback (VOCR3-FL10 by default) read from the card and recognizing |
| `textbox` | your region, the configured model | the configured model (VOCR3-H15 by default) read from the card (its load time), the strip capture, the finder and the recognizer |
| `fullscreen` | the whole frame | the full screen through the strips, the configured model kept resident |
| `probe` | each capture path into each memory type, one strip | whether the display hook can write the game's `USER_RW`, `CDRAM` and `PHYCONT` memory, the kernel strip, and a user-mode `sceDisplayGetFrameBuf` read; each with its rc and the rows' checksum |
| `textbox2` | your region again | the warm time; its text is the job's result |

The marker is removed after the last stage, so one arming is one run, failed stages included
(#74: the API 18 test removed it only after a successful recognition, stayed armed after the
capture failed, and kept forcing the full-screen region). If the screen changes during the
stages, the job is cancelled and they run again on the next settled screen. A run that stopped
part way (a crash, the power) leaves the marker: the next boot runs the test again, the report
shows the stage that began and never wrote its record, and `status.txt` keeps the previous boot's
copy as `status.prev.txt`.

If the game is disabled in the **Overlay** app (**JP Overlay Settings** in older builds), the
report says so and no OCR runs. If the boot is a safe one (the previous boot did not complete),
the report has the boot sample only and says so; reboot once more.

## What it writes

`shell/selftest.c` writes one line per event; every step has a memory sample by every route:

```
selftest v2 api=19
boot seq=2 mode=0 prev=0/0/0/0 failures=0 write=0x00000000
config backend=2 model=H15_w8.vocr fallback=FL10_w8.vocr dictionary=3 anki=0
step boot t=0
mem boot rc=0x00000000 main=7340032 cdram=4194304 phycont=0 valid=1 paf_free=3733640
ledger boot pages=299008/299008 paf=8192/8192
kmem boot route=block part=kernel rc=0x00000000 step=0 total=37748736 free=9437184
kmem boot route=block part=shell rc=0x00000000 step=0 total=52428800 free=98304
kmem boot route=cas part=shell rc=0x00000000 step=0 main=98304/52428800 cdram=0/4194304 phycont=0/0
kmem boot route=as part=kernel rc=0x00000000 step=3 parts=2 skipped=1 p0=9437184/37748736 p1=4194304/16777216
kernel boot ledger=131072/131072 limit=2220032 capture=0
step ok t=60999
...
step game t=142999 title=PCSG00415 enabled=1 mode=1 region=user rect=1536,48188,62600,15059
...
stage textbox begin t=145499 flags=0x0 model=H15_w8.vocr fallback=FL10_w8.vocr
gstep textbox mem kind=- arg=0 rc=0x00000000 bytes=0 sum=0x00000000 main=91226112 cdram=117440512 phycont=0 us=10
gstep textbox alloc kind=user arg=capture rc=0x00000041 bytes=61440 sum=0x00000000 main=91164672 cdram=117440512 phycont=0 us=30
gstep textbox pass kind=hook arg=3 rc=0x00000000 bytes=114625 sum=0x00000000 main=89817088 cdram=117440512 phycont=0 us=34000
...
line textbox i=0 w=877 h=30 frames=110 us=273333
stage textbox end rc=0 t=147999 ms=2500 region=917x125+22+400 fb=960x544/960/0x00000000 model=H15_w8.vocr fallback=0 lines=3 recognized=3 frames=330
result textbox source=hook mem=user capture=61440 strip_rows=62 strips=9 tears=0 capture_ms=34 workspace=188416 resident=1159168 weights=read find_ms=9 load_ms=1050 recognize_ms=820
memory textbox free_user=91226112 reserve=4194304 need_workspace=249856 need_weights=1159168 release=0 io=0x00000000 stat=0x00000000 cleanup=0 sum=0x5EED0003
...
step done t=155499
```

(The figures above are the host writer's synthetic ones, `tools/tests/fixtures/selftest-v2-pass.txt`.)

| line | source | meaning |
|---|---|---|
| `mem` | `sceKernelGetFreeMemorySize` in SceShell | SceShell's budget as the user API reports it: each figure is the free size minus a system margin [SDK: Kernel-Reference], so a negative one (#74: `-2097152` after boot) is under the margin, 0 B free above it, not an error (`valid=0`); `paf_free` is the PAF heap's `GetFreeSize()` |
| `ledger` | the Shell ledger ([memory-ledger.md](memory-ledger.md)) | current/peak bytes we hold in SceShell's partition and in its PAF heap |
| `kmem` | `vjoMemInfo` (`kernel/meminfo.c`), one line per route | `route=block`: FindMemBlockByAddr (size 0) → MemBlockGetInfoEx → PhyMemPartInfoCore for the kernel's own block (`part=kernel`, ScePhyMemPartKD when the total is 36 MiB) and the caller's (`part=shell`); `step` names where a route stopped (0 read; 1 find; 2 block or address-space info; 3 a pointer outside the kernel range, below 0x40000000; 4 in no kernel memory block; 5 InfoCore). `route=cas`: the caller's address-space block at +328/+332/+316 (main/CDRAM/phycont), as PSVshell and PSVshellPlus read it. `route=as`: the kernel process's address-space partitions (SceSysmemForKernel_C3EF4055), as faps-coredump reads them, or the caller's (`part=shell`) when the kernel's is refused; `skipped`: partitions not read, `step` the reason. Every route reports its own rc |
| `kernel` | `vjoMemInfo` | the kernel ledger and the capture block held (Lens/Anki only; OCR holds none) |
| `stage` | the Shell | `begin` with the model and flags; `end` with the rc, time, region, frame (size/pitch/format), model, fallback, lines; `wait` (first stage only) when that screen had no line in your region (rc 0) or changed under every read (rc -120): its time, rc and `screens=k/30`; the stages run on a later settled screen |
| `gstep` | the game worker's step log | each step of the job, at most 20: `mem`, `check`, `alloc`/`free` (memory type, block, native rc, bytes; the `free` of every block the job can still free keeps a slot), `pass` (the job's first capture pass, or a failed one), `sig`, `find`, `load`, `recognize`, `fb`, `probe` (API 19 also logged `fallback`; since API 20 the fallback's `check`, `arg=1`, says it runs, with `sum=1` when the self-test forced it), and since API 20 `settle` (the stable-text wait), `blank` (a skipped line, [vita-vn-ocr.md](vita-vn-ocr.md#stable-text-and-blank-lines-api-20)), `norm` (the text normalised, [vita-vn-ocr.md](vita-vn-ocr.md#text-normalisation-shown-and-looked-up)), `lm` (the beam + LM decoder: its tier as `kind`, lines changed and doubtful, [vita-vn-ocr.md](vita-vn-ocr.md#beam--lm-decoding-api-20)), `cascade` (the doubtful lines read again, W20's frames, [vita-vn-ocr.md](vita-vn-ocr.md#confidence-cascade-api-20)) and `refine` (the finder's boxes refined, its mode as `kind`, [vita-vn-ocr.md](vita-vn-ocr.md#finder-refinement-api-20)); `check`'s `kind` is the LM tier it chose, plus 8 with the cascade, each with the game's free main/CDRAM/phycont after it |
| `line` | the game worker | each recognized line: size, CTC frames, time |
| `result` / `memory` | the game worker's result | capture source and memory, block size, strip rows, strips read, rows that changed between reads, timings; the job's memory check |

The kernel routes are looked up at run time by library and function NID through taiHEN's
`module_get_export_func`, never imported, and only when the Shell asks; nothing of this runs at
boot or in a hook. #74's 3.65 console returned `0x80020001` from the block route at every step;
the analyser reports such a route as failed, never its zeros as figures.

## What the analyser says

`tools/analyse_selftest.py` (tests: `tools/tests/test_analyse_selftest.py`, with #74's own report
as `tools/tests/fixtures/selftest-74-report.txt`, and v2 reports written by the real
`shell/selftest.c` on the host, `host/tests/selftest_writer.c`) prints two tables. The first is
the run and its OCR, PASS, FAIL or NOTEXT; these rows alone decide the verdict:

```
the run and its OCR (PASS/FAIL, NOTEXT: no line in your region; these rows decide the verdict)
  stage            result  detail
  boot             PASS    normal boot (seq 2, 0 failed boots before)
  ok               PASS    stable minute at 60999 ms
  game             PASS    PCSG00415 enabled, region user 1536,48188,62600,15059
  fallback         PASS    917x125+22+400 FL10_w8.vocr, 3 line(s), capture hook into user 61,440 B ...
  textbox          FAIL    out of memory (rc -103); failed steps: alloc user/workspace 0x80024302 ...
  fullscreen       PASS    960x544+0+0 H15_w8.vocr, 5 line(s), ...
  textbox2         PASS    ...
  recognitions     PASS    11 line(s) recognized in all, 6 in your region
  done             PASS    at 155499 ms, the marker removed
```

A region stage (`fallback`, `textbox`, `textbox2`) that completed but found no line in your region
is `NOTEXT`, not PASS: it shows the capture and the memory, not recognition. #81's report (the
v0.8.0-test.1 console run) had all three at 0 lines while the full screen found 5, and this tool
then said PASS. `recognitions` is NOTEXT when no region stage recognized a line. A run with no
FAIL but a NOTEXT row ends `NO TEXT: ...` and exits 1; a FAIL wins over it. The verdict names the
stages without a line and either what the full screen found (no region stage read a line: the
text was elsewhere) or what the other region stages read there (the screen changed between the
stages, when the region's checksum did). The `fallback` row and the findings count the settled screens
the stages waited past (`selftest-v2-waited.txt`), and say when the wait reached its bound
(`selftest-v2-notext.txt`, shaped like #81's run); a report that ends while the stages still wait
is INCOMPLETE and says so (`selftest-v2-waiting.txt`).

The second is the diagnostics, PASS or WARN, never FAIL. They say how OCR ran and what the
console measured, so a diagnostic that read nothing or did not work is a WARN and leaves the
verdict alone. #75's first build checked the wrong address range in every kernel memory route;
its report would have said FAIL with OCR working. `selftest-v2-diagnostics.txt` is that case: OCR
works in every stage while every diagnostic a passing run can warn does (probe,
capture<64KiB, kernel-pool, kernel-memory; tearing and game-memory cannot warn when OCR passes).

```
diagnostics (PASS/WARN: a WARN never changes the verdict)
  stage            result  detail
  probe            WARN    the display hook could not write the game's USER memory: worked: kstrip->user, ...
  capture<64KiB    WARN    capture block over 65,536 B in: fullscreen 262,144 B of cdram (USER_RW refused it there, ...
  kernel-pool      WARN    the kernel strip (<= 64 KiB of the kernel pool) was used in: fallback, textbox, ...
  tearing          PASS    0 row(s) changed between two reads (the job reads again)
  kernel-memory    WARN    no KD figure or no user figure at: boot, ok, game, done; routes that read nothing: ...
  game-memory      PASS    free USER 91,226,112 B, CDRAM 117,440,512 B in the game before its first block
```

| diagnostic | WARN means |
|---|---|
| `probe` | the display hook could not write the game's `USER_RW`; the paths that did work are listed |
| `capture<64KiB` | a capture block over 64 KiB: USER_RW refused it and CDRAM (256 KiB units) or phycont (1 MiB) gave one |
| `kernel-pool` | the ladder's last rung, the kernel strip (at most 64 KiB, one call), carried a stage's capture |
| `kernel-memory` | no KD or no user figure at some step; each route that read nothing is named with its rc and step |
| `game-memory` | the game's free memory could not be read |

Then come the memory routes per step (a WARN cell gives the route's native result and the step it
stopped at), the game worker's memory per allocation, the findings, a `diagnostics:` line (`PASS`,
or `WARN (rows)`) and the verdict: `PASS` (it names any diagnostics that warned),
`FAIL: <rows> failed`, `NO TEXT: ...` (no FAIL, but no line in your region), or
`INCOMPLETE: stage '<name>' began and never wrote its record` (or `INCOMPLETE: the stages were
still waiting for text in your region`). A stop
is the run's failure in whichever stage it happens, the probe included, and never a WARN. An API
18 (`selftest v1`) report gets the same table for its steps (`boot`, `ok`, `game`, `ocr`, `done`),
with its memory routes as diagnostics; for #74 it says `ocr FAIL: never reached: the capture
failed 5 time(s) (no memory)` and names the 2,088,960 B kernel buffer.

Exit status follows the verdict: 0 for PASS (diagnostic WARNs included), 1 for FAIL, NO TEXT or
INCOMPLETE, 2 for a file that is not a report. `--json` gives `ocr_result` (`PASS`, `FAIL`,
`NOTEXT` or `INCOMPLETE`), `diagnostics_result`,
`warnings` and each row's `kind` (`ocr` or `diagnostic`).

## Memory headroom and the model tier

After the diagnostics the analyser prints each OCR stage's **headroom**: the game's free USER
memory at the job's memory check, plus another model's resident weights the job released first,
minus the 4 MiB reserve, the job's workspace and capture block and the weights it had to load. A
negative headroom is a refused job. The Shell's `game vocr memory:` log line carries the same figure
for every OCR job, not only the self-test's.

Then a **model tier** table: the Small / Normal / Large recognizer tiers of
[`tools/model_tiers.json`](../tools/model_tiers.json), the one table the Overlay app's tier choice
reads as well, so the analyser and the app cannot price different tiers. Each tier is priced at its
ceilings, an estimate, with vita-vn-ocr's formula (`docs/model-tiers.md` there, section 1.5):
page(weights) + page(max(the line finder's 83,323 B arena, the tier's W8I arena) + 3,760 B of
runtime structs) + the 61,440 B capture block + the 516,096 B line-block ceiling, plus the tier's
own reserve (4 / 8 / 16 MiB), against the free USER the game had before the worker's first block.
The recommendation is the largest tier whose first job fits: it is one sample in one game, and
memory alone. The weights ceilings are the owner's of 2026-10-14 (Small under 1 MB, Normal at most
1.6 MB, Large at most 2.5 MB of int8 weights); the box time is each tier's limit for a typical full
textbox, 3 lines of about 640 px on one Cortex-A9 core at 444 MHz, estimated, and wins where it is
tighter. A table the analyser cannot read leaves the tiers out with a finding; the verdict never
depends on them.

| tier | weights at most | box time | W8I arena ceiling | first job needs | reserve | free USER needed |
|---|---|---|---|---|---|---|
| Large | 2,500,000 B | < 3 s | 1,310,720 B | 4,395,008 B | 16 MiB | 21,172,224 B |
| Normal | 1,600,000 B | < 2 s | 262,144 B | 2,445,312 B | 8 MiB | 10,833,920 B |
| Small | 999,999 B | < 1 s | 131,072 B | 1,716,224 B | 4 MiB | 5,910,528 B |

## Collecting everything: `--collect`

```sh
bash tools/install_ftp.sh --collect VITA_IP
```

With VitaShell's FTP running, this fetches, **read-only** (the Vita receives only the login and
`PWD`, `CWD`, `TYPE`, `PASV`/`EPSV`, `LIST`, `SIZE`, `RETR` and `QUIT`; nothing that writes, renames or
deletes; `tools/collect_vita.py`), into one timestamped folder
`build/device/collect/vjo-collect-YYYYMMDD-HHMMSSZ/` and a zip of it next to it:

- `ux0:data/VitaJPOverlay/`: `log.txt`, `log.old.txt`, `selftest-report.txt`, `selftest.txt` (the
  marker, when the test is still armed), `status.txt`, `status.prev.txt`, `kernel.txt`,
  `kernel.prev.txt`, `config.ini` with every `*api_key` value replaced by its length before anything
  is written (also a key line without `=`; any `api_key` text in the logs' config warnings is masked
  too), `games.ini`, `region.ini`, `anki_host.txt` and the whole `anki_queue/`;
- `ur0:data/VitaJPOverlay/boot.txt`; `ur0:tai/config.txt` and `ux0:tai/config.txt` with the
  installer's `.vjo-bak` copies; every `ux0:data/*.psp2dmp` crash dump;
- the listing of each directory, and the SHA-256 of the installed plugins, RCO, settings VPK, OCR
  models, the beam + LM decoder's LMs (`*.vlm`) and their `LM-ATTRIBUTION.md` (downloaded to a
  temporary file and deleted, never kept), compared with the package's `package-manifest.json`; the
  dictionary is compared by size.

Each file keeps its device path in the folder (`ux0/data/VitaJPOverlay/log.txt`, ...); `COLLECT.json`
lists every file with its bytes and SHA-256, what was not on the Vita and every error. Then
`analyse_selftest.py --collect` writes `analysis.txt` and `analysis.json` into the folder and prints
the summary: whether the installed files match the package (a file it does not ship, such as a
removed engine's model, is listed as not in this package), the configuration, where taiHEN loads
each plugin (the worker under `*TITLEID` only), the boot record, the kernel's start and hooks, every
OCR job (results, models and fallbacks, capture route, find/load/recognize times, per-line and
per-frame times, free USER and headroom, failed worker steps), the dictionary jobs, the overlay's
slowest frame, Anki saves and sends, the lines that report a problem, the Anki queue, crash dumps, the
model tier from each free-memory sample, the full self-test analysis when a report is there, and what
it cannot decide, with where in the zip the data to decide it is. The zip is the one file to send.

## Cost and limits

- Shell module pages: 299,008 B, unchanged by API 19 (the stages share the job's one result
  buffer; `vjo_capture_raw` was removed).
- The report is capped at 48 KiB and written line by line; it is also copied into `status.txt`.
- It does not launch the game: the owner does (the expert's rule against automating risky
  actions). It runs one job of five stages, once per arming.
- The game's exact partition figure is not sampled by the kernel; the game's free `USER`, CDRAM
  and phycont come from the worker's own `sceKernelGetFreeMemorySize` before and after each
  allocation (`gstep ... main= cdram= phycont=`).
- Unmeasured until the first run: whether the display hook can write each memory type on the
  owner's 3.65 (the probe answers it), the capture's time per strip (`capture_ms`), and which
  kernel memory route reads there.
