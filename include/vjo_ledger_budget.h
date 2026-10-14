/* The memory ledger's static budget: every pool our three plugins take memory
 * from, every item they hold there, its worst case in bytes, and the limit
 * each pool's worst case must stay under. tools/check_memory_ledger.py reads
 * this file (plain `#define NAME expression` lines, integer arithmetic only)
 * and fails the build when a worst case exceeds its limit or a packaged
 * module has more pages than declared; the C code uses the same constants,
 * so the table and the binaries cannot drift apart. docs/memory-ledger.md
 * explains the ledger; docs/vita-memory.md is the reference for the pools.
 *
 * Tags: [measured] read on the console or from the packaged binaries (where
 * is in docs/memory-ledger.md), [SDK] the Sony documentation, [expert] the
 * plugin developer's guidance (AGENTS.md), [estimate] neither. */
#ifndef VJO_LEDGER_BUDGET_H
#define VJO_LEDGER_BUDGET_H

/* Every thread costs its stack plus at least 4 KiB of TLS from its process's
 * budget [SDK: Kernel-Reference, sceKernelCreateThread]. */
#define VJO_LEDGER_TLS 4096u
#define VJO_LEDGER_PAGE 4096u

/* ---- SceShell's partition (ScePhyMemPartShell, 50 MiB; pool "shell pages") ----
 * Module pages are permanent; they may only shrink (AGENTS.md rule 1). The
 * values are the packaged SELF segments rounded to pages [measured, PR #60]. */
#define VJO_BUDGET_SHELL_MODULE_PAGES 299008u      /* VitaJPOverlay_Shell.suprx: 217,088 text + 81,920 data/bss */
/* VitaJPOverlay_OCR.suprx: 57,344 text + 12,288 data/bss [measured, API 21 build: 55,988 B and 10,296 B].
 * Raised from 45,056 B (36,864 + 8,192): the in-game capture, its memory ladder, probe and step log run in
 * the worker, in the game's own USER_RW (91,226,112 B free measured in CLANNAD, docs/vita-memory.md), never
 * in SceShell. Raised from 53,248 B (45,056 + 8,192) by one text page for API 20's job policy: the
 * stable-text wait and the blank-line check (core/vocr_ocr.c), in the same game partition. Raised from
 * 57,344 B (49,152 + 8,192) by two text pages and one data page for API 21 (ocr-speed): the recognizer's
 * faster kernels (vita-vn-ocr runtime/vocr.c, +1.5 KB), parallel lines and the line cache (core/vocr_ocr.c,
 * its 3,152 B VjoVocrCache and the 1,312 B line queue in .bss), the helper threads and the clock; 12 KiB of
 * the game's partition, which had 85.6 MB above the reserve beside every job of #81's session. */
#define VJO_BUDGET_WORKER_MODULE_PAGES 69632u
/* The installer registers the worker per game (*TITLEID), never under *ALL,
 * so taiHEN does not load it into SceShell. Were it loaded there (an older
 * install's *ALL line, a hand edit), its module_start returns
 * SCE_KERNEL_START_NO_RESIDENT outside a candidate game, so the pages are
 * given back at once [SDK: Kernel-Overview, module_start]. */
#define VJO_BUDGET_WORKER_IN_SHELL_PAGES 0u
#define VJO_STACK_SHELL_NET 65536u                 /* VjoNet: BearSSL needs it for TLS */
#define VJO_STACK_SHELL_CTL 16384u                 /* VjoControl */
#define VJO_STACK_SHELL_ANKI 65536u                /* VjoAnki, while an Anki job runs: with anki_mining on, a count
                                                    * of the queue on every overlay open, a save (×), a send (△) */
