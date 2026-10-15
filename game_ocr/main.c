/* Local OCR runs in the enabled game's process. No Paf, newlib startup, hooks,
 * network, or borrowed game allocations. Model chunks cross the kernel
 * transport; weights and inference use private memblocks: the verified weights
 * stay resident between jobs, the job workspace does not.
 *
 * API 19: the capture is the game's own too. The worker asks the kernel for one
 * strip of the job's region at a time (vjoOcrCaptureRows); the display hook,
 * running in the game's thread on the frame it submits, converts those rows to
 * 8-bit grey and copies them straight into the worker's capture block. There is
 * no copy of the whole frame anywhere, in the kernel or here (#74: the kernel
 * could not give the 2,088,960 B full-frame block it used to take). Every
 * allocation tries USER_RW first, then USER_CDRAM_RW, then
 * USER_MAIN_PHYCONT_NC_RW, and a strip the hook cannot write into this block is
 * read through a small kernel strip instead: each rung's native result is in
 * the job's step log, so one console run says which works. */
#include "main.h"
#include "../core/fbgrey.h"
#include "../core/textnorm.h"
#include "../core/vocr_ocr.h"
#include "../include/vjo_vocr_sizes.h"
#include "../include/vjo_ledger_budget.h"
#include "../core/ledger.h"
#define WORKER_STACK_BYTES VJO_STACK_GAME
/* The lowest user priority (SDK: default 0x10000100 + 31) on every game core
 * (SCE_KERNEL_CPU_MASK_USER_ALL, which the SDK says the default mask equals;
 * named here so the intent is explicit). */
#define WORKER_PRIORITY (0x10000100 + 31)
#define WORKER_AFFINITY (0x00010000 | 0x00020000 | 0x00040000) /* SCE_KERNEL_CPU_MASK_USER_ALL (psp2/kernel/cpu.h) */
#include "../core/boot_guard.h"
#include "../core/games.h"
#include "../include/vjo_api.h"
#include "../include/vjo_import.h"
#include <psp2/appmgr.h>
#include <psp2/display.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#define GAME_RESERVE VJO_GAME_RESERVE
#define JOIN_US 2000000u
#define MODEL_WAIT_US 2000000u
/* Only when a model syscall still answers BUSY after its own bounded wait
 * for the kernel worker (a slow card): yield, then repeat it. */
#define MODEL_POLL_US 1000u
#define PAGE_ROUND(x) (((uint32_t)(x) + 4095u) & ~4095u)
#define ROUND_TO(x, unit) (((uint32_t)(x) + ((unit) - 1u)) & ~((unit) - 1u))
#define CDRAM_UNIT (256u * 1024u)     /* SDK: USER_CDRAM_RW sizes are multiples of 256 KB */
#define PHYCONT_UNIT (1024u * 1024u)  /* SDK: USER_MAIN_PHYCONT_NC_RW sizes are multiples of 1 MB */
#ifdef VJO_HOST
#define IMPORT_READY(fn) 1
#else
#define IMPORT_READY(fn) VJO_IMPORT_READY(fn)
#endif

static SceUID worker = -1, workspace_uid = -1, weights_uid = -1, capture_uid = -1, line_uid = -1, lm_uid = -1;
static SceUID cascade_uid = -1;
/* A rows call returned BUSY: the display hook may still write into the capture
 * (or tall-line) block, so neither is freed (they go with the process). */
static int hook_owns_capture;
static uint32_t workspace_bytes_held, weights_bytes_held, capture_bytes_held, line_bytes_held, lm_bytes_held;
static uint32_t cascade_bytes_held;
static uint32_t capture_memory, workspace_memory; /* VJO_GAME_OCR_MEM_* of the blocks held */
static void *line_base;

/* The game worker's memory ledger (core/ledger.h): what we hold in the game's
 * partition. The game has no file access, so it is read through the result
 * fields (workspace_bytes, resident_bytes, capture_bytes, the step log) rather
 * than printed here. */
enum { GL_MODULE = 0, GL_STACK, GL_WEIGHTS, GL_WORKSPACE, GL_CAPTURE, GL_LINE, GL_LM, GL_CASCADE, GL_COUNT };
static const VjoLedgerItemDef ledger_defs[GL_COUNT] = {
    [GL_MODULE] = {"OCR worker pages", VJO_LEDGER_POOL_GAME},
    [GL_STACK] = {"VjoGameVocr stack+TLS", VJO_LEDGER_POOL_GAME},
    [GL_WEIGHTS] = {"VjoGameVocrWeights", VJO_LEDGER_POOL_GAME},
    [GL_WORKSPACE] = {"VjoGameVocr workspace", VJO_LEDGER_POOL_GAME},
    [GL_CAPTURE] = {"VjoGameVocrCapture", VJO_LEDGER_POOL_GAME},
    [GL_LINE] = {"VjoGameVocrLine", VJO_LEDGER_POOL_GAME},
    [GL_LM] = {"VjoGameVocrLM", VJO_LEDGER_POOL_GAME},
    [GL_CASCADE] = {"VjoGameVocrCascade", VJO_LEDGER_POOL_GAME},
};
VjoLedger g_game_ledger;

static void ledger_init(void)
{
    vjo_ledger_init(&g_game_ledger, ledger_defs, GL_COUNT);
    vjo_ledger_add(&g_game_ledger, GL_MODULE, VJO_BUDGET_GAME_MODULE_PAGES);
}

/* ---- the job's step log (VjoGameOcrResult.step) ---- */
static VjoGameOcrResult *log_to;   /* the result being filled, or NULL */
static uint64_t job_started_us;

static uint64_t now_wide(void)
{
    return sceKernelGetProcessTimeWide();
}

/* The game's free memory as sceKernelGetFreeMemorySize reports it (each figure
 * is the free size minus a system margin [SDK: Kernel-Reference]); main holds
 * the error, the others 0, when the query failed. */
static int free_sample(int32_t *main_bytes, int32_t *cdram, int32_t *phycont)
{
    SceKernelFreeMemorySizeInfo f = {.size = sizeof(f)};
    int rc = IMPORT_READY(sceKernelGetFreeMemorySize) ? sceKernelGetFreeMemorySize(&f) : -1;
    if (rc < 0) {
        *main_bytes = rc;
        *cdram = *phycont = 0;
        return rc;
    }
    *main_bytes = f.size_user;
    *cdram = f.size_cdram;
    *phycont = f.size_phycont;
    return 0;
}

/* The confidence under which the cascade reads a line again: the core's
 * default (VJO_VOCR_CASCADE_CONF); host tests force it. */
#ifndef VJO_GAME_CASCADE_CONF
#define VJO_GAME_CASCADE_CONF 0u
#endif

/* Blocks the worker holds. Each one this job can still free keeps a slot of
 * the step log for its FREE step: the job's own blocks, and a resident block
 * (weights, LM, the cascade's model) only while it is not verified, since a
 * verified one stays for the next job (blocks_kept). */
static unsigned blocks_held;
static unsigned blocks_kept(void);

static VjoGameOcrStep *step_add(unsigned id, unsigned kind, unsigned arg, int32_t rc, uint32_t bytes,
                                uint32_t sum, uint32_t us, int sample)
{
    VjoGameOcrStep *s;
    unsigned kept, reserve;
    if (!log_to || log_to->n_steps >= VJO_GAME_OCR_STEPS) return NULL;
    /* The log is full before a block's release is: another step is dropped. */
    kept = blocks_kept();
    reserve = blocks_held > kept ? blocks_held - kept : 0u;
    if (id != VJO_GSTEP_FREE && log_to->n_steps + reserve >= VJO_GAME_OCR_STEPS) return NULL;
    s = &log_to->step[log_to->n_steps++];
    sceClibMemset(s, 0, sizeof(*s));
    s->id = (uint8_t)id;
    s->kind = (uint8_t)kind;
    s->arg = (uint16_t)(arg > 0xFFFFu ? 0xFFFFu : arg);
    s->rc = rc;
    s->bytes = bytes;
    s->sum = sum;
    s->us = us ? us : (uint32_t)(now_wide() - job_started_us);
    if (sample) free_sample(&s->free_main, &s->free_cdram, &s->free_phycont);
    return s;
}

