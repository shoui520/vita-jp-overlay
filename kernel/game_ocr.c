/* OCR requests, copied capture/model bytes and results. Inference stays in-game.
 * g.lock serializes this transport with capture allocation/read/free. */
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <taihen.h>
#include "vjo_kernel.h"
#include "game_ocr.h"
#include "../include/vjo_vocr_sizes.h"

static VjoGameOcrRequest request;
static VjoGameOcrResult result, incoming; /* Never put a 4 KiB result on syscall stacks. */
static SceUID worker_pid, worker_module, request_pid, request_shell;
static uint32_t worker_epoch, request_epoch, sequence;
static int valid, pending, claimed, cancelled, completed, stopping;
/* Threads inside vjoOcrTake's or vjoOcrRead's wait (under g.lock): module_stop
 * wakes them and waits until they have left before the module goes. */
static int waiters;

enum { MODEL_IDLE, MODEL_QUEUED, MODEL_RUNNING, MODEL_DONE };
enum { MODEL_OPEN = 1, MODEL_READ, MODEL_CLOSE };
static struct {
    uint32_t seq, offset, bytes, size;
    uint32_t which;    /* VJO_GAME_OCR_OPEN_*: the request's model or its fallback */
    SceUID pid, fd;
    int phase, operation, rc, closing, orphan;
    int stat_rc;       /* Open: the modification-time query's native error, or 0 */
    uint64_t retry_us;
    uint64_t mtime;    /* Open: packed modification time, 0 if unknown */
    char path[VJO_GAME_OCR_MODEL_DIR_BYTES];
} model_io;
/* One kernel staging buffer; weights and inference remain in game USER_RW. */
static unsigned char model_stage[VJO_GAME_OCR_MODEL_CHUNK];

/* The worker's rows request (vjoOcrCaptureRows), armed by its syscall and
 * copied by the display hook in the game's own thread. state moves IDLE ->
 * PENDING (the syscall, under g.lock) -> COPYING (the hook, CAS) -> DONE (the
 * hook) -> IDLE (the syscall, under g.lock), or PENDING -> IDLE when the
 * syscall gives up (CAS). The syscall returns only once the state is IDLE
 * again, so the kernel never holds the worker's buffer beyond that call. */
enum { ROWS_IDLE, ROWS_PENDING, ROWS_COPYING, ROWS_DONE };
static struct {
    volatile int state;
    SceUID pid;
    OcrRowsJob job;
    int rc;                         /* the hook's: rows copied or the error */
    uint32_t fb_pitch, fb_fmt, fb_w, fb_h;
    int64_t armed_us, copy_start_us, copy_end_us;
    uint32_t sig_next;              /* the first pass's next row while it runs */
    uint32_t sig_seq;               /* the job it belongs to */
    int orphan;                     /* its process exited during a copy: the kernel worker tidies up */
    int abandoned;                  /* the copy outlived its rows call (BUSY): the kernel worker returns
                                       the state to IDLE once the copy ends; no new request meanwhile */
    int in_call;                    /* a rows call is between arming and its last use of the state and
                                       the kernel strip: nobody else resets or frees them meanwhile */
    uint64_t gone_us;               /* when its process exited during the call */
} rows;
/* A frame has to come within this; cancellation is checked every slice. */
#define ROWS_WAIT_US 1500000u
#define ROWS_SLICE_US 50000u
/* A copy the hook started finishes within this (one strip, a few ms). */
#define ROWS_COPY_WAIT_US 2000000u
/* Longer than any call can stay inside the kernel (ROWS_WAIT_US + ROWS_COPY_WAIT_US). */
#define ROWS_GONE_US 10000000u
/* A model syscall waits this long for the kernel worker before answering
 * BUSY; the game then yields and repeats it, as before. */
#define MODEL_SYSCALL_WAIT_US 50000u
/* With no job queued, the game worker's vjoOcrTake waits this long for one
 * (MEV_JOB) before answering 1, so a job is claimed when it is submitted, not
 * at the worker's next 100 ms poll; with no result yet, the Shell's vjoOcrRead
 * waits this long for it (MEV_RESULT). One wait each, without the lock; the
 * periods match the polls they replace, so a caller's loop keeps its pace. */
#define TAKE_WAIT_US 100000u
#define READ_WAIT_US 50000u

/* Waits for bits of g.mevf for at most us, without g.lock (the caller cleared
 * them under the lock when it saw nothing to take). */
static void event_wait(unsigned int bits, SceUInt us)
{
    unsigned int out = 0;
    SceUInt timeout = us;
    ksceKernelWaitEventFlag(g.mevf, bits, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &out, &timeout);
}

static void model_reset_locked(void)
{
    memset(&model_io, 0, sizeof(model_io));
    model_io.fd = -1;
}

static void model_wake_locked(void)
{
    ksceKernelSetEventFlag(g.ievf, IEV_MODEL);
}

static void model_invalidate_locked(void)
{
    if (!model_io.seq || model_io.closing) return;
    model_io.closing = 1;
    model_wake_locked();
}

static int shell_caller(SceUID pid)
{
    return pid > 0 && pid == g.shell_pid;
}

static int registration_current(SceUID pid)
{
    return pid > 0 && pid == worker_pid && pid == g.game_pid && g.game_active &&
           !stopping &&
           worker_epoch == __atomic_load_n(&g.ocr_epoch, __ATOMIC_ACQUIRE);
}

static void invalidate_locked(void)
{
    if (!valid) return;
    cancelled = 1;
    completed = 0;
    pending = 0;
    model_invalidate_locked();
    /* A taken job can still be executing after suspend, timeout or Shell
     * restart. Only its drained completion or actual process exit unpins it. */
}

