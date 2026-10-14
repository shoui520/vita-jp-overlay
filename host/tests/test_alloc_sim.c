/* The allocation simulator (#74 preflight): the real game worker
 * (game_ocr/main.c) and the real kernel (kernel/game_ocr.c, capture.c,
 * buffers.c) run whole OCR jobs on a 960x544 frame while every pool answers as
 * the owner's PCH-1001 on 3.65 did, or worse:
 *
 *   ScePhyMemPartKD  2,088,960 B refused five times with 0x80024302 [#74];
 *                    458,752 B granted in an earlier session [#74, 917x125x4].
 *                    Its free size is unknown (both exact queries failed), so a
 *                    request is granted only up to kd_largest, and the runs
 *                    repeat with the pool refusing everything.
 *   game USER        91,226,112 B free in CLANNAD [docs/vita-memory.md];
 *                    CDRAM and phycont are modelled pools, refusable one by one.
 *   PAF (SceShell)   2,346,312 B free at the game start [#74].
 *
 * API 18's kernel capture buffer is the same allocator the JPEG capture still
 * uses (buffers_alloc), so its refusal is shown with the real code. Each case
 * prints one line; preflight.sh collects them. Not device evidence: the pools
 * are models of the measured numbers. */
#include "acutest.h"
#include <stdlib.h>
#include <psp2/host_stubs.h>
#include <psp2kern/host_stubs.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <taihen.h>
#include "../../kernel/vjo_kernel.h"
#include "../../kernel/game_ocr.h"
#include "../../include/vjo_vocr_sizes.h"
#include "../../game_ocr/main.c"

#define FB_W 960u
#define FB_H 544u
#define KD_GRANTED_74 458752u        /* granted in an earlier session (917 x 125 x 4, page-rounded) */
#define KD_REFUSED_74 2088960u       /* refused five times, 0x80024302 */
#define USER_FREE_CLANNAD 91226112   /* docs/vita-memory.md */
#define PAF_FREE_74 2346312u         /* at the game start */
#define NO_PAGE ((int)0x80024302u)

VjoKernelState g;
unsigned syscall_depth;
static SceUID caller;
static unsigned locks;
static uint64_t clock_us;
static FILE *file;
static uint32_t frame[FB_W * FB_H];
static int in_hook, hook_copy_fails;

/* ---- the pools ---- */
static uint32_t kd_largest;      /* the largest KERNEL_RW block the kernel pool grants */
static uint32_t kd_taken, kd_peak, kd_refusals;
static int64_t user_free, cdram_free, phycont_free;
static int64_t user_used, cdram_used, phycont_used, user_peak;
static unsigned refuse;          /* bits: 1 USER capture, 2 CDRAM capture, 4 PHYCONT capture */
typedef struct { SceUID uid; void *base; size_t bytes; int type; int kernel; } Block;
static Block blocks[16];
static SceUID next_uid = 100;

static Block *find_uid(SceUID uid)
{
    for (unsigned i = 0; i < 16; i++) if (blocks[i].uid == uid && blocks[i].base) return &blocks[i];
    return NULL;
}
static SceUID add_block(size_t bytes, int type, int kernel)
{
    for (unsigned i = 0; i < 16; i++)
        if (!blocks[i].base) {
            if (posix_memalign(&blocks[i].base, 4096, bytes)) return NO_PAGE;
            memset(blocks[i].base, 0xA5, bytes);
            blocks[i].bytes = bytes; blocks[i].type = type; blocks[i].kernel = kernel;
            blocks[i].uid = next_uid++;
            return blocks[i].uid;
        }
    return NO_PAGE;
}
static int64_t *used_of(int type)
{
    return type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? &cdram_used
           : type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW ? &phycont_used : &user_used;
}
static int64_t free_of(int type)
{
    return type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? cdram_free
           : type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW ? phycont_free : user_free;
}

