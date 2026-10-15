/* Real vocr + real game worker + real kernel transport: the model broker and,
 * since API 19, the capture: the worker's strip requests are copied by the
 * real kernel display-hook code (game_ocr_rows_hook, capture_grey_rows) from a
 * simulated game frame straight into the worker's capture block. File syscalls
 * run only when the harness schedules the persistent kernel worker; the game
 * namespace reproduces physical ENODEV. Host execution is not device proof. */
#include "acutest.h"
#include <psp2/host_stubs.h>
#include <psp2kern/host_stubs.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <taihen.h>
#include "../../kernel/vjo_kernel.h"
#include "../../kernel/game_ocr.h"
#include "../../include/vjo_vocr_sizes.h"
#include "../../game_ocr/main.c"

VjoKernelState g;
unsigned syscall_depth;
static SceUID caller;
static unsigned locks, user_opens, kernel_opens, kernel_reads, kernel_closes, kernel_stats;
static unsigned allocated, released, received, weights_allocated, weights_released, game_delays;
static unsigned char arena[2*1024*1024] __attribute__((aligned(64)));
static unsigned char weights_memory[VJO_VOCR_F20_BYTES + 4096] __attribute__((aligned(64)));
static int weights_live, kernel_waits, done_pending;
static unsigned live_bytes, peak_bytes, job_block, weights_block, kernel_wait_calls;
static SceDateTime file_mtime;
static uint32_t pixels[200 * 60]; /* the game's 200x60 frame, A8B8G8R8 */
static unsigned char capture_mem_block[64 * 1024] __attribute__((aligned(64))); /* VjoGameVocrCapture */
static int capture_block_live, in_hook;
static unsigned hook_frames, capture_blocks;
static int live, open_error, read_error, close_error, bad_length, early_eof, corrupt, short_read;
static int schedule_kernel, cancel_after, substitute_legacy;
static uint64_t clock_us;
static FILE *file;
static unsigned boundary_busy, capture_allocations, capture_releases;
static int capture_live;

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
static int retail_return(int rc)
{ return rc < 0 ? (int32_t)((uint32_t)rc & ~0x40000000u) : rc; }
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
{ int rc = retail_return(test_kernel_model_open(seq, which, info)); if ((uint32_t)rc == 0xBFFFFFFDu) boundary_busy++; return rc; }
int vjoOcrModelRead(uint32_t seq, uint32_t off, uint32_t bytes, void *dst)
{ int rc = retail_return(test_kernel_model_read(seq, off, bytes, dst)); if ((uint32_t)rc == 0xBFFFFFFDu) boundary_busy++; return rc; }
int vjoOcrModelClose(uint32_t seq)
{ int rc = retail_return(test_kernel_model_close(seq)); if ((uint32_t)rc == 0xBFFFFFFDu) boundary_busy++; return rc; }

void klog(const char *format, ...) { (void)format; }
SceUID ksceKernelGetProcessId(void) { return caller; }
uint64_t ksceKernelGetSystemTimeWide(void) { return clock_us; }
/* vjoOcrSubmit's MEV_JOB and vjoOcrComplete's MEV_RESULT (a waiting Take or Read returns at once). */
static int job_bit, result_bit;
int ksceKernelSetEventFlag(SceUID uid, unsigned bits)
{
    if (g.mevf > 0 && uid == g.mevf && bits == MEV_ROWS) return 0;
    if (g.mevf > 0 && uid == g.mevf && bits == MEV_JOB) { job_bit = 1; return 0; }
    if (g.mevf > 0 && uid == g.mevf && bits == MEV_RESULT) { result_bit = 1; return 0; }
    if (g.mevf > 0 && uid == g.mevf) { TEST_CHECK(bits == MEV_DONE); done_pending = 1; return 0; }
    if (uid == g.evf && bits == VJO_EV_CAPTURE_DONE) return 0;
    TEST_CHECK(uid == g.ievf && bits == IEV_MODEL); return 0;
}
int ksceKernelClearEventFlag(SceUID uid, unsigned bits)
{
    TEST_CHECK(uid == g.mevf && (bits == ~MEV_ROWS || bits == ~MEV_JOB || bits == ~MEV_RESULT));
    if (bits == ~MEV_JOB) job_bit = 0;
    if (bits == ~MEV_RESULT) result_bit = 0;
    return 0;
}
int ksceKernelDelayThread(SceUInt us) { clock_us += us; return 0; }
SceUID ksceKernelFindMemBlockByAddr(const void *addr, SceSize size) { return 40; }
/* Only the worker's capture block is memory of the game process 7. */
SceUID ksceKernelFindProcMemBlockByAddr(SceUID pid, const void *addr, SceSize size)
{
    const unsigned char *p = addr;
    TEST_CHECK(size == 0);
    return pid == 7 && capture_block_live && p >= capture_mem_block && p < capture_mem_block + sizeof(capture_mem_block) ? 41 : -1;
}
/* The game's model syscall waits in the kernel; with kernel_waits the kernel
 * worker thread runs meanwhile and finishes the operation. */
