/* Actual IPC and capture ownership code with bounded user-copy substitutes. */
#include "acutest.h"
#include <psp2kern/host_stubs.h>
typedef struct { size_t size; SceUID modid; } tai_module_info_t;
int taiGetModuleInfoForKernel(SceUID, const char *, tai_module_info_t *);
#include "../../kernel/vjo_kernel.h"
#include "../../kernel/game_ocr.c"
#include "../../kernel/buffers.c"
#include "../../kernel/capture.c"
#include "../../kernel/capture_request.c"

VjoKernelState g;
unsigned syscall_depth;
static SceUID caller;
static int module_missing, module_id, copy_in_fail, copy_out_fail;
static unsigned locks, frees, allocations;
static uint8_t raw_storage[VJO_MAX_W * VJO_MAX_H * 4];
static VjoGameOcrResult message;
static unsigned model_opens, model_reads, model_closes;
static unsigned model_events;
static int file_live, open_error, seek_error, read_error, close_error, eof, short_read, bad_size;
static int during_io;
static uint64_t model_clock;
static uint32_t expected_model_size;
static unsigned done_events, waits, stats;
static int expect_fallback;
static const char *expect_lm; /* API 20: the LM file Open should name */
static int done_pending, wait_runs_worker, stat_error;
static SceIoStat file_stat;
/* The game's frame, as the display hook sees it (32 x 32 A8B8G8R8, pitch 40). */
#define FB_W 32u
#define FB_H 32u
#define FB_PITCH 40u
static uint32_t game_fb[FB_PITCH * FB_H];
static uint32_t fb_fmt;
static unsigned fb_w = FB_W, fb_h = FB_H;
static int frames_come, frame_pid, in_hook, user_block_ok, hook_copy_fail, kstrip_alloc_fail;
static int stall_copy; /* the hook starts the copy and has not finished it (another core) */
static int gone_after_frame; /* the game exits right after its frame, while the rows call is in the kernel */
static unsigned frees_seen_at_exit;
static unsigned rows_events, capture_events, hook_frames, kstrip_allocs, kstrip_frees;
static unsigned char user_rows[64 * 64]; /* the worker's capture block */

static unsigned klogs, delays;
static const char *last_klog = "";
static void (*before_lock)(void); /* another thread, just before a syscall's lock */
void klog(const char *format, ...) { klogs++; last_klog = format; }
SceUID ksceKernelGetProcessId(void) { return caller; }
uint64_t ksceKernelGetSystemTimeWide(void) { return model_clock; }
/* MEV_JOB and MEV_RESULT as the event flag holds them, and what runs on another
 * thread while vjoOcrTake or vjoOcrRead waits for them (the Shell's submission, the
 * game's completion, a cancellation). */
static int job_bit, result_bit;
static unsigned job_events, result_events, job_waits, result_waits;
static void (*during_event_wait)(void);
int ksceKernelSetEventFlag(SceUID uid, unsigned bits)
{
    if (uid == g.mevf && g.mevf > 0 && bits == MEV_ROWS) { rows_events++; return 0; }
    if (uid == g.mevf && g.mevf > 0 && (bits & (MEV_JOB | MEV_RESULT)) && !(bits & ~(MEV_JOB | MEV_RESULT))) {
        TEST_CHECK(locks == 1);
        if (bits & MEV_JOB) { job_events++; job_bit = 1; }
        if (bits & MEV_RESULT) { result_events++; result_bit = 1; }
        return 0;
    }
    if (uid == g.mevf && g.mevf > 0) { TEST_CHECK(bits == MEV_DONE); done_events++; done_pending = 1; return 0; }
    if (uid == g.evf && bits == VJO_EV_CAPTURE_DONE) { capture_events++; return 0; }
    TEST_CHECK(uid == g.ievf && bits == IEV_MODEL); model_events++; return 0;
}
int ksceKernelClearEventFlag(SceUID uid, unsigned bits)
{
    TEST_CHECK(uid == g.mevf && (bits == ~MEV_ROWS || bits == ~MEV_JOB || bits == ~MEV_RESULT));
    if (bits == ~MEV_JOB) { TEST_CHECK(locks == 1); job_bit = 0; } /* cleared under the lock: no lost wakeup */
    if (bits == ~MEV_RESULT) { TEST_CHECK(locks == 1); result_bit = 0; }
    return 0;
}
int ksceKernelDelayThread(SceUInt us) { model_clock += us; delays++; return 0; }
SceUID ksceKernelFindMemBlockByAddr(const void *addr, SceSize size) { (void)addr; (void)size; return 40; }
/* The worker's memory: only its capture block counts as a block of process 7. */
SceUID ksceKernelFindProcMemBlockByAddr(SceUID pid, const void *addr, SceSize size)
{
    const unsigned char *p = addr;
    TEST_CHECK(size == 0);
    if (pid != 7 || !user_block_ok || p < user_rows || p >= user_rows + sizeof(user_rows)) return -1;
    return 41;
}
/* The game submits a frame: the display hook runs in its thread, no lock held. */
static void game_frame(void)
{
    in_hook = 1; hook_frames++;
    game_ocr_rows_hook(frame_pid, (uintptr_t)game_fb, FB_PITCH, fb_fmt, fb_w, fb_h);
    in_hook = 0;
}
static void model_tick(void);
/* A model syscall waiting for the kernel worker: g.lock is not held, so the
 * worker can run (wait_runs_worker) and finish the queued operation. */
int ksceKernelWaitEventFlag(SceUID uid, unsigned bits, unsigned wait, unsigned *out, SceUInt *timeout)
{
    if (bits == MEV_ROWS) { /* a rows call waits for the game's next frame, without the lock */
        TEST_ASSERT(uid == g.mevf && !locks && syscall_depth == 1 && timeout && *timeout <= 50000);
        if (stall_copy && __sync_bool_compare_and_swap(&rows.state, ROWS_PENDING, ROWS_COPYING)) {
            model_clock += *timeout;
            return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        }
        if (frames_come) {
            frames_come--; game_frame();
            if (gone_after_frame) { gone_after_frame = 0; game_ocr_process_gone(7); frees_seen_at_exit = kstrip_frees; }
            if (out) *out = MEV_ROWS;
            return 0;
        }
        model_clock += *timeout;
        return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    }
    if (bits == MEV_JOB || bits == MEV_RESULT) { /* vjoOcrTake / vjoOcrRead, without the lock */
        int *bit = bits == MEV_JOB ? &job_bit : &result_bit;
        TEST_ASSERT(uid == g.mevf && !locks && syscall_depth == 1 && timeout);
        TEST_CHECK(wait == (SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT));
        TEST_CHECK(*timeout == (bits == MEV_JOB ? 100000u : 50000u));
        if (bits == MEV_JOB) job_waits++; else result_waits++;
        if (during_event_wait) { /* another thread's syscall meanwhile */
            void (*fn)(void) = during_event_wait;
            SceUID saved = caller; unsigned depth = syscall_depth;
            during_event_wait = NULL; syscall_depth = 0; fn(); caller = saved; syscall_depth = depth;
        }
        if (*bit) { *bit = 0; if (out) *out = bits; return 0; }
        model_clock += *timeout;
        return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    }
    TEST_ASSERT(uid == g.mevf && bits == MEV_DONE && !locks && syscall_depth == 1);
    TEST_CHECK(wait == (SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT) && timeout && *timeout <= 50000);
    waits++;
    if (!done_pending && wait_runs_worker) {
        unsigned depth = syscall_depth; syscall_depth = 0; model_tick(); syscall_depth = depth;
    }
    if (done_pending) { done_pending = 0; if (out) *out = MEV_DONE; return 0; }
    model_clock += *timeout; /* nothing finished in time */
    return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
}
int ksceIoGetstatByFd(SceUID fd, SceIoStat *out)
{
    TEST_ASSERT(caller == 0 && !locks && !syscall_depth && file_live && fd == 31);
    stats++;
    if (stat_error) return stat_error;
    *out = file_stat; out->st_size = expected_model_size; return 0;
}
static void interrupt_io(void)
{
    int action = during_io; during_io = 0;
    TEST_ASSERT(!locks && !syscall_depth);
    if (action == 1) { /* Foreground cancellation during unlocked file I/O. */
        VJO_LOCK(); g.game_active = 0; game_ocr_foreground_changed_locked(); VJO_UNLOCK();
    } else if (action == 2) game_ocr_process_gone(7);
    else if (action == 3) {
        VJO_LOCK(); g.shell_pid = 4; game_ocr_shell_changed_locked(); VJO_UNLOCK();
    } else if (action == 4) {
        SceUID saved = caller; caller = 7;
        TEST_CHECK(vjoOcrModelClose(request.seq) == VJO_ERR_BUSY);
        caller = saved;
    }
}
SceUID ksceIoOpen(const char *path, int flags, unsigned mode)
{
    TEST_ASSERT(caller == 0 && !locks && !syscall_depth && !file_live);
    char expected[320]; snprintf(expected,sizeof(expected),"%s/%s",VJO_VOCR_MODEL_ROOT,
                                 expect_lm ? expect_lm : expect_fallback ? request.fallback : request.model);
    TEST_CHECK(!strcmp(path, expected) && flags == SCE_O_RDONLY);
    model_opens++; interrupt_io();
    if (open_error) return open_error;
    file_live = 1; return 31;
}
SceOff ksceIoLseek(SceUID fd, SceOff offset, int whence)
{ TEST_CHECK(fd == 31 && file_live && !locks && !syscall_depth); return seek_error ? (SceOff)seek_error : (SceOff)expected_model_size + bad_size; }
int ksceIoPread(SceUID fd, void *out, SceSize bytes, SceOff offset)
{
    TEST_ASSERT(caller == 0 && !locks && !syscall_depth && file_live && fd == 31);
    TEST_CHECK(bytes <= VJO_GAME_OCR_MODEL_CHUNK && offset >= 0 && offset + bytes <= expected_model_size);
    model_reads++; interrupt_io();
    if (read_error) return read_error;
    if (eof) return 0;
    if (short_read && bytes > 37) bytes = 37;
    for (unsigned i=0; i<bytes; i++) ((unsigned char *)out)[i] = (unsigned char)((offset+i)%251);
    return (int)bytes;
}
int ksceIoClose(SceUID fd)
{
    TEST_ASSERT(caller == 0 && !locks && !syscall_depth && file_live && fd == 31);
    model_closes++;
    if (close_error) return close_error;
    file_live = 0; return 0;
}
static void model_tick(void)
{ SceUID saved = caller; caller = 0; game_ocr_model_tick(); caller = saved; }
static int record_unlocks, unlock_waiters[8], lock_waiters[8];
static unsigned unlock_count, lock_count;
int ksceKernelLockMutex(SceUID uid, int count, void *timeout)
{
    (void)count; (void)timeout;
    if (before_lock) before_lock();
    TEST_CHECK(uid == g.lock && locks == 0); locks++;
    if (record_unlocks && syscall_depth == 1 && lock_count < 8) lock_waiters[lock_count++] = waiters;
    return 0;
}
/* The waiter count as another thread (module_stop) would see it right after each unlock
 * inside a syscall: it must already count a caller about to wait, and no longer count
 * one that has left the wait (the count is changed under the lock, never after it). */