/* the game */
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out)
{
    out->size_user = (int)(user_free - user_used);
    out->size_cdram = (int)(cdram_free - cdram_used);
    out->size_phycont = (int)(phycont_free - phycont_used);
    return 0;
}
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned n, void *opt)
{
    int capture = !strcmp(name, "VjoGameVocrCapture");
    (void)opt;
    if (capture && ((type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && (refuse & 1)) ||
                    (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW && (refuse & 2)) ||
                    (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW && (refuse & 4))))
        return NO_PAGE;
    if (*used_of(type) + n > free_of(type)) return NO_PAGE;
    SceUID uid = add_block(n, type, 0);
    if (uid < 0) return uid;
    *used_of(type) += n;
    if (user_used > user_peak) user_peak = user_used;
    return uid;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    Block *b = find_uid(uid);
    if (!b || b->kernel) return -1;
    *base = b->base;
    return 0;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    Block *b = find_uid(uid);
    if (!b || b->kernel) return -1;
    *used_of(b->type) -= (int64_t)b->bytes;
    free(b->base);
    memset(b, 0, sizeof(*b));
    return 0;
}
/* the kernel pool */
SceUID ksceKernelAllocMemBlock(const char *name, unsigned type, SceSize bytes, void *opt)
{
    (void)name; (void)opt;
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW);
    if (bytes > kd_largest) { kd_refusals++; return NO_PAGE; }
    SceUID uid = add_block(bytes, (int)type, 1);
    if (uid < 0) return uid;
    kd_taken += bytes;
    if (kd_taken > kd_peak) kd_peak = kd_taken;
    return uid;
}
int ksceKernelGetMemBlockBase(SceUID uid, void **base)
{
    Block *b = find_uid(uid);
    if (!b || !b->kernel) return -1;
    *base = b->base;
    return 0;
}
int ksceKernelFreeMemBlock(SceUID uid)
{
    Block *b = find_uid(uid);
    if (!b || !b->kernel) return -1;
    kd_taken -= (uint32_t)b->bytes;
    free(b->base);
    memset(b, 0, sizeof(*b));
    return 0;
}
/* The worker's blocks are memory of the game process 7. */
SceUID ksceKernelFindProcMemBlockByAddr(SceUID pid, const void *addr, SceSize size)
{
    const unsigned char *p = addr;
    (void)size;
    if (pid != 7) return -1;
    for (unsigned i = 0; i < 16; i++)
        if (blocks[i].base && !blocks[i].kernel && p >= (unsigned char *)blocks[i].base &&
            p < (unsigned char *)blocks[i].base + blocks[i].bytes)
            return blocks[i].uid;
    return -1;
}
SceUID ksceKernelFindMemBlockByAddr(const void *addr, SceSize size) { (void)addr; (void)size; return 40; }