int ksceKernelWaitEventFlag(SceUID uid, unsigned bits, unsigned wait, unsigned *out, SceUInt *timeout)
{
    if (bits == MEV_ROWS) {
        /* The game shows its next frame: the display hook runs in its thread. */
        TEST_ASSERT(uid == g.mevf && !locks && syscall_depth == 1 && timeout);
        in_hook = 1; hook_frames++;
        game_ocr_rows_hook(7, (uintptr_t)pixels, 200, 0x00000000u, 200, 60);
        in_hook = 0;
        return 0;
    }
    if (bits == MEV_JOB || bits == MEV_RESULT) { /* an idle Take, a pending Read: the bit, or the timeout */
        int *bit = bits == MEV_JOB ? &job_bit : &result_bit;
        TEST_ASSERT(uid == g.mevf && !locks && syscall_depth == 1 && timeout);
        if (*bit) { *bit = 0; return 0; }
        clock_us += *timeout;
        return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    }
    TEST_ASSERT(uid == g.mevf && bits == MEV_DONE && !locks && syscall_depth == 1 && timeout);
    kernel_wait_calls++;
    if (!done_pending && kernel_waits) {
        SceUID saved = caller; unsigned depth = syscall_depth;
        caller = 0; syscall_depth = 0; game_ocr_model_tick(); caller = saved; syscall_depth = depth;
    }
    if (done_pending) { done_pending = 0; return 0; }
    clock_us += *timeout;
    return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
}
int ksceIoGetstatByFd(SceUID fd, SceIoStat *st)
{
    TEST_ASSERT(caller == 0 && !locks && !syscall_depth && fd == 31 && file);
    kernel_stats++; memset(st, 0, sizeof(*st)); st->st_mtime = file_mtime; return 0;
}
int ksceKernelLockMutex(SceUID uid, int n, void *timeout)
{ TEST_CHECK(uid == g.lock && !locks); locks++; return 0; }
int ksceKernelUnlockMutex(SceUID uid, int n)
{ TEST_CHECK(uid == g.lock && locks == 1); locks--; return 0; }
int taiGetModuleInfoForKernel(SceUID pid, const char *name, tai_module_info_t *info)
{ TEST_CHECK(pid == 7 && !strcmp(name, VJO_GAME_OCR_MODULE) && syscall_depth == 1 && !locks); info->modid = 99; return 0; }
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize n)
{ TEST_CHECK(syscall_depth == 1); if (!dst || !src) return -1; memcpy(dst, src, n); return 0; }
int ksceKernelMemcpyKernelToUser(void *dst, const void *src, SceSize n)
{ TEST_CHECK(syscall_depth == 1 && (in_hook ? !locks : locks == 1)); if (!dst || !src) return -1; memcpy(dst, src, n); return 0; }
static unsigned char jpeg_capture[200 * 60 * 4 + 4096];
SceUID ksceKernelAllocMemBlock(const char *name, unsigned type, SceSize bytes, void *opt)
{ TEST_CHECK(!capture_live && type == SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW && bytes <= sizeof(jpeg_capture)); capture_live = 1; capture_allocations++; return 27; }
int ksceKernelGetMemBlockBase(SceUID uid, void **base)
{ TEST_CHECK(uid == 27 && capture_live); *base = jpeg_capture; return 0; }
int ksceKernelFreeMemBlock(SceUID uid)
{ TEST_CHECK(uid == 27 && capture_live); capture_live = 0; capture_releases++; return 0; }