static void sync_locked(void)
{
    if (valid && (request_epoch != __atomic_load_n(&g.ocr_epoch, __ATOMIC_ACQUIRE) ||
                  request_pid != g.game_pid || !g.game_active ||
                  request_shell != g.shell_pid))
        invalidate_locked();
}

void game_ocr_init(void)
{
    memset(&request, 0, sizeof(request));
    memset(&result, 0, sizeof(result));
    memset(&incoming, 0, sizeof(incoming));
    worker_pid = worker_module = request_pid = request_shell = 0;
    worker_epoch = request_epoch = sequence = 0;
    valid = pending = claimed = cancelled = completed = stopping = 0;
    waiters = 0;
    model_reset_locked();
    memset(&rows, 0, sizeof(rows));
}

int game_ocr_capture_busy_locked(void)
{
    sync_locked();
    return pending || claimed || model_io.seq || rows.state != ROWS_IDLE;
}

SceUID game_ocr_worker_pid_locked(void)
{
    return worker_pid > 0 && registration_current(worker_pid) ? worker_pid : 0;
}

/* No job is queued, claimed or in a model or rows call: the worker is refused
 * from now on. A job slot left (cancelled, read, or completed and not read yet)
 * is kept as it is, so an undone stop (game_ocr_stop) leaves the transport as
 * it found it; a take or read waiting in the kernel wakes and re-checks. */
int game_ocr_shutdown_locked(void)
{
    sync_locked();
    if (pending || claimed || model_io.seq || rows.state != ROWS_IDLE) return VJO_ERR_BUSY;
    stopping = 1;
    if (waiters && g.mevf > 0) ksceKernelSetEventFlag(g.mevf, MEV_JOB | MEV_RESULT);
    return 0;
}

int game_ocr_waiters_locked(void)
{
    return waiters;
}

/* module_stop's part: the shutdown, then until the takes and reads it woke have
 * left their wait (they give their count back under the lock), at most
 * STOP_DRAIN_POLLS ms, and 1 ms more for the few instructions such a thread
 * still runs after its unlock. One that does not leave in time fails the stop,
 * which is undone (the transport works as before; a game worker refused
 * meanwhile reloads its weights at its next job). 0, or VJO_ERR_BUSY. */
#define STOP_DRAIN_POLLS 200
int game_ocr_stop(void)
{
    int busy, woke;
    VJO_LOCK();
    busy = game_ocr_shutdown_locked();
    woke = waiters;
    VJO_UNLOCK();
    if (busy) return VJO_ERR_BUSY;
    for (int i = 0;; i++) {
        VJO_LOCK();
        busy = waiters;
        if (busy && i >= STOP_DRAIN_POLLS) stopping = 0;
        VJO_UNLOCK();
        if (!busy) break;
        if (i >= STOP_DRAIN_POLLS) {
            klog("module_stop: %d OCR waiter(s) still in the kernel after %d ms; stop undone", busy, STOP_DRAIN_POLLS);
            return VJO_ERR_BUSY;
        }
        ksceKernelDelayThread(1000);
    }
    if (woke) ksceKernelDelayThread(1000);
    return 0;
}

void game_ocr_shell_changed_locked(void)
{
    __atomic_add_fetch(&g.ocr_epoch, 1, __ATOMIC_RELEASE);
    invalidate_locked();
    worker_epoch = 0;
}

void game_ocr_foreground_changed_locked(void)
{
    sync_locked();
}

void game_ocr_process_gone(SceUID pid)
{
    VJO_LOCK();
    if (pid == request_pid) {
        invalidate_locked();
        claimed = pending = 0;
    }
    if (pid == model_io.pid && model_io.seq) {
        model_io.orphan = 1;
        model_invalidate_locked();
    }
    /* Its rows call will never return to it: nothing may be copied into the
     * process any more, and an idle kernel strip goes. A copy in progress runs
     * in that process's own display call and ends by itself. */
    if (pid == rows.pid) {
        if (rows.in_call) {
            /* its rows call is still inside the kernel: it sees the process gone,
             * stops, and tidies up itself (nothing is freed under it) */
            rows.orphan = 1;
            rows.gone_us = ksceKernelGetSystemTimeWide();
        } else if (__sync_bool_compare_and_swap(&rows.state, ROWS_PENDING, ROWS_IDLE) ||
                   rows.state == ROWS_DONE) {
            rows.state = ROWS_IDLE;
            rows.orphan = rows.abandoned = 0;
            buffers_kstrip_free();
        } else if (rows.state == ROWS_COPYING) {
            rows.orphan = 1;
        }
        rows.sig_seq = 0;
    }
    if (pid == worker_pid) {
        worker_pid = worker_module = 0;
        worker_epoch = 0;
    }
    VJO_UNLOCK();
}