/* ---- the rest of the SDK boundary, as test_game_model_broker.c has it ---- */
int test_kernel_register(void);
int test_kernel_submit(const VjoGameOcrRequest *request);
int test_kernel_take(VjoGameOcrRequest *out);
int test_kernel_capture_rows(const VjoGameOcrRows *request, void *dst, VjoGameOcrRowsInfo *info);
int test_kernel_cancelled(uint32_t seq);
int test_kernel_complete(const VjoGameOcrResult *result);
int test_kernel_read(uint32_t seq, VjoGameOcrResult *out);
int test_kernel_cancel(uint32_t seq);
int test_kernel_model_open(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info);
int test_kernel_model_read(uint32_t seq, uint32_t off, uint32_t bytes, void *dst);
int test_kernel_model_close(uint32_t seq);
static int retail_return(int rc) { return rc < 0 ? (int32_t)((uint32_t)rc & ~0x40000000u) : rc; }
int vjoBootInfo(VjoBootInfo *out) { out->mode = VJO_BOOT_NORMAL; return 0; }
int vjoOcrRegister(void) { return retail_return(test_kernel_register()); }
int vjoOcrSubmit(const VjoGameOcrRequest *r) { return retail_return(test_kernel_submit(r)); }
int vjoOcrTake(VjoGameOcrRequest *out) { return retail_return(test_kernel_take(out)); }
int vjoOcrCaptureRows(const VjoGameOcrRows *request, void *dst, VjoGameOcrRowsInfo *info)
{ return retail_return(test_kernel_capture_rows(request, dst, info)); }
int vjoOcrCancelled(uint32_t seq) { return retail_return(test_kernel_cancelled(seq)); }
int vjoOcrComplete(const VjoGameOcrResult *r) { return retail_return(test_kernel_complete(r)); }
int vjoOcrRead(uint32_t seq, VjoGameOcrResult *out) { return retail_return(test_kernel_read(seq, out)); }
int vjoOcrCancel(uint32_t seq) { return retail_return(test_kernel_cancel(seq)); }
int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info)
{ return retail_return(test_kernel_model_open(seq, which, info)); }
int vjoOcrModelRead(uint32_t seq, uint32_t off, uint32_t bytes, void *dst)
{ return retail_return(test_kernel_model_read(seq, off, bytes, dst)); }
int vjoOcrModelClose(uint32_t seq) { return retail_return(test_kernel_model_close(seq)); }
int vjoMemInfo(VjoMemInfo *out) { (void)out; return VJO_ERR_PERM; }
void klog(const char *format, ...) { (void)format; }
SceUID ksceKernelGetProcessId(void) { return caller; }
uint64_t ksceKernelGetSystemTimeWide(void) { return clock_us; }
int ksceKernelSetEventFlag(SceUID uid, unsigned bits) { (void)uid; (void)bits; return 0; }
int ksceKernelClearEventFlag(SceUID uid, unsigned bits) { (void)uid; (void)bits; return 0; }
int ksceKernelDelayThread(SceUInt us) { clock_us += us; return 0; }
int ksceKernelWaitEventFlag(SceUID uid, unsigned bits, unsigned wait, unsigned *out, SceUInt *timeout)
{
    (void)uid; (void)wait; (void)out;
    if (bits == MEV_ROWS) { /* the game shows its next frame: the display hook runs in its thread */
        in_hook = 1;
        game_ocr_rows_hook(7, (uintptr_t)frame, FB_W, 0x00000000u, FB_W, FB_H);
        in_hook = 0;
        clock_us += 16667;
        return 0;
    }
    { SceUID saved = caller; unsigned depth = syscall_depth;
      caller = 0; syscall_depth = 0; game_ocr_model_tick(); caller = saved; syscall_depth = depth; }
    clock_us += *timeout;
    return 0;
}
int ksceKernelLockMutex(SceUID uid, int n, void *timeout) { (void)uid; (void)n; (void)timeout; locks++; return 0; }
int ksceKernelUnlockMutex(SceUID uid, int n) { (void)uid; (void)n; locks--; return 0; }
int taiGetModuleInfoForKernel(SceUID pid, const char *name, tai_module_info_t *info)
{ (void)pid; (void)name; info->modid = 99; return 0; }
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize n) { if (!dst || !src) return -1; memcpy(dst, src, n); return 0; }
int ksceKernelMemcpyKernelToUser(void *dst, const void *src, SceSize n)
{
    if (!dst || !src || (in_hook && hook_copy_fails)) return -1; /* hook_copy_fails: the hook cannot write the game */
    memcpy(dst, src, n);
    return 0;
}
int ksceIoGetstatByFd(SceUID fd, SceIoStat *st) { (void)fd; memset(st, 0, sizeof(*st)); st->st_mtime = (SceDateTime){2026, 10, 13, 0, 0, 0, 0}; return 0; }
SceUID ksceIoOpen(const char *path, int flags, unsigned mode)
{
    char local[1024];
    (void)flags; (void)mode;
    if (!VJO_VOCR_MODEL_DIR[0] || file) return (int)0x80010002u;
    snprintf(local, sizeof(local), "%s/%s", VJO_VOCR_MODEL_DIR, path + sizeof(VJO_VOCR_MODEL_ROOT));
    file = fopen(local, "rb");
    return file ? 31 : (int)0x80010002u;
}
SceOff ksceIoLseek(SceUID fd, SceOff off, int whence) { (void)fd; fseek(file, (long)off, whence); return (SceOff)ftell(file); }
int ksceIoPread(SceUID fd, void *out, SceSize n, SceOff off)
{ (void)fd; fseek(file, (long)off, SEEK_SET); return (int)fread(out, 1, n, file); }
int ksceIoClose(SceUID fd) { (void)fd; int rc = fclose(file); file = NULL; return rc; }
SceUID sceKernelGetProcessId(void) { return 7; }
uint64_t sceKernelGetProcessTimeWide(void) { return clock_us++; }
int sceKernelDelayThread(unsigned us)
{ SceUID saved = caller; clock_us += us; caller = 0; game_ocr_model_tick(); caller = saved; return 0; }
SceUID sceKernelCreateThread(const char *name, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu, void *opt) { return 20; }
int sceKernelStartThread(SceUID uid, SceSize n, void *args) { return 0; }

/* ocr-speed (API 21): the thread run time and the ARM clock; see test_game_vocr.c for the tested ones. */
int sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo *info) { (void)thid; (void)info; return -1; }
SceUID sceKernelGetThreadId(void) { return 21; }
int scePowerGetArmClockFrequency(void) { return 333; }
int scePowerSetArmClockFrequency(int mhz) { (void)mhz; return 0; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { return 0; }
int sceKernelDeleteThread(SceUID uid) { return 0; }
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size) { snprintf(out, size, "PCSG00415"); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize size) { return -1; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
SceUID sceIoOpen(const char *path, int flags, unsigned mode) { return (int)0x80010013u; } /* the game cannot see ux0 */
int sceDisplayGetFrameBuf(SceDisplayFrameBuf *fb, int sync) { (void)fb; (void)sync; return -1; }
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info) { (void)base; (void)info; return -1; }