/* The confidence cascade's step (include/vjo_game_ocr.h VJO_GSTEP_CASCADE). */
static void cascade_step(const VjoVocrStats *st)
{
    step_add(VJO_GSTEP_CASCADE, st->cascade_blanked, st->cascade_lines, st->cascade_rc,
             (st->cascade_loaded ? VJO_GLM_READ : st->cascade_cached ? VJO_GLM_RESIDENT : 0u) |
                 (st->cascade_changed > 0x3Fu ? 0x3Fu : st->cascade_changed) << 24 |
                 (st->cascade_skipped > 0xFFu ? 0xFFu : st->cascade_skipped) << 16 |
                 (st->cascade_frames > 0xFFFFu ? 0xFFFFu : st->cascade_frames),
             st->cascade_load_us, st->cascade_us + 1u, 0);
}

/* ---- memory: the one USER allocation site of the worker ---- */
static const struct {
    int type;
    uint32_t unit, mem;
} ladder[] = {
    {SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4096u, VJO_GAME_OCR_MEM_USER},
    {SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, CDRAM_UNIT, VJO_GAME_OCR_MEM_CDRAM},
    {SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, PHYCONT_UNIT, VJO_GAME_OCR_MEM_PHYCONT},
};
#define LADDER_ALL 7u  /* USER, CDRAM, PHYCONT */
#define LADDER_USER 1u

/* One block for ledger item `item`, from the first memory type in `which`
 * (bits of ladder[]) that gives it, with its base, or < 0 (the last native
 * error). Each attempt is a step: its type, native result and the free
 * memory after it. A block whose base cannot be read is freed again; if even
 * that fails its uid is returned in *leaked so the caller keeps it recorded
 * for cleanup. *held: the bytes actually taken (rounded to the type's unit). */
static SceUID game_memblock_alloc(int item, unsigned gitem, const char *name, uint32_t bytes, unsigned which,
                                  void **base, SceUID *leaked, uint32_t *held, uint32_t *mem)
{
    SceUID uid = -1;
    *base = NULL;
    *leaked = -1;
    *held = 0;
    *mem = VJO_GAME_OCR_MEM_NONE;
    for (unsigned i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
        uint32_t rounded = ROUND_TO(bytes, ladder[i].unit);
        if (!(which & (1u << i))) continue;
        uid = sceKernelAllocMemBlock(name, ladder[i].type, rounded, NULL);
        if (uid >= 0) blocks_held++; /* from here its FREE step has a slot */
        step_add(VJO_GSTEP_ALLOC, ladder[i].mem, gitem, uid, rounded, 0, 0, 1);
        if (uid < 0) continue;
        if (sceKernelGetMemBlockBase(uid, base) < 0 || !*base || ((uintptr_t)*base & 15u)) {
            *base = NULL;
            if (sceKernelFreeMemBlock(uid) < 0) *leaked = uid;
            else blocks_held--;
            return -1;
        }
        vjo_ledger_add(&g_game_ledger, item, rounded);
        *held = rounded;
        *mem = ladder[i].mem;
        return uid;
    }
    return uid < 0 ? uid : -1;
}

static int game_memblock_free(int item, unsigned gitem, SceUID uid, uint32_t bytes)
{
    int rc = sceKernelFreeMemBlock(uid);
    step_add(VJO_GSTEP_FREE, 0, gitem, rc, bytes, 0, 0, 1);
    if (rc < 0) return -1;
    if (blocks_held) blocks_held--;
    vjo_ledger_sub(&g_game_ledger, item, bytes);
    return 0;
}

static uint32_t model_seq;
static int model_owned;
static uint32_t quitting, active_seq;
static int worker_started, claim_outstanding, blocked, io_error, stat_error;
/* A PSP game (vjo_game_is_psp: NPXS10028, or a PSPEMU* bubble): the refinement's
 * "psp" mode applies (set by module_start from the title). */
static int psp_title;

/* VJO_GAME_OCR_OPEN_*: which of the request's two pinned files this job opens;
 * and whether the cascade's model is the job's to open by its name. */
static uint32_t open_which;
static int open_cascade;
static VjoGameOcrResult pending_result; /* never put large results on thread stacks */
static void *weights_alloc(void *ud, size_t bytes);
static int weights_release(void *ud);
/* Verified weights kept between jobs while this game is the active one. */
static VjoVocrResident resident = {weights_alloc, weights_release, NULL, NULL, NULL, 0, 0, 0};
/* The same for the beam + LM decoder's LM (API 20). */
static void *lm_alloc(void *ud, size_t bytes);
static int lm_release(void *ud);
static VjoVocrLmResident lm_res = {lm_alloc, lm_release, NULL, NULL, NULL, 0, 0, 0};
static void *cascade_alloc(void *ud, size_t bytes);
static int cascade_release(void *ud);
/* The confidence cascade's larger model, kept like the weights (API 20). */
static VjoVocrResident cascade_res = {cascade_alloc, cascade_release, NULL, NULL, NULL, 0, 0, 0};
int sceKernelGetProcessTitleId(SceUID pid, char *titleid, SceSize len);

static int cancelled(void *ud)
{
    (void)ud;
    return __atomic_load_n(&quitting, __ATOMIC_ACQUIRE) ||
           !active_seq || vjoOcrCancelled(active_seq) != 0;
}

/* ---- the capture: strips of the region from the display hook ---- */
typedef struct {
    const VjoGameOcrRequest *r;
    uint32_t mode;          /* VJO_GAME_OCR_SRC_HOOK, then _KSTRIP once the hook could not write here */
    VjoGameOcrRowsInfo last;
    uint32_t pass_bytes, pass_strips, pass_us;
    int failed;             /* the rows call that ended the job, for the log */
    int fixed;              /* the probe: test this mode only, no ladder */
    int logged;             /* the first pass's PASS and SIG steps are in (a retry's are not logged) */
} Capture;

/* A BUSY from a request that never reached the hook (info.armed 0) is a
 * previous copy the kernel worker has not tidied yet (it does within a 16 ms
 * tick): ask again, a few times. */
#define ROWS_BUSY_TRIES 25u
#define ROWS_BUSY_DELAY_US 20000u

/* Errors after which the kernel strip may still work: the hook could not
 * write into this block, or the kernel refused its range. */
static int kernel_strip_may_help(int rc)
{
    return rc == VJO_ERR_COPY || rc == VJO_ERR_ARG;
}

static int capture_rows(Capture *c, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    VjoGameOcrRows req;
    unsigned busy = 0;
    int rc;
    for (;;) {
        sceClibMemset(&req, 0, sizeof(req));
        req.size = sizeof(req);
        req.seq = c->r->seq;
        req.y = y;
        req.n = n;
        req.mode = c->mode;
        req.flags = (flags & VJO_STRIP_FIRST ? VJO_ROWS_SIG : 0u) | (flags & VJO_STRIP_LAST ? VJO_ROWS_LAST : 0u);
        req.dst_stride = (uint32_t)stride;
        sceClibMemset(&c->last, 0, sizeof(c->last));
        c->last.size = sizeof(c->last);
        rc = vjoOcrCaptureRows(&req, dst, &c->last);
        if (rc == (int)n) {
            c->pass_bytes += n * c->r->width;
            c->pass_strips++;
            c->pass_us += c->last.wait_us;
            return rc;
        }
        if (rc == VJO_ERR_BUSY && !c->last.armed && busy < ROWS_BUSY_TRIES && !cancelled(NULL)) {
            if (!busy++) step_add(VJO_GSTEP_PASS, c->mode, y, rc, 0, 0, 0, 0);
            sceKernelDelayThread(ROWS_BUSY_DELAY_US);
            continue;
        }
        /* The ladder's last rung, once per job: the same rows through a kernel strip. */
        if (!c->fixed && c->mode == VJO_GAME_OCR_SRC_HOOK && kernel_strip_may_help(rc)) {
            step_add(VJO_GSTEP_PASS, VJO_GAME_OCR_SRC_HOOK, y, rc, 0, 0, c->last.wait_us, 0);
            c->mode = VJO_GAME_OCR_SRC_KSTRIP;
            continue;
        }
        if (c->mode == VJO_GAME_OCR_SRC_KSTRIP && c->last.kstrip_rc < 0)
            step_add(VJO_GSTEP_ALLOC, VJO_GAME_OCR_MEM_KERNEL, VJO_GITEM_CAPTURE, c->last.kstrip_rc,
                     n * c->r->width, 0, 0, 0);
        c->failed = rc;
        /* An armed request's copy outlived the call: the hook may still write dst, so the
         * block is never freed and this worker takes no more jobs. */
        if (rc == VJO_ERR_BUSY && c->last.armed) blocked = hook_owns_capture = 1;
        return rc == VJO_ERR_CANCELLED ? VJO_E_CANCELLED : rc < 0 ? rc : VJO_E_SOURCE;
    }
}