#define VJO_BUDGET_SHELL_NET_STACK (VJO_STACK_SHELL_NET + VJO_LEDGER_TLS)
#define VJO_BUDGET_SHELL_CTL_STACK (VJO_STACK_SHELL_CTL + VJO_LEDGER_TLS)
#define VJO_BUDGET_SHELL_ANKI_STACK (VJO_STACK_SHELL_ANKI + VJO_LEDGER_TLS)
/* The Vita build takes result memory from the PAF heap (VJO_PAF_ALLOC); the
 * USER_RW path exists for the host tests only. */
#define VJO_BUDGET_SHELL_USERRW_RESULTS 0u
#define VJO_WORST_SHELL_PAGES_PERMANENT (VJO_BUDGET_SHELL_MODULE_PAGES + VJO_BUDGET_WORKER_IN_SHELL_PAGES + \
                                         VJO_BUDGET_SHELL_NET_STACK + VJO_BUDGET_SHELL_CTL_STACK)
#define VJO_WORST_SHELL_PAGES (VJO_WORST_SHELL_PAGES_PERMANENT + VJO_BUDGET_SHELL_ANKI_STACK + VJO_BUDGET_SHELL_USERRW_RESULTS)
/* A whole Shell plugin has "genuinely under 2 MiB", and SceShell wants that
 * headroom itself [expert]; the limit keeps half of it. The partition had
 * 28,672 and 98,304 B free beside a game [measured], so this is a ceiling
 * on our footprint, not a promise that it fits: only shrinking is safe. */
#define VJO_LIMIT_SHELL_PAGES (2u * 1024u * 1024u / 2u)

/* ---- SceShell's PAF global heap (5,242,880 B, mapped once; pool "shell paf") ----
 * Allocations here take no new pages; they take room firmware's PAF users
 * need. The runtime reserve guard (shell/memory.cpp) refuses anything that
 * would leave less than VJO_PAF_RESERVE free. */
#define VJO_PAF_HEAP 5242880u                      /* [measured, docs/meiki-ocr.md on experiment/meiki-ocr] */
#define VJO_PAF_FREE_MEASURED 2557952u             /* 2,498 KiB free beside CLANNAD, 2026-10-10 [measured, same] */
#define VJO_PAF_RESERVE (1536u * 1024u)            /* shell/memory.cpp RESERVE */
#define VJO_BUDGET_PAF_PLUGIN 8192u                /* the RCO (6,368 B file; 5,632 B before the region cursor) and the Paf Plugin object [estimate] */
#define VJO_BUDGET_PAF_RESULTS_LOCAL 65536u        /* one result arena, local dictionary (worker.c LOCAL_RESULT_ARENA_SIZE) */
#define VJO_BUDGET_PAF_RESULTS_NET 571888u         /* 2 x 262,144 arenas + 47,600 connection pool (worker.c MEM_SIZE) */
/* An Anki job (anki.c ANKI_TEXT_MEM): a text-only card. The Picture field is off in the
 * build (VJO_ANKI_SCREENSHOT 0): its screenshot would be the kernel's 2,088,960 B full-frame
 * capture (VJO_BUDGET_KERNEL_CAPTURE), which #74's console refused, and 393,216 B here (a
 * picture card's arena, ANKI_PICTURE_MEM). anki.c checks its arena against this at compile time. */
#define VJO_BUDGET_PAF_ANKI 131072u
#define VJO_BUDGET_PAF_UI 20480u                   /* the overlay's text arena while its page is open: 17,952 B needed (overlay.cpp) */
#define VJO_WORST_SHELL_PAF_PERMANENT VJO_BUDGET_PAF_PLUGIN
/* The shipped default (local JMdict): one 64 KiB arena, the overlay open, an
 * Anki card being saved. */
#define VJO_WORST_SHELL_PAF (VJO_WORST_SHELL_PAF_PERMANENT + VJO_BUDGET_PAF_RESULTS_LOCAL + VJO_BUDGET_PAF_ANKI + VJO_BUDGET_PAF_UI)
/* The opt-in network dictionary (two arenas and a connection pool) with the
 * same overlay and Anki card. With a picture card's 384 KiB arena this was over
 * the best-case limit by 8,688 B (the checker warned; the runtime reserve guard
 * refuses the last allocation, which the code survives); a text-only card fits. */