int ksceKernelUnlockMutex(SceUID uid, int count)
{
    (void)count; TEST_CHECK(uid == g.lock && locks == 1); locks--;
    if (record_unlocks && syscall_depth == 1 && unlock_count < 8) unlock_waiters[unlock_count++] = waiters;
    return 0;
}
int taiGetModuleInfoForKernel(SceUID pid, const char *name, tai_module_info_t *info)
{
    TEST_CHECK(pid == caller && syscall_depth == 1 && locks == 0);
    TEST_CHECK(!strcmp(name, VJO_GAME_OCR_MODULE) && info->size == sizeof(*info));
    info->modid = module_id;
    return module_missing ? -1 : 0;
}
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize bytes)
{
    TEST_CHECK(syscall_depth <= 1);
    if (copy_in_fail || !src) return -1;
    memcpy(dst, src, bytes); return 0;
}
int ksceKernelMemcpyKernelToUser(void *dst, const void *src, SceSize bytes)
{
    /* The hook writes the worker's rows in the game's thread with no lock; every
     * other copy out is a syscall's, under the lock. */
    if (in_hook) { TEST_CHECK(!locks); if (hook_copy_fail || !dst) return -1; memcpy(dst, src, bytes); return 0; }
    TEST_CHECK(syscall_depth == 1 && locks == 1);
    if (copy_out_fail || !dst) return -1;
    memcpy(dst, src, bytes); return 0;
}
static unsigned char kstrip_storage[VJO_GAME_OCR_KSTRIP_MAX];
SceUID ksceKernelAllocMemBlock(const char *name, unsigned type, SceSize bytes, void *opt)
{
    (void)opt;
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW);
    if (!strcmp(name, "VjoOcrStrip")) {
        TEST_CHECK(bytes <= sizeof(kstrip_storage) && !(bytes & 4095) && locks == 1);
        kstrip_allocs++;
        return kstrip_alloc_fail ? (int)0x80024302u : 28;
    }
    TEST_CHECK(bytes <= sizeof(raw_storage));
    allocations++; return 27;
}
int ksceKernelGetMemBlockBase(SceUID uid, void **base)
{
    TEST_CHECK(uid == 27 || uid == 28); *base = uid == 28 ? (void *)kstrip_storage : (void *)raw_storage; return 0;
}
int ksceKernelFreeMemBlock(SceUID uid)
{
    TEST_CHECK(uid == 27 || uid == 28);
    if (uid == 28) kstrip_frees++; else frees++;
    return 0;
}

static void setup(void)
{
    memset(&g, 0, sizeof(g));
    game_ocr_init();
    buffers_kstrip_free();
    g.lock = 1; g.shell_pid = 3; g.game_pid = 7; g.game_active = VJO_GAME; g.ocr_epoch = 1;
    g.evf = 2; g.ievf = 6; g.mevf = 5;
    g.capture_state = CAPTURE_IDLE;
    g.capture_seq = g.done_seq = 11; g.fb_w = FB_W; g.fb_h = FB_H;
    for (unsigned y = 0; y < FB_H; y++)
        for (unsigned x = 0; x < FB_PITCH; x++) /* R, G, B, A bytes: R = x * 7, G = y * 5, B = x ^ y */
            game_fb[y * FB_PITCH + x] = 0xFF000000u | ((x ^ y) & 0xFFu) << 16 | ((y * 5u) & 0xFFu) << 8 | ((x * 7u) & 0xFFu);
    fb_fmt = VJO_FB_A8B8G8R8; fb_w = FB_W; fb_h = FB_H;
    frames_come = 0; frame_pid = 7; in_hook = 0; user_block_ok = 1; hook_copy_fail = kstrip_alloc_fail = 0;
    stall_copy = gone_after_frame = 0;
    kledger_init();
    rows_events = capture_events = hook_frames = kstrip_allocs = kstrip_frees = 0;
    memset(user_rows, 0xCC, sizeof(user_rows));
    caller = 7; module_id = 99;
    module_missing = copy_in_fail = copy_out_fail = 0;
    locks = allocations = frees = syscall_depth = 0;
    memset(&message, 0, sizeof(message));
    model_clock = 1000000; expected_model_size = VJO_VOCR_FL10_BYTES;
    model_opens = model_reads = model_closes = model_events = 0;
    file_live = open_error = seek_error = read_error = close_error = eof = short_read = bad_size = during_io = 0;
    done_events = waits = stats = 0; done_pending = wait_runs_worker = stat_error = 0; expect_fallback = 0; expect_lm = NULL;
    job_bit = result_bit = 0; job_events = result_events = job_waits = result_waits = 0; during_event_wait = NULL;
    record_unlocks = 0; unlock_count = lock_count = 0; klogs = delays = 0; last_klog = ""; before_lock = NULL;
    memset(&file_stat, 0, sizeof(file_stat));
    file_stat.st_mtime = (SceDateTime){2026, 10, 11, 9, 43, 58, 123456};
    TEST_ASSERT(vjoOcrRegister() == 0);
    caller = 3;
}

static VjoGameOcrRequest make_request(void)
{
    VjoGameOcrRequest r = {0};
    r.size = sizeof(r);
    strcpy(r.model, "FL10_w8.vocr");
    strcpy(r.model_dir, "ux0:data/VitaJPOverlay/ocr");
    return r;
}

static uint32_t submit(void)
{
    VjoGameOcrRequest r = make_request();
    caller = 3;
    int seq = vjoOcrSubmit(&r);
    TEST_ASSERT(seq > 0);
    return (uint32_t)seq;
}

static void take(uint32_t seq)
{
    VjoGameOcrRequest r = {0};
    caller = 7;
    TEST_ASSERT(vjoOcrTake(&r) == 0);
    /* Submit filled in the capture this job takes and the region in the last frame. */
    TEST_CHECK(r.size == sizeof(r) && r.seq == seq && r.capture_seq > 11);
    TEST_CHECK(r.x == 0 && r.y == 0 && r.width == 32 && r.height == 32 && r.fb_width == 32 && r.fb_height == 32);
    memset(&message, 0, sizeof(message));
    message.size = sizeof(message); message.seq = seq;
    strcpy(message.text, "猫");
}

/* The hand-off without polls: an idle vjoOcrTake waits (once, at most 100 ms,
 * without the lock) for vjoOcrSubmit's MEV_JOB, a pending vjoOcrRead (once, at
 * most 50 ms) for vjoOcrComplete's MEV_RESULT. A job or result already there
 * is answered without waiting; the bit is cleared under the lock first, so one
 * set while the caller is between its check and its wait is not lost. */
static void shell_submits(void) { VjoGameOcrRequest r = make_request(); caller = 3; TEST_CHECK(vjoOcrSubmit(&r) > 0); }
static void game_completes(void) { caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0); }
static void shell_cancels(void) { caller = 3; TEST_CHECK(vjoOcrCancel(request.seq) == 0); }
static void game_exits(void) { game_ocr_process_gone(7); }
static int waiters_seen;
static void module_stops(void) /* module_stop's shutdown, on another thread */
{
    waiters_seen = waiters;
    VJO_LOCK(); TEST_CHECK(game_ocr_shutdown_locked() == 0); VJO_UNLOCK();
}