static int strip_source(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    Capture *c = ud;
    int rc;
    if (cancelled(NULL)) return VJO_E_CANCELLED;
    rc = capture_rows(c, y, n, dst, stride, flags);
    /* The first pass is in: one step for it, and the kernel's publication (the
     * job's first; a retry's is the same source, its count is the TEAR step's). */
    if (rc > 0 && (flags & VJO_STRIP_LAST) && !c->logged) {
        c->logged = 1;
        step_add(VJO_GSTEP_PASS, c->mode, c->pass_strips, 0, c->pass_bytes, c->last.fb_format, c->pass_us, 0);
        step_add(VJO_GSTEP_SIG, c->mode, 0, 0, c->last.fb_pitch, c->last.fb_width << 16 | c->last.fb_height, 0, 0);
    }
    return rc;
}

/* The core never asks for more than the ledger counts. */
typedef char vjo_line_budget_check[VJO_VOCR_LINE_MAX_BYTES <= VJO_BUDGET_GAME_LINE ? 1 : -1];
typedef char vjo_capture_budget_check[VJO_VOCR_CAPTURE_LIMIT <= VJO_BUDGET_GAME_CAPTURE ? 1 : -1];
typedef char vjo_lm_budget_check[PAGE_ROUND(VJO_VOCR_LM_MAX_BYTES) <= VJO_BUDGET_GAME_LM ? 1 : -1];
typedef char vjo_cascade_budget_check[PAGE_ROUND(VJO_VOCR_CASCADE_BYTES) <= VJO_BUDGET_GAME_CASCADE ? 1 : -1];

/* One block for a line taller than a strip (VjoGreySource.line_buffer). */
static void *line_buffer(void *ud, size_t bytes)
{
    void *base = NULL;
    SceUID leaked;
    uint32_t held, mem;
    (void)ud;
    if (line_uid >= 0 && line_bytes_held >= bytes) return line_base;
    if (line_uid >= 0) {
        if (game_memblock_free(GL_LINE, VJO_GITEM_LINE, line_uid, line_bytes_held) < 0) return NULL;
        line_uid = -1;
        line_bytes_held = 0;
        line_base = NULL;
    }
    if (bytes > VJO_VOCR_LINE_MAX_BYTES) return NULL;
    line_uid = game_memblock_alloc(GL_LINE, VJO_GITEM_LINE, "VjoGameVocrLine", (uint32_t)bytes, LADDER_ALL,
                                   &base, &leaked, &held, &mem);
    if (line_uid < 0) {
        line_uid = leaked;
        return NULL;
    }
    line_bytes_held = held;
    line_base = base;
    return base;
}

/* ---- the model broker ---- */
static int file_read(void *ud, uint64_t off, void *dst, size_t bytes)
{
    (void)ud;
    if (!model_owned || !dst || off > UINT32_MAX || bytes > UINT32_MAX-off || cancelled(NULL)) return -1;
    for (size_t used = 0; used < bytes;) {
        if (cancelled(NULL)) return -1;
        size_t n = bytes-used < VJO_GAME_OCR_MODEL_CHUNK ? bytes-used : VJO_GAME_OCR_MODEL_CHUNK;
        uint64_t started = sceKernelGetProcessTimeWide();
        int got;
        do {
            if (cancelled(NULL)) return -1;
            got = vjoOcrModelRead(model_seq, (uint32_t)(off+used), (uint32_t)n, (unsigned char *)dst+used);
            if (got != VJO_ERR_BUSY) break;
            if (sceKernelGetProcessTimeWide()-started >= MODEL_WAIT_US) { got = VJO_ERR_TIMEOUT; break; }
            sceKernelDelayThread(MODEL_POLL_US);
        } while (1);
        if (got <= 0 || (size_t)got > n) { io_error = got < 0 ? got : -1; return -1; }
        used += (size_t)got;
    }
    return 0;
}

static void file_close(void *ud)
{
    (void)ud;
    if (model_owned) {
        uint64_t started = sceKernelGetProcessTimeWide();
        int rc;
        do {
            /* Cleanup must drain even after cancellation or active_seq reset. */
            rc = vjoOcrModelClose(model_seq);
            if (rc != VJO_ERR_BUSY) break;
            if (sceKernelGetProcessTimeWide()-started >= MODEL_WAIT_US) { rc = VJO_ERR_TIMEOUT; break; }
            sceKernelDelayThread(MODEL_POLL_US);
        } while (1);
        if (rc >= 0) { model_owned = 0; model_seq = 0; }
        else io_error = rc; /* cleanup retries; never acknowledge a leaked descriptor */
    }
}

/* Which of the request's files a path names: an LM by its pinned name (API 20),
 * else the job's model or fallback (open_which). */
static uint32_t which_file(const char *path)
{
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') name = p + 1;
    for (unsigned t = 1; t <= VJO_VOCR_LM_TIERS; t++)
        if (!sceClibStrcmp(name, vjo_vocr_lm_tier(t)->name)) return VJO_GAME_OCR_OPEN_LM + t - 1u;
    if (open_cascade && !sceClibStrcmp(name, VJO_VOCR_CASCADE_NAME)) return VJO_GAME_OCR_OPEN_CASCADE;
    return open_which;
}

static int file_open(void *ud, const char *path, VjoFile *out)
{
    (void)ud;
    if (model_owned || !path || !out || cancelled(NULL)) return -1;
    /* Reserve ownership before enqueue: a timed-out Open may still finish.
     * The kernel resolves only this claimed job's pinned model path. */
    model_seq = active_seq;
    model_owned = 1;
    uint64_t started = sceKernelGetProcessTimeWide();
    VjoGameOcrModelInfo info;
    uint32_t which = which_file(path);
    int size;
    do {
        if (cancelled(NULL)) return -1;
        sceClibMemset(&info, 0, sizeof(info));
        size = vjoOcrModelOpen(model_seq, which, &info);
        if (size != VJO_ERR_BUSY) break;
        if (sceKernelGetProcessTimeWide()-started >= MODEL_WAIT_US) { size = VJO_ERR_TIMEOUT; break; }
        sceKernelDelayThread(MODEL_POLL_US);
    } while (1);
    if (size <= 0) { io_error = size; return -1; }
    /* The kernel's view of the file version: resident weights are reused only
     * while it is known and unchanged (an unknown time means: read again). */
    stat_error = info.stat_rc;
    uint64_t stamp = info.size == sizeof(info) && info.bytes == (uint32_t)size && !info.stat_rc ? info.mtime : 0;
    *out = (VjoFile){NULL, (uint64_t)size, file_read, file_close, stamp};
    return 0;
}

static uint64_t now_us(void *ud)
{
    (void)ud;
    return sceKernelGetProcessTimeWide();
}

/* The game's free USER bytes. A negative figure from the API is the free size
 * under its system margin, i.e. nothing free: 0 (the research's reading of
 * #74's shell_main=-2097152). -1 only when the query itself is unavailable. */
static int32_t free_user(void)
{
    int32_t main_bytes, cdram, phycont;
    if (free_sample(&main_bytes, &cdram, &phycont) < 0) return main_bytes < 0 ? main_bytes : -1;
    return main_bytes < 0 ? 0 : main_bytes;
}

/* USER bytes free after keeping the game reserve, or -1 if unknown. */
static int64_t free_user_after_reserve(void)
{
    int32_t free_bytes = free_user();
    return free_bytes < 0 ? -1 : (int64_t)free_bytes - (int64_t)GAME_RESERVE;
}

/* VjoVocrResident.alloc: one page-rounded USER_RW block for the weights, under
 * the same reserve as the job workspace. */
static void *weights_alloc(void *ud, size_t bytes)
{
    (void)ud;
    uint32_t rounded = PAGE_ROUND(bytes), held, mem;
    void *base = NULL;
    SceUID leaked;
    if (weights_uid >= 0 || !bytes || bytes > VJO_VOCR_F20_BYTES) return NULL; /* never two blocks */
    if (free_user_after_reserve() < (int64_t)rounded) return NULL;
    weights_uid = game_memblock_alloc(GL_WEIGHTS, VJO_GITEM_WEIGHTS, "VjoGameVocrWeights", rounded, LADDER_USER,
                                      &base, &leaked, &held, &mem);
    if (weights_uid < 0) {
        /* A block that cannot be released stays recorded; cleanup retries it. */
        weights_uid = leaked;
        return NULL;
    }
    weights_bytes_held = held;
    return base;
}