#define VJO_WORST_SHELL_PAF_NET (VJO_WORST_SHELL_PAF_PERMANENT + VJO_BUDGET_PAF_RESULTS_NET + VJO_BUDGET_PAF_ANKI + VJO_BUDGET_PAF_UI)
/* What the measured free heap leaves us once the reserve is kept: the
 * reserve is the headroom. Later sessions saw 355-441 KiB and 190 KiB free
 * [measured]; under those every allocation is refused, which the code
 * survives (tested), so this limit is the best case, not a guarantee. */
#define VJO_LIMIT_SHELL_PAF (VJO_PAF_FREE_MEASURED - VJO_PAF_RESERVE)

/* ---- the kernel's partition (ScePhyMemPartKD, 36 MiB shared by every driver; pool "kernel") ---- */
/* VitaJPOverlay_Kernel.skprx: 40,960 text + 77,824 data/bss [measured, API 21 build: 37,044 B and
 * 77,752 B]. Raised from 114,688 B (36,864 text) by one text page for API 21's frame-pacing step
 * (kernel/game_ocr.c game_ocr_frame, VJO_GSTEP_PACE): the display hook is the only place that sees the
 * game's frames, and a job's helpers must be shown not to cost it frames; 344 B of code, 40 B of .bss. */
#define VJO_BUDGET_KERNEL_MODULE_PAGES 118784u
#define VJO_STACK_KERNEL 16384u                    /* VjoWorker */
#define VJO_BUDGET_KERNEL_STACK VJO_STACK_KERNEL
/* The Shell's JPEG capture (Lens; an Anki screenshot only with VJO_ANKI_SCREENSHOT) at a full frame, KERNEL_RW,
 * per request. Local OCR takes no kernel block since API 19 (#74: on the owner's
 * 3.65 this 2,088,960 B request failed with 0x80024302 at every attempt). */
#define VJO_BUDGET_KERNEL_CAPTURE (960u * 544u * 4u)
/* The OCR ladder's last rung (kernel/buffers.c buffers_kstrip_alloc): one strip of
 * grey rows for one vjoOcrCaptureRows call, only after the display hook could not
 * write into the game's own block; never held together with the JPEG capture
 * (an OCR job and a capture exclude each other), so it adds nothing to the worst case. */
#define VJO_BUDGET_KERNEL_OCR_STRIP (64u * 1024u)
#define VJO_WORST_KERNEL_PERMANENT (VJO_BUDGET_KERNEL_MODULE_PAGES + VJO_BUDGET_KERNEL_STACK)
#define VJO_WORST_KERNEL (VJO_WORST_KERNEL_PERMANENT + VJO_BUDGET_KERNEL_CAPTURE)
/* No cited safe size exists for this pool: its free space on a retail console
 * is unmeasured, and a 2,088,960 B capture has failed there once (#14) and
 * succeeded since. Until the self-test measures the partition, the limit is
 * today's worst case: a ratchet, so nothing grows unnoticed. */
#define VJO_LIMIT_KERNEL VJO_WORST_KERNEL

/* ---- the game's partition (ScePhyMemPartGame, 256 MiB minus the game's own use; pool "game") ---- */
#define VJO_BUDGET_GAME_MODULE_PAGES VJO_BUDGET_WORKER_MODULE_PAGES
#define VJO_STACK_GAME 32768u                      /* VjoGameVocr */
#define VJO_BUDGET_GAME_STACK (VJO_STACK_GAME + VJO_LEDGER_TLS)
#define VJO_BUDGET_GAME_WEIGHTS 1560576u           /* F20, the largest accepted model, page-rounded (core/vocr_ocr.c) */
/* F20 at a 960x544 region with two helpers, page-rounded (vjo_vocr_job_bytes_helpers, host build:
 * 858,896 B; 245,808 B without helpers). API 21 (ocr-speed) adds per helper a recognizer arena and
 * context, a line's pixels (at most VJO_VOCR_STRIP_BYTES) and its text (VJO_OCR_TEXT_CAP): 304,496 B for
 * F20, 246,384 B for VOCR3-H15; plus the worker's own line text. The worker takes helpers only when its
 * memory check passes with them (one fewer at a time, down to none: API 20's job). API 19 holds no grey
 * copy of the region (it was 770,048 B with one at 960x544). */