static void take_wakes_on_submit(void)
{
    VjoGameOcrRequest r = {0};
    setup(); caller = 7;
    uint64_t t0 = model_clock;
    TEST_CHECK(vjoOcrTake(&r) == 1 && job_waits == 1); /* idle: one bounded wait, then 1 */
    TEST_CHECK(model_clock - t0 == 100000);
    /* a submission while the game worker waits: claimed at once, no timeout passes */
    t0 = model_clock; during_event_wait = shell_submits; caller = 7;
    TEST_CHECK(vjoOcrTake(&r) == 0 && job_waits == 2 && job_events == 1);
    TEST_CHECK(model_clock == t0 && r.seq == request.seq && claimed && !pending);
    TEST_CHECK(locks == 0 && syscall_depth == 0);
    /* claimed: BUSY at once, no wait */
    TEST_CHECK(vjoOcrTake(&r) == VJO_ERR_BUSY && job_waits == 2);
    /* a job already queued: no wait */
    setup(); uint32_t seq = submit(); caller = 7;
    TEST_CHECK(vjoOcrTake(&r) == 0 && r.seq == seq && job_waits == 0);
    /* an older job's bit left set never makes an idle take skip its wait */
    setup(); seq = submit(); take(seq); TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(job_bit == 1); /* nobody waited for that submission */
    t0 = model_clock;
    TEST_CHECK(vjoOcrTake(&r) == 1 && job_waits == 1 && model_clock - t0 == 100000);
    /* the module stopping while the worker waits: the shutdown wakes it, it leaves at once, and
     * module_stop sees no thread inside the wait any more */
    setup(); caller = 7; during_event_wait = module_stops; t0 = model_clock;
    TEST_CHECK(vjoOcrTake(&r) == VJO_ERR_PERM && waiters_seen == 1 && model_clock == t0);
    VJO_LOCK(); TEST_CHECK(game_ocr_waiters_locked() == 0); VJO_UNLOCK();
    TEST_CHECK(locks == 0 && syscall_depth == 0);
    /* not the registered, current worker: refused without waiting */
    setup(); caller = 8;
    TEST_CHECK(vjoOcrTake(&r) == VJO_ERR_PERM && !job_waits);
    caller = 7; g.game_active = 0;
    TEST_CHECK(vjoOcrTake(&r) == VJO_ERR_PERM && !job_waits);
}

static void read_wakes_on_complete(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    caller = 3;
    uint64_t t0 = model_clock;
    TEST_CHECK(vjoOcrRead(seq, &message) == 1 && result_waits == 1 && model_clock - t0 == 50000);
    /* the game completes while the Shell waits: the result at once */
    memset(&message, 0, sizeof(message)); message.size = sizeof(message); message.seq = seq; strcpy(message.text, "猫");
    t0 = model_clock; during_event_wait = game_completes; caller = 3;
    TEST_CHECK(vjoOcrRead(seq, &message) == 0 && !strcmp(message.text, "猫"));
    TEST_CHECK(result_waits == 2 && result_events == 1 && model_clock == t0);
    TEST_CHECK(locks == 0 && syscall_depth == 0);
    /* completed already: no wait */
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == 0 && result_waits == 2);
    /* cancelled while the Shell waits (its own cancel, the game's exit): CANCELLED after the wait */
    setup(); seq = submit(); take(seq);
    during_event_wait = shell_cancels; caller = 3;
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED && result_waits == 1);
    setup(); seq = submit(); take(seq);
    during_event_wait = game_exits; caller = 3;
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED);
    /* an earlier job's result bit, left set (no read was waiting), never cuts a later read's wait short */
    setup(); seq = submit(); take(seq); TEST_CHECK(vjoOcrComplete(&message) == 0); TEST_CHECK(result_bit == 1);
    uint32_t next = submit(); take(next); caller = 3; t0 = model_clock;
    TEST_CHECK(vjoOcrRead(next, &message) == 1 && result_waits == 1 && model_clock - t0 == 50000);
    /* a queued or claimed job refuses the shutdown (module_stop fails then, and nothing changed) */
    setup(); seq = submit(); caller = 3;
    VJO_LOCK(); TEST_CHECK(game_ocr_shutdown_locked() == VJO_ERR_BUSY && !game_ocr_waiters_locked()); VJO_UNLOCK();
    /* a stale or foreign read: refused without waiting */
    setup(); seq = submit(); caller = 3;
    TEST_CHECK(vjoOcrRead(seq + 1, &message) == VJO_ERR_ARG && !result_waits);
    caller = 4; TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_PERM && !result_waits);
}

/* The read side of the waiter count (audit-overlay-perf-4, B1; its cases): a read can be
 * waiting when the shutdown passes, since the game's exit during the read drops the claim
 * that would refuse it. The shutdown counts that reader and wakes it; every waited read
 * gives its count back; a read of a cancelled job never waits. */
static int stop_rc = 99;
static void game_exits_then_module_stops(void)
{
    game_ocr_process_gone(7); /* its claim goes: the shutdown is not refused */
    VJO_LOCK(); waiters_seen = waiters; stop_rc = game_ocr_shutdown_locked(); VJO_UNLOCK();
}

static void read_waiters_are_counted_and_woken(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    caller = 3; during_event_wait = game_exits_then_module_stops; stop_rc = 99;
    uint64_t t0 = model_clock;
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED);
    TEST_CHECK(stop_rc == 0 && waiters_seen == 1 && model_clock == t0); /* counted, woken at once */
    TEST_MSG("shutdown rc %d, waiters %d, %llu us", stop_rc, waiters_seen, (unsigned long long)(model_clock - t0));
    VJO_LOCK(); TEST_CHECK(game_ocr_waiters_locked() == 0); VJO_UNLOCK();
    /* every waited read gives its count back, or module_stop would fail for good */
    setup(); seq = submit(); take(seq); caller = 3;
    TEST_CHECK(vjoOcrRead(seq, &message) == 1 && result_waits == 1);
    VJO_LOCK(); TEST_CHECK(game_ocr_waiters_locked() == 0); VJO_UNLOCK();
    TEST_CHECK(vjoOcrRead(seq, &message) == 1 && result_waits == 2);
    VJO_LOCK(); TEST_CHECK(game_ocr_waiters_locked() == 0); VJO_UNLOCK();
    /* a read of a job already cancelled answers at once, as main did */
    setup(); seq = submit(); caller = 3; TEST_CHECK(vjoOcrCancel(seq) == 0);
    t0 = model_clock;
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED && !result_waits && model_clock == t0);
}

static void game_suspends(void) { g.game_active = 0; }

/* The count changes under the lock: right after the unlock before a wait it counts the
 * caller, right after the unlock that follows the wait it no longer does. And the re-check
 * after a wait sees what changed meanwhile: a suspend during a read's wait ends it CANCELLED
 * at once, not one read later. */
static void waiter_count_changes_under_the_lock(void)
{
    VjoGameOcrRequest r = {0};
    /* and never between an unlock and the next lock (outside it): the count at the re-check's
     * lock is the one at the wait's unlock (audit-overlay-perf-4b's case) */
    setup(); caller = 7; record_unlocks = 1; unlock_count = lock_count = 0;
    TEST_CHECK(vjoOcrTake(&r) == 1);
    record_unlocks = 0;
    TEST_CHECK(unlock_count == 2 && unlock_waiters[0] == 1 && unlock_waiters[1] == 0);
    TEST_CHECK(lock_count == 2 && lock_waiters[0] == 0 && lock_waiters[1] == unlock_waiters[0]);
    TEST_MSG("take: locks see %d %d, unlocks %d %d", lock_waiters[0], lock_waiters[1], unlock_waiters[0],
             unlock_waiters[1]);
    setup(); uint32_t seq = submit(); take(seq); caller = 3; record_unlocks = 1; unlock_count = lock_count = 0;
    TEST_CHECK(vjoOcrRead(seq, &message) == 1);
    record_unlocks = 0;
    TEST_CHECK(unlock_count == 2 && unlock_waiters[0] == 1 && unlock_waiters[1] == 0);
    TEST_CHECK(lock_count == 2 && lock_waiters[0] == 0 && lock_waiters[1] == unlock_waiters[0]);
    TEST_MSG("read: locks see %d %d, unlocks %d %d", lock_waiters[0], lock_waiters[1], unlock_waiters[0],
             unlock_waiters[1]);
    setup(); seq = submit(); take(seq); caller = 3; during_event_wait = game_suspends;
    uint64_t t0 = model_clock;
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED && result_waits == 1 && model_clock - t0 == 50000);
    setup(); caller = 7; during_event_wait = game_suspends;
    TEST_CHECK(vjoOcrTake(&r) == VJO_ERR_PERM && job_waits == 1);
}

/* module_stop's part (game_ocr_stop): the shutdown, the drain until the woken takes and reads
 * have left, 1 ms of grace after any it woke, and an undone stop when one does not leave in
 * time (cases after audit-overlay-perf-4b's harness of the drain). */
static int stop_result = 99;
static void module_stop_runs(void) { stop_result = game_ocr_stop(); }
static void game_exits_then_module_stop_runs(void) { game_ocr_process_gone(7); stop_result = game_ocr_stop(); }

/* A take inside the wait for the whole drain: 200 polls, the stop undone once and logged,
 * BUSY; the take, woken by the shutdown, re-checks after the undo, gives its count back and
 * answers 1 without a second wait; the next job goes through. */
static void a_failed_stop_keeps_the_transport(void)
{
    VjoGameOcrRequest r = {0};
    setup(); caller = 7; during_event_wait = module_stop_runs; stop_result = 99;
    uint64_t t0 = model_clock;
    unsigned k0 = klogs;
    TEST_CHECK(vjoOcrTake(&r) == 1 && job_waits == 1);
    TEST_CHECK(stop_result == VJO_ERR_BUSY && delays == 200 && model_clock - t0 == 200000);
    TEST_MSG("drain: %u delays, %llu us", delays, (unsigned long long)(model_clock - t0));
    TEST_CHECK(klogs - k0 == 1 && strstr(last_klog, "stop undone"));
    VJO_LOCK(); TEST_CHECK(game_ocr_waiters_locked() == 0 && !stopping); VJO_UNLOCK();
    caller = 7; TEST_CHECK(vjoOcrRegister() == 0);
    uint32_t seq = submit(); take(seq); TEST_CHECK(vjoOcrComplete(&message) == 0);
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == 0 && !strcmp(message.text, "猫"));
    TEST_CHECK(locks == 0 && syscall_depth == 0);
    /* the same with a reader: the game exits during the read, the stop gives up; CANCELLED, the
     * count back, a new job works */
    setup(); seq = submit(); take(seq); caller = 3; during_event_wait = game_exits_then_module_stop_runs;
    stop_result = 99; k0 = klogs;
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED && result_waits == 1);
    TEST_CHECK(stop_result == VJO_ERR_BUSY && delays == 200 && klogs - k0 == 1);
    VJO_LOCK(); TEST_CHECK(game_ocr_waiters_locked() == 0 && !stopping); VJO_UNLOCK();
    caller = 7; TEST_CHECK(vjoOcrRegister() == 0);
    seq = submit(); take(seq); TEST_CHECK(vjoOcrComplete(&message) == 0);
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == 0 && !strcmp(message.text, "猫"));
    /* an undone stop keeps a result the Shell has not read yet: no invalidation */
    setup(); seq = submit(); take(seq); TEST_CHECK(vjoOcrComplete(&message) == 0);
    caller = 7; during_event_wait = module_stop_runs; stop_result = 99;
    TEST_CHECK(vjoOcrTake(&r) == 1 && stop_result == VJO_ERR_BUSY);
    caller = 3; memset(&message, 0, sizeof(message)); message.size = sizeof(message);
    TEST_CHECK(vjoOcrRead(seq, &message) == 0 && !strcmp(message.text, "猫"));
}