static int weights_release(void *ud)
{
    (void)ud;
    if (weights_uid < 0) return 0;
    if (game_memblock_free(GL_WEIGHTS, VJO_GITEM_WEIGHTS, weights_uid, weights_bytes_held) < 0) return -1;
    weights_uid = -1;
    weights_bytes_held = 0;
    return 0;
}

/* Releases the resident weights, and a block whose base lookup failed. */
static int drop_weights(void)
{
    if (vjo_vocr_resident_drop(&resident)) return -1;
    return weights_release(NULL);
}

/* VjoVocrLmResident.alloc: one page-rounded USER_RW block for the LM, under the
 * same reserve. */
static void *lm_alloc(void *ud, size_t bytes)
{
    (void)ud;
    uint32_t rounded = PAGE_ROUND(bytes), held, mem;
    void *base = NULL;
    SceUID leaked;
    if (lm_uid >= 0 || !bytes || bytes > VJO_VOCR_LM_MAX_BYTES) return NULL; /* never two blocks */
    if (free_user_after_reserve() < (int64_t)rounded) return NULL;
    lm_uid = game_memblock_alloc(GL_LM, VJO_GITEM_LM, "VjoGameVocrLM", rounded, LADDER_USER, &base, &leaked, &held,
                                 &mem);
    if (lm_uid < 0) {
        lm_uid = leaked;
        return NULL;
    }
    lm_bytes_held = held;
    return base;
}

static int lm_release(void *ud)
{
    (void)ud;
    if (lm_uid < 0) return 0;
    if (game_memblock_free(GL_LM, VJO_GITEM_LM, lm_uid, lm_bytes_held) < 0) return -1;
    lm_uid = -1;
    lm_bytes_held = 0;
    return 0;
}

/* Releases the resident LM (and a block whose base lookup failed). */
static int drop_lm(void)
{
    if (vjo_vocr_lm_drop(&lm_res)) return -1;
    return lm_release(NULL);
}

/* VjoVocrResident.alloc of the cascade's model: one page-rounded USER_RW block,
 * under the same reserve. */
static void *cascade_alloc(void *ud, size_t bytes)
{
    (void)ud;
    uint32_t rounded = PAGE_ROUND(bytes), held, mem;
    void *base = NULL;
    SceUID leaked;
    if (cascade_uid >= 0 || !bytes || bytes > VJO_VOCR_CASCADE_BYTES) return NULL; /* never two blocks */
    if (free_user_after_reserve() < (int64_t)rounded) return NULL;
    cascade_uid = game_memblock_alloc(GL_CASCADE, VJO_GITEM_CASCADE, "VjoGameVocrCascade", rounded, LADDER_USER,
                                      &base, &leaked, &held, &mem);
    if (cascade_uid < 0) {
        cascade_uid = leaked;
        return NULL;
    }
    cascade_bytes_held = held;
    return base;
}

static int cascade_release(void *ud)
{
    (void)ud;
    if (cascade_uid < 0) return 0;
    if (game_memblock_free(GL_CASCADE, VJO_GITEM_CASCADE, cascade_uid, cascade_bytes_held) < 0) return -1;
    cascade_uid = -1;
    cascade_bytes_held = 0;
    return 0;
}

static int drop_cascade(void)
{
    if (vjo_vocr_resident_drop(&cascade_res)) return -1;
    return cascade_release(NULL);
}

/* Everything kept between jobs: the weights, the LM and the cascade's model. */
static int drop_resident(void)
{
    int rc = drop_weights();
    rc |= drop_lm();
    return drop_cascade() || rc ? -1 : 0;
}

static uint32_t cascade_resident_bytes(void)
{
    return cascade_res.model && cascade_uid >= 0 ? PAGE_ROUND(cascade_res.model->bytes) : 0;
}

static uint32_t lm_resident_bytes(void)
{
    return lm_res.lm && lm_uid >= 0 ? PAGE_ROUND(lm_res.lm->bytes) : 0;
}

static uint32_t resident_bytes(void)
{
    return resident.model && weights_uid >= 0 ? PAGE_ROUND(resident.model->bytes) : 0;
}

/* Resident blocks with verified contents: no job frees them but on a path that
 * drops them before its recognition, so their FREE steps need no slot. */
static unsigned blocks_kept(void)
{
    return (resident.model && weights_uid >= 0) + (lm_res.lm && lm_uid >= 0) + (cascade_res.model && cascade_uid >= 0);
}

/* The job workspace's bytes after the model's own, with the LM (lm) and the
 * cascade's larger model (casc): the top-K extras and the greedy text with
 * either, the cascade's arena and decoder with it. */
static size_t extra_job_bytes(const VjoVocrModel *m, const VjoVocrLm *lm, const VjoVocrModel *casc)
{
    if (!lm && !casc) return 0;
    return vjo_vocr_lm_job_bytes(m->name) + (casc ? vjo_vocr_cascade_job_bytes(casc->name) : 0) +
           VJO_GAME_OCR_TEXT_BYTES;
}

/* The memory check for model m (with LM lm and the cascade's model casc, each
 * or NULL) and a job of job_bytes plus the capture block: records what the job
 * needs in out (its page-rounded workspace and capture block; the weights, the
 * LM and the cascade's model unless they are resident; resident ones of another
 * model or tier, or not used, it would release first) and returns whether that
 * fits beside the reserve in out->free_user. */
static int check_memory(VjoGameOcrResult *out, const VjoVocrModel *m, size_t job_bytes, size_t capture_bytes,
                        const VjoVocrLm *lm, const VjoVocrModel *casc)
{
    out->need_workspace = PAGE_ROUND(job_bytes + extra_job_bytes(m, lm, casc)) + PAGE_ROUND(capture_bytes);
    out->need_weights = (resident.model == m ? 0 : PAGE_ROUND(m->bytes)) +
                        (!lm || lm_res.lm == lm ? 0 : PAGE_ROUND(lm->bytes)) +
                        (!casc || cascade_res.model == casc ? 0 : PAGE_ROUND(casc->bytes));
    out->release_bytes = (resident.model && resident.model != m ? resident_bytes() : 0) +
                         (lm_res.lm && lm_res.lm != lm ? lm_resident_bytes() : 0) +
                         (cascade_res.model && cascade_res.model != casc ? cascade_resident_bytes() : 0);
    return out->free_user >= 0 &&
           (int64_t)out->free_user + out->release_bytes - (int64_t)GAME_RESERVE >=
               (int64_t)out->need_workspace + out->need_weights;
}

/* The check with the largest LM tier up to max that fits beside model m and the
 * cascade's casc (*lm; NULL when none does, or max is 0: then the check
 * without one). */
static int check_lm_tiers(VjoGameOcrResult *out, const VjoVocrModel *m, size_t job_bytes, size_t capture_bytes,
                          unsigned max, const VjoVocrModel *casc, const VjoVocrLm **lm)
{
    for (unsigned t = max; t >= 1; t--) {
        const VjoVocrLm *c = vjo_vocr_lm_tier(t);
        if (c && check_memory(out, m, job_bytes, capture_bytes, c, casc)) {
            *lm = c;
            return 1;
        }
    }
    *lm = NULL;
    return check_memory(out, m, job_bytes, capture_bytes, NULL, casc);
}

/* What fits beside model m, in the order of what each brings on the dialogue
 * lines measured (docs/vita-vn-ocr.md): the cascade with the largest LM tier
 * that fits, else the largest LM alone, else the cascade alone, else neither.
 * *casc is set to NULL when the cascade does not fit. */
static int check_with_lm(VjoGameOcrResult *out, const VjoVocrModel *m, size_t job_bytes, size_t capture_bytes,
                         unsigned max, const VjoVocrModel **casc, const VjoVocrLm **lm)
{
    if (*casc && *casc != m && check_lm_tiers(out, m, job_bytes, capture_bytes, max, *casc, lm) && (*lm || !max))
        return 1;
    if (check_lm_tiers(out, m, job_bytes, capture_bytes, max, NULL, lm) && (*lm || !max || !*casc)) {
        *casc = NULL;
        return 1;
    }
    if (*casc && *casc != m && check_memory(out, m, job_bytes, capture_bytes, NULL, *casc)) {
        *lm = NULL;
        return 1;
    }
    *casc = NULL;
    *lm = NULL;
    return check_memory(out, m, job_bytes, capture_bytes, NULL, NULL);
}