/* ---- the frame: a VN screen, a dark textbox with three lines of glyph-like strokes ---- */
static uint32_t seed = 74;
static unsigned rnd(unsigned n) { seed = seed * 1103515245u + 12345u; return (seed >> 8) % n; }
static void box(unsigned x0, unsigned y0, unsigned x1, unsigned y1, uint32_t abgr)
{
    for (unsigned y = y0; y < y1; y++)
        for (unsigned x = x0; x < x1; x++) frame[y * FB_W + x] = abgr;
}
static void glyph_line(unsigned x0, unsigned y0, unsigned x1, unsigned gh, uint32_t abgr)
{
    for (unsigned x = x0; x + gh <= x1; x += gh + gh / 6) {
        unsigned strokes = 3 + rnd(4), t = gh / 10 ? gh / 10 : 1;
        for (unsigned k = 0; k < strokes; k++) {
            if (rnd(2)) { unsigned yy = y0 + rnd(gh - t); box(x, yy, x + gh, yy + t, abgr); }
            else { unsigned xx = x + rnd(gh - t); box(xx, y0, xx + t, y0 + gh, abgr); }
        }
    }
}
static void make_frame(void)
{
    seed = 74;
    for (unsigned y = 0; y < FB_H; y++)
        for (unsigned x = 0; x < FB_W; x++)
            frame[y * FB_W + x] = 0xFF000000u | 150u << 16 | (100 + x * 60 / FB_W) << 8 | (70 + y * 100 / FB_H + rnd(9));
    box(20, 380, 940, 536, 0xFF221812u);
    glyph_line(44, 404, 900, 22, 0xFFF0F0F0u);
    glyph_line(44, 440, 760, 22, 0xFF96ECFAu);
    glyph_line(44, 476, 620, 22, 0xFFECE4E4u);
}

/* ---- one case ---- */
typedef struct {
    int rc;
    uint32_t kd_peak, capture_bytes, capture_memory, source;
    int64_t user_peak;
    unsigned refused_steps, lines;
} Outcome;

static void reset_worker(void)
{
    if (weights_uid >= 0 || resident.data) drop_weights();
    worker = workspace_uid = weights_uid = capture_uid = line_uid = -1;
    resident.model = NULL; resident.data = NULL; resident.capacity = 0;
    model_owned = 0; model_seq = 0; quitting = active_seq = 0;
    worker_started = claim_outstanding = blocked = io_error = stat_error = hook_owns_capture = 0;
}

static Outcome run_case(const char *model, uint32_t flags, uint16_t rx, uint16_t ry, uint16_t rw, uint16_t rh)
{
    static VjoGameOcrResult out;
    Outcome o;
    memset(&o, 0, sizeof(o));
    reset_worker();
    kd_taken = kd_peak = 0;
    user_used = cdram_used = phycont_used = user_peak = 0;
    memset(&g, 0, sizeof(g));
    game_ocr_init();
    clock_us = 1000000;
    g.lock = g.ievf = 1; g.evf = 2; g.mevf = 5; g.shell_pid = 3; g.game_pid = 7; g.game_active = VJO_GAME;
    g.ocr_epoch = 1; g.capture_state = CAPTURE_IDLE; g.fb_w = FB_W; g.fb_h = FB_H; g.done_seq = g.capture_seq = 11;
    g.region = (VjoRect){rx, ry, rw, rh};
    caller = 7; TEST_ASSERT(!vjoOcrRegister()); caller = 3;
    VjoGameOcrRequest request = {.size = sizeof(request), .flags = flags};
    strcpy(request.model_dir, VJO_VOCR_MODEL_ROOT);
    strcpy(request.model, model);
    int seq = vjoOcrSubmit(&request);
    TEST_ASSERT(seq > 0);
    caller = 7;
    TEST_ASSERT(!vjoOcrTake(&request));
    o.rc = vjo_game_ocr_run_request(&request, &out);
    TEST_CHECK(!vjoOcrComplete(&out));
    o.kd_peak = kd_peak;
    o.user_peak = user_peak;
    o.capture_bytes = out.capture_bytes;
    o.capture_memory = out.capture_memory;
    o.source = out.source;
    o.lines = out.lines;
    for (unsigned i = 0; i < out.n_steps && i < VJO_GAME_OCR_STEPS; i++)
        if (out.step[i].id == VJO_GSTEP_ALLOC && out.step[i].rc < 0) o.refused_steps++;
    /* every block went back: nothing held after the job (weights stay resident) */
    TEST_CHECK(kd_taken == 0 && cdram_used == 0 && phycont_used == 0);
    TEST_CHECK(user_used == (int64_t)(resident.data ? weights_bytes_held : 0));
    return o;
}