/* The drain's paths: a job queued or claimed refuses the stop (nothing changes); no waiter:
 * no delay; a waiter leaving after 3 polls: 3 polls and 1 ms; a woken waiter that leaves before
 * the first poll: still the 1 ms; a successful stop refuses the worker from then on. */
static unsigned locks_seen, leave_at;
static uint64_t left_at;
static void waiter_leaves_on_lock(void)
{
    if (++locks_seen == leave_at) { waiters = 0; left_at = model_clock; }
}
static void stop_drains_its_waiters(void)
{
    setup(); uint32_t seq = submit(); (void)seq;
    TEST_CHECK(game_ocr_stop() == VJO_ERR_BUSY && !stopping && !delays);
    setup();
    TEST_CHECK(game_ocr_stop() == 0 && stopping && !delays);
    caller = 7; TEST_CHECK(vjoOcrRegister() == VJO_ERR_NO_GAME);
    setup(); waiters = 1; locks_seen = 0; leave_at = 5; before_lock = waiter_leaves_on_lock;
    uint64_t t0 = model_clock;
    TEST_CHECK(game_ocr_stop() == 0);
    before_lock = NULL;
    TEST_CHECK(delays == 4 && model_clock - t0 == 4000 && model_clock - left_at == 1000);
    TEST_MSG("%u delays, %llu us, %llu us after the count went to 0", delays, (unsigned long long)(model_clock - t0),
             (unsigned long long)(model_clock - left_at));
    TEST_CHECK(job_bit && result_bit); /* the shutdown woke both kinds */
    setup(); waiters = 1; locks_seen = 0; leave_at = 2; before_lock = waiter_leaves_on_lock;
    TEST_CHECK(game_ocr_stop() == 0);
    before_lock = NULL;
    TEST_CHECK(delays == 1 && model_clock - left_at == 1000);
    TEST_MSG("%u delays, %llu us after the count went to 0", delays, (unsigned long long)(model_clock - left_at));
}

static void registration_and_permissions(void)
{
    setup(); caller = 8;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_NO_GAME);
    TEST_CHECK(vjoOcrTake(&request) == VJO_ERR_PERM);
    TEST_CHECK(vjoOcrSubmit(&request) == VJO_ERR_PERM);
    TEST_CHECK(vjoOcrRead(1, &message) == VJO_ERR_PERM);
    TEST_CHECK(vjoOcrCancel(1) == VJO_ERR_PERM);
    caller = 7; module_missing = 1;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_PERM);
    module_missing = 0; g.game_active = 0;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_NO_GAME);
    TEST_CHECK(locks == 0 && syscall_depth == 0);
}

static void complete_and_replacement(void)
{
    setup(); uint32_t seq = submit();
    TEST_CHECK(game_ocr_capture_busy_locked());
    TEST_CHECK(vjoOcrRead(seq, &message) == 1);
    take(seq);
    TEST_CHECK(vjoOcrRegister() == 0); /* Repeated registration keeps the claim. */
    TEST_CHECK(vjoOcrTake(&request) == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrCancelled(seq) == 0);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(!game_ocr_capture_busy_locked());
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG);
    caller = 3;
    TEST_CHECK(vjoOcrRead(seq, &message) == 0 && !strcmp(message.text, "猫"));
    uint32_t second = submit();
    TEST_CHECK(second > seq && vjoOcrRead(seq, &message) == VJO_ERR_ARG);
    take(second); message.seq = seq;
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG && game_ocr_capture_busy_locked());
}

static void capture_pins_queued_and_claimed(void)
{
    setup(); uint32_t seq = submit();
    TEST_CHECK(capture_request(0) == VJO_ERR_BUSY);
    TEST_CHECK(capture_request(VJO_CAPTURE_DISCARD) == VJO_ERR_BUSY);
    take(seq);
    TEST_CHECK(capture_request(0) == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(capture_request(0) > 0);
}

static void cancellation_drains_reader(void)
{
    setup(); uint32_t seq = submit(); take(seq); caller = 3;
    TEST_CHECK(vjoOcrCancel(seq) == 0);
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED);
    TEST_CHECK(capture_request(0) == VJO_ERR_BUSY);
    caller = 7;
    TEST_CHECK(vjoOcrCancelled(seq) == 1);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(!game_ocr_capture_busy_locked());
    caller = 3;
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED);
    TEST_CHECK(capture_request(0) > 0);
}

static void cancel_before_take(void)
{
    setup(); uint32_t seq = submit();
    TEST_CHECK(vjoOcrCancel(seq) == 0 && !game_ocr_capture_busy_locked());
    caller = 7; VjoGameOcrRequest r;
    TEST_CHECK(vjoOcrTake(&r) == 1);
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG);
}

static void suspended_and_same_pid_epoch(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    g.game_active = 0; g.ocr_epoch++;
    game_ocr_foreground_changed_locked();
    TEST_CHECK(game_ocr_capture_busy_locked());
    TEST_CHECK(vjoOcrCancelled(seq) == 1);
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_NO_GAME);
    g.game_active = VJO_GAME; g.ocr_epoch++;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(vjoOcrRegister() == 0);
    g.capture_epoch = g.ocr_epoch; /* A fresh capture in the resumed session. */
    caller = 3; uint32_t next = submit(); TEST_CHECK(next > seq);
    caller = 7;
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG); /* Old reply cannot claim a new job. */
}

static void foreground_replacement_and_exit(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    g.game_pid = 8; g.game_active = VJO_GAME; g.ocr_epoch++;
    game_ocr_foreground_changed_locked();
    caller = 8; TEST_CHECK(vjoOcrRegister() == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_PERM);
    game_ocr_process_gone(7);
    TEST_CHECK(!game_ocr_capture_busy_locked() && vjoOcrRegister() == 0);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG);
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED);
}

static void shell_restart(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    g.shell_pid = 4; game_ocr_shell_changed_locked();
    TEST_CHECK(game_ocr_capture_busy_locked() && vjoOcrCancelled(seq) == 1);
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_PERM);
    caller = 4; TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_ARG);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(vjoOcrRegister() == 0 && !game_ocr_capture_busy_locked());
    caller = 4; VjoGameOcrRequest r = make_request();
    /* No kernel crop to go stale (API 19): the new Shell's first job is accepted. */
    TEST_CHECK(vjoOcrSubmit(&r) > 0);
}

static void cleanup_failure_and_module_replacement(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    message.cleanup_status = -1; message.rc = -12; message.text[0] = 0;
    TEST_CHECK(vjoOcrComplete(&message) == 0 && game_ocr_capture_busy_locked());
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == 0 && message.cleanup_status == -1);
    TEST_CHECK(capture_request(0) == VJO_ERR_BUSY);
    caller = 7; module_id++;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_BUSY);
    module_id--; message.cleanup_status = 0;
    TEST_CHECK(vjoOcrComplete(&message) == 0 && !game_ocr_capture_busy_locked());
}

static void invalid_capture_and_wire(void)
{
    setup(); VjoGameOcrRequest r = make_request();
    r.size--; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); r.size++;
    r.model[31] = 'x'; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); r.model[31] = 0;
    r.model_dir[255] = 'x'; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); r.model_dir[255] = 0;
    r.flags = 0x4000; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); /* API 20: bits 14-15 are still unknown */
    r.flags = 0x8000; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    r.flags = 0;
    /* A Lens/Anki capture in flight: the OCR job waits for it. */
    g.capture_state = CAPTURE_PENDING; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_BUSY);
    g.capture_state = CAPTURE_IDLE; uint32_t seq = submit(); take(seq);
    message.size--; TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG); message.size++;
    message.text[4095] = 'x'; TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG); message.text[4095] = 0;
    message.cleanup_status = -1; TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG);
    TEST_CHECK(game_ocr_capture_busy_locked());
}

static void user_copy_failures_and_sequence_exhaustion(void)
{
    setup(); VjoGameOcrRequest r = make_request(); copy_in_fail = 1;
    TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_COPY && !game_ocr_capture_busy_locked());
    copy_in_fail = 0; uint32_t seq = submit(); caller = 7; copy_out_fail = 1;
    TEST_CHECK(vjoOcrTake(&r) == VJO_ERR_COPY && !claimed && pending);
    copy_out_fail = 0; take(seq); copy_in_fail = 1;
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_COPY && claimed);
    copy_in_fail = 0; TEST_CHECK(vjoOcrComplete(&message) == 0);
    caller = 3; copy_out_fail = 1; TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_COPY);
    copy_out_fail = 0; sequence = 0x7FFFFFFFu;
    TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_BUSY);
    TEST_CHECK(locks == 0 && syscall_depth == 0);
}