int vjoOcrRegister(void)
{
    uint32_t state;
    tai_module_info_t info;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    ENTER_SYSCALL(state);
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    rc = taiGetModuleInfoForKernel(pid, VJO_GAME_OCR_MODULE, &info);
    if (rc < 0) { rc = VJO_ERR_PERM; goto out; }
    VJO_LOCK();
    sync_locked();
    if (stopping || pid <= 0 || pid != g.game_pid || !g.game_active) rc = VJO_ERR_NO_GAME;
    else if (claimed && (pid != worker_pid || worker_module != info.modid ||
                        worker_epoch != __atomic_load_n(&g.ocr_epoch, __ATOMIC_ACQUIRE)))
        rc = VJO_ERR_BUSY;
    else {
        /* The worker asks every 100 ms: only a new registration is logged, so kernel.txt
         * says which game process the worker loaded into (its *TITLEID section works). */
        if (pid != worker_pid || worker_module != info.modid)
            klog("ocr worker registered: game pid 0x%X module 0x%X", pid, info.modid);
        worker_pid = pid;
        worker_module = info.modid;
        worker_epoch = __atomic_load_n(&g.ocr_epoch, __ATOMIC_ACQUIRE);
        rc = 0;
    }
    VJO_UNLOCK();
out:
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrSubmit(const VjoGameOcrRequest *user_request)
{
    uint32_t state;
    VjoGameOcrRequest next;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    if (!shell_caller(pid)) return VJO_ERR_PERM;
    ENTER_SYSCALL(state);
    if (!user_request || ksceKernelMemcpyUserToKernel(&next, user_request, sizeof(next)) < 0) {
        rc = VJO_ERR_COPY; goto out;
    }
    if (next.size != sizeof(next) || !next.model[0] || next.model[sizeof(next.model)-1] != 0 ||
        !next.model_dir[0] || next.model_dir[sizeof(next.model_dir)-1] != 0 ||
        next.fallback[sizeof(next.fallback)-1] != 0 || (next.flags & ~VJO_GAME_OCR_F_ALL)) {
        rc = VJO_ERR_ARG; goto out;
    }
    VJO_LOCK();
    sync_locked();
    if (!shell_caller(pid)) rc = VJO_ERR_PERM;
    else if (pending || claimed || model_io.seq || rows.state != ROWS_IDLE) rc = VJO_ERR_BUSY;
    else if (!registration_current(worker_pid)) rc = VJO_ERR_NO_GAME;
    else if (g.capture_state != CAPTURE_IDLE) rc = VJO_ERR_BUSY; /* a Lens/Anki capture in flight */
    else if (sequence == 0x7FFFFFFFu) rc = VJO_ERR_BUSY; /* Never recycle a sequence. */
    else {
        /* The region in the game's last frame. Every row request reads that
         * geometry; a frame of another size fails it (VJO_ERR_FORMAT). */
        uint32_t fw = g.fb_w ? g.fb_w : VJO_MAX_W, fh = g.fb_h ? g.fb_h : VJO_MAX_H;
        if (fw > VJO_MAX_W) fw = VJO_MAX_W;
        if (fh > VJO_MAX_H) fh = VJO_MAX_H;
        if (next.flags & VJO_GAME_OCR_F_FULL) memset(&next.region, 0, sizeof(next.region));
        else next.region = g.region;
        capture_crop_of(&next.region, fw, fh, &next.x, &next.y, &next.width, &next.height);
        next.fb_width = fw;
        next.fb_height = fh;
        next.seq = ++sequence;
        /* The capture this job takes: VjoState.done_seq reaches it with the first pass. */
        g.capture_seq = (g.capture_seq + 1) & 0x7FFFFFFF;
        if (!g.capture_seq) g.capture_seq = 1;
        next.capture_seq = g.capture_seq;
        g.capture_game_pid = worker_pid;
        g.capture_epoch = __atomic_load_n(&g.ocr_epoch, __ATOMIC_ACQUIRE);
        request = next;
        request_pid = worker_pid;
        request_shell = pid;
        request_epoch = __atomic_load_n(&g.ocr_epoch, __ATOMIC_ACQUIRE);
        memset(&result, 0, sizeof(result));
        valid = pending = 1;
        claimed = cancelled = completed = 0;
        rc = (int)request.seq;
        if (g.mevf > 0) ksceKernelSetEventFlag(g.mevf, MEV_JOB); /* a waiting vjoOcrTake claims it now */
        klog("ocr job %u: %ux%u at %u,%u of %ux%u, capture %u, flags 0x%X, %s / %s", request.seq,
             request.width, request.height, request.x, request.y, fw, fh, request.capture_seq,
             request.flags, request.model, request.fallback[0] ? request.fallback : "-");
    }
    VJO_UNLOCK();
out:
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrTake(VjoGameOcrRequest *out)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc, waited = 0;
    ENTER_SYSCALL(state);
    for (;;) {
        int wait;
        VJO_LOCK();
        if (waited) waiters--;
        sync_locked();
        if (!registration_current(pid)) rc = VJO_ERR_PERM;
        else if (claimed) rc = VJO_ERR_BUSY;
        else if (!pending || !valid || cancelled) rc = 1;
        else if (!out || ksceKernelMemcpyKernelToUser(out, &request, sizeof(request)) < 0)
            rc = VJO_ERR_COPY;
        else { pending = 0; claimed = 1; rc = 0; }
        /* Nothing queued: wait once. The bit is cleared under the lock, so a
         * vjoOcrSubmit after this point wakes the wait. */
        wait = rc == 1 && !waited && g.mevf > 0;
        if (wait) {
            ksceKernelClearEventFlag(g.mevf, ~MEV_JOB);
            waiters++;
        }
        VJO_UNLOCK();
        if (!wait) break;
        waited = 1;
        event_wait(MEV_JOB, TAKE_WAIT_US);
    }
    EXIT_SYSCALL(state);
    return rc;
}

/* Display hook, game context, no lock: the armed rows of this process from
 * this frame (see the rows state machine above). */
void game_ocr_rows_hook(SceUID pid, uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t w, uint32_t h)
{
    if (__atomic_load_n(&rows.state, __ATOMIC_ACQUIRE) != ROWS_PENDING || pid != rows.pid)
        return;
    if (!__sync_bool_compare_and_swap(&rows.state, ROWS_PENDING, ROWS_COPYING))
        return;
    rows.copy_start_us = ksceKernelGetSystemTimeWide();
    rows.fb_pitch = pitch;
    rows.fb_fmt = fmt;
    rows.fb_w = w;
    rows.fb_h = h;
    rows.rc = capture_grey_rows(base, pitch, fmt, w, h, &rows.job);
    rows.copy_end_us = ksceKernelGetSystemTimeWide();
    __atomic_store_n(&rows.state, ROWS_DONE, __ATOMIC_RELEASE);
    if (g.mevf > 0) ksceKernelSetEventFlag(g.mevf, MEV_ROWS);
}

/* The first pass of the job's capture is in (VJO_ROWS_LAST): its signature
 * against the current screen, published like a region capture's
 * (main.c finish_capture), so the Shell can tell whether the job's text is for
 * the screen it shows now. */
static void publish_capture_locked(void)
{
    g.capture_scene = scene_match(&g.tracker, &g.capture_sig);
    g.crop_w = request.width;
    g.crop_h = request.height;
    g.capture_result = 0;
    g.raw_valid = 0;
    g.capture_full = 0;
    g.region_seq = request.capture_seq;
    g.region_us = rows.copy_start_us;
    g.done_seq = request.capture_seq;
}

/* The caller's memory holds [dst, dst + bytes): one block of process pid. */
static int user_range_ok(SceUID pid, uintptr_t dst, uint32_t bytes)
{
    SceUID first, last;
    if (!dst || !bytes || dst + bytes < dst) return 0;
    first = ksceKernelFindProcMemBlockByAddr(pid, (const void *)dst, 0);
    last = ksceKernelFindProcMemBlockByAddr(pid, (const void *)(dst + bytes - 1u), 0);
    return first >= 0 && first == last;
}

int vjoOcrCaptureRows(const VjoGameOcrRows *user_req, void *dst, VjoGameOcrRowsInfo *user_info)
{
    uint32_t state;
    VjoGameOcrRows req;
    VjoGameOcrRowsInfo info;
    SceUID pid = ksceKernelGetProcessId();
    uint64_t deadline;
    uint32_t span = 0, kbytes = 0;
    int rc = 0, armed = 0, kalloc = 0;
    ENTER_SYSCALL(state);
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (!user_req || ksceKernelMemcpyUserToKernel(&req, user_req, sizeof(req)) < 0) {
        rc = VJO_ERR_COPY; goto out;
    }
    VJO_LOCK();
    sync_locked();
    if (pid <= 0 || pid != request_pid) rc = VJO_ERR_PERM;
    else if (!valid || !claimed || req.seq != request.seq || req.size != sizeof(req)) rc = VJO_ERR_ARG;
    else if (cancelled || !registration_current(pid)) rc = VJO_ERR_CANCELLED;
    else if (!req.n || req.n > VJO_GAME_OCR_ROWS_MAX || req.y >= request.height || req.n > request.height - req.y ||
             req.dst_stride < request.width || req.dst_stride > VJO_MAX_W ||
             (req.flags & ~(VJO_ROWS_SIG | VJO_ROWS_LAST)) ||
             ((req.flags & VJO_ROWS_LAST) && !(req.flags & VJO_ROWS_SIG)) ||
             (req.mode != VJO_GAME_OCR_SRC_HOOK && req.mode != VJO_GAME_OCR_SRC_KSTRIP))
        rc = VJO_ERR_ARG;
    else if ((req.flags & VJO_ROWS_SIG) && !(req.y == 0 || (rows.sig_seq == req.seq && req.y == rows.sig_next)))
        rc = VJO_ERR_ARG; /* the first pass runs from row 0, in order */
    /* A previous copy (also one whose process exited, or that outlived its call)
     * is not tidied yet: the kernel worker returns it to IDLE within a tick. */
    else if (rows.state != ROWS_IDLE) rc = VJO_ERR_BUSY;
    else {
        uint64_t bytes = (uint64_t)(req.n - 1u) * req.dst_stride + request.width; /* <= 61,440: no wrap */
        span = (uint32_t)bytes;
        if (bytes > 0xFFFFFFFFu || !user_range_ok(pid, (uintptr_t)dst, span)) rc = VJO_ERR_ARG;
    }
    if (!rc && req.mode == VJO_GAME_OCR_SRC_KSTRIP) {
        kbytes = req.n * request.width;
        rc = buffers_kstrip_alloc(kbytes);
        info.kstrip_rc = rc;
        if (!rc) kalloc = 1;
    }
    if (!rc) {
        memset(&rows.job, 0, sizeof(rows.job));
        rows.job.x0 = request.x;
        rows.job.y0 = request.y;
        rows.job.w = request.width;
        rows.job.h = request.height;
        rows.job.fb_w = request.fb_width;
        rows.job.fb_h = request.fb_height;
        rows.job.y = req.y;
        rows.job.n = req.n;
        if (kalloc) {
            rows.job.dst_kernel = buffers_kstrip();
            rows.job.stride = request.width;
        } else {
            rows.job.dst_user = (uintptr_t)dst;
            rows.job.stride = req.dst_stride;
        }
        rows.job.sig = (req.flags & VJO_ROWS_SIG) != 0;
        if (rows.job.sig && req.y == 0) {
            g.region_seq = 0; /* capture_sig gets rewritten */
            capture_sig_begin(request.width, request.height);
            rows.sig_seq = req.seq;
        }
        rows.pid = pid;
        rows.rc = 0;
        rows.orphan = rows.abandoned = 0;
        rows.in_call = 1;
        rows.fb_w = rows.fb_h = rows.fb_pitch = rows.fb_fmt = 0;
        rows.copy_start_us = rows.copy_end_us = 0;
        rows.armed_us = ksceKernelGetSystemTimeWide();
        ksceKernelClearEventFlag(g.mevf, ~MEV_ROWS);
        __atomic_store_n(&rows.state, ROWS_PENDING, __ATOMIC_RELEASE);
        armed = 1;
    }
    VJO_UNLOCK();
    if (!armed) goto out;

    /* The next frame the game shows copies the rows (display.c). Wait for it
     * without the lock, waking every slice to see whether the job was
     * cancelled. */
    deadline = (uint64_t)ksceKernelGetSystemTimeWide() + ROWS_WAIT_US;
    for (;;) {
        unsigned int bits = 0;
        SceUInt timeout = ROWS_SLICE_US;
        int stop;
        if (__atomic_load_n(&rows.state, __ATOMIC_ACQUIRE) == ROWS_DONE) break;
        if ((uint64_t)ksceKernelGetSystemTimeWide() >= deadline) break;
        ksceKernelWaitEventFlag(g.mevf, MEV_ROWS, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &timeout);
        VJO_LOCK();
        sync_locked();
        stop = cancelled || !registration_current(pid);
        VJO_UNLOCK();
        if (stop) break;
    }
    /* Give up a copy that never started; let one that did finish. */
    if (__sync_bool_compare_and_swap(&rows.state, ROWS_PENDING, ROWS_IDLE)) {
        VJO_LOCK();
        sync_locked();
        rc = cancelled || !registration_current(pid) ? VJO_ERR_CANCELLED : VJO_ERR_TIMEOUT;
        VJO_UNLOCK();
    } else {
        uint64_t copy_deadline = (uint64_t)ksceKernelGetSystemTimeWide() + ROWS_COPY_WAIT_US;
        while (__atomic_load_n(&rows.state, __ATOMIC_ACQUIRE) == ROWS_COPYING &&
               (uint64_t)ksceKernelGetSystemTimeWide() < copy_deadline)
            ksceKernelDelayThread(1000);
        if (__atomic_load_n(&rows.state, __ATOMIC_ACQUIRE) != ROWS_DONE) {
            /* Never seen: the hook still owns dst (and a kernel strip). The caller must
             * not free dst; the kernel worker frees the strip and returns the state to
             * IDLE once the copy ends, so a later Lens/Anki capture is not refused. */
            klog("ocr rows: copy still running after %u us", ROWS_COPY_WAIT_US);
            VJO_LOCK();
            rows.abandoned = 1;
            rows.in_call = 0;
            model_wake_locked();
            VJO_UNLOCK();
            rc = VJO_ERR_BUSY;
            goto info_out;
        }
        rc = rows.rc;
    }
    VJO_LOCK();
    /* The copy is over (DONE) or never started (IDLE): this call's last use of
     * the state and the strip, then both are free for the next request. */
    if (rows.state == ROWS_DONE) rows.state = ROWS_IDLE;
    rows.orphan = rows.abandoned = 0;
    if (rc > 0 && kalloc && !buffers_kstrip()) rc = VJO_ERR_NO_MEMORY; /* never: freed only by this call */
    if (rc > 0 && kalloc) {
        /* Last rung of the ladder: the hook filled the kernel strip; copy it out here. */
        const uint8_t *k = buffers_kstrip();
        for (uint32_t r = 0; r < req.n && rc > 0; r++)
            if (ksceKernelMemcpyKernelToUser((uint8_t *)dst + r * req.dst_stride, k + r * request.width,
                                             request.width) < 0)
                rc = VJO_ERR_COPY;
    }
    if (kalloc) buffers_kstrip_free();
    rows.in_call = 0;
    if (rc > 0 && (req.flags & VJO_ROWS_SIG)) {
        rows.sig_next = req.y + req.n;
        /* Only a region job's capture is the region's: a full-frame or probe
         * pass (the self-test's) must not stand in for the Shell's region. */
        if ((req.flags & VJO_ROWS_LAST) && valid && claimed && !cancelled && req.seq == request.seq &&
            !(request.flags & (VJO_GAME_OCR_F_FULL | VJO_GAME_OCR_F_PROBE))) {
            publish_capture_locked();
            VJO_UNLOCK();
            ksceKernelSetEventFlag(g.evf, VJO_EV_CAPTURE_DONE);
            klog("ocr job %u: first pass in (%u rows, frame %ux%u fmt %08X pitch %u), scene %u",
                 req.seq, request.height, rows.fb_w, rows.fb_h, rows.fb_fmt, rows.fb_pitch, g.capture_scene);
            VJO_LOCK();
        }
    }
    VJO_UNLOCK();
    if (rc < 0)
        klog("ocr rows %u..%u (%s) -> 0x%08X", req.y, req.y + req.n,
             req.mode == VJO_GAME_OCR_SRC_KSTRIP ? "kernel strip" : "hook", (unsigned)rc);
info_out:
    info.fb_width = rows.fb_w;
    info.fb_height = rows.fb_h;
    info.fb_pitch = rows.fb_pitch;
    info.fb_format = rows.fb_fmt;
    info.wait_us = (uint32_t)(ksceKernelGetSystemTimeWide() - rows.armed_us);
    info.copy_us = rows.copy_end_us > rows.copy_start_us ? (uint32_t)(rows.copy_end_us - rows.copy_start_us) : 0;
    info.kstrip_bytes = kalloc ? kbytes : 0;
out:
    info.rc = rc;
    info.armed = (uint32_t)armed;
    if (user_info) {
        uint32_t size = 0;
        if (ksceKernelMemcpyUserToKernel(&size, user_info, sizeof(size)) >= 0 && size == sizeof(info)) {
            VJO_LOCK(); /* every copy out of a syscall is made under the lock */
            ksceKernelMemcpyKernelToUser(user_info, &info, sizeof(info));
            VJO_UNLOCK();
        }
    }
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrCancelled(uint32_t seq)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    sync_locked();
    if (pid <= 0 || pid != request_pid) rc = VJO_ERR_PERM;
    else if (!valid || !claimed || seq != request.seq) rc = VJO_ERR_ARG;
    else rc = cancelled ? 1 : 0;
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrComplete(const VjoGameOcrResult *user_result)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    sync_locked();
    if (pid <= 0 || pid != request_pid) rc = VJO_ERR_PERM;
    else if (!valid || !claimed) rc = VJO_ERR_ARG;
    else if (!user_result || ksceKernelMemcpyUserToKernel(&incoming, user_result, sizeof(incoming)) < 0)
        rc = VJO_ERR_COPY;
    else if (!incoming.cleanup_status && model_io.seq) rc = VJO_ERR_BUSY;
    else if (incoming.size != sizeof(incoming) || incoming.seq != request.seq ||
             incoming.text[sizeof(incoming.text)-1] != 0 ||
             (incoming.rc > 0 || (!incoming.rc && incoming.cleanup_status) ||
              (incoming.rc && incoming.text[0]))) rc = VJO_ERR_ARG;
    else {
        if (!cancelled) {
            result = incoming;
            completed = 1;
            if (g.mevf > 0) ksceKernelSetEventFlag(g.mevf, MEV_RESULT); /* a waiting vjoOcrRead returns it now */
        }
        if (!incoming.cleanup_status) claimed = 0;
        /* Failed cleanup retains the claim and may be retried. Completed
         * cancellation drains the reader but never republishes stale text. */
        rc = 0;
    }
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrRead(uint32_t seq, VjoGameOcrResult *out)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc, waited = 0;
    if (!shell_caller(pid)) return VJO_ERR_PERM;
    ENTER_SYSCALL(state);
    for (;;) {
        int wait;
        VJO_LOCK();
        if (waited) waiters--;
        sync_locked();
        if (!shell_caller(pid)) rc = VJO_ERR_PERM;
        else if (!valid || seq != request.seq || request_shell != pid) rc = VJO_ERR_ARG;
        else if (cancelled) rc = VJO_ERR_CANCELLED;
        else if (!completed) rc = 1;
        else if (!out || ksceKernelMemcpyKernelToUser(out, &result, sizeof(result)) < 0)
            rc = VJO_ERR_COPY;
        else rc = 0;
        wait = rc == 1 && !waited && g.mevf > 0; /* as vjoOcrTake */
        if (wait) {
            ksceKernelClearEventFlag(g.mevf, ~MEV_RESULT);
            waiters++;
        }
        VJO_UNLOCK();
        if (!wait) break;
        waited = 1;
        event_wait(MEV_RESULT, READ_WAIT_US);
    }
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrCancel(uint32_t seq)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    if (!shell_caller(pid)) return VJO_ERR_PERM;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    sync_locked();
    if (!shell_caller(pid)) rc = VJO_ERR_PERM;
    else if (!valid || seq != request.seq || request_shell != pid) rc = VJO_ERR_ARG;
    else { invalidate_locked(); rc = 0; }
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return rc;
}

static uint32_t model_size(const char *name)
{
    if (!strcmp(name, VJO_VOCR3_H15_NAME)) return VJO_VOCR_H15_BYTES;
    if (!strcmp(name, VJO_VOCR3_FL10_NAME)) return VJO_VOCR_FL10_BYTES;
    if (!strcmp(name, "FL10_w8.vocr")) return VJO_VOCR_FL10_BYTES;
    if (!strcmp(name, "H15_w8.vocr")) return VJO_VOCR_H15_BYTES;
    if (!strcmp(name, "F20_w8.vocr")) return VJO_VOCR_F20_BYTES;
    if (!strcmp(name, VJO_VOCR4_W20_NAME)) return VJO_VOCR_CASCADE_BYTES;
    return 0;
}

/* The pinned LM of a tier (1..VJO_VOCR_LM_TIERS): its name and length. */
static const char *lm_name(uint32_t tier)
{
    static const char *const names[VJO_VOCR_LM_TIERS] = {VJO_VOCR_LM1_NAME, VJO_VOCR_LM2_NAME, VJO_VOCR_LM3_NAME,
                                                          VJO_VOCR_LM4_NAME};
    return names[tier - 1u];
}

static uint32_t lm_size(uint32_t tier)
{
    static const uint32_t sizes[VJO_VOCR_LM_TIERS] = {VJO_VOCR_LM1_BYTES, VJO_VOCR_LM2_BYTES, VJO_VOCR_LM3_BYTES,
                                                      VJO_VOCR_LM4_BYTES};
    return sizes[tier - 1u];
}

static int model_caller_locked(SceUID pid, uint32_t seq, int cleanup)
{
    sync_locked();
    if (pid <= 0 || pid != request_pid) return VJO_ERR_PERM;
    if (!valid || !claimed || seq != request.seq) return VJO_ERR_ARG;
    if (!cleanup && (cancelled || !registration_current(pid))) return VJO_ERR_CANCELLED;
    return 0;
}

/* The kernel worker finished a model operation, or the time is up: waits
 * without g.lock until deadline. 0: there is nothing to wait on. */
static int model_wait(uint64_t deadline)
{
    uint64_t now = (uint64_t)ksceKernelGetSystemTimeWide();
    unsigned int bits = 0;
    SceUInt timeout;
    if (g.mevf <= 0 || now >= deadline) return 0;
    timeout = (SceUInt)(deadline - now);
    return ksceKernelWaitEventFlag(g.mevf, MEV_DONE, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
                                   &bits, &timeout) >= 0;
}

/* SceDateTime as one number, each field masked to its range. */
static uint64_t pack_time(const SceDateTime *t)
{
    return ((uint64_t)(t->year & 0xFFFu) << 46) | ((uint64_t)(t->month & 0xFu) << 42) |
           ((uint64_t)(t->day & 0x1Fu) << 37) | ((uint64_t)(t->hour & 0x1Fu) << 32) |
           ((uint64_t)(t->minute & 0x3Fu) << 26) | ((uint64_t)(t->second & 0x3Fu) << 20) |
           (uint64_t)(t->microsecond & 0xFFFFFu);
}

static int model_info_out_locked(VjoGameOcrModelInfo *info)
{
    VjoGameOcrModelInfo out;
    if (!info) return 0;
    memset(&out, 0, sizeof(out));
    out.size = sizeof(out);
    out.bytes = model_io.size;
    out.stat_rc = model_io.stat_rc;
    out.mtime = model_io.mtime;
    return ksceKernelMemcpyKernelToUser(info, &out, sizeof(out)) < 0 ? VJO_ERR_COPY : 0;
}

static int model_open_locked(SceUID pid, uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info)
{
    int rc = model_caller_locked(pid, seq, 0);
    const char *name;
    if (rc) return rc;
    if (model_io.seq) {
        if (model_io.seq != seq || model_io.pid != pid) return VJO_ERR_BUSY;
        if (model_io.closing) return VJO_ERR_CANCELLED;
        if (model_io.which != which) return VJO_ERR_ARG; /* one file per open */
        if (model_io.phase == MODEL_DONE && model_io.operation == MODEL_OPEN) {
            rc = model_io.rc;
            model_io.phase = MODEL_IDLE;
        } else if (model_io.phase == MODEL_IDLE && model_io.fd >= 0) rc = (int)model_io.size;
        else return VJO_ERR_BUSY;
        /* A failed copy can be repeated: the open file still reports itself. */
        if (rc > 0 && model_info_out_locked(info)) rc = VJO_ERR_COPY;
        return rc;
    }
    /* Only the two names the Shell submitted for this job, each a pinned model,
     * or (API 20) a pinned LM up to the tier the request allows, or the
     * cascade's pinned model when the request gives it a time ceiling. */
    uint32_t size, lm_tier = which >= VJO_GAME_OCR_OPEN_LM && which < VJO_GAME_OCR_OPEN_CASCADE
                             ? which - VJO_GAME_OCR_OPEN_LM + 1u : 0u;
    if (which == VJO_GAME_OCR_OPEN_MODEL) name = request.model;
    else if (which == VJO_GAME_OCR_OPEN_FALLBACK) name = request.fallback;
    else if (lm_tier && lm_tier <= VJO_VOCR_LM_TIERS &&
             lm_tier <= ((request.flags >> VJO_GAME_OCR_LM_SHIFT) & VJO_GAME_OCR_LM_MASK)) name = lm_name(lm_tier);
    else if (which == VJO_GAME_OCR_OPEN_CASCADE &&
             ((request.flags >> VJO_GAME_OCR_CASCADE_SHIFT) & VJO_GAME_OCR_CASCADE_MASK)) name = VJO_VOCR_CASCADE_NAME;
    else return VJO_ERR_ARG;
    size = lm_tier ? lm_size(lm_tier) : model_size(name);
    if (!size || strcmp(request.model_dir, VJO_VOCR_MODEL_ROOT)) return VJO_ERR_ARG;
    model_io.seq = seq;
    model_io.pid = pid;
    model_io.which = which;
    model_io.size = size;
    memcpy(model_io.path, VJO_VOCR_MODEL_ROOT "/", sizeof(VJO_VOCR_MODEL_ROOT));
    /* Both request strings are bounded and NUL-validated by Submit; a pinned LM
     * name is a shorter literal: copy up to the NUL only. */
    {
        uint32_t n = 0;
        while (n + 1u < VJO_GAME_OCR_MODEL_BYTES && name[n]) n++;
        memcpy(model_io.path + sizeof(VJO_VOCR_MODEL_ROOT), name, n);
        model_io.path[sizeof(VJO_VOCR_MODEL_ROOT) + n] = '\0';
    }
    model_io.operation = MODEL_OPEN;
    model_io.phase = MODEL_QUEUED;
    model_wake_locked();
    return VJO_ERR_BUSY;
}

int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    uint64_t deadline;
    int rc;
    ENTER_SYSCALL(state);
    deadline = (uint64_t)ksceKernelGetSystemTimeWide() + MODEL_SYSCALL_WAIT_US;
    do {
        VJO_LOCK();
        rc = model_open_locked(pid, seq, which, info);
        VJO_UNLOCK();
    } while (rc == VJO_ERR_BUSY && model_wait(deadline));
    EXIT_SYSCALL(state);
    return rc;
}