SceUID ksceIoOpen(const char *path, int flags, unsigned mode)
{
    TEST_ASSERT(caller == 0 && !locks && !syscall_depth && !file);
    TEST_CHECK(flags == SCE_O_RDONLY && !strncmp(path, VJO_VOCR_MODEL_ROOT "/", sizeof(VJO_VOCR_MODEL_ROOT)));
    const char *name = path + sizeof(VJO_VOCR_MODEL_ROOT);
    TEST_ASSERT(vjo_vocr_model(name) != NULL);
    kernel_opens++;
    if (open_error) return open_error;
    char local[1024]; snprintf(local, sizeof(local), "%s/%s", VJO_VOCR_MODEL_DIR,
                               substitute_legacy ? "FL10_w8.vocr" : name);
    file = fopen(local, "rb"); TEST_ASSERT(file != NULL); return 31;
}
SceOff ksceIoLseek(SceUID fd, SceOff off, int whence)
{ TEST_ASSERT(caller == 0 && !locks && !syscall_depth && fd == 31 && file); TEST_ASSERT(!fseek(file, (long)off, whence)); return (SceOff)ftell(file)+bad_length; }
int ksceIoPread(SceUID fd, void *out, SceSize n, SceOff off)
{
    TEST_ASSERT(caller == 0 && !locks && !syscall_depth && fd == 31 && file);
    TEST_CHECK(n && n <= VJO_GAME_OCR_MODEL_CHUNK && off >= 0);
    kernel_reads++;
    if (read_error && kernel_reads >= 3) return read_error;
    if (early_eof && kernel_reads >= 3) return 0;
    if (short_read && n > 37) n = 37;
    TEST_ASSERT(!fseek(file, (long)off, SEEK_SET));
    int got = (int)fread(out, 1, n, file);
    if (corrupt && off <= 1024 && off+got > 1024) ((unsigned char *)out)[1024-off] ^= 1;
    received += (unsigned)got; return got;
}
int ksceIoClose(SceUID fd)
{ TEST_ASSERT(caller == 0 && !locks && !syscall_depth && fd == 31 && file); kernel_closes++; if (close_error) return close_error; int rc = fclose(file); file = NULL; return rc; }