static void shutdown_refuses_claim_and_closes_registration(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    TEST_CHECK(game_ocr_shutdown_locked() == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(game_ocr_shutdown_locked() == 0);
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_NO_GAME);
    caller = 3; VjoGameOcrRequest r = make_request();
    TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_NO_GAME);
}

static int rows_call(uint32_t seq, unsigned y, unsigned n, uint32_t mode, uint32_t flags, unsigned stride, void *dst,
                     VjoGameOcrRowsInfo *info)
{
    VjoGameOcrRows r = {sizeof(r), seq, y, n, mode, flags, stride, 0};
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    caller = 7;
    return vjoOcrCaptureRows(&r, dst, info);
}
static unsigned grey_of(unsigned x, unsigned y)
{
    uint32_t v = game_fb[y * FB_PITCH + x];
    return (77u * (v & 0xFF) + 150u * ((v >> 8) & 0xFF) + 29u * ((v >> 16) & 0xFF)) >> 8;
}
static int rows_match(const unsigned char *dst, unsigned y0, unsigned n, unsigned stride, unsigned x0)
{
    for (unsigned r = 0; r < n; r++)
        for (unsigned x = 0; x < 32; x++)
            if (dst[r * stride + x] != grey_of(x0 + x, y0 + r)) return 0;
    return 1;
}
/* The display hook copies grey rows of the region straight into the worker's
 * block on the game's next frame; the first pass publishes the capture. No
 * kernel block is taken. */