static int model_read_locked(SceUID pid, uint32_t seq, uint32_t offset, uint32_t bytes, void *dst)
{
    int rc = model_caller_locked(pid, seq, 0);
    if (rc) return rc;
    if (model_io.seq != seq || model_io.pid != pid || model_io.fd < 0 ||
        !dst || !bytes || bytes > sizeof(model_stage) || offset > model_io.size ||
        bytes > model_io.size - offset) return VJO_ERR_ARG;
    if (model_io.closing) return VJO_ERR_CANCELLED;
    if (model_io.phase != MODEL_IDLE) {
        if (model_io.operation != MODEL_READ || offset != model_io.offset || bytes != model_io.bytes)
            return VJO_ERR_ARG;
        if (model_io.phase != MODEL_DONE) return VJO_ERR_BUSY;
        if (model_io.rc > 0 &&
            ksceKernelMemcpyKernelToUser(dst, model_stage, (SceSize)model_io.rc) < 0)
            return VJO_ERR_COPY; /* The exact completed chunk remains retryable. */
        model_io.phase = MODEL_IDLE;
        return model_io.rc;
    }
    model_io.offset = offset;
    model_io.bytes = bytes;
    model_io.operation = MODEL_READ;
    model_io.phase = MODEL_QUEUED;
    model_wake_locked();
    return VJO_ERR_BUSY;
}