SceUID sceKernelGetProcessId(void) { TEST_CHECK(caller == 7); return 7; }
uint64_t sceKernelGetProcessTimeWide(void) { return clock_us++; }
int sceKernelDelayThread(unsigned us)
{
    game_delays++;
    clock_us += us;
    if (schedule_kernel) {
        SceUID saved = caller; caller = 0; game_ocr_model_tick(); caller = saved;
        if (cancel_after && kernel_reads >= (unsigned)cancel_after) {
            VJO_LOCK(); g.game_active = 0; game_ocr_foreground_changed_locked(); VJO_UNLOCK();
        }
    }
    return 0;
}
static int64_t game_free = 16*1024*1024; /* the game's free USER without the worker's blocks */
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out)
{ out->size_user = (int)(game_free - live_bytes); return 0; }
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned n, void *opt)
{
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && !(n & 4095));
    if (!strcmp(name, "VjoGameVocrCapture")) {
        TEST_CHECK(!capture_block_live && n <= sizeof(capture_mem_block) && n < 64 * 1024 + 1);
        capture_block_live = 1; capture_blocks++; live_bytes += n; if (live_bytes > peak_bytes) peak_bytes = live_bytes;
        return 11;
    }
    if (!strcmp(name, "VjoGameVocrWeights")) {
        TEST_CHECK(!weights_live && n <= sizeof(weights_memory));
        weights_live = 1; weights_allocated++; weights_block = n;
    } else {
        TEST_CHECK(!strcmp(name, "VjoGameVocr") && !live && n <= sizeof(arena));
        live = 1; allocated++; job_block = n;
    }
    live_bytes += n; if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return weights_live && !strcmp(name, "VjoGameVocrWeights") ? 10 : 9;
}
static unsigned capture_held(void) { return PAGE_ROUND(vjo_vocr_capture_bytes(200, 60)); }
/* Only the probe's user-mode read uses these (not run here). */
int sceDisplayGetFrameBuf(SceDisplayFrameBuf *fb, int sync) { (void)fb; (void)sync; return -1; }
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info) { (void)base; (void)info; return -1; }
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid == 11) { TEST_CHECK(capture_block_live); *base = capture_mem_block; return 0; }
    if (uid == 10) { TEST_CHECK(weights_live); *base = weights_memory; return 0; }
    TEST_CHECK(uid == 9 && live); *base = arena; return 0;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    if (uid == 11) { TEST_CHECK(capture_block_live); capture_block_live = 0; live_bytes -= capture_held(); return 0; }
    if (uid == 10) { TEST_CHECK(weights_live); weights_live = 0; weights_released++; live_bytes -= weights_block; return 0; }
    TEST_CHECK(uid == 9 && live); live = 0; released++; live_bytes -= job_block; return 0;
}
SceUID sceKernelCreateThread(const char *name, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu, void *opt) { return 20; }
int sceKernelStartThread(SceUID uid, SceSize n, void *args) { return 0; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { return 0; }
int sceKernelDeleteThread(SceUID uid) { return 0; }
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size)
{ snprintf(out, size, "PCSG00415"); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize size) { return -1; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
SceUID sceIoOpen(const char *path, int flags, unsigned mode)
{ user_opens++; return (int)0x80010013u; }

/* A fresh kernel, capture and claimed job; keep_weights keeps the game
 * worker's resident weights from the previous job, as one game process would. */
static const char *submit_fallback = "";
static VjoGameOcrRequest setup_job(const char *name, int keep_weights)
{
    TEST_ASSERT(!file && !live);
    memset(&g, 0, sizeof(g)); game_ocr_init();
    worker = workspace_uid = -1; model_owned = 0; model_seq = 0;
    if (!keep_weights) {
        if (weights_live) { TEST_CHECK(!drop_weights()); }
        weights_uid = -1; resident.model = NULL; resident.data = NULL; resident.capacity = 0;
        weights_allocated = weights_released = 0; live_bytes = peak_bytes = 0;
        file_mtime = (SceDateTime){2026, 10, 12, 21, 0, 0, 0};
    }
    game_delays = kernel_stats = kernel_wait_calls = 0; kernel_waits = done_pending = 0;
    quitting = active_seq = 0; worker_started = claim_outstanding = blocked = io_error = 0;
    locks = syscall_depth = user_opens = kernel_opens = kernel_reads = kernel_closes = 0;
    allocated = released = received = 0;
    boundary_busy = capture_allocations = capture_releases = 0; capture_live = 0;
    hook_frames = capture_blocks = 0; capture_block_live = 0; capture_uid = line_uid = -1;
    open_error = read_error = close_error = bad_length = early_eof = corrupt = short_read = cancel_after = 0;
    schedule_kernel = 1; substitute_legacy = 0; clock_us = 1000000;
    g.lock = g.ievf = 1; g.evf = 2; g.mevf = 5; g.shell_pid = 3; g.game_pid = 7; g.game_active = VJO_GAME; g.ocr_epoch = 1;
    g.capture_state = CAPTURE_IDLE; g.fb_w = 200; g.fb_h = 60; g.done_seq = g.capture_seq = 11;
    /* the game's frame: black, one line of strokes at rows 15-38, the same grey
     * as the API 18 test's raw region (memset(.., 235, 6): one white pixel and
     * one with red and green only, every 6 px) */
    for (unsigned i = 0; i < 200 * 60; i++) pixels[i] = 0xFF000000u;
    for (unsigned y=15; y<39; y++) for (unsigned x=10; x<190; x+=6) {
        pixels[y*200+x] = 0xFFEBEBEBu; pixels[y*200+x+1] = 0xFF00EBEBu;
    }
    caller = 7; TEST_ASSERT(!vjoOcrRegister()); caller = 3;
    VjoGameOcrRequest request = {.size=sizeof(request)};
    strcpy(request.model_dir, VJO_VOCR_MODEL_ROOT); strcpy(request.model, name);
    strcpy(request.fallback, submit_fallback);
    int seq = vjoOcrSubmit(&request); TEST_ASSERT(seq > 0);
    caller = 7; TEST_ASSERT(!vjoOcrTake(&request)); TEST_CHECK(request.seq == (unsigned)seq);
    return request;
}
static VjoGameOcrRequest setup(const char *name) { return setup_job(name, 0); }
static void finish(const VjoGameOcrResult *out)
{
    TEST_CHECK(!file && !live && !model_owned && allocated == released && !locks && !syscall_depth);
    TEST_CHECK(!user_opens && !vjoOcrComplete(out));
    VjoGameOcrResult received_result; caller = 3;
    int rc = vjoOcrRead(out->seq, &received_result);
    if (out->rc == VJO_E_CANCELLED) TEST_CHECK(rc == VJO_ERR_CANCELLED);
    else TEST_CHECK(!rc && !memcmp(out, &received_result, sizeof(*out)));
    /* No kernel block was taken for OCR, and the worker's capture block is gone. */
    TEST_CHECK(!capture_live && !capture_allocations && !capture_block_live && !game_ocr_capture_busy_locked());
    if (g.game_active) {
        /* The first hardware failure pinned ownership and blocked this next
         * real capture request even though all previous pixel copies worked. */
        TEST_CHECK(retail_return(capture_request(VJO_CAPTURE_ONCE)) > 0);
        TEST_CHECK(capture_live && g.capture_state == CAPTURE_PENDING);
        g.capture_state = CAPTURE_IDLE; buffers_free();
        TEST_CHECK(!capture_live && capture_allocations == 1 && capture_releases == 1);
    }
}
static void real_models_cross_namespace(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Pinned release weights absent; real-model integration skipped"); return; }
    const char *names[] = {"FL10_w8.vocr", "H15_w8.vocr", VJO_VOCR3_FL10_NAME};
    for (unsigned i=0; i<sizeof(names)/sizeof(names[0]); i++) {
        VjoGameOcrRequest request = setup(names[i]); VjoGameOcrResult out;
        /* Demonstrate the physical failing boundary before using the broker. */
        TEST_CHECK(sceIoOpen("ux0:data/VitaJPOverlay/ocr/FL10_w8.vocr", 0, 0) == (int)0x80010013u); user_opens = 0;
        short_read = i == 1;
        int run_rc = vjo_game_ocr_run_request(&request, &out);
        TEST_CHECK(!run_rc);
        TEST_MSG("%s rc=%d io=0x%08X cleanup=%d owned=%d boundary_busy=%u",
                 names[i], run_rc, (unsigned)out.io_error, out.cleanup_status, model_owned, boundary_busy);
        TEST_CHECK(out.lines == 1 && out.workspace_bytes <= sizeof(arena));
        TEST_CHECK(received == vjo_vocr_model(names[i])->bytes);
        /* The real runtime's frames for the one line, and the memory check. */
        TEST_CHECK(out.recognized == 1 && out.frames == out.line[0].frames && out.frames > 0);
        TEST_CHECK(out.line[0].width > 0 && out.line[0].height > 0 && out.frames <= out.line[0].width);
        TEST_CHECK(out.free_user == 16*1024*1024 && out.reserve_bytes == 4u*1024u*1024u && out.need_weights);
        /* The capture: the region in strips through the display hook, into a
         * capture block under 64 KiB; the first pass published the capture. */
        TEST_CHECK(out.source == VJO_GAME_OCR_SRC_HOOK && out.capture_memory == VJO_GAME_OCR_MEM_USER);
        TEST_CHECK(out.capture_bytes == capture_held() && out.capture_bytes < 64u * 1024u && capture_blocks == 1);
        TEST_CHECK(out.width == 200 && out.height == 60 && out.fb_width == 200 && out.fb_format == 0 && !out.tears);
        /* 60 rows fit one strip: read once for the finder, once more to check it. */
        TEST_CHECK(out.captures == hook_frames && hook_frames == 2 && out.strip_rows == 60 && g.done_seq == request.capture_seq);
        TEST_CHECK(out.n_steps > 4 && out.step[0].id == VJO_GSTEP_MEM);
        printf("  %s capture: %u strips of %u rows, %u B block, %u steps\n", names[i], out.captures,
               out.strip_rows, out.capture_bytes, out.n_steps);
        printf("  %s: line %ux%u, %u frames\n", names[i], out.line[0].width, out.line[0].height, out.frames);
        TEST_CHECK(kernel_opens == 1 && kernel_closes == 1 && kernel_reads > 1);
        TEST_CHECK(boundary_busy > 2); /* Open, Read and Close traverse real-return wrappers. */
        finish(&out);
    }
}
/* The kernel worker answers each model call while it waits: one 32 KiB read
 * per piece, no BUSY reaching the game and no game sleep; the next job of the
 * same game reuses the verified weights and reads nothing; a new file time
 * makes it read and check them again. */
static void real_models_resident_through_waiting_broker(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Pinned release weights absent; resident integration skipped"); return; }
    const char *names[] = {"FL10_w8.vocr", "H15_w8.vocr"};
    for (unsigned i=0; i<2; i++) {
        const uint32_t bytes = vjo_vocr_model(names[i])->bytes;
        const unsigned pieces = (bytes + VJO_GAME_OCR_MODEL_CHUNK - 1) / VJO_GAME_OCR_MODEL_CHUNK;
        VjoGameOcrRequest request = setup(names[i]); VjoGameOcrResult first, out;
        g.mevf = 5; kernel_waits = 1;
        TEST_CHECK(!vjo_game_ocr_run_request(&request, &first) && first.lines == 1);
        TEST_CHECK(first.weights == VJO_GAME_OCR_WEIGHTS_READ && received == bytes && kernel_reads == pieces);
        TEST_CHECK(!boundary_busy && !game_delays && kernel_opens == 1 && kernel_closes == 1);
        TEST_CHECK(first.resident_bytes == PAGE_ROUND(bytes) && weights_live && weights_allocated == 1);
        printf("  %s first job: %u kernel reads of <= %u B, %u in-kernel waits, %u BUSY at the game, %u game sleeps\n",
               names[i], kernel_reads, VJO_GAME_OCR_MODEL_CHUNK, kernel_wait_calls, boundary_busy, game_delays);
        finish(&first);
        request = setup_job(names[i], 1); g.mevf = 5; kernel_waits = 1;
        TEST_CHECK(!vjo_game_ocr_run_request(&request, &out) && !strcmp(out.text, first.text));
        TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT && !received && !kernel_reads);
        TEST_CHECK(kernel_opens == 1 && kernel_stats == 1 && kernel_closes == 1 && !game_delays);
        TEST_CHECK(weights_allocated == 1 && !weights_released && out.resident_bytes == PAGE_ROUND(bytes));
        printf("  %s next job: %u kernel reads, %u opens, %u stats, %u closes, %u game sleeps, weights=%u\n",
               names[i], kernel_reads, kernel_opens, kernel_stats, kernel_closes, game_delays, out.weights);
        finish(&out);
        request = setup_job(names[i], 1); g.mevf = 5; kernel_waits = 1;
        file_mtime.second++; /* replaced on the card: read and checked again, into the same block */
        TEST_CHECK(!vjo_game_ocr_run_request(&request, &out) && !strcmp(out.text, first.text));
        TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_READ && received == bytes && kernel_reads == pieces);
        TEST_CHECK(weights_allocated == 1 && !weights_released);
        finish(&out);
    }
}
/* H15 configured, FL10 fallback, real weights and runtime through the real
 * kernel broker: with one byte too few for H15 the same job reads FL10 from
 * the fallback name and recognizes the line as a direct FL10 job does. */