static void rows_hook_copies_grey_and_publishes_the_capture(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    VjoGameOcrRowsInfo info;
    uint32_t done_before = g.done_seq;
    frames_come = 1;
    TEST_CHECK(rows_call(seq, 0, 16, VJO_GAME_OCR_SRC_HOOK, VJO_ROWS_SIG, 40, user_rows, &info) == 16);
    TEST_CHECK(rows_match(user_rows, 0, 16, 40, 0) && user_rows[32] == 0xCC); /* only width bytes per row */
    TEST_CHECK(info.rc == 16 && info.fb_width == 32 && info.fb_pitch == FB_PITCH && info.fb_format == 0);
    TEST_CHECK(hook_frames == 1 && rows_events == 1 && !allocations && !kstrip_allocs && !locks && !syscall_depth);
    TEST_CHECK(g.done_seq == done_before && !capture_events); /* the pass is not over */
    frames_come = 1;
    TEST_CHECK(rows_call(seq, 16, 16, VJO_GAME_OCR_SRC_HOOK, VJO_ROWS_SIG | VJO_ROWS_LAST, 40, user_rows, &info) == 16);
    TEST_CHECK(rows_match(user_rows, 16, 16, 40, 0));
    TEST_CHECK(g.done_seq == request.capture_seq && g.region_seq == request.capture_seq && capture_events == 1);
    TEST_CHECK(g.crop_w == 32 && g.crop_h == 32 && !g.capture_result);
    /* Later reads (the second pass, a line) need no signature. */
    frames_come = 1;
    TEST_CHECK(rows_call(seq, 3, 5, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == 5 && rows_match(user_rows, 3, 5, 32, 0));
    TEST_CHECK(game_ocr_capture_busy_locked()); /* the job still owns the capture */
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0 && !game_ocr_capture_busy_locked());
}
static void rows_refuse_bad_requests_without_arming(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    VjoGameOcrRowsInfo info;
    frames_come = 100;
    caller = 8; { VjoGameOcrRows r = {sizeof(r), seq, 0, 1, VJO_GAME_OCR_SRC_HOOK, 0, 32, 0};
                  TEST_CHECK(vjoOcrCaptureRows(&r, user_rows, &info) == VJO_ERR_PERM); }
    TEST_CHECK(rows_call(seq + 1, 0, 1, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(rows_call(seq, 0, 0, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(rows_call(seq, 0, VJO_GAME_OCR_ROWS_MAX + 1, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(rows_call(seq, 30, 3, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(rows_call(seq, 0, 1, VJO_GAME_OCR_SRC_HOOK, 0, 31, user_rows, &info) == VJO_ERR_ARG); /* stride < width */
    TEST_CHECK(rows_call(seq, 0, 1, 7, 0, 32, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(rows_call(seq, 0, 1, VJO_GAME_OCR_SRC_HOOK, VJO_ROWS_LAST, 32, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(rows_call(seq, 4, 1, VJO_GAME_OCR_SRC_HOOK, VJO_ROWS_SIG, 32, user_rows, &info) == VJO_ERR_ARG);
    /* The destination must lie in one block of the caller: here, the end runs past it. */
    TEST_CHECK(rows_call(seq, 0, 32, VJO_GAME_OCR_SRC_HOOK, 0, 200, user_rows, &info) == VJO_ERR_ARG);
    user_block_ok = 0;
    TEST_CHECK(rows_call(seq, 0, 1, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(!hook_frames && rows.state == ROWS_IDLE && user_rows[0] == 0xCC && !locks && !syscall_depth);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
}
/* No frame within the wait: a timeout, and nothing is armed afterwards; a
 * frame of another process or another size does not satisfy the request. */
static void rows_timeout_other_process_and_size(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    VjoGameOcrRowsInfo info;
    uint64_t before = model_clock;
    TEST_CHECK(rows_call(seq, 0, 4, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_TIMEOUT);
    TEST_CHECK(model_clock - before >= ROWS_WAIT_US && rows.state == ROWS_IDLE && user_rows[0] == 0xCC);
    frame_pid = 8; frames_come = 1000;
    TEST_CHECK(rows_call(seq, 0, 4, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_TIMEOUT);
    TEST_CHECK(user_rows[0] == 0xCC && rows.state == ROWS_IDLE);
    frame_pid = 7; fb_w = 24; frames_come = 1;
    TEST_CHECK(rows_call(seq, 0, 4, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_FORMAT);
    fb_w = FB_W; fb_fmt = 0x12345678u; frames_come = 1;
    TEST_CHECK(rows_call(seq, 0, 4, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_FORMAT);
    TEST_CHECK(info.fb_format == 0x12345678u); /* the log names the format */
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0 && !game_ocr_capture_busy_locked());
}
/* Cancellation during the wait ends it; the hook copies nothing afterwards. */
static void rows_cancelled_while_waiting(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    VjoGameOcrRowsInfo info;
    caller = 3; TEST_CHECK(vjoOcrCancel(seq) == 0);
    TEST_CHECK(rows_call(seq, 0, 4, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_CANCELLED);
    frames_come = 1; game_frame();
    TEST_CHECK(user_rows[0] == 0xCC && rows.state == ROWS_IDLE);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
}
/* The ladder's last rung: the hook fills a small kernel strip and the call
 * copies it out; the strip is freed before the call returns. */
static void rows_kernel_strip_rung(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    VjoGameOcrRowsInfo info;
    hook_copy_fail = 1; frames_come = 1;
    TEST_CHECK(rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_HOOK, 0, 32, user_rows, &info) == VJO_ERR_COPY);
    TEST_CHECK(user_rows[0] == 0xCC && !kstrip_allocs);
    frames_come = 1;
    TEST_CHECK(rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_KSTRIP, 0, 48, user_rows, &info) == 8);
    TEST_CHECK(rows_match(user_rows, 0, 8, 48, 0));
    TEST_CHECK(kstrip_allocs == 1 && kstrip_frees == 1 && info.kstrip_bytes == 8 * 32 && !info.kstrip_rc);
    TEST_CHECK(vjo_ledger_current(&g_kernel_ledger, VJO_KL_OCR_STRIP) == 0 &&
               vjo_ledger_peak(&g_kernel_ledger, VJO_KL_OCR_STRIP) == 4096);
    TEST_MSG("ledger current %u peak %u", (unsigned)vjo_ledger_current(&g_kernel_ledger, VJO_KL_OCR_STRIP),
             (unsigned)vjo_ledger_peak(&g_kernel_ledger, VJO_KL_OCR_STRIP));
    /* never while the Shell's JPEG capture holds its buffer */
    g.mem_uid = 5;
    TEST_CHECK(rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_KSTRIP, 0, 48, user_rows, &info) == VJO_ERR_NO_MEMORY);
    TEST_CHECK(info.kstrip_rc == VJO_ERR_NO_MEMORY && kstrip_allocs == 1 && rows.state == ROWS_IDLE);
    g.mem_uid = 0;
    kstrip_alloc_fail = 1;
    TEST_CHECK(rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_KSTRIP, 0, 48, user_rows, &info) == (int)0x80024302u);
    TEST_CHECK(info.kstrip_rc == (int)0x80024302u && rows.state == ROWS_IDLE && kstrip_frees == 1);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
}
/* A pending request holds off Lens/Anki captures and Submit; the game exiting
 * clears it without any copy. */
static void rows_pending_pins_and_process_exit(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    VjoGameOcrRowsInfo info;
    (void)info;
    rows.state = ROWS_PENDING; rows.pid = 7; /* as a waiting syscall leaves it */
    VJO_LOCK(); TEST_CHECK(game_ocr_capture_busy_locked()); VJO_UNLOCK();
    game_ocr_process_gone(7);
    TEST_CHECK(rows.state == ROWS_IDLE);
    frames_come = 1; game_frame();
    TEST_CHECK(user_rows[0] == 0xCC);
    /* A copy in progress when the process exits: the kernel worker tidies up. */
    setup(); seq = submit(); take(seq);
    rows.state = ROWS_COPYING; rows.pid = 7;
    game_ocr_process_gone(7);
    TEST_CHECK(rows.state == ROWS_COPYING && rows.orphan);
    rows.state = ROWS_DONE;
    model_tick();
    TEST_CHECK(rows.state == ROWS_IDLE && !rows.orphan);
}

/* A copy the hook started and has not finished when the call gives up: BUSY,
 * the destination stays the hook's (and so does a kernel strip), no new
 * request is armed meanwhile, and once the copy ends the kernel worker returns
 * the state to IDLE so a Lens/Anki capture is not refused for the rest of the game. */
static void rows_copy_outliving_its_call(void)
{
    for (int mode = 0; mode < 2; mode++) {
        setup(); uint32_t seq = submit(); take(seq);
        VjoGameOcrRowsInfo info;
        uint32_t m = mode ? VJO_GAME_OCR_SRC_KSTRIP : VJO_GAME_OCR_SRC_HOOK;
        stall_copy = 1;
        TEST_CHECK(rows_call(seq, 0, 8, m, 0, 48, user_rows, &info) == VJO_ERR_BUSY);
        TEST_CHECK(rows.state == ROWS_COPYING && rows.abandoned && !rows.orphan);
        TEST_CHECK(kstrip_frees == 0 && kstrip_allocs == (unsigned)mode); /* the hook may still write it */
        VJO_LOCK(); TEST_CHECK(game_ocr_capture_busy_locked()); VJO_UNLOCK();
        stall_copy = 0; frames_come = 1;
        TEST_CHECK(rows_call(seq, 8, 8, m, 0, 48, user_rows, &info) == VJO_ERR_BUSY); /* not re-armed */
        TEST_CHECK(rows.state == ROWS_COPYING && frames_come == 1);
        model_tick();
        TEST_CHECK(rows.state == ROWS_COPYING); /* still copying: left alone */
        rows.state = ROWS_DONE;                 /* the hook ends its copy */
        model_tick();
        TEST_CHECK(rows.state == ROWS_IDLE && !rows.abandoned && kstrip_frees == (unsigned)mode);
        TEST_CHECK(rows_call(seq, 0, 8, m, 0, 48, user_rows, &info) == 8); /* usable again */
        caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
        VJO_LOCK(); TEST_CHECK(!game_ocr_capture_busy_locked()); VJO_UNLOCK(); /* Lens/Anki may capture */
    }
}

/* N1: the destination stride is bounded, so the span the kernel checks is the
 * span the hook writes. */
static void rows_stride_is_bounded(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    VjoGameOcrRowsInfo info;
    frames_come = 1;
    TEST_CHECK(rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_HOOK, 0, VJO_MAX_W + 1, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_HOOK, 0, 0x04104105u, user_rows, &info) == VJO_ERR_ARG);
    /* a stride whose span wraps 32 bits: 31 x 0x08421085 + 32 is 59 B mod 2^32, inside the block */
    TEST_CHECK(rows_call(seq, 0, 32, VJO_GAME_OCR_SRC_HOOK, 0, 0x08421085u, user_rows, &info) == VJO_ERR_ARG);
    TEST_CHECK(!info.armed && rows.state == ROWS_IDLE && frames_come == 1 && user_rows[0] == 0xCC);
    TEST_CHECK(rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_HOOK, 0, 48, user_rows, &info) == 8 && info.armed);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
}

/* N2: a copy whose process exited is never overtaken by a new request (BUSY,
 * not armed: the worker asks again), and a process exit while its rows call is
 * still in the kernel frees nothing the call still uses. */
static void rows_process_exit_cannot_pull_state_from_a_call(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    VjoGameOcrRowsInfo info;
    rows.state = ROWS_COPYING; rows.pid = 9; rows.orphan = 1; /* the last game's copy, its process gone */
    frames_come = 1;
    TEST_CHECK(rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_HOOK, 0, 48, user_rows, &info) == VJO_ERR_BUSY);
    TEST_CHECK(!info.armed && rows.pid == 9 && frames_come == 1); /* not armed, the old copy untouched */
    rows.state = ROWS_DONE; model_tick();                      /* it ends; the kernel worker tidies */
    TEST_CHECK(rows.state == ROWS_IDLE && !rows.orphan);
    TEST_CHECK(rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_HOOK, 0, 48, user_rows, &info) == 8 && info.armed);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
    /* The game exits after its frame, while the kernel-strip call is still inside:
     * the strip stays until the call has copied it out. */
    setup(); seq = submit(); take(seq);
    gone_after_frame = 1; frames_come = 1;
    int rc = rows_call(seq, 0, 8, VJO_GAME_OCR_SRC_KSTRIP, 0, 48, user_rows, &info);
    TEST_CHECK(frees_seen_at_exit == 0);                       /* process_gone freed nothing */
    TEST_CHECK(rc == 8 && kstrip_allocs == 1 && kstrip_frees == 1); /* the call freed it, after its copy */
    TEST_CHECK(rows.state == ROWS_IDLE && !rows.in_call && !rows.orphan);
    VJO_LOCK(); TEST_CHECK(!game_ocr_capture_busy_locked()); VJO_UNLOCK();
    /* A call that never returns after its process exited: tidied after ROWS_GONE_US. */
    rows.state = ROWS_DONE; rows.in_call = 1; rows.orphan = 1; rows.gone_us = model_clock;
    model_tick();
    TEST_CHECK(rows.state == ROWS_DONE && rows.in_call);         /* not yet */
    model_clock += ROWS_GONE_US + 1; model_tick();
    TEST_CHECK(rows.state == ROWS_IDLE && !rows.in_call && !rows.orphan);
}

static void open_model(uint32_t seq)
{
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_BUSY && !file_live);
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_BUSY && !model_opens);
    model_tick();
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == (int)VJO_VOCR_FL10_BYTES && file_live);
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == (int)VJO_VOCR_FL10_BYTES && model_opens == 1);
}
static void close_model(uint32_t seq)
{
    TEST_CHECK(vjoOcrModelClose(seq) == VJO_ERR_BUSY);
    model_tick();
    TEST_CHECK(!vjoOcrModelClose(seq) && !file_live);
}
static void model_permissions_paths_and_bounds(void)
{
    setup(); uint32_t seq = submit();
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_PERM);
    take(seq); caller = 8;
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_PERM);
    TEST_CHECK(vjoOcrModelClose(seq) == VJO_ERR_PERM);
    caller = 7;
    TEST_CHECK(vjoOcrModelOpen(seq+1, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_ARG);
    open_model(seq);
    static unsigned char bytes[VJO_GAME_OCR_MODEL_CHUNK + 1];
    TEST_CHECK(vjoOcrModelRead(seq, 0, VJO_GAME_OCR_MODEL_CHUNK + 1, bytes) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrModelRead(seq, UINT32_MAX, 1, bytes) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrModelRead(seq, VJO_VOCR_FL10_BYTES, 1, bytes) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrModelRead(seq, 0, 0, bytes) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrModelRead(seq, 0, 8, NULL) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_BUSY); /* No premature cleanup ack. */
    close_model(seq);
    TEST_CHECK(!vjoOcrComplete(&message));
    setup(); seq = submit(); take(seq);
    strcpy(request.model_dir, "ux0:data/../../tai");
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_ARG && !model_opens);
    strcpy(request.model_dir, VJO_VOCR_MODEL_ROOT); strcpy(request.model, "../secret");
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_ARG && !model_opens);
    TEST_CHECK(!vjoOcrModelClose(seq));
}
static void removed_models_are_refused(void)
{
    TEST_CHECK(sizeof(model_stage)==32768);
    /* The broker opens only the VOCR pins; the removed engines' files (the Meiki
     * recognizer, the VOCR2 trial's FL10-sized model) are unlisted, though they
     * may still be on the memory card. */
    static const char *const removed[] = {"meiki-stream-int8.mnn", "meiki-detect-int8.mnn", "VOCR2_FL10_w8.vocr"};
    for (unsigned i = 0; i < sizeof(removed) / sizeof(removed[0]); i++) {
        TEST_CHECK(!model_size(removed[i]));
        setup(); uint32_t seq=submit(); take(seq);
        strcpy(request.model, removed[i]);
        TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL)==VJO_ERR_ARG && !model_opens && !file_live);
        TEST_CHECK(!vjoOcrModelClose(seq) && !vjoOcrComplete(&message));
        TEST_MSG("%s", removed[i]);
    }
}

/* A pinned name opens only as itself (no path around it) and reads end at its
 * pinned size, the last chunk included. */
static void exact_pin_keeps_canonical_path_and_final_chunk_bounds(void)
{
    TEST_CHECK(!model_size("../" VJO_VOCR3_FL10_NAME) && !model_size(VJO_VOCR3_FL10_NAME "/"));
    setup(); uint32_t seq = submit(); take(seq);
    strcpy(request.model, VJO_VOCR3_FL10_NAME);
    TEST_CHECK(model_size(VJO_VOCR3_FL10_NAME) == VJO_VOCR_FL10_BYTES);
    expected_model_size = VJO_VOCR_FL10_BYTES;
    open_model(seq);
    unsigned char tail[2] = {0xCC, 0xCC};
    TEST_CHECK(vjoOcrModelRead(seq, VJO_VOCR_FL10_BYTES - 1, 2, tail) == VJO_ERR_ARG);
    TEST_CHECK(vjoOcrModelRead(seq, VJO_VOCR_FL10_BYTES - 1, 1, tail) == VJO_ERR_BUSY);
    model_tick();
    TEST_CHECK(vjoOcrModelRead(seq, VJO_VOCR_FL10_BYTES - 1, 1, tail) == 1 && tail[1] == 0xCC);
    close_model(seq);
    TEST_CHECK(!vjoOcrComplete(&message));
}

static void model_short_read_copy_retry_and_errors(void)
{
    setup(); uint32_t seq = submit(); take(seq); open_model(seq);
    unsigned char bytes[8192]; memset(bytes, 0xCC, sizeof(bytes));
    short_read = 1;
    TEST_CHECK(vjoOcrModelRead(seq, 9, sizeof(bytes), bytes) == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrModelRead(seq, 10, sizeof(bytes), bytes) == VJO_ERR_ARG);
    model_tick(); copy_out_fail = 1;
    TEST_CHECK(vjoOcrModelRead(seq, 9, sizeof(bytes), bytes) == VJO_ERR_COPY && bytes[0] == 0xCC);
    copy_out_fail = 0;
    TEST_CHECK(vjoOcrModelRead(seq, 9, sizeof(bytes), bytes) == 37 && model_reads == 1);
    TEST_CHECK(bytes[0] == 9 && bytes[36] == 45 && bytes[37] == 0xCC);
    short_read = 0; eof = 1;
    TEST_CHECK(vjoOcrModelRead(seq, 0, 8, bytes) == VJO_ERR_BUSY); model_tick();
    TEST_CHECK(vjoOcrModelRead(seq, 0, 8, bytes) == 0);
    eof = 0; read_error = (int)0x80010005u;
    TEST_CHECK(vjoOcrModelRead(seq, 0, 8, bytes) == VJO_ERR_BUSY); model_tick();
    TEST_CHECK(vjoOcrModelRead(seq, 0, 8, bytes) == read_error);
    close_model(seq);
    for (unsigned failure=0; failure<3; failure++) {
        setup(); seq = submit(); take(seq);
        if (failure == 0) open_error = (int)0x80010013u;
        if (failure == 1) seek_error = (int)0x80010005u;
        if (failure == 2) bad_size = 1;
        TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_BUSY); model_tick();
        int rc = vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL);
        TEST_CHECK(rc == (failure == 0 ? open_error : failure == 1 ? seek_error : VJO_ERR_FORMAT));
        TEST_CHECK(file_live == (failure != 0));
        if (failure != 0) {
            close_error = (int)0x80010005u;
            TEST_CHECK(vjoOcrModelClose(seq) == VJO_ERR_BUSY); model_tick();
            TEST_CHECK(vjoOcrModelClose(seq) == close_error && file_live);
            close_error = 0;
        }
        close_model(seq);
        TEST_CHECK(!vjoOcrComplete(&message));
    }
}
static void model_cancel_exit_restart_and_late_io(void)
{
    for (int action=1; action<=4; action++) for (int at_read=0; at_read<2; at_read++) {
        setup(); uint32_t seq = submit(); take(seq);
        unsigned char bytes[64]; memset(bytes, 0xCC, sizeof(bytes));
        if (at_read) { open_model(seq); TEST_CHECK(vjoOcrModelRead(seq, 0, sizeof(bytes), bytes) == VJO_ERR_BUSY); }
        else TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_BUSY);
        during_io = action; model_tick();
        /* The operation completed after cancellation/exit/restart/Close;
         * ownership still pins the live descriptor until the kernel drains it. */
        TEST_CHECK(file_live && game_ocr_capture_busy_locked());
        TEST_CHECK(game_ocr_shutdown_locked() == VJO_ERR_BUSY);
        if (action != 4) TEST_CHECK(vjoOcrModelRead(seq, 0, sizeof(bytes), bytes) < 0);
        TEST_CHECK(bytes[0] == 0xCC);
        model_tick(); TEST_CHECK(!file_live);
        if (action != 2) TEST_CHECK(!vjoOcrModelClose(seq));
        if (action == 2) TEST_CHECK(!game_ocr_capture_busy_locked() && !game_ocr_shutdown_locked());
    }
    setup(); uint32_t seq = submit(); take(seq);
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrModelClose(seq) == VJO_ERR_BUSY); model_tick();
    TEST_CHECK(!file_live && !model_opens && !vjoOcrModelClose(seq)); /* Cancel queued Open. */
}
static void model_failed_close_and_orphan_recovery(void)
{
    setup(); uint32_t seq = submit(); take(seq); open_model(seq);
    close_error = (int)0x80010005u;
    TEST_CHECK(vjoOcrModelClose(seq) == VJO_ERR_BUSY); model_tick();
    TEST_CHECK(vjoOcrModelClose(seq) == close_error && file_live);
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrModelClose(seq+1) == VJO_ERR_ARG && file_live);
    close_error = 0; close_model(seq); TEST_CHECK(!vjoOcrComplete(&message));
    setup(); seq = submit(); take(seq); open_model(seq);
    close_error = (int)0x80010005u; g.game_active = 0; game_ocr_process_gone(7); model_tick();
    TEST_CHECK(file_live && model_closes == 1);
    unsigned events = model_events;
    for (int i=0; i<20; i++) model_tick();
    TEST_CHECK(model_closes == 1 && model_events == events); /* No event-driven busy loop. */
    caller = 3; VjoGameOcrRequest r = make_request();
    TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_BUSY);
    close_error = 0; model_clock += 100000; model_tick();
    TEST_CHECK(!file_live && model_closes == 2 && !game_ocr_capture_busy_locked());
}
static void model_syscalls_wait_for_the_worker(void)
{
    setup(); g.mevf = 5; wait_runs_worker = 1;
    uint32_t seq = submit(); take(seq);
    VjoGameOcrModelInfo info; memset(&info, 0xCC, sizeof(info));
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, &info) == (int)VJO_VOCR_FL10_BYTES && file_live && model_opens == 1);
    TEST_CHECK(info.size == sizeof(info) && info.bytes == VJO_VOCR_FL10_BYTES && !info.stat_rc && stats == 1);
    TEST_CHECK(info.mtime && info.mtime == pack_time(&file_stat.st_mtime));
    static unsigned char chunk[VJO_GAME_OCR_MODEL_CHUNK];
    TEST_CHECK(vjoOcrModelRead(seq, 4096, sizeof(chunk), chunk) == (int)sizeof(chunk) && model_reads == 1);
    TEST_CHECK(chunk[0] == 4096 % 251 && chunk[sizeof(chunk)-1] == (4096 + sizeof(chunk) - 1) % 251);
    TEST_CHECK(!vjoOcrModelClose(seq) && !file_live && model_closes == 1);
    /* One wait per operation, each ended by the worker's completion. */
    TEST_CHECK(waits == 3 && done_events == 3 && !locks && !syscall_depth);
    TEST_MSG("waits=%u done=%u", waits, done_events);
    TEST_CHECK(!vjoOcrComplete(&message));
}
static void model_wait_is_bounded(void)
{
    setup(); g.mevf = 5; /* the kernel worker does not run during the wait */
    uint32_t seq = submit(); take(seq);
    uint64_t before = model_clock;
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_BUSY && !model_opens && waits == 1);
    TEST_CHECK(model_clock - before == MODEL_SYSCALL_WAIT_US);
    model_tick(); /* the worker, later: its completion is left pending */
    TEST_CHECK(done_pending);
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == (int)VJO_VOCR_FL10_BYTES && waits == 1);
    /* The next wait consumes that stale completion, re-checks, then times out. */
    TEST_CHECK(vjoOcrModelClose(seq) == VJO_ERR_BUSY && file_live && waits == 3 && !done_pending);
    model_tick();
    TEST_CHECK(!vjoOcrModelClose(seq) && !file_live && waits == 3);
    TEST_CHECK(!vjoOcrComplete(&message));
}
static void model_open_reports_modification_time(void)
{
    setup(); g.mevf = 5; wait_runs_worker = 1; stat_error = (int)0x80010016u;
    uint32_t seq = submit(); take(seq);
    VjoGameOcrModelInfo info; memset(&info, 0xCC, sizeof(info));
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, &info) == (int)VJO_VOCR_FL10_BYTES && file_live);
    TEST_CHECK(info.stat_rc == stat_error && !info.mtime && info.bytes == VJO_VOCR_FL10_BYTES);
    copy_out_fail = 1; /* a failed copy of the report is retryable while the file stays open */
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, &info) == VJO_ERR_COPY);
    copy_out_fail = 0;
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, &info) == (int)VJO_VOCR_FL10_BYTES && model_opens == 1 && stats == 1);
    TEST_CHECK(!vjoOcrModelClose(seq) && !file_live && !vjoOcrComplete(&message));
}
static void model_cancel_while_waiting(void)
{
    setup(); g.mevf = 5; wait_runs_worker = 1;
    uint32_t seq = submit(); take(seq);
    during_io = 1; /* the foreground changes while the worker opens the file */
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_CANCELLED);
    TEST_CHECK(file_live && game_ocr_capture_busy_locked());
    TEST_CHECK(!vjoOcrModelClose(seq) && !file_live && model_closes == 1);
    TEST_CHECK(!vjoOcrComplete(&message) && !game_ocr_capture_busy_locked());
}
/* API 20: an LM is a pinned name the kernel knows; the game names its tier
 * (which), up to the tier the Shell's request allows, never a path. */