int vjoOcrModelRead(uint32_t seq, uint32_t offset, uint32_t bytes, void *dst)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    uint64_t deadline;
    int rc;
    ENTER_SYSCALL(state);
    deadline = (uint64_t)ksceKernelGetSystemTimeWide() + MODEL_SYSCALL_WAIT_US;
    do {
        VJO_LOCK();
        rc = model_read_locked(pid, seq, offset, bytes, dst);
        VJO_UNLOCK();
    } while (rc == VJO_ERR_BUSY && model_wait(deadline));
    EXIT_SYSCALL(state);
    return rc;
}

static int model_close_locked(SceUID pid, uint32_t seq)
{
    int rc = model_caller_locked(pid, seq, 1);
    if (rc) return rc;
    if (!model_io.seq) return 0;
    if (model_io.seq != seq || model_io.pid != pid) return VJO_ERR_ARG;
    if (model_io.phase == MODEL_DONE && model_io.operation == MODEL_CLOSE && model_io.rc < 0) {
        model_io.phase = MODEL_IDLE; /* Retain FD; a subsequent Close retries. */
        return model_io.rc;
    }
    if (!model_io.closing || model_io.phase == MODEL_IDLE) {
        model_io.closing = 1;
        model_io.retry_us = 0; /* Explicit cleanup retry need not wait for orphan backoff. */
        model_wake_locked();
    }
    return VJO_ERR_BUSY;
}