static const char *const mem_name[] = {"-", "USER", "CDRAM", "PHYCONT", "kernel"};
static const char *const src_name[] = {"-", "hook", "kernel strip", "user"};

static void say(const char *what, const Outcome *o)
{
    fprintf(stderr, "alloc-sim: %-58s rc=%-4d KD %7u B, capture %s %s %6u B, game USER peak %9lld B, %u line(s)%s\n",
            what, o->rc, (unsigned)o->kd_peak, o->source < 4 ? src_name[o->source] : "?",
            o->capture_memory < 5 ? mem_name[o->capture_memory] : "?", (unsigned)o->capture_bytes,
            (long long)o->user_peak, o->lines, o->refused_steps ? " (refused rungs logged)" : "");
}

static int have_models(void)
{
    char path[1024];
    FILE *f;
    static const char *const names[] = {"VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr", "H15_w8.vocr"};
    if (!VJO_VOCR_MODEL_DIR[0]) return 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", VJO_VOCR_MODEL_DIR, names[i]);
        if (!(f = fopen(path, "rb"))) return 0;
        fclose(f);
    }
    return 1;
}

/* API 18: the kernel's region-sized capture buffer, the real allocator. */
static void api18_kernel_capture_is_refused_at_the_full_screen(void)
{
    kd_largest = KD_GRANTED_74; kd_refusals = 0;
    memset(&g, 0, sizeof(g));
    g.capture_state = CAPTURE_IDLE;
    int full = buffers_alloc(FB_W * FB_H * 4);
    TEST_CHECK(full == NO_PAGE && kd_refusals == 1);
    fprintf(stderr, "alloc-sim: %-58s rc=0x%08X (as #74: 0x80024302, five times)\n",
            "API 18 full screen: kernel VjoCapture 2,088,960 B", (unsigned)full);
    buffers_free();
    int box = buffers_alloc(917 * 125 * 4);
    TEST_CHECK(box == 0);
    fprintf(stderr, "alloc-sim: %-58s rc=%d (granted, as in #74's earlier session)\n",
            "API 18 textbox 917x125: kernel VjoCapture 458,752 B", box);
    buffers_free();
    TEST_CHECK(kd_taken == 0);
}

/* API 19 under #74's pools: no kernel pool for OCR, at any region, any model. The
 * shipped default and fallback (VOCR3), read through the real kernel broker and checked
 * against their pins by the real worker, and v0.2.0's H15, the documented fallback. */
static void api19_runs_with_no_kernel_memory(void)
{
    static const struct { const char *what, *model; uint32_t flags; uint16_t r[4]; } cases[] = {
        {"API 19 full screen 960x544, VOCR3-H15", "VOCR3_H15_w8.vocr", VJO_GAME_OCR_F_FULL, {0, 0, 0, 0}},
        {"API 19 textbox 917x125, VOCR3-H15", "VOCR3_H15_w8.vocr", 0, {1536, 48188, 62600, 15059}},
        {"API 19 full screen 960x544, VOCR3-FL10", "VOCR3_FL10_w8.vocr", VJO_GAME_OCR_F_FULL, {0, 0, 0, 0}},
        {"API 19 textbox 917x125, VOCR3-FL10", "VOCR3_FL10_w8.vocr", 0, {1536, 48188, 62600, 15059}},
        {"API 19 textbox 917x125, v0.2.0 H15", "H15_w8.vocr", 0, {1536, 48188, 62600, 15059}},
    };
    if (!have_models()) { TEST_MSG("release weights absent (VJO_VOCR_MODEL_DIR): simulator skipped"); return; }
    make_frame();
    for (unsigned pass = 0; pass < 2; pass++) {
        kd_largest = pass ? 0 : KD_GRANTED_74; /* pass 1: the kernel pool refuses everything */
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            char what[96];
            user_free = USER_FREE_CLANNAD; cdram_free = 128 << 20; phycont_free = 16 << 20; refuse = 0;
            hook_copy_fails = 0;
            Outcome o = run_case(cases[i].model, cases[i].flags, cases[i].r[0], cases[i].r[1], cases[i].r[2], cases[i].r[3]);
            snprintf(what, sizeof(what), "%s%s", cases[i].what, pass ? ", KD refuses all" : "");
            say(what, &o);
            TEST_CHECK_(o.rc == 0 && o.kd_peak == 0 && o.source == VJO_GAME_OCR_SRC_HOOK &&
                        o.capture_memory == VJO_GAME_OCR_MEM_USER && o.capture_bytes <= 64u * 1024u && o.lines > 0,
                        "%s: rc %d KD %u capture %u", what, o.rc, (unsigned)o.kd_peak, (unsigned)o.capture_bytes);
        }
    }
}