static void lm_tiers_open_up_to_the_requested_tier(void)
{
    static const struct { uint32_t tier; const char *name; uint32_t bytes; } lms[] = {
        {1, VJO_VOCR_LM1_NAME, VJO_VOCR_LM1_BYTES}, {2, VJO_VOCR_LM2_NAME, VJO_VOCR_LM2_BYTES},
        {3, VJO_VOCR_LM3_NAME, VJO_VOCR_LM3_BYTES}, {4, VJO_VOCR_LM4_NAME, VJO_VOCR_LM4_BYTES}};
    for (unsigned allowed = 0; allowed <= 4; allowed++)
        for (unsigned i = 0; i < 4; i++) {
            setup(); VjoGameOcrRequest r = make_request();
            r.flags = allowed << VJO_GAME_OCR_LM_SHIFT;
            caller = 3; int seq = vjoOcrSubmit(&r); TEST_ASSERT(seq > 0); take((uint32_t)seq);
            uint32_t which = VJO_GAME_OCR_OPEN_LM + lms[i].tier - 1u;
            if (lms[i].tier > allowed) {
                TEST_CHECK(vjoOcrModelOpen(seq, which, NULL) == VJO_ERR_ARG && !model_opens);
                TEST_MSG("tier %u opened with tier %u allowed", lms[i].tier, allowed);
            } else {
                expect_lm = lms[i].name; expected_model_size = lms[i].bytes;
                TEST_CHECK(vjoOcrModelOpen(seq, which, NULL) == VJO_ERR_BUSY);
                model_tick();
                TEST_CHECK(vjoOcrModelOpen(seq, which, NULL) == (int)lms[i].bytes && model_opens == 1);
                close_model(seq);
                expect_lm = NULL;
            }
            TEST_CHECK(!vjoOcrModelClose(seq) && !vjoOcrComplete(&message));
        }
    setup(); VjoGameOcrRequest r = make_request(); /* past the last tier */
    r.flags = 4u << VJO_GAME_OCR_LM_SHIFT;
    caller = 3; int seq = vjoOcrSubmit(&r); TEST_ASSERT(seq > 0); take((uint32_t)seq);
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_LM + 4u, NULL) == VJO_ERR_ARG && !model_opens);
    TEST_CHECK(!vjoOcrModelClose(seq) && !vjoOcrComplete(&message));
}