int vjoOcrModelClose(uint32_t seq)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    uint64_t deadline;
    int rc;
    ENTER_SYSCALL(state);
    deadline = (uint64_t)ksceKernelGetSystemTimeWide() + MODEL_SYSCALL_WAIT_US;
    do {
        VJO_LOCK();
        rc = model_close_locked(pid, seq);
        VJO_UNLOCK();
    } while (rc == VJO_ERR_BUSY && model_wait(deadline));
    EXIT_SYSCALL(state);
    return rc;
}

void game_ocr_model_tick(void)
{
    int operation, rc, stat_rc = 0;
    SceUID fd;
    uint32_t offset, bytes, size;
    uint64_t mtime = 0;
    char path[sizeof(model_io.path)];
    VJO_LOCK();
    /* A rows copy whose process exited during it, or that outlived its call:
     * nobody waits for it now. */
    if ((rows.orphan || rows.abandoned) && rows.state == ROWS_DONE && !rows.in_call) {
        rows.state = ROWS_IDLE;
        rows.orphan = rows.abandoned = 0;
        buffers_kstrip_free();
    }
    /* A call whose process exited and that never reached its end: a call waits
     * at most ROWS_WAIT_US + ROWS_COPY_WAIT_US, so after ROWS_GONE_US its thread
     * is gone and nothing uses the state or the strip any more. */
    if (rows.in_call && rows.orphan && rows.state != ROWS_COPYING &&
        (uint64_t)ksceKernelGetSystemTimeWide() - rows.gone_us > ROWS_GONE_US) {
        klog("ocr rows: the exited process's call never returned; tidied");
        rows.state = ROWS_IDLE;
        rows.in_call = rows.orphan = rows.abandoned = 0;
        buffers_kstrip_free();
    }
    sync_locked();
    if (!model_io.seq || model_io.phase == MODEL_RUNNING) { VJO_UNLOCK(); return; }
    if (model_io.closing || model_io.orphan) {
        if (model_io.retry_us > (uint64_t)ksceKernelGetSystemTimeWide()) { VJO_UNLOCK(); return; }
        /* Pending I/O has no caller pointer and can be discarded before it
         * starts. In-flight I/O reaches this only after its completion. */
        model_io.operation = MODEL_CLOSE;
        model_io.phase = MODEL_QUEUED;
    }
    if (model_io.phase != MODEL_QUEUED) { VJO_UNLOCK(); return; }
    operation = model_io.operation;
    fd = model_io.fd;
    offset = model_io.offset;
    bytes = model_io.bytes;
    size = model_io.size;
    memcpy(path, model_io.path, sizeof(path));
    model_io.phase = MODEL_RUNNING;
    VJO_UNLOCK();

    /* Only the persistent kernel worker executes these calls. Calling them
     * directly in a game's syscall would retain its unavailable ux0 namespace. */
    if (operation == MODEL_OPEN) {
        fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
        rc = fd;
        if (fd >= 0) {
            SceOff length = ksceIoLseek(fd, 0, SCE_SEEK_END);
            rc = length < 0 ? (int)length : length == size ? (int)size : VJO_ERR_FORMAT;
            if (rc > 0) {
                /* The game keys its resident weights on this. Without it they
                 * are read and checked again for every job, as before. */
                SceIoStat st;
                memset(&st, 0, sizeof(st));
                stat_rc = ksceIoGetstatByFd(fd, &st);
                if (stat_rc < 0) mtime = 0;
                else { stat_rc = 0; mtime = pack_time(&st.st_mtime); }
            }
        }
    } else if (operation == MODEL_READ) {
        rc = ksceIoPread(fd, model_stage, bytes, (SceOff)offset);
        if (rc > (int)bytes) rc = VJO_ERR_FORMAT;
    } else {
        rc = fd < 0 ? 0 : ksceIoClose(fd);
        if (rc >= 0) fd = -1;
    }

    VJO_LOCK();
    /* No syscall can release/reuse the owner while phase is RUNNING. */
    model_io.fd = fd;
    model_io.rc = rc;
    model_io.phase = MODEL_DONE;
    if (operation == MODEL_OPEN) {
        model_io.stat_rc = stat_rc;
        model_io.mtime = mtime;
    }
    sync_locked();
    if (operation == MODEL_CLOSE && rc >= 0) model_reset_locked();
    else if (operation == MODEL_CLOSE) {
        model_io.closing = 0;
        if (model_io.orphan || cancelled) {
            model_io.closing = 1;
            model_io.retry_us = ksceKernelGetSystemTimeWide() + 100000u;
        }
    } else if (model_io.closing || model_io.orphan) model_wake_locked();
    VJO_UNLOCK();
    /* Wake a model syscall waiting for this result (none may be). */
    if (g.mevf > 0) ksceKernelSetEventFlag(g.mevf, MEV_DONE);
}