static int cleanup(void)
{
    int rc = 0;
    if (workspace_uid >= 0) {
        if (game_memblock_free(GL_WORKSPACE, VJO_GITEM_WORKSPACE, workspace_uid, workspace_bytes_held) < 0)
            rc = VJO_E_OCR_UNAVAILABLE;
        else { workspace_uid = -1; workspace_bytes_held = 0; workspace_memory = 0; }
    }
    /* A capture block the display hook may still write into (a rows call that
     * timed out on a copy in progress) is never freed; one whose free failed is
     * freed again here. */
    if (capture_uid >= 0 && !hook_owns_capture) {
        if (game_memblock_free(GL_CAPTURE, VJO_GITEM_CAPTURE, capture_uid, capture_bytes_held) < 0)
            rc = VJO_E_OCR_UNAVAILABLE;
        else { capture_uid = -1; capture_bytes_held = 0; capture_memory = 0; }
    }
    if (line_uid >= 0 && !hook_owns_capture) { /* rows calls write tall lines here too */
        if (game_memblock_free(GL_LINE, VJO_GITEM_LINE, line_uid, line_bytes_held) < 0) rc = VJO_E_OCR_UNAVAILABLE;
        else { line_uid = -1; line_bytes_held = 0; line_base = NULL; }
    }
    file_close(NULL);
    if (model_owned) rc = VJO_E_OCR_UNAVAILABLE;
    /* Between jobs a weights (or LM) block holds verified bytes or nothing. */
    if (!resident.model && (resident.data || weights_uid >= 0) && drop_weights()) rc = VJO_E_OCR_UNAVAILABLE;
    if (!lm_res.lm && (lm_res.data || lm_uid >= 0) && drop_lm()) rc = VJO_E_OCR_UNAVAILABLE;
    if (!cascade_res.model && (cascade_res.data || cascade_uid >= 0) && drop_cascade()) rc = VJO_E_OCR_UNAVAILABLE;
    return rc;
}

/* ---- the self-test's probe (VJO_GAME_OCR_F_PROBE): every rung, one strip each ---- */
static uint32_t grey_sum(const unsigned char *p, unsigned w, unsigned n, size_t stride)
{
    uint32_t h = VJO_FNV_INIT;
    for (unsigned r = 0; r < n; r++) {
        uint32_t row = vjo_fnv1a(VJO_FNV_INIT, p + (size_t)r * stride, w);
        h = vjo_fnv1a(h, (const unsigned char *)&row, 4);
    }
    return h;
}

/* sceDisplayGetFrameBuf's frame read by this thread, as VITA2PC does
 * (psvita/main.c:146-161): rows [0, n) of the region into dst, grey. The frame's
 * memory is checked to be a readable block of this process first, so a frame
 * the game freed is refused instead of read. Probe only. */