/* API 20: the cascade's larger model is a pinned name the kernel knows; the
 * game may open it only when the request gives the cascade a time ceiling. */
static void cascade_model_opens_only_with_a_ceiling(void)
{
    for (unsigned ceiling = 0; ceiling <= 1; ceiling++) {
        setup(); VjoGameOcrRequest r = make_request();
        r.flags = ceiling ? 8u << VJO_GAME_OCR_CASCADE_SHIFT : 0u;
        caller = 3; int seq = vjoOcrSubmit(&r); TEST_ASSERT(seq > 0); take((uint32_t)seq);
        if (!ceiling) {
            TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_CASCADE, NULL) == VJO_ERR_ARG && !model_opens);
        } else {
            expect_lm = VJO_VOCR_CASCADE_NAME; expected_model_size = VJO_VOCR_CASCADE_BYTES;
            TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_CASCADE, NULL) == VJO_ERR_BUSY);
            model_tick();
            TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_CASCADE, NULL) == (int)VJO_VOCR_CASCADE_BYTES &&
                       model_opens == 1);
            close_model(seq);
            expect_lm = NULL;
        }
        /* the slots between the LM tiers and it name nothing */
        TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_LM + VJO_VOCR_LM_TIERS, NULL) == VJO_ERR_ARG);
        TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_CASCADE + 1u, NULL) == VJO_ERR_ARG);
        TEST_CHECK(!vjoOcrModelClose(seq) && !vjoOcrComplete(&message));
    }
}

/* The fallback is a second pinned name the Shell submits; the game only
 * picks which of the two to open, never a path or another name. */
static void fallback_is_one_of_the_submitted_pinned_names(void)
{
    setup(); VjoGameOcrRequest r = make_request();
    memset(r.fallback, 'x', sizeof(r.fallback)); /* unterminated */
    caller = 3; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    setup(); r = make_request(); strcpy(r.model, "H15_w8.vocr"); strcpy(r.fallback, "FL10_w8.vocr");
    caller = 3; int seq = vjoOcrSubmit(&r); TEST_ASSERT(seq > 0); take((uint32_t)seq);
    TEST_CHECK(!strcmp(request.fallback, "FL10_w8.vocr"));
    TEST_CHECK(vjoOcrModelOpen(seq, 2, NULL) == VJO_ERR_ARG && !model_opens);
    expect_fallback = 1; expected_model_size = VJO_VOCR_FL10_BYTES;
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_FALLBACK, NULL) == VJO_ERR_BUSY);
    model_tick();
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_FALLBACK, NULL) == (int)VJO_VOCR_FL10_BYTES && model_opens == 1);
    /* One file per open: the other name is refused while this one is open. */
    TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_ARG && model_opens == 1);
    close_model(seq);
    TEST_CHECK(!vjoOcrComplete(&message));
    /* No fallback, or a name outside the pinned set: nothing is opened. */
    const char *refused[] = {"", "../secret", "meiki-stream-int8.mnn", "FL10_w8.vocr/../x"};
    for (unsigned i = 0; i < sizeof(refused)/sizeof(refused[0]); i++) {
        setup(); r = make_request(); strcpy(r.fallback, refused[i]);
        caller = 3; seq = vjoOcrSubmit(&r); TEST_ASSERT(seq > 0); take((uint32_t)seq);
        TEST_CHECK(vjoOcrModelOpen(seq, VJO_GAME_OCR_OPEN_FALLBACK, NULL) == VJO_ERR_ARG && !model_opens);
        TEST_CHECK(!vjoOcrModelClose(seq) && !vjoOcrComplete(&message));
    }
}

static void modification_times_pack_distinctly(void)
{
    const SceDateTime a = {2026, 10, 11, 9, 43, 58, 123456};
    const uint64_t base = pack_time(&a);
    SceDateTime b;
    TEST_CHECK(base != 0);
    b = a; b.year++; TEST_CHECK(pack_time(&b) > base);
    b = a; b.month++; TEST_CHECK(pack_time(&b) > base);
    b = a; b.day++; TEST_CHECK(pack_time(&b) > base);
    b = a; b.hour++; TEST_CHECK(pack_time(&b) > base);
    b = a; b.minute++; TEST_CHECK(pack_time(&b) > base);
    b = a; b.second++; TEST_CHECK(pack_time(&b) > base);
    b = a; b.microsecond++; TEST_CHECK(pack_time(&b) > base);
    b = a; b.second = 59; b.microsecond = 999999;
    SceDateTime c = a; c.minute++; c.second = 0; c.microsecond = 0;
    TEST_CHECK(pack_time(&c) > pack_time(&b));
}

TEST_LIST = {
    {"hand-off: an idle take waits for a submission, bounded", take_wakes_on_submit},
    {"hand-off: a pending read waits for the completion, bounded", read_wakes_on_complete},
    {"hand-off: a read waiting at a shutdown is counted and woken; every waited read gives its count back",
     read_waiters_are_counted_and_woken},
    {"hand-off: a failed module stop keeps the transport working", a_failed_stop_keeps_the_transport},
    {"hand-off: module_stop's drain: refused, no waiter, waiters leaving, the grace", stop_drains_its_waiters},
    {"hand-off: the waiter count changes under the lock; the re-check sees a suspend", waiter_count_changes_under_the_lock},
    {"model syscalls wait for the kernel worker: Open Read32K Close each finish in one call", model_syscalls_wait_for_the_worker},
    {"model wait is bounded; BUSY still means repeat; a stale completion is re-checked", model_wait_is_bounded},
    {"Open reports the file modification time; a failed stat leaves it unknown and retryable", model_open_reports_modification_time},
    {"cancellation during a waited Open is reported and Close drains in one call", model_cancel_while_waiting},
    {"packed modification times differ for every field", modification_times_pack_distinctly},
    {"the fallback is one of the submitted pinned names; Open picks which", fallback_is_one_of_the_submitted_pinned_names},
    {"an LM tier opens by its pinned name, up to the request's tier", lm_tiers_open_up_to_the_requested_tier},
    {"the cascade's model opens by its pinned name, only with a ceiling", cascade_model_opens_only_with_a_ceiling},
    {"removed_Meiki_and_VOCR2_models_are_not_in_the_broker_allowlist",removed_models_are_refused},
    {"exact_pin_keeps_canonical_path_and_final_chunk_bounds",exact_pin_keeps_canonical_path_and_final_chunk_bounds},
    {"model broker permissions canonical paths bounds and cleanup acknowledgement", model_permissions_paths_and_bounds},
    {"model broker short reads copy retry EOF and native errors", model_short_read_copy_retry_and_errors},
    {"model broker late I/O cancellation exit Shell restart and pending close", model_cancel_exit_restart_and_late_io},
    {"model broker failed close and automatic orphan recovery", model_failed_close_and_orphan_recovery},
    {"rows: the hook copies grey rows into the worker block; the first pass publishes the capture", rows_hook_copies_grey_and_publishes_the_capture},
    {"rows: bad requests are refused without arming", rows_refuse_bad_requests_without_arming},
    {"rows: no frame, another process's frame or another size", rows_timeout_other_process_and_size},
    {"rows: cancellation during the wait", rows_cancelled_while_waiting},
    {"rows: the kernel strip rung, freed before the call returns", rows_kernel_strip_rung},
    {"rows: a copy outliving its call is abandoned, then tidied by the kernel worker", rows_copy_outliving_its_call},
    {"rows: the destination stride is bounded (no wrapped span)", rows_stride_is_bounded},
    {"rows: a process exit never pulls state or the strip from a call", rows_process_exit_cannot_pull_state_from_a_call},
    {"rows: a pending request pins captures; process exit clears it", rows_pending_pins_and_process_exit},
    {"registration and caller permissions", registration_and_permissions},
    {"completion and stale replacement", complete_and_replacement},
    {"queued and claimed capture pins", capture_pins_queued_and_claimed},
    {"cancelled reader must drain", cancellation_drains_reader},
    {"cancel before take", cancel_before_take},
    {"suspend and same PID epoch", suspended_and_same_pid_epoch},
    {"foreground replacement and process exit", foreground_replacement_and_exit},
    {"Shell restart", shell_restart},
    {"failed cleanup and module replacement", cleanup_failure_and_module_replacement},
    {"capture and wire validation", invalid_capture_and_wire},
    {"copy failures and sequence exhaustion", user_copy_failures_and_sequence_exhaustion},
    {"shutdown ownership", shutdown_refuses_claim_and_closes_registration},
    {NULL, NULL}
};