/* The ladder, one run per rung: USER -> CDRAM -> PHYCONT -> the kernel strip. */
static void api19_ladder_under_refusals(void)
{
    if (!have_models()) { TEST_MSG("release weights absent: ladder skipped"); return; }
    make_frame();
    kd_largest = KD_GRANTED_74;
    user_free = USER_FREE_CLANNAD; cdram_free = 128 << 20; phycont_free = 16 << 20;
    const uint16_t box[4] = {1536, 48188, 62600, 15059};

    refuse = 1; hook_copy_fails = 0;
    Outcome o = run_case("VOCR3_H15_w8.vocr", 0, box[0], box[1], box[2], box[3]);
    say("USER refuses the capture block -> CDRAM", &o);
    TEST_CHECK(o.rc == 0 && o.capture_memory == VJO_GAME_OCR_MEM_CDRAM && o.kd_peak == 0 && o.refused_steps == 1);

    refuse = 3;
    o = run_case("VOCR3_H15_w8.vocr", 0, box[0], box[1], box[2], box[3]);
    say("USER and CDRAM refuse it -> PHYCONT", &o);
    TEST_CHECK(o.rc == 0 && o.capture_memory == VJO_GAME_OCR_MEM_PHYCONT && o.kd_peak == 0 && o.refused_steps == 2);

    refuse = 0; hook_copy_fails = 1;
    o = run_case("VOCR3_H15_w8.vocr", 0, box[0], box[1], box[2], box[3]);
    say("the hook cannot write the game's memory -> kernel strip", &o);
    TEST_CHECK(o.rc == 0 && o.source == VJO_GAME_OCR_SRC_KSTRIP && o.kd_peak > 0 && o.kd_peak <= 64u * 1024u);

    kd_largest = 0;
    o = run_case("VOCR3_H15_w8.vocr", 0, box[0], box[1], box[2], box[3]);
    say("hook cannot write and KD refuses the strip", &o);
    TEST_CHECK(o.rc != 0 && o.kd_peak == 0);

    kd_largest = KD_GRANTED_74; hook_copy_fails = 0; refuse = 7;
    o = run_case("VOCR3_H15_w8.vocr", 0, box[0], box[1], box[2], box[3]);
    say("every rung refuses the capture block", &o);
    TEST_CHECK(o.rc == VJO_E_OOM && o.refused_steps == 3 && o.kd_peak == 0);
    refuse = 0;
}

/* The Shell's side at #74's game start: its result arena and the overlay's UI
 * arena above the PAF reserve (ledger constants; the Shell takes nothing else). */
static void shell_paf_fits_at_the_74_game_start(void)
{
    uint32_t reserve = 1536u * 1024u, need = VJO_BUDGET_PAF_RESULTS_LOCAL + VJO_BUDGET_PAF_UI;
    fprintf(stderr, "alloc-sim: %-58s %u B needed, %u B above the %u B reserve\n",
            "Shell PAF at the game start (2,346,312 B free)", (unsigned)need, (unsigned)(PAF_FREE_74 - reserve),
            (unsigned)reserve);
    TEST_CHECK(need <= PAF_FREE_74 - reserve);
}

TEST_LIST = {
    {"API 18 kernel capture refused at the full screen under #74's pool", api18_kernel_capture_is_refused_at_the_full_screen},
    {"API 19 OCR takes no kernel memory under #74's pools", api19_runs_with_no_kernel_memory},
    {"API 19 ladder under refusals, each rung logged", api19_ladder_under_refusals},
    {"Shell PAF fits at #74's game start", shell_paf_fits_at_the_74_game_start},
    {NULL, NULL}
};