#define VJO_BUDGET_GAME_WORKSPACE 860160u
/* API 21: one thread per line a helper recognizes, created for the line and deleted once it is
 * placed (game_ocr/main.c pool_run): at most VJO_VOCR_HELPERS_MAX at once, none between jobs.
 * Its frames: vocr_run 432 + run_head 1,240 + mm_i16 616 B with the Vita compiler (-fstack-usage). */
#define VJO_STACK_GAME_HELPER 16384u
#define VJO_BUDGET_GAME_HELPER_STACK (VJO_STACK_GAME_HELPER + VJO_LEDGER_TLS)
#define VJO_BUDGET_GAME_HELPERS (2u * VJO_BUDGET_GAME_HELPER_STACK)
/* The capture block: one strip of grey rows and, per region row, its checksum,
 * its recent states and two bits, at most 65,536 B at any region (vjo_vocr_capture_bytes,
 * VJO_VOCR_CAPTURE_LIMIT; 65,480 B at 960x544, 61,388 B at 917x125). Its fallback rungs take 256 KiB of CDRAM or 1 MiB of
 * phycont instead, other partitions, only when USER_RW refused it. */
#define VJO_BUDGET_GAME_CAPTURE 65536u
/* A line taller than a strip, kept whole (core/vocr_ocr.h VJO_VOCR_LINE_MAX_BYTES): line
 * width x line height, so the recognizer gets exactly API 18's input for any line the finder
 * reports (host/tests/test_capture_parity.c; 78,884 B at most on the linefind dev regions).
 * Only when such a line occurs, freed with the job. Sized when the job ceiling was 2,392,064 B:
 * 2,392,064 - 1,560,576 (F20's weights) - 249,856 (the serial workspace) - 65,536 (the capture
 * block) = 516,096 B; a larger band is left out and counted. A tall line is recognized on the
 * worker's own thread, never copied for a helper. */
#define VJO_BUDGET_GAME_LINE 516096u
#define VJO_GAME_RESERVE (4u * 1024u * 1024u)      /* kept free in the game before either block (game_ocr/main.c) */
#define VJO_WORST_GAME_PERMANENT (VJO_BUDGET_GAME_MODULE_PAGES + VJO_BUDGET_GAME_STACK)
#define VJO_WORST_GAME_JOB (VJO_BUDGET_GAME_WEIGHTS + VJO_BUDGET_GAME_WORKSPACE + VJO_BUDGET_GAME_CAPTURE + \
                            VJO_BUDGET_GAME_LINE + VJO_BUDGET_GAME_HELPERS)
/* The recognizer ceiling tier: weights <= 1,601,536 B [vita-vn-ocr docs/vita-memory-budget.md].
 * Raised for API 21's helpers (ocr-speed) from a 790,528 B workspace and a 2,392,064 B peak: the
 * worst case is now 3,043,328 B (F20, a full screen, two helpers, a tall line). The game had
 * 90,177,536-91,226,112 B of USER free beside every job of the owner's CLANNAD session [measured,
 * vita-jp-overlay #81, v0.8.0-test.1 log.txt], 85.6 MB above the 4 MiB reserve; every job checks its
 * own need, helpers included, against the game's free memory minus that reserve, and runs fewer
 * helpers (down to none) or the fallback model when it is short. */
#define VJO_LIMIT_GAME_WEIGHTS 1601536u
#define VJO_LIMIT_GAME_WORKSPACE 1048576u
#define VJO_LIMIT_GAME_JOB 3145728u

#endif