static void real_fallback_runs_fl10_when_h15_does_not_fit(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Pinned release weights absent; fallback integration skipped"); return; }
    VjoGameOcrRequest request = setup("FL10_w8.vocr"); VjoGameOcrResult direct, out;
    g.mevf = 5; kernel_waits = 1;
    TEST_CHECK(!vjo_game_ocr_run_request(&request, &direct) && direct.lines == 1);
    finish(&direct);
    const uint32_t h15 = PAGE_ROUND(vjo_vocr_job_bytes("H15_w8.vocr", 200, 60)) + PAGE_ROUND(VJO_VOCR_H15_BYTES);
    submit_fallback = "FL10_w8.vocr";
    request = setup("H15_w8.vocr"); g.mevf = 5; kernel_waits = 1;
    game_free = 4u*1024u*1024u + h15 - 1;
    TEST_CHECK(!vjo_game_ocr_run_request(&request, &out) && out.fallback == 1 && out.lines == 1);
    TEST_CHECK(!strcmp(out.text, direct.text) && out.frames == direct.frames);
    TEST_CHECK(received == VJO_VOCR_FL10_BYTES && kernel_opens == 1 && out.resident_bytes == PAGE_ROUND(VJO_VOCR_FL10_BYTES));
    TEST_CHECK(out.model_need_weights == PAGE_ROUND(VJO_VOCR_H15_BYTES) && out.need_weights == PAGE_ROUND(VJO_VOCR_FL10_BYTES));
    TEST_CHECK(peak_bytes <= 4u*1024u*1024u + h15); /* never more than the H15 job would have used */
    printf("  H15 needs %u B with its job, %lld free above the reserve: ran FL10 (%u B peak)\n",
           h15, (long long)game_free - 4*1024*1024, peak_bytes);
    finish(&out);
    /* Enough memory again: H15 runs, FL10's block released first. */
    request = setup_job("H15_w8.vocr", 1); g.mevf = 5; kernel_waits = 1;
    game_free = 64*1024*1024;
    TEST_CHECK(!vjo_game_ocr_run_request(&request, &out) && !out.fallback && out.lines == 1);
    TEST_CHECK(received == VJO_VOCR_H15_BYTES && weights_released == 1 && out.resident_bytes == PAGE_ROUND(VJO_VOCR_H15_BYTES));
    finish(&out);
    submit_fallback = ""; game_free = 16*1024*1024;
}
static void pin_rejects_renamed_same_sized_model_through_real_broker(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Pinned release weights absent; renamed-model hash test skipped"); return; }
    VjoGameOcrRequest request = setup(VJO_VOCR3_FL10_NAME);
    VjoGameOcrResult out;
    /* Simulate an installation that renamed v0.2.0's same-sized FL10 to VOCR3's name. */
    substitute_legacy = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&request, &out) == VJO_E_OCR_MODEL);
    TEST_CHECK(!out.text[0] && out.lines == 1 && !out.io_error && !out.cleanup_status);
    TEST_CHECK(received == VJO_VOCR_FL10_BYTES && kernel_opens == 1 && kernel_closes == 1);
    finish(&out);
}
static void real_model_failure_recovery(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Pinned release weights absent; real-model failure integration skipped"); return; }
    for (int failure=0; failure<7; failure++) {
        VjoGameOcrRequest request = setup("FL10_w8.vocr"); VjoGameOcrResult out;
        if (failure == 0) open_error = (int)0x80010013u;
        if (failure == 1) bad_length = 1;
        if (failure == 2) read_error = (int)0x80010005u;
        if (failure == 3) early_eof = 1;
        if (failure == 4) corrupt = 1;
        if (failure == 5) cancel_after = 3;
        if (failure == 6) close_error = (int)0x80010005u;
        int rc = vjo_game_ocr_run_request(&request, &out);
        TEST_CHECK(rc == (failure == 5 ? VJO_E_CANCELLED : failure == 6 ? VJO_E_OCR_UNAVAILABLE : VJO_E_OCR_MODEL));
        TEST_CHECK(!out.text[0] && !live && !user_opens);
        if (failure == 0) TEST_CHECK(out.io_error == open_error && !kernel_closes);
        if (failure == 1) TEST_CHECK(out.io_error == VJO_ERR_FORMAT);
        if (failure == 2) TEST_CHECK(out.io_error == read_error);
        if (failure == 6) {
            TEST_CHECK(file && model_owned && out.cleanup_status && model_seq == request.seq);
            TEST_CHECK(!vjoOcrComplete(&out)); /* Failure retains the claimed job. */
            pending_result = out; claim_outstanding = 1; close_error = 0;
            TEST_CHECK(!vjo_game_ocr_worker_stop() && !file && !model_owned);
            caller = 3;
            TEST_CHECK(retail_return(capture_request(VJO_CAPTURE_ONCE)) > 0);
            g.capture_state = CAPTURE_IDLE; buffers_free();
            continue;
        }
        finish(&out);
    }
}
static void queued_open_timeout_and_drain(void)
{
    /* No weights needed: Open never reaches the filesystem before Close. */
    VjoGameOcrRequest request = setup("FL10_w8.vocr");
    active_seq = request.seq; schedule_kernel = 0; VjoFile out;
    TEST_CHECK(file_open(NULL, "ignored", &out) == -1 && io_error == VJO_ERR_TIMEOUT);
    TEST_CHECK(model_owned && model_seq == request.seq && !kernel_opens);
    active_seq = 0; file_close(NULL);
    TEST_CHECK(model_owned && io_error == VJO_ERR_TIMEOUT);
    schedule_kernel = 1; TEST_CHECK(!vjo_game_ocr_worker_stop());
    TEST_CHECK(!model_owned && !kernel_opens && !file && !locks);
    VjoGameOcrResult result = {.size=sizeof(result), .seq=request.seq, .rc=VJO_E_CANCELLED};
    TEST_CHECK(!vjoOcrComplete(&result));
    caller = 3; TEST_CHECK(retail_return(capture_request(VJO_CAPTURE_ONCE)) > 0);
    g.capture_state = CAPTURE_IDLE; buffers_free();
}
static void native_status_contract(void)
{
    const int errors[] = {VJO_ERR_NO_GAME, VJO_ERR_NO_MEMORY, VJO_ERR_BUSY, VJO_ERR_TIMEOUT,
        VJO_ERR_FORMAT, VJO_ERR_ARG, VJO_ERR_COPY, VJO_ERR_PERM, VJO_ERR_CANCELLED};
    for (unsigned i=0; i<sizeof(errors)/sizeof(errors[0]); i++) {
        TEST_CHECK(errors[i] < 0 && ((uint32_t)errors[i] & 0xC0000000u) == 0x80000000u);
        TEST_CHECK(retail_return(errors[i]) == errors[i]);
        TEST_CHECK((uint32_t)errors[i] == 0xBFFFFFFFu-i);
        for (unsigned j=0; j<i; j++) TEST_CHECK(errors[i] != errors[j]);
    }
    TEST_CHECK(retail_return(-3) == (int32_t)0xBFFFFFFDu && retail_return(-3) != -3);
    const int unchanged[] = {0,1,13,657664, (int32_t)0x80010013u, (int32_t)0x8001000Du, (int32_t)0x80010005u};
    for (unsigned i=0; i<sizeof(unchanged)/sizeof(unchanged[0]); i++) TEST_CHECK(retail_return(unchanged[i]) == unchanged[i]);
}
TEST_LIST = {
    {"API13 stable statuses survive retail return encoding; native errno unchanged", native_status_contract},
    {"actual game worker and kernel broker load every pinned trial model despite game ENODEV", real_models_cross_namespace},
    {"real weights: waited 32 KiB reads, no game sleeps, resident reuse and re-read on a new file time", real_models_resident_through_waiting_broker},
    {"real weights: H15 with an FL10 fallback runs FL10 when H15 does not fit, then H15 again", real_fallback_runs_fl10_when_h15_does_not_fit},
    {"a pin rejects renamed same-sized bytes (v0.2.0 FL10 as VOCR3-FL10) through the real broker", pin_rejects_renamed_same_sized_model_through_real_broker},
    {"actual broker model I/O corruption cancellation close failure and recovery", real_model_failure_recovery},
    {"actual queued Open timeout and later Close drain without weights", queued_open_timeout_and_drain},
    {NULL, NULL}
};