static int user_frame_rows(const VjoGameOcrRequest *r, unsigned n, unsigned char *dst, uint32_t *fmt_out)
{
    SceDisplayFrameBufExt fb; /* 3.60 writes 0x1C bytes even for a 0x18-byte size (vita-headers) */
    SceKernelMemBlockInfo mb;
    uint32_t bpp, span;
    uintptr_t first;
    int rc;
    if (!IMPORT_READY(sceDisplayGetFrameBuf) || !IMPORT_READY(sceKernelGetMemBlockInfoByAddr)) return -1;
    sceClibMemset(&fb, 0, sizeof(fb));
    fb.size = sizeof(SceDisplayFrameBuf);
    rc = sceDisplayGetFrameBuf((SceDisplayFrameBuf *)&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    step_add(VJO_GSTEP_FB, VJO_GAME_OCR_SRC_USER, fb.width, rc, fb.pitch, fb.pixelformat, 0, 0);
    if (rc < 0) return rc;
    *fmt_out = fb.pixelformat;
    bpp = vjo_fb_bpp(fb.pixelformat);
    if (!fb.base || !bpp) return VJO_ERR_FORMAT;
    if (fb.width != r->fb_width || fb.height != r->fb_height || fb.pitch < fb.width) return VJO_ERR_FORMAT;
    first = (uintptr_t)fb.base + ((size_t)r->y * fb.pitch + r->x) * bpp;
    span = ((n - 1u) * fb.pitch + r->width) * bpp;
    sceClibMemset(&mb, 0, sizeof(mb));
    mb.size = sizeof(mb);
    rc = sceKernelGetMemBlockInfoByAddr(fb.base, &mb);
    if (rc < 0) return rc;
    if (!(mb.access & SCE_KERNEL_MEMORY_ACCESS_R) || first < (uintptr_t)mb.mappedBase ||
        first + span > (uintptr_t)mb.mappedBase + mb.mappedSize)
        return VJO_ERR_ARG;
    for (unsigned row = 0; row < n; row++) {
        const unsigned char *src = (const unsigned char *)first + (size_t)row * fb.pitch * bpp;
        uint32_t abgr[64];
        for (unsigned x = 0; x < r->width; x += 64) {
            unsigned k = r->width - x < 64 ? r->width - x : 64;
            vjo_fb_to_abgr(abgr, src + (size_t)x * bpp, k, fb.pixelformat);
            vjo_abgr_to_grey(dst + (size_t)row * r->width + x, abgr, k);
        }
    }
    return (int)n;
}

static int run_probe(const VjoGameOcrRequest *r, VjoGameOcrResult *out)
{
    unsigned rows = vjo_vocr_strip_rows(r->width, r->height);
    uint32_t bytes = rows * r->width;
    Capture c = {r, VJO_GAME_OCR_SRC_HOOK, {0}, 0, 0, 0, 0, 1, 0};
    int rc = VJO_OK, any = 0;
    if (!rows) return VJO_E_OCR_REGION;
    for (unsigned i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
        void *base = NULL;
        SceUID uid, leaked;
        uint32_t held, mem;
        if (cancelled(NULL)) return VJO_E_CANCELLED;
        uid = game_memblock_alloc(GL_CAPTURE, VJO_GITEM_PROBE, "VjoGameVocrProbe", bytes, 1u << i, &base, &leaked,
                                  &held, &mem);
        if (uid < 0) {
            if (leaked >= 0) { capture_uid = leaked; capture_bytes_held = held; blocked = 1; return VJO_E_OCR_UNAVAILABLE; }
            continue;
        }
        capture_uid = uid;
        capture_bytes_held = held;
        for (uint32_t mode = VJO_GAME_OCR_SRC_HOOK; mode <= VJO_GAME_OCR_SRC_KSTRIP; mode++) {
            int got;
            sceClibMemset(base, 0, bytes);
            c.mode = mode;
            got = capture_rows(&c, 0, rows, base, r->width, i == 0 && mode == VJO_GAME_OCR_SRC_HOOK
                                                              ? VJO_STRIP_FIRST | (rows == r->height ? VJO_STRIP_LAST : 0u) : 0u);
            step_add(VJO_GSTEP_PROBE, mode, mem, got, got > 0 ? bytes : 0, got > 0 ? grey_sum(base, r->width, rows, r->width) : 0,
                     c.last.wait_us, 0);
            if (got > 0) {
                any = 1;
                out->fb_width = c.last.fb_width; out->fb_height = c.last.fb_height;
                out->fb_pitch = c.last.fb_pitch; out->fb_format = c.last.fb_format;
            }
            if (blocked) return VJO_E_OCR_UNAVAILABLE;
        }
        if (i == 0) {
            /* The user-mode read, into the same USER_RW block. */
            uint32_t fmt = 0;
            uint64_t t0 = now_wide();
            int got;
            sceClibMemset(base, 0, bytes);
            got = user_frame_rows(r, rows, base, &fmt);
            step_add(VJO_GSTEP_PROBE, VJO_GAME_OCR_SRC_USER, mem, got, got > 0 ? bytes : 0,
                     got > 0 ? grey_sum(base, r->width, rows, r->width) : 0, (uint32_t)(now_wide() - t0), 0);
        }
        if (game_memblock_free(GL_CAPTURE, VJO_GITEM_PROBE, capture_uid, capture_bytes_held) < 0) {
            blocked = 1;
            return VJO_E_OCR_UNAVAILABLE;
        }
        capture_uid = -1;
        capture_bytes_held = 0;
    }
    out->strip_rows = rows;
    return any ? rc : VJO_E_SOURCE;
}

/* The job's policy, from its request's flags (include/vjo_game_ocr.h, API 20):
 * the stable-text wait and its limit, whether to wait first, blank lines. */
static void policy_sleep(uint32_t us)
{
    sceKernelDelayThread(us);
}

static void request_policy(const VjoGameOcrRequest *r, VjoVocrPolicy *p)
{
    sceClibMemset(p, 0, sizeof(*p));
    p->settle_us = ((r->flags >> VJO_GAME_OCR_SETTLE_SHIFT) & 0xFFu) * VJO_GAME_OCR_SETTLE_UNIT_US;
    p->settle_max_us = (r->flags >> VJO_GAME_OCR_SETTLE_MAX_SHIFT) * VJO_GAME_OCR_SETTLE_MAX_UNIT_US;
    p->settle_first = (r->flags & VJO_GAME_OCR_F_SETTLE_FIRST) != 0;
    p->blank = (r->flags & VJO_GAME_OCR_F_BLANK) != 0;
    p->sleep = policy_sleep;
}

int vjo_game_ocr_run_request(const VjoGameOcrRequest *r, VjoGameOcrResult *out)
{
    if (!r || !out) return VJO_E_SOURCE;
    if (!g_game_ledger.defs) ledger_init();
    sceClibMemset(out, 0, sizeof(*out));
    out->size = sizeof(*out); out->seq = r->seq;
    active_seq = r->seq; io_error = 0; stat_error = 0; open_which = VJO_GAME_OCR_OPEN_MODEL; open_cascade = 0;
    log_to = out;
    job_started_us = now_wide();
    int rc = VJO_E_SOURCE;
    VjoVocrStats stats = {0};
    Capture capture = {r, VJO_GAME_OCR_SRC_HOOK, {0}, 0, 0, 0, 0, 0, 0};
    if (blocked || workspace_uid >= 0 || capture_uid >= 0 || model_owned) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
    /* The refinement's mode (bits 12-13): 3 is refused here, as unknown bits are. */
    unsigned refine_mode = (r->flags >> VJO_GAME_OCR_REFINE_SHIFT) & VJO_GAME_OCR_REFINE_MASK;
    if (r->size != sizeof(*r) || !r->seq || !r->capture_seq || (r->flags & ~VJO_GAME_OCR_F_ALL) ||
        refine_mode > VJO_GAME_OCR_REFINE_PSP ||
        !r->width || r->width > VJO_OCR_MAX_WIDTH || !r->height || r->height > VJO_OCR_MAX_HEIGHT ||
        !r->fb_width || r->fb_width > VJO_OCR_MAX_WIDTH || !r->fb_height || r->fb_height > VJO_OCR_MAX_HEIGHT ||
        r->x > r->fb_width - r->width || r->y > r->fb_height - r->height ||
        !r->model_dir[0] || !r->model[0] ||
        r->model_dir[sizeof(r->model_dir)-1] || r->model[sizeof(r->model)-1] ||
        r->fallback[sizeof(r->fallback)-1]) goto done;
    out->region_x = r->x; out->region_y = r->y; out->width = r->width; out->height = r->height;
    out->fb_width = r->fb_width; out->fb_height = r->fb_height;
    if (cancelled(NULL)) { rc = VJO_E_CANCELLED; goto done; }
    step_add(VJO_GSTEP_MEM, 0, 0, 0, 0, 0, 0, 1);
    if (r->flags & VJO_GAME_OCR_F_PROBE) { rc = run_probe(r, out); goto done; }
    const VjoVocrModel *model = vjo_vocr_model(r->model);
    /* The finder's boxes refined (API 20): the mode, here or in a PSP game. */
    unsigned refine = refine_mode == VJO_GAME_OCR_REFINE_ON || (refine_mode == VJO_GAME_OCR_REFINE_PSP && psp_title)
                      ? VJO_VOCR_REFINE_ALL : 0u;
    size_t refine_bytes = refine ? vjo_vocr_refine_bytes(r->width, r->height) : 0;
    size_t bytes = vjo_vocr_job_bytes(r->model, r->width, r->height);
    size_t capture_bytes = vjo_vocr_capture_bytes(r->width, r->height);
    if (!bytes || !capture_bytes) { rc = model ? VJO_E_OCR_REGION : VJO_E_OCR_MODEL; goto done; }
    bytes += refine_bytes;
    /* A fallback is used only if it is another pinned model needing less memory. */
    const VjoVocrModel *fallback = r->fallback[0] ? vjo_vocr_model(r->fallback) : NULL;
    size_t fallback_bytes = fallback ? vjo_vocr_job_bytes(r->fallback, r->width, r->height) : 0;
    if (fallback_bytes) fallback_bytes += refine_bytes;
    if (!fallback_bytes || fallback == model ||
        PAGE_ROUND(fallback_bytes) + PAGE_ROUND(fallback->bytes) >= PAGE_ROUND(bytes) + PAGE_ROUND(model->bytes))
        fallback = NULL;
    /* Weights of a model this job cannot use: useless now, and two are never held. */
    if (resident.model && resident.model != model && resident.model != fallback && drop_weights()) {
        rc = VJO_E_OCR_UNAVAILABLE; goto done;
    }
    /* The LM: the largest tier the request allows that fits (API 20); none
     * allowed, and an LM still resident from an earlier setting goes. */
    unsigned lm_max = (r->flags >> VJO_GAME_OCR_LM_SHIFT) & VJO_GAME_OCR_LM_MASK;
    const VjoVocrLm *lm = NULL;
    if (lm_max > VJO_VOCR_LM_TIERS) lm_max = VJO_VOCR_LM_TIERS;
    if (!lm_max && (lm_res.data || lm_uid >= 0) && drop_lm()) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
    /* The confidence cascade (API 20): the request's time ceiling, and the larger
     * model beside a smaller one only; none asked for, and a resident one goes. */
    uint32_t casc_us = ((r->flags >> VJO_GAME_OCR_CASCADE_SHIFT) & VJO_GAME_OCR_CASCADE_MASK) *
                       VJO_GAME_OCR_CASCADE_UNIT_US;
    const VjoVocrModel *casc_model = casc_us ? vjo_vocr_model(VJO_VOCR_CASCADE_NAME) : NULL;
    const VjoVocrModel *casc = casc_model && casc_model->bytes > model->bytes ? casc_model : NULL;
    if (!casc_us && (cascade_res.data || cascade_uid >= 0) && drop_cascade()) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
    /* Reported also when refused: each title's real free memory. */
    out->free_user = free_user();
    out->reserve_bytes = GAME_RESERVE;
    const VjoVocrModel *use = model;
    size_t use_bytes = bytes;
    int fits = check_with_lm(out, model, bytes, capture_bytes, lm_max, &casc, &lm);
    step_add(VJO_GSTEP_CHECK, (lm ? lm->tier : 0u) | (casc ? 8u : 0u), 0, fits ? 0 : 1,
             out->need_workspace + out->need_weights, 0, 0, 0);
    /* The self-test runs the fallback on purpose (its FL10 stage). */
    int forced = (r->flags & VJO_GAME_OCR_F_FORCE_FALLBACK) && fallback;
    if ((!fits || forced) && fallback) {
        /* The configured model does not fit this game now; the smaller one may. */
        out->fallback = 1;
        out->model_need_workspace = out->need_workspace;
        out->model_need_weights = out->need_weights;
        out->model_release_bytes = out->release_bytes;
        use = fallback;
        use_bytes = fallback_bytes;
        casc = casc_model && casc_model->bytes > fallback->bytes ? casc_model : NULL;
        fits = check_with_lm(out, fallback, fallback_bytes, capture_bytes, lm_max, &casc, &lm);
        /* The fallback's check (arg 1) is the record that the fallback runs, and its sum
         * says whether the self-test forced it: no FALLBACK step, so the first job of a
         * PSP game (the refinement's step besides the LM's and the cascade's) fits the log. */
        step_add(VJO_GSTEP_CHECK, (lm ? lm->tier : 0u) | (casc ? 8u : 0u), 1, fits ? 0 : 1,
                 out->need_workspace + out->need_weights, forced ? 1u : 0u, 0, 0);
    }
    stats.workspace = use_bytes;
    if (!fits) {
        /* Too little memory: give the game back the resident weights and LM too. */
        if (drop_resident()) rc = VJO_E_OCR_UNAVAILABLE;
        else rc = VJO_E_OOM;
        goto done;
    }
    /* The check counted another model's weights, or another LM (or one this job
     * has no room for), as free: release them. */
    if (resident.model && resident.model != use && drop_weights()) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
    if ((lm_res.data || lm_uid >= 0) && lm_res.lm != lm && drop_lm()) { rc = VJO_E_OCR_UNAVAILABLE; goto done; }
    if ((cascade_res.data || cascade_uid >= 0) && cascade_res.model != casc && drop_cascade()) {
        rc = VJO_E_OCR_UNAVAILABLE; goto done;
    }
    size_t lm_extra = extra_job_bytes(use, lm, casc);
    /* the core's extras end where the greedy text kept for the log begins */
    size_t core_extra = lm_extra ? lm_extra - VJO_GAME_OCR_TEXT_BYTES : 0;
    void *capture_base = NULL, *base = NULL;
    SceUID leaked;
    uint32_t held, mem;
    capture_uid = game_memblock_alloc(GL_CAPTURE, VJO_GITEM_CAPTURE, "VjoGameVocrCapture", (uint32_t)capture_bytes,
                                      LADDER_ALL, &capture_base, &leaked, &held, &mem);
    if (capture_uid < 0) { capture_uid = leaked; rc = VJO_E_OOM; goto done; }
    capture_bytes_held = held;
    capture_memory = mem;
    workspace_uid = game_memblock_alloc(GL_WORKSPACE, VJO_GITEM_WORKSPACE, "VjoGameVocr",
                                        PAGE_ROUND(use_bytes + lm_extra), LADDER_ALL, &base, &leaked, &held, &mem);
    if (workspace_uid < 0) { workspace_uid = leaked; rc = VJO_E_OOM; goto done; }
    workspace_bytes_held = held;
    workspace_memory = mem;
    VjoPlatform platform = {.file_open=file_open, .now_us=now_us};
    VjoGreySource image = {&capture, r->width, r->height, strip_source, cancelled, line_buffer};
    VjoVocrPolicy policy;
    request_policy(r, &policy);
    if (lm_extra) { /* the LM (and cascade) extras and then the greedy text follow the job's workspace */
        policy.raw = (char *)base + use_bytes + core_extra;
        policy.raw_cap = VJO_GAME_OCR_TEXT_BYTES;
    }
    if (lm) {
        policy.lm = lm;
        policy.lm_res = &lm_res;
    }
    if (casc) {
        policy.cascade = casc;
        policy.cascade_res = &cascade_res;
        policy.cascade_us = casc_us;
        policy.cascade_conf = VJO_GAME_CASCADE_CONF;
    }
    policy.refine = refine;
    open_which = use == model ? VJO_GAME_OCR_OPEN_MODEL : VJO_GAME_OCR_OPEN_FALLBACK;
    open_cascade = casc != NULL;
    rc = vjo_vocr_ocr_policy(&platform, r->model_dir, use->name, &image, &resident, base, use_bytes + core_extra,
                             capture_base, capture_bytes_held, &policy, out->text, sizeof(out->text), &stats);
    /* A native capture error (the kernel's VJO_ERR_*, an SDK code) ends the job
     * as a source failure; its exact code is in the step log (VJO_GSTEP_PASS). */
    if (rc < -199) rc = VJO_E_SOURCE;
    if (stats.tears) step_add(VJO_GSTEP_TEAR, 0, stats.tears, (int32_t)stats.tries, 0, 0, 0, 0);
    if (capture.failed) step_add(VJO_GSTEP_PASS, capture.mode, capture.pass_strips, capture.failed, capture.pass_bytes,
                                 capture.last.fb_format, capture.pass_us, 0);
    step_add(VJO_GSTEP_FIND, 0, stats.lines, rc == VJO_E_OCR_REGION ? rc : 0, 0, stats.dark, stats.find_us, 0);
    if (policy.settle_us)
        step_add(VJO_GSTEP_SETTLE, 0, stats.settle_passes,
                 stats.settled ? VJO_GSETTLE_HELD : stats.settle_timeout ? VJO_GSETTLE_TIMEOUT : VJO_GSETTLE_NONE,
                 stats.settle_changes, (stats.cycling_rows > 0xFFFFu ? 0xFFFFu : stats.cycling_rows) << 16 |
                 (stats.cycles > 0xFFFFu ? 0xFFFFu : stats.cycles), stats.settle_us ? stats.settle_us : 1u, 0);
    for (unsigned i = 0; i < stats.blank && i < VJO_VOCR_BLANK_LOGGED; i++) {
        const VjoVocrBlankStat *b = &stats.blank_line[i];
        step_add(VJO_GSTEP_BLANK, 0, b->index, (int32_t)stats.blank, (uint32_t)b->width << 16 | b->height,
                 (uint32_t)(b->ink.hi - b->ink.lo) << 16 | b->ink.edges, stats.blank_us ? stats.blank_us : 1u, 0);
    }
    if (refine && stats.lines) /* each count at most VJO_VOCR_MAX_LINES: the boxes of one try */
        step_add(VJO_GSTEP_REFINE, refine_mode, stats.refine_lines, stats.refine_rc,
                 stats.refine_boxes << 16 | (stats.refine_capped & 0xFFu) << 8 | (stats.refine_split & 0xFFu),
                 (uint32_t)stats.refine_rows, stats.refine_us + 1u, 0);
    if (stats.model_loaded || stats.model_cached)
        step_add(VJO_GSTEP_LOAD, stats.model_loaded ? VJO_GAME_OCR_WEIGHTS_READ : VJO_GAME_OCR_WEIGHTS_RESIDENT, 0, 0,
                 resident_bytes(), 0, stats.load_us, 1);
    if (stats.lines)
        step_add(VJO_GSTEP_RECOGNIZE, 0, stats.recognized, rc, stats.frames, stats.tall_skipped, stats.recognize_us, 0);
    if (lm && stats.lines) {
        unsigned low = 0;
        for (unsigned i = 0; i < stats.recognized && i < VJO_VOCR_MAX_LINES; i++)
            low += (stats.line[i].flags & VJO_VOCR_LINE_CONF) < VJO_GAME_OCR_LOW_CONF;
        step_add(VJO_GSTEP_LM, stats.lm ? stats.lm->tier : lm->tier, stats.lm_changed, stats.lm_rc,
                 (stats.lm_loaded ? VJO_GLM_READ : stats.lm_cached ? VJO_GLM_RESIDENT : 0u) | low << 16 | stats.lm_lines,
                 stats.lm_load_us, stats.lm_us + 1u, 0);
    }
    if (casc && stats.lines) cascade_step(&stats);
    if (!rc) {
        /* What is shown and looked up is normalised; the recognizer's own (greedy)
         * text follows it in the same buffer, for the Shell's log. An empty text too:
         * this zeroes what an earlier try of the job left after its NUL (a try
         * that read a line, then a retry that found the box empty), which the
         * Shell would otherwise log as the recognizer's text. */
        VjoTextNorm k;
        uint64_t t0 = now_wide();
        unsigned changes = vjo_text_normalize_keep(out->text, sizeof(out->text), lm_extra ? policy.raw : NULL, &k);
#define CLAMP8(v) ((v) > 255u ? 255u : (v))
        if (out->text[0])
            step_add(VJO_GSTEP_NORM, 0, changes, 0, k.width,
                     CLAMP8(k.tilde) << 24 | CLAMP8(k.dash) << 16 | CLAMP8(k.ellipsis) << 8 |
                     (k.long_vowel > 15u ? 15u : k.long_vowel) << 4 | (k.kanji_one > 15u ? 15u : k.kanji_one),
                     (uint32_t)(now_wide() - t0) + 1u, 0);
#undef CLAMP8
    }
done:
    out->workspace_bytes = (uint32_t)stats.workspace;
    out->lines = stats.lines; out->find_us = stats.find_us;
    out->load_us = stats.load_us; out->recognize_us = stats.recognize_us;
    out->weights = stats.model_loaded ? VJO_GAME_OCR_WEIGHTS_READ :
                   stats.model_cached ? VJO_GAME_OCR_WEIGHTS_RESIDENT : VJO_GAME_OCR_WEIGHTS_NONE;
    out->frames = stats.frames;
    out->recognized = stats.recognized < VJO_GAME_OCR_LINES ? stats.recognized : VJO_GAME_OCR_LINES;
    for (uint32_t i = 0; i < out->recognized; i++) {
        out->line[i].width = stats.line[i].width;
        out->line[i].height = stats.line[i].height;
        out->line[i].frames = stats.line[i].frames;
        out->line[i].flags = stats.line[i].flags;
        out->line[i].us = stats.line[i].us;
    }
    if (!(r && (r->flags & VJO_GAME_OCR_F_PROBE))) {
        out->source = capture.pass_strips ? capture.mode : VJO_GAME_OCR_SRC_NONE;
        out->strip_rows = stats.strip_rows;
        out->captures = stats.strips;
        out->tears = stats.tears;
        out->capture_us = stats.capture_us;
        out->region_sum = stats.region_sum;
        if (capture.last.fb_width) {
            out->fb_width = capture.last.fb_width; out->fb_height = capture.last.fb_height;
            out->fb_pitch = capture.last.fb_pitch; out->fb_format = capture.last.fb_format;
        }
    }
    out->capture_memory = capture_memory;
    out->capture_bytes = capture_bytes_held;
    out->cleanup_status = cleanup();
    out->resident_bytes = resident_bytes();
    out->io_error = io_error;
    out->stat_error = stat_error;
    if (out->cleanup_status) { blocked = 1; rc = VJO_E_OCR_UNAVAILABLE; }
    else if (cancelled(NULL)) rc = VJO_E_CANCELLED;
    if (rc) sceClibMemset(out->text, 0, sizeof(out->text));
    out->rc = rc; active_seq = 0;
    log_to = NULL;
    return rc;
}

/* A vjoOcrTake the kernel held for at least this long, waiting for a job
 * (kernel/game_ocr.c TAKE_WAIT_US, 100 ms), is followed by the next round at
 * once, whatever it answered (none came, or the game stopped being the active
 * one meanwhile: the next Register sees it). One that answers sooner (a job,
 * an error, a kernel that does not wait) is followed by the worker's own
 * 100 ms sleep, as before, so the loop never spins. */
#define TAKE_WAITED_US 50000u

static int worker_main(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (!__atomic_load_n(&quitting, __ATOMIC_ACQUIRE) && !blocked) {
        int nap = 1;
        if (claim_outstanding) {
            if (!vjoOcrComplete(&pending_result) && !pending_result.cleanup_status) claim_outstanding = 0;
        } else {
            int registered = vjoOcrRegister();
            if (!registered) {
                static VjoGameOcrRequest request; /* not on the 32 KiB stack */
                uint64_t asked = now_wide();
                int took;
                sceClibMemset(&request, 0, sizeof(request));
                request.size = sizeof(request);
                took = vjoOcrTake(&request);
                if (!took) {
                    claim_outstanding = 1;
                    vjo_game_ocr_run_request(&request, &pending_result);
                    if (!vjoOcrComplete(&pending_result) && !pending_result.cleanup_status) claim_outstanding = 0;
                    nap = claim_outstanding; /* done: the next job may be queued already */
                } else if (now_wide() - asked >= TAKE_WAITED_US) {
                    nap = 0; /* the kernel held the take: its wait was the pace */
                }
            } else if (registered != VJO_ERR_BUSY &&
                       (resident.data || weights_uid >= 0 || lm_res.data || lm_uid >= 0 || cascade_res.data ||
                        cascade_uid >= 0) &&
                       drop_resident()) {
                /* Not the active game (suspended, behind another app, or its
                 * title disabled): the weights go back to it. */
                blocked = 1;
            }
        }
        if (nap) sceKernelDelayThread(claim_outstanding ? 50000 : 100000);
    }
    return 0;
}

int vjo_game_ocr_worker_start(void)
{
    if (worker >= 0 || blocked) return VJO_E_OCR_UNAVAILABLE;
    if (!IMPORT_READY(vjoGetVersion) || !IMPORT_READY(vjoOcrRegister) || !IMPORT_READY(vjoOcrTake) ||
        !IMPORT_READY(vjoOcrCancelled) || !IMPORT_READY(vjoOcrCaptureRows) || !IMPORT_READY(vjoOcrComplete) ||
        !IMPORT_READY(vjoOcrModelOpen) || !IMPORT_READY(vjoOcrModelRead) || !IMPORT_READY(vjoOcrModelClose) ||
        !IMPORT_READY(sceKernelAllocMemBlock) || !IMPORT_READY(sceKernelGetMemBlockBase) ||
        !IMPORT_READY(sceKernelFreeMemBlock) || vjoGetVersion() != VJO_API_VERSION) return VJO_E_OCR_UNAVAILABLE;
    __atomic_store_n(&quitting, 0, __ATOMIC_RELEASE);
    if (!g_game_ledger.defs) ledger_init();
    /* The scheduler is fixed-priority, not time-sharing: a recognition
     * (hundreds of ms per line) at the default priority would make a game
     * thread of the same priority wait for a core. At the lowest user
     * priority every game thread preempts this one [SDK: Kernel-Overview,
     * 'Notes on Priority and CPU Affinity Mask Combinations']. */
    worker = sceKernelCreateThread("VjoGameVocr", worker_main, WORKER_PRIORITY, WORKER_STACK_BYTES, 0,
                                   WORKER_AFFINITY, NULL);
    if (worker < 0) return VJO_E_OCR_UNAVAILABLE;
    vjo_ledger_add(&g_game_ledger, GL_STACK, VJO_BUDGET_GAME_STACK);
    if (sceKernelStartThread(worker, 0, NULL) < 0) {
        if (sceKernelDeleteThread(worker) >= 0) { worker = -1; vjo_ledger_sub(&g_game_ledger, GL_STACK, VJO_BUDGET_GAME_STACK); }
        return VJO_E_OCR_UNAVAILABLE;
    }
    worker_started = 1;
    return VJO_OK;
}

int vjo_game_ocr_worker_stop(void)
{
    __atomic_store_n(&quitting, 1, __ATOMIC_RELEASE);
    if (worker >= 0) {
        SceUInt timeout = JOIN_US;
        if (worker_started && sceKernelWaitThreadEnd(worker, NULL, &timeout) < 0) return VJO_E_OCR_UNAVAILABLE;
        worker_started = 0;
        if (sceKernelDeleteThread(worker) < 0) return VJO_E_OCR_UNAVAILABLE;
        worker = -1;
        vjo_ledger_sub(&g_game_ledger, GL_STACK, VJO_BUDGET_GAME_STACK);
    }
    if (cleanup()) return VJO_E_OCR_UNAVAILABLE;
    /* The game is exiting or the plugin stops: no weights or LM stay behind. */
    if (drop_resident()) return VJO_E_OCR_UNAVAILABLE;
    if (claim_outstanding) {
        pending_result.cleanup_status = 0; pending_result.rc = VJO_E_CANCELLED;
        sceClibMemset(pending_result.text, 0, sizeof(pending_result.text));
        if (vjoOcrComplete(&pending_result)) return VJO_E_OCR_UNAVAILABLE;
        claim_outstanding = 0;
    }
    blocked = 0;
    return VJO_OK;
}

#ifndef VJO_GAME_OCR_NO_MODULE_ENTRY
/* The boot guard's verdict for this boot (core/boot_guard.h): in a safe boot
 * the kernel has no hooks and nothing of ours may stay in any process. An
 * older kernel without the syscall means a normal boot. */
static int safe_boot(void)
{
    VjoBootInfo info = {.size=sizeof(info)};
    if (!IMPORT_READY(vjoBootInfo)) return 0;
    return vjoBootInfo(&info) == 0 && info.size == sizeof(info) && info.mode == VJO_BOOT_SAFE;
}

int _start(SceSize args, void *argp) __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp)
{
    (void)args; (void)argp;
    char title[VJO_TITLE_ID_SIZE] = {0};
    SceUID pid = sceKernelGetProcessId();
    int rc = -1;
    if (safe_boot()) return SCE_KERNEL_START_NO_RESIDENT;
    if (IMPORT_READY(sceKernelGetProcessTitleId)) rc = sceKernelGetProcessTitleId(pid, title, sizeof(title));
    if ((rc < 0 || !title[0]) && IMPORT_READY(sceAppMgrAppParamGetString))
        rc = sceAppMgrAppParamGetString(pid, 12, title, sizeof(title));
    title[sizeof(title)-1] = 0;
    /* *ALL also reaches SceShell and the system apps: there the module gives
     * its pages back at once (NO_RESIDENT unloads it without module_stop
     * [SDK: Kernel-Overview]), instead of holding its pages in SceShell's
     * partition for code that never runs (#59). An unreadable title is
     * treated the same way: nothing is started, nothing stays. Candidate
     * games keep an idle worker until the Shell confirms their games.ini policy. */
    if (rc < 0 || !vjo_game_is_candidate(title)) return SCE_KERNEL_START_NO_RESIDENT;
    psp_title = vjo_game_is_psp(title);
    vjo_game_ocr_worker_start();
    /* Import/start failures leave the host app usable. Retained thread objects
     * keep the module loaded until module_stop can safely delete them. */
    return SCE_KERNEL_START_SUCCESS;
}
int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    return vjo_game_ocr_worker_stop() ? SCE_KERNEL_STOP_FAIL : SCE_KERNEL_STOP_SUCCESS;
}
#endif
