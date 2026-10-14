/* Real game worker and real vocr, with Vita scheduling/USER_RW/file boundaries.
 * No emulator claims: the caller PID and SDK responses are failure-injected. */
#include "acutest.h"
#include <sys/mman.h>
#include <psp2/host_stubs.h>
#include "../../game_ocr/main.c"

static unsigned char workspace[2*1024*1024] __attribute__((aligned(64)));
static unsigned char weights_memory[VJO_VOCR_F20_BYTES + 4096] __attribute__((aligned(64)));
static unsigned char capture_mem[1024*1024] __attribute__((aligned(64)));
static unsigned char pixels[200*60*4];
/* The capture block and the memory-type ladder: which types refuse. */
static int capture_live, refuse_user, refuse_cdram, refuse_phycont, rows_error, hook_write_error;
static int busy_unarmed, busy_armed, allow_capture_held; /* rows calls that answer BUSY, before / after arming */
static unsigned capture_block, capture_allocs, capture_frees, rows_calls, kstrip_calls, last_type;
static unsigned allocations, frees, reads_count, bytes_read, thread_creates, weight_allocations, weight_frees;
static int memory_live, alloc_failure, base_failure, free_failure, close_failure, weights_live;
static unsigned live_bytes, job_block, weights_block;
static int join_failure, start_failure, delete_failure, budget_failure, cancel_reads;
static int available, cancel_job, short_reads, open_failure, bad_read;
static FILE *model;
static char model_path[512];
static unsigned user_open_calls;
static int virtual_file, open_busy, read_busy, close_busy, read_eof;
static char own_title[12];
static uint64_t clock_value;
static unsigned frame_us; /* each rows call waits this long for its frame (0: none) */
static unsigned change_at_rows; /* at this rows call the line moves down 6 rows (the player advanced); 0: never */

SceUID sceKernelGetProcessId(void) { return 7; }
uint64_t sceKernelGetProcessTimeWide(void) { return clock_value++; }
/* Free USER memory as the game sees it: what remains after the worker's blocks. */
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out)
{ TEST_CHECK(out->size == sizeof(*out)); out->size_user = available < 0 ? available : available - (int)live_bytes; return budget_failure ? -1 : 0; }
static unsigned workspace_tries, workspace_types[8];
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned size, void *opts)
{
    TEST_CHECK(!(size&4095));
    last_type = (unsigned)type;
    if (!strcmp(name, "VjoGameVocrCapture") || !strcmp(name, "VjoGameVocrProbe")) {
        /* SDK units: CDRAM 256 KiB, PHYCONT 1 MiB */
        if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && refuse_user) return (int)0x80024302u;
        if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW) { TEST_CHECK(!(size % (256*1024))); if (refuse_cdram) return (int)0x80024302u; }
        if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW) { TEST_CHECK(!(size % (1024*1024))); if (refuse_phycont) return (int)0x80024302u; }
        TEST_CHECK(!capture_live && size <= sizeof(capture_mem));
        capture_allocs++; capture_live = 1; capture_block = size; live_bytes += size; return 11;
    }
    if (!strcmp(name, "VjoGameVocrWeights")) { /* resident verified weights, at most one block, USER only */
        TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
        TEST_CHECK(!weights_live && size <= sizeof(weights_memory));
        weight_allocations++;
        if (alloc_failure) return -1;
        weights_live = 1; weights_block = size; live_bytes += size; return 10;
    }
    /* the job's workspace: the same ladder as the capture, USER first */
    TEST_CHECK(!strcmp(name, "VjoGameVocr") && !memory_live);
    if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW) TEST_CHECK(!(size % (256*1024)));
    else if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW) TEST_CHECK(!(size % (1024*1024)));
    else TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && size <= sizeof(workspace));
    if (workspace_tries < 8) workspace_types[workspace_tries] = (unsigned)type;
    workspace_tries++;
    allocations++;
    if (alloc_failure) return (int)0x80024302u;
    TEST_CHECK(size <= sizeof(workspace));
    memory_live = 1; job_block = size; live_bytes += size; return 9;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid == 11) { TEST_CHECK(capture_live); *base = capture_mem; return 0; }
    if (uid == 10) { TEST_CHECK(weights_live); *base = weights_memory; return 0; }
    TEST_CHECK(uid == 9 && memory_live); *base = base_failure ? NULL : workspace; return base_failure ? -1 : 0;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    if (uid == 11) { TEST_CHECK(capture_live); if (free_failure) return -1; capture_live = 0; capture_frees++; live_bytes -= capture_block; return 0; }
    if (uid == 10) {
        TEST_CHECK(weights_live); if (free_failure) return -1;
        weights_live = 0; weight_frees++; live_bytes -= weights_block; return 0;
    }
    TEST_CHECK(uid == 9 && memory_live); if (free_failure) return -1;
    memory_live = 0; frees++; live_bytes -= job_block; return 0;
}
/* API 21: a helper thread (game_ocr/main.c pool_run) runs its line at once, in StartThread. */
static unsigned helper_threads, helper_live, helper_starts;
static int helper_create_failure;
SceUID sceKernelCreateThread(const char *n, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu, void *opt)
{
    if (entry == helper_entry) {
        TEST_CHECK(stack == VJO_STACK_GAME_HELPER && p == 0x10000100 + 31 && cpu == 0x70000 && !strcmp(n, "VjoGameVocrHelper"));
        if (helper_create_failure) return (int)0x80020190u;
        helper_threads++; helper_live++;
        return 40 + (int)helper_threads;
    }
    TEST_CHECK(stack == 32*1024 && entry == worker_main && p == 0x10000100 + 31 && cpu == 0x70000); thread_creates++; return 20;
}
int sceKernelStartThread(SceUID uid, SceSize size, void *args)
{
    if (uid > 40) { helper_starts++; return helper_entry(size, args); }
    return start_failure ? -1 : 0;
}
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { return uid > 40 ? 0 : join_failure ? -1 : 0; }
int sceKernelDeleteThread(SceUID uid)
{
    if (uid > 40) { TEST_CHECK(helper_live > 0); helper_live--; return 0; }
    return delete_failure ? -1 : 0;
}
int sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo *info)
{ TEST_CHECK(info->size == sizeof(*info)); info->runClocks = clock_value; return 0; }
SceUID sceKernelGetThreadId(void) { return 20; }
/* The ARM clock (ocr_boost_clock): what the game runs at, and the calls that set it. */
static int arm_mhz = 333, arm_sets, arm_set_last;
int scePowerGetArmClockFrequency(void) { return arm_mhz; }
int scePowerSetArmClockFrequency(int mhz) { arm_sets++; arm_set_last = mhz; arm_mhz = mhz; return 0; }
int sceKernelDelayThread(unsigned us) { clock_value += us; return 0; }
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size)
{ TEST_CHECK(pid == 7); snprintf(out, size, "%s", own_title); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize cap) { return -1; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
static int safe_boot_mode, boot_info_calls;
int vjoBootInfo(VjoBootInfo *out) { boot_info_calls++; TEST_CHECK(out->size == sizeof(*out)); out->mode = safe_boot_mode; return 0; }
int vjoOcrRegister(void) { return VJO_ERR_NO_GAME; }
int vjoOcrTake(VjoGameOcrRequest *out) { return 1; }
int vjoOcrCancelled(uint32_t seq) { return cancel_job || (cancel_reads && reads_count >= (unsigned)cancel_reads); }
int vjoOcrComplete(const VjoGameOcrResult *out)
{ TEST_CHECK(!memory_live && !model && (!capture_live || allow_capture_held)); return 0; }
/* The kernel's rows call: grey rows of the 200x60 region into the worker's block. */
int vjoOcrCaptureRows(const VjoGameOcrRows *req, void *dst, VjoGameOcrRowsInfo *info)
{
    TEST_CHECK(req->size == sizeof(*req) && req->seq == 12 && req->y < 60 && req->n && req->n <= 60 - req->y);
    TEST_CHECK(req->dst_stride >= 200 && info && info->size == sizeof(*info));
    TEST_CHECK(capture_live && (unsigned char *)dst >= capture_mem && (unsigned char *)dst < capture_mem + capture_block);
    rows_calls++;
    clock_value += frame_us;
    if (change_at_rows && rows_calls == change_at_rows) { /* the next screen: the line 6 rows lower */
        memset(pixels, 0, sizeof(pixels));
        for (unsigned y = 21; y < 45; y++) for (unsigned x = 10; x < 190; x += 6) memset(pixels + (y * 200 + x) * 4, 235, 6);
    }
    if (req->mode == VJO_GAME_OCR_SRC_KSTRIP) kstrip_calls++;
    info->fb_width = 200; info->fb_height = 60; info->fb_pitch = 200; info->fb_format = 0; info->wait_us = 16000;
    info->armed = 1;
    if (busy_unarmed > 0) { busy_unarmed--; info->armed = 0; return VJO_ERR_BUSY; } /* a previous copy not tidied */
    if (busy_armed) { /* this copy outlived the call; until the kernel worker tidies it, every
                         later call is refused before arming */
        busy_armed = 0; busy_unarmed = 1000;
        return VJO_ERR_BUSY;
    }
    if (rows_error) return rows_error;
    if (hook_write_error && req->mode == VJO_GAME_OCR_SRC_HOOK) return VJO_ERR_COPY;
    for (unsigned r = 0; r < req->n; r++)
        for (unsigned x = 0; x < 200; x++) {
            const unsigned char *p = pixels + ((req->y + r) * 200 + x) * 4;
            ((unsigned char *)dst)[r * req->dst_stride + x] = (unsigned char)((77u*p[0] + 150u*p[1] + 29u*p[2]) >> 8);
        }
    return (int)req->n;
}
/* The probe's user-mode read: the frame sceDisplayGetFrameBuf names (frame_at,
 * none by default) and the block sceKernelGetMemBlockInfoByAddr reports for it. */
static unsigned char *frame_at;
static uintptr_t block_base;
static unsigned block_size, block_access, frame_info_calls, frame_w = 200;
int sceDisplayGetFrameBuf(SceDisplayFrameBuf *fb, int sync)
{
    if (!frame_at) return -1;
    TEST_CHECK(fb->size == sizeof(SceDisplayFrameBuf) && sync == SCE_DISPLAY_SETBUF_NEXTFRAME);
    fb->base = frame_at; fb->pitch = 200; fb->pixelformat = VJO_FB_A8B8G8R8; fb->width = frame_w; fb->height = 60;
    return 0;
}
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info)
{
    if (!frame_at) return -1;
    TEST_CHECK(base == frame_at && info->size == sizeof(*info));
    frame_info_calls++;
    info->mappedBase = (void *)block_base; info->mappedSize = block_size; info->access = block_access;
    return 0;
}
SceUID sceIoOpen(const char *path, int flags, unsigned mode)
{ user_open_calls++; return (int)0x80010013u; /* Physical game's ux0 is unavailable. */ }
static uint64_t model_mtime = 0x1234567;
static int model_stat_error;
int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info)
{
    if (open_busy) return VJO_ERR_BUSY;
    if (open_failure) return -123;
    TEST_ASSERT(!model && seq == 12);
    model = virtual_file ? tmpfile() : fopen(model_path, "rb");
    if (!model) return -123;
    if (virtual_file) {
        const unsigned char bytes[16] = {1,2,3,4};
        TEST_ASSERT(fwrite(bytes, 1, sizeof(bytes), model) == sizeof(bytes));
    }
    TEST_ASSERT(!fseek(model, 0, SEEK_END));
    int size = (int)ftell(model);
    if (info) *info = (VjoGameOcrModelInfo){sizeof(*info), (uint32_t)size, model_stat_error, 0,
                                            model_stat_error ? 0 : model_mtime};
    return size;
}
int vjoOcrModelRead(uint32_t seq, uint32_t offset, uint32_t n, void *out)
{
    TEST_CHECK(seq == 12 && model && n <= VJO_GAME_OCR_MODEL_CHUNK);
    if (read_busy) return VJO_ERR_BUSY;
    TEST_ASSERT(!fseek(model, (long)offset, SEEK_SET));
    reads_count++;
    if (bad_read) return -456;
    if (read_eof) return 0;
    if (short_reads && n > 37) n = 37;
    int got = (int)fread(out, 1, n, model); bytes_read += (unsigned)got; return got;
}
int vjoOcrModelClose(uint32_t seq)
{ TEST_CHECK(seq == 12); if (close_busy) return VJO_ERR_BUSY; if (!model) return 0; if (close_failure) return -789; int rc = fclose(model); model = NULL; return rc; }

static VjoGameOcrRequest setup(void)
{
    if (model) { fclose(model); model = NULL; }
    worker = workspace_uid = weights_uid = capture_uid = line_uid = -1;
    capture_live = refuse_user = refuse_cdram = refuse_phycont = rows_error = hook_write_error = 0;
    busy_unarmed = busy_armed = allow_capture_held = 0; hook_owns_capture = 0;
    capture_block = capture_allocs = capture_frees = rows_calls = kstrip_calls = 0;
    resident.model = NULL; resident.data = NULL; resident.capacity = 0; resident.size = resident.stamp = 0;
    weight_allocations = weight_frees = 0; weights_live = 0; live_bytes = 0;
    model_mtime = 0x1234567; model_stat_error = 0;
    model_owned = 0; model_seq = 0;
    quitting = active_seq = 0; worker_started = claim_outstanding = blocked = io_error = 0;
    allocations = frees = reads_count = bytes_read = thread_creates = 0;
    user_open_calls = 0;
    virtual_file = open_busy = read_busy = close_busy = read_eof = 0; clock_value = 0; frame_us = 0;
    memory_live = alloc_failure = base_failure = free_failure = close_failure = 0;
    join_failure = start_failure = delete_failure = budget_failure = cancel_reads = 0;
    cancel_job = short_reads = open_failure = bad_read = 0;
    available = 16*1024*1024;
    memset(pixels, 0, sizeof(pixels)); strcpy(own_title, "PCSG00415");
    frame_at = NULL; frame_info_calls = 0; frame_w = 200;
    change_at_rows = 0; blocks_held = 0;
    VjoGameOcrRequest r = {.size=sizeof(r), .seq=12, .capture_seq=11, .fb_width=200, .fb_height=60, .width=200, .height=60};
    snprintf(r.model, sizeof(r.model), "FL10_w8.vocr");
    /* Blank images never open weights, but requests still need a valid directory.
     * CI intentionally has no private release assets in its source checkout. */
    snprintf(r.model_dir, sizeof(r.model_dir), "%s",
             VJO_VOCR_MODEL_DIR[0] ? VJO_VOCR_MODEL_DIR : "unused-model-dir");
    snprintf(model_path, sizeof(model_path), "%s/%s", r.model_dir, r.model);
    return r;
}
static void line(void)
{
    for (unsigned y=15; y<39; y++) for (unsigned x=10; x<190; x+=6)
        memset(pixels+(y*200+x)*4, 235, 6);
}
static void repeated_jobs_and_reserve(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    for (unsigned i=0; i<100; i++) {
        TEST_CHECK(!vjo_game_ocr_run_request(&r, &out));
        TEST_CHECK(!memory_live && !out.cleanup_status && !out.text[0] && !model);
    }
    TEST_CHECK(allocations == 100 && frees == 100 && capture_allocs == 100 && capture_frees == 100 && !capture_live);
    /* the ledger saw every block in and out, and its peak fits the budget */
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_WORKSPACE) == 0 && vjo_ledger_current(&g_game_ledger, GL_WEIGHTS) == 0);
    TEST_CHECK(vjo_ledger_peak(&g_game_ledger, GL_WORKSPACE) > 0 && vjo_ledger_peak(&g_game_ledger, GL_WORKSPACE) <= VJO_BUDGET_GAME_WORKSPACE);
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_CAPTURE) == 0 && vjo_ledger_peak(&g_game_ledger, GL_CAPTURE) == 16384); /* 200x60: 12,000 B strip + 8 B and 3 bits per row */
    TEST_CHECK(vjo_ledger_peak(&g_game_ledger, GL_CAPTURE) <= VJO_BUDGET_GAME_CAPTURE);
    TEST_CHECK(vjo_ledger_pool_peak(&g_game_ledger, VJO_LEDGER_POOL_GAME) <= VJO_WORST_GAME_PERMANENT + VJO_WORST_GAME_JOB);
    available = 4*1024*1024;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && allocations == 100);
    available = -1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && allocations == 100);
    available = 16*1024*1024; budget_failure = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && allocations == 100);
}
static void memory_failure_and_retry(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    alloc_failure = 1; workspace_tries = 0;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && !frees);
    /* every rung was tried, in order, and each attempt is in the step log */
    TEST_CHECK(workspace_tries == 3 && workspace_types[0] == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW &&
               workspace_types[1] == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW &&
               workspace_types[2] == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW);
    unsigned refused = 0;
    for (unsigned i = 0; i < out.n_steps && i < VJO_GAME_OCR_STEPS; i++)
        if (out.step[i].id == VJO_GSTEP_ALLOC && out.step[i].arg == VJO_GITEM_WORKSPACE &&
            out.step[i].kind == refused + 1u /* VJO_GAME_OCR_MEM_USER, CDRAM, PHYCONT */ &&
            out.step[i].rc == (int32_t)0x80024302u) refused++;
    TEST_CHECK(refused == 3);
    TEST_MSG("workspace rungs refused in the step log: %u of %u steps", refused, out.n_steps);
    alloc_failure = 0; base_failure = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && frees == 1);
    base_failure = 0; free_failure = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(memory_live && blocked && out.cleanup_status && !out.text[0]);
    unsigned before = allocations;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE && allocations == before);
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_E_OCR_UNAVAILABLE && memory_live);
    free_failure = 0;
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !memory_live && !blocked);
    TEST_CHECK(!vjo_game_ocr_worker_start());
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out));
    TEST_CHECK(!vjo_game_ocr_worker_stop());
}
static void cancel_and_wire(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    cancel_job = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_CANCELLED && !allocations);
    cancel_job = 0; r.width = 961;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
    r.width = 200; r.x = 1; /* past the frame's edge */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
    r.x = 0; r.flags = 0x80;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
    r.flags = 0; r.speed = 0x8; /* API 21: VJO_GAME_OCR_S_* bits 3-31 unknown */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
    r.speed = 0;
    r.flags = 0; r.model[31] = 'x';
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations);
}
static void real_model_io_and_cancellation(void)
{
    if (!VJO_VOCR_MODEL_DIR[0]) { TEST_MSG("Release weights absent; model I/O test skipped"); return; }
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out; line(); short_reads = 1;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out));
    TEST_CHECK(bytes_read == 657664 && reads_count > 1000 && out.lines == 1);
    TEST_CHECK(!memory_live && !model && !out.cleanup_status);
    TEST_CHECK(!user_open_calls); /* Model access never uses the game's unavailable ux0. */
    open_failure = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_MODEL && out.io_error == -123);
    open_failure = 0; bad_read = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_MODEL && out.io_error == -456);
    bad_read = 0; close_failure = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE && model && !out.text[0]);
    close_failure = 0; TEST_CHECK(!vjo_game_ocr_worker_stop() && !model);
    reads_count = 0; cancel_reads = 4;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_CANCELLED && !model && !memory_live);
}
/* These are always run in CI, including checkouts without release weights. */
static void model_callbacks_timeouts_errors_and_cleanup(void)
{
    setup(); active_seq = 12; virtual_file = 1;
    VjoFile file; unsigned char bytes[16];
    open_failure = 1;
    TEST_CHECK(file_open(NULL, "ignored", &file) == -1 && io_error == -123);
    TEST_CHECK(model_owned && model_seq == 12);
    file_close(NULL); TEST_CHECK(!model_owned && !model_seq);
    open_failure = 0; open_busy = close_busy = 1;
    TEST_CHECK(file_open(NULL, "ignored", &file) == -1 && io_error == VJO_ERR_TIMEOUT);
    TEST_CHECK(model_owned && model_seq == 12 && !model);
    active_seq = 0; file_close(NULL);
    TEST_CHECK(model_owned && model_seq == 12 && io_error == VJO_ERR_TIMEOUT);
    close_busy = 0; TEST_CHECK(!vjo_game_ocr_worker_stop() && !model_owned && !model_seq);
    quitting = 0; active_seq = 12; open_busy = 0;
    TEST_CHECK(!file_open(NULL, "ignored", &file) && file.size == sizeof(bytes));
    TEST_CHECK(!file.read(NULL, 0, bytes, sizeof(bytes)) && bytes[0] == 1 && bytes[3] == 4);
    read_busy = 1;
    TEST_CHECK(file.read(NULL, 0, bytes, sizeof(bytes)) == -1 && io_error == VJO_ERR_TIMEOUT);
    read_busy = 0; bad_read = 1;
    TEST_CHECK(file.read(NULL, 0, bytes, sizeof(bytes)) == -1 && io_error == -456);
    bad_read = 0; read_eof = 1;
    TEST_CHECK(file.read(NULL, 0, bytes, sizeof(bytes)) == -1 && io_error == -1);
    read_eof = 0;
    TEST_CHECK(file.read(NULL, UINT32_MAX, bytes, sizeof(bytes)) == -1);
    TEST_CHECK(file.read(NULL, 0, NULL, sizeof(bytes)) == -1);
    cancel_job = 1; TEST_CHECK(file.read(NULL, 0, bytes, sizeof(bytes)) == -1);
    active_seq = 0; close_failure = 1; file.close(NULL);
    TEST_CHECK(model && model_owned && model_seq == 12 && io_error == -789);
    close_failure = 0; TEST_CHECK(!vjo_game_ocr_worker_stop() && !model && !model_owned);
}
static void system_app_gate_and_failed_joins(void)
{
    setup();
    /* SceShell ("main") and the system apps: the module unloads itself at
     * once (NO_RESIDENT), so *ALL costs them no pages; so does an unreadable title. */
    const char *excluded[] = {"main", "NPXS10015", "VITASHELL", "VJOSET001", ""};
    for (unsigned i=0; i<sizeof(excluded)/sizeof(excluded[0]); i++) {
        strcpy(own_title, excluded[i]);
        TEST_CHECK_(module_start(0, NULL) == SCE_KERNEL_START_NO_RESIDENT && !thread_creates && !allocations, "%s", excluded[i]);
    }
    /* A safe boot (the previous one failed): the module unloads itself in
     * every process, a candidate game included, before reading any title. */
    safe_boot_mode = VJO_BOOT_SAFE; boot_info_calls = 0;
    strcpy(own_title, "PCSG00415");
    TEST_CHECK(module_start(0, NULL) == SCE_KERNEL_START_NO_RESIDENT && boot_info_calls == 1 && !thread_creates && !allocations);
    safe_boot_mode = VJO_BOOT_NORMAL;
    strcpy(own_title, "PCSG00415"); TEST_CHECK(!module_start(0, NULL) && thread_creates == 1);
    join_failure = 1;
    TEST_CHECK(module_stop(0, NULL) == SCE_KERNEL_STOP_FAIL && worker == 20 && worker_started);
    join_failure = 0; TEST_CHECK(!module_stop(0, NULL) && worker == -1);
    start_failure = delete_failure = 1;
    TEST_CHECK(!module_start(0, NULL) && worker == 20 && !worker_started);
    TEST_CHECK(module_stop(0, NULL) == SCE_KERNEL_STOP_FAIL && worker == 20);
    delete_failure = 0; TEST_CHECK(!module_stop(0, NULL));
}
/* The capture's memory ladder: USER_RW, then CDRAM, then PHYCONT, each attempt
 * a step with its native result; nothing left at all: out of memory, logged. */
static void capture_memory_ladder_is_logged(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    line();
    refuse_user = 1;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) || out.rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
    TEST_CHECK(out.capture_memory == VJO_GAME_OCR_MEM_CDRAM && out.capture_bytes == 256u * 1024u);
    unsigned allocs = 0, refused = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_ALLOC && out.step[i].arg == VJO_GITEM_CAPTURE) {
            allocs++;
            if (out.step[i].rc == (int)0x80024302u) { refused++; TEST_CHECK(out.step[i].kind == VJO_GAME_OCR_MEM_USER); }
            else TEST_CHECK(out.step[i].kind == VJO_GAME_OCR_MEM_CDRAM && out.step[i].bytes == 256u * 1024u);
            TEST_CHECK(out.step[i].free_main == 16*1024*1024 - (int)live_bytes || out.step[i].free_main > 0);
        }
    TEST_CHECK(allocs == 2 && refused == 1 && !capture_live);
    refuse_cdram = 1;
    vjo_game_ocr_run_request(&r, &out);
    TEST_CHECK(out.capture_memory == VJO_GAME_OCR_MEM_PHYCONT && out.capture_bytes == 1024u * 1024u && !capture_live);
    refuse_phycont = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && !capture_live && !memory_live);
    allocs = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_ALLOC && out.step[i].arg == VJO_GITEM_CAPTURE) allocs++;
    TEST_CHECK(allocs == 3);
}
/* The hook cannot write into the block: the same rows through the kernel strip,
 * for the rest of the job, and the step log says so. */
static void hook_failure_falls_back_to_the_kernel_strip(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    hook_write_error = 1;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && !out.text[0]);
    TEST_CHECK(out.source == VJO_GAME_OCR_SRC_KSTRIP && kstrip_calls == rows_calls - 1);
    int saw = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_PASS && out.step[i].kind == VJO_GAME_OCR_SRC_HOOK && out.step[i].rc == VJO_ERR_COPY) saw = 1;
    TEST_CHECK(saw);
    /* a timeout (no frame) is not something a kernel strip fixes: the job fails, logged */
    hook_write_error = 0; rows_error = VJO_ERR_TIMEOUT; rows_calls = kstrip_calls = 0;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !kstrip_calls && !capture_live && !memory_live);
    int logged = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_PASS && out.step[i].rc == VJO_ERR_TIMEOUT) logged = 1;
    TEST_CHECK(logged);
}
/* A BUSY before arming (a previous copy not tidied yet) is asked again; one
 * after arming (the copy outlived the call) keeps the block for the hook and
 * stops the worker. */
static void busy_rows_calls(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    busy_unarmed = 3;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && !blocked && !hook_owns_capture && !capture_live);
    int logged = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_PASS && out.step[i].rc == VJO_ERR_BUSY) logged++;
    TEST_CHECK(logged == 1 && busy_unarmed == 0);
    busy_unarmed = 1000; /* never tidied: the job gives up, the worker stays usable */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !blocked && !capture_live && !memory_live);
    busy_unarmed = 0; busy_armed = 1; allow_capture_held = 1; rows_calls = 0;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) != VJO_OK);
    /* the armed BUSY is not retried, so the unarmed ones after it never hide it */
    TEST_CHECK(rows_calls == 1 && blocked && hook_owns_capture && capture_live && !memory_live);
}

/* The probe tries every rung once and recognizes nothing. */
static void probe_tries_every_rung(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    r.flags = VJO_GAME_OCR_F_PROBE;
    refuse_phycont = 1;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && !memory_live && !capture_live && !model);
    unsigned probes = 0, hook_ok = 0, kstrip_ok = 0, user_fail = 0, phycont_refused = 0;
    uint32_t sum = 0;
    for (unsigned i = 0; i < out.n_steps; i++) {
        const VjoGameOcrStep *s = &out.step[i];
        if (s->id == VJO_GSTEP_ALLOC && s->kind == VJO_GAME_OCR_MEM_PHYCONT && s->rc < 0) phycont_refused++;
        if (s->id != VJO_GSTEP_PROBE) continue;
        probes++;
        if (s->kind == VJO_GAME_OCR_SRC_HOOK && s->rc == 60) { hook_ok++; if (!sum) sum = s->sum; TEST_CHECK(s->sum == sum); }
        if (s->kind == VJO_GAME_OCR_SRC_KSTRIP && s->rc == 60) { kstrip_ok++; TEST_CHECK(s->sum == sum); }
        if (s->kind == VJO_GAME_OCR_SRC_USER && s->rc < 0) user_fail++;
    }
    /* USER and CDRAM blocks, hook and kernel strip each, plus the user-mode read once */
    TEST_CHECK(probes == 5 && hook_ok == 2 && kstrip_ok == 2 && user_fail == 1 && phycont_refused == 1);
    TEST_CHECK(out.strip_rows == 60 && !out.lines && allocations == 0);
}
/* The probe's user-mode read of the frame stays inside the frame's own block:
 * the frame (200x60, 48,000 B) is in 64 KiB of mapped memory, and each refused
 * case makes the bytes outside the block it reports unreadable, so a read past
 * the check faults this test instead of passing it. */
static int probe_user_read(const VjoGameOcrRequest *r, uint32_t *sum)
{
    VjoGameOcrResult out;
    int rc = INT32_MIN;
    TEST_CHECK(!vjo_game_ocr_run_request(r, &out) && !capture_live && !memory_live);
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_PROBE && out.step[i].kind == VJO_GAME_OCR_SRC_USER) {
            rc = out.step[i].rc;
            *sum = out.step[i].sum;
        } else if (out.step[i].id == VJO_GSTEP_PROBE && out.step[i].kind == VJO_GAME_OCR_SRC_HOOK && !*sum) {
            *sum = out.step[i].sum; /* the hook's rows of the same frame */
        }
    return rc;
}
static void probe_user_read_stays_in_the_frame_block(void)
{
    VjoGameOcrRequest r = setup();
    uint32_t hook_sum = 0, sum = 0;
    r.flags = VJO_GAME_OCR_F_PROBE;
    refuse_phycont = 1;
    line();
    unsigned char *map = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(map != MAP_FAILED);
    frame_at = map;
    memcpy(frame_at, pixels, sizeof(pixels));
    /* the whole frame in one readable block: 60 rows, the same grey as the hook's */
    block_base = (uintptr_t)map; block_size = 0x10000; block_access = SCE_KERNEL_MEMORY_ACCESS_R;
    int rc = probe_user_read(&r, &hook_sum);
    sum = hook_sum;
    TEST_CHECK_(rc == 60, "user-mode read: rc %d", rc);
    TEST_CHECK(frame_info_calls == 1 && hook_sum != 0);
    /* the block ends before the frame's last row: refused, the rest never read */
    block_size = 0xA000;
    TEST_ASSERT(mprotect(map + 0xA000, 0x6000, PROT_NONE) == 0);
    sum = 0;
    TEST_CHECK(probe_user_read(&r, &sum) == VJO_ERR_ARG);
    TEST_ASSERT(mprotect(map, 0x10000, PROT_READ | PROT_WRITE) == 0);
    /* the exact edge: the block one byte short of the frame's last byte, nothing unreadable.
     * The read's last byte is first + (n-1)*pitch*bpp + width*bpp - 1 = 48,000-1; a block of
     * 47,999 B must refuse it, so a check that drops the last row's width or a row of the end
     * (which still fits 47,999) is caught here, not only by the unreadable bytes. */
    block_size = 48000 - 1;
    sum = 0;
    TEST_CHECK(probe_user_read(&r, &sum) == VJO_ERR_ARG);
    block_size = 48000; /* exactly enough: read */
    sum = 0;
    TEST_CHECK(probe_user_read(&r, &sum) == 60);
    block_size = 0x10000;
    /* the block begins after the frame's first row */
    block_base = (uintptr_t)map + 0x1000; block_size = 0xF000;
    TEST_ASSERT(mprotect(map, 0x1000, PROT_NONE) == 0);
    sum = 0;
    TEST_CHECK(probe_user_read(&r, &sum) == VJO_ERR_ARG);
    /* a block this process cannot read */
    block_base = (uintptr_t)map; block_size = 0x10000; block_access = 0x02; /* write only */
    TEST_ASSERT(mprotect(map, 0x10000, PROT_NONE) == 0);
    sum = 0;
    TEST_CHECK(probe_user_read(&r, &sum) == VJO_ERR_ARG);
    /* a frame of another size than the job's: refused before its block is asked */
    TEST_ASSERT(mprotect(map, 0x10000, PROT_READ | PROT_WRITE) == 0);
    block_access = SCE_KERNEL_MEMORY_ACCESS_R;
    frame_w = 240; frame_info_calls = 0;
    sum = 0;
    TEST_CHECK(probe_user_read(&r, &sum) == VJO_ERR_FORMAT && !frame_info_calls);
    frame_w = 200;
    munmap(map, 0x10000);
    frame_at = NULL;
}
/* API 20: the job's policy rides in the request's flags. A wait of 150 ms
 * (15 x 10 ms) first, at 60 frames a second, on a still screen: the job reads
 * the region until it held still, records the wait as a step, and only then
 * finds its line; the blank-line check passes a line of strokes. Unknown flag
 * bits are refused before anything is allocated. */
/* The request's flags become the job's policy (include/vjo_game_ocr.h): the
 * wait in 10 ms steps, its limit in 50 ms steps, settle_first, the blank-line
 * check; a request without them is API 19's job (a zero policy but for the
 * sleep callback, which only a wait uses). */
static void request_flags_become_the_policy(void)
{
    VjoGameOcrRequest r = setup();
    VjoVocrPolicy p;
    request_policy(&r, &p);
    TEST_CHECK(!p.settle_us && !p.settle_max_us && !p.settle_first && !p.blank && p.sleep == policy_sleep);
    r.flags = VJO_GAME_OCR_F_BLANK;
    request_policy(&r, &p);
    TEST_CHECK(p.blank == 1 && !p.settle_us && !p.settle_first);
    r.flags = VJO_GAME_OCR_F_SETTLE_FIRST | 15u << VJO_GAME_OCR_SETTLE_SHIFT | 40u << VJO_GAME_OCR_SETTLE_MAX_SHIFT;
    request_policy(&r, &p);
    TEST_CHECK(p.settle_us == 150000u && p.settle_max_us == 2000000u && p.settle_first == 1 && !p.blank);
    r.flags = 255u << VJO_GAME_OCR_SETTLE_SHIFT | 255u << VJO_GAME_OCR_SETTLE_MAX_SHIFT; /* the largest the bits hold */
    request_policy(&r, &p);
    TEST_CHECK(p.settle_us == 2550000u && p.settle_max_us == 12750000u && !p.settle_first);
}

static void policy_in_the_flags(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    unsigned plain;
    line();
    frame_us = 16667;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) || out.rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
    plain = rows_calls;
    for (unsigned i = 0; i < out.n_steps; i++) TEST_CHECK(out.step[i].id != VJO_GSTEP_SETTLE);
    rows_calls = 0;
    r.flags = VJO_GAME_OCR_F_BLANK | VJO_GAME_OCR_F_SETTLE_FIRST | 15u << VJO_GAME_OCR_SETTLE_SHIFT |
              40u << VJO_GAME_OCR_SETTLE_MAX_SHIFT;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) || out.rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
    const VjoGameOcrStep *settle = NULL;
    unsigned blank = 0;
    for (unsigned i = 0; i < out.n_steps; i++) {
        if (out.step[i].id == VJO_GSTEP_SETTLE) settle = &out.step[i];
        if (out.step[i].id == VJO_GSTEP_BLANK) blank++;
    }
    TEST_ASSERT(settle != NULL);
    TEST_CHECK(settle->rc == VJO_GSETTLE_HELD && settle->arg >= 2 && settle->bytes == 0 && settle->us >= 150000);
    TEST_MSG("waited %u us in %u passes; %u rows calls against %u without", (unsigned)settle->us, settle->arg,
             rows_calls, plain);
    TEST_CHECK(!blank && out.lines == 1 && rows_calls > plain && !memory_live && !capture_live);
    allocations = 0;
    r.flags = 0x20u; /* bits 5-15 mean nothing yet */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_SOURCE && !allocations && !out.n_steps);
}

/* API 21 (ocr-speed): helpers and the ARM clock ride in the flags. Helpers are
 * counted in the memory check and dropped one at a time when they do not fit;
 * the clock goes to 444 MHz around the recognition only and back after it. */
static void helpers_and_clock_in_the_flags(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    uint32_t plain_need, extra2 = (uint32_t)helpers_need(r.model, 200, 60, 2);
    uint32_t extra1 = (uint32_t)helpers_need(r.model, 200, 60, 1);
    const VjoGameOcrStep *clock = NULL, *par = NULL;
    line();
    arm_mhz = 333; arm_sets = 0; helper_threads = helper_live = helper_starts = 0;
    vjo_game_ocr_run_request(&r, &out);
    plain_need = out.need_workspace;
    TEST_CHECK(arm_sets == 0 && plain_need > 0);
    for (unsigned i = 0; i < out.n_steps; i++) TEST_CHECK(out.step[i].id != VJO_GSTEP_CLOCK && out.step[i].id != VJO_GSTEP_PAR);
    /* two helpers asked for, room for both: counted, stacks included */
    TEST_CHECK(extra1 > 2u * VJO_BUDGET_GAME_HELPER_STACK / 2u && extra2 > extra1);
    r.speed = 2u | VJO_GAME_OCR_S_BOOST;
    memset(&line_cache, 0, sizeof(line_cache)); /* the plain job's line is cached: recognize it again */
    int rc = vjo_game_ocr_run_request(&r, &out);
    TEST_CHECK(!rc || out.rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
    TEST_CHECK(out.need_workspace == plain_need + extra2);
    TEST_MSG("need %u, without helpers %u, two helpers add %u", (unsigned)out.need_workspace, plain_need, extra2);
    TEST_CHECK(!memory_live && !capture_live && helper_live == 0);
    for (unsigned i = 0; i < out.n_steps; i++) {
        if (out.step[i].id == VJO_GSTEP_CLOCK) clock = &out.step[i];
        if (out.step[i].id == VJO_GSTEP_PAR) par = &out.step[i];
    }
    if (VJO_VOCR_MODEL_DIR[0] && !rc && out.recognized) {
        /* one line: it runs on the worker's own thread (the last line always does) */
        TEST_ASSERT(clock != NULL && par != NULL);
        TEST_CHECK(clock->arg == 333 && clock->bytes == 444 && clock->sum == 333 && clock->rc == 0);
        TEST_CHECK(arm_sets == 2 && arm_set_last == 333 && arm_mhz == 333);
        TEST_CHECK(par->arg == 2 && par->rc == 0 && par->bytes == extra2 && helper_threads == 0);
        TEST_CHECK(((par->sum >> 16) & 0xFFu) == 0); /* no line from the cache */
        /* already at 444 or more: not raised, not lowered */
        arm_mhz = 444; arm_sets = 0;
        resident.model = NULL; /* weights again, so the line is recognized, not cached */
        memset(&line_cache, 0, sizeof(line_cache));
        TEST_CHECK(!vjo_game_ocr_run_request(&r, &out));
        clock = NULL;
        for (unsigned i = 0; i < out.n_steps; i++) if (out.step[i].id == VJO_GSTEP_CLOCK) clock = &out.step[i];
        TEST_CHECK(clock && clock->rc == 1 && clock->arg == 444 && arm_sets == 0 && arm_mhz == 444);
        /* the same screen again: the line comes from the cache, no clock change, no recognizer */
        arm_mhz = 333; arm_sets = 0;
        TEST_CHECK(!vjo_game_ocr_run_request(&r, &out));
        par = NULL;
        for (unsigned i = 0; i < out.n_steps; i++) if (out.step[i].id == VJO_GSTEP_PAR) par = &out.step[i];
        TEST_CHECK(arm_sets == 0 && out.frames == 0 && out.recognized == 1 && par && ((par->sum >> 16) & 0xFFu) == 1);
    }
    /* free memory for one helper only, then for none: fewer helpers, never a refusal
     * (the resident weights, if any, are already part of live_bytes) */
    TEST_ASSERT(resident.model != NULL || !VJO_VOCR_MODEL_DIR[0]); /* weights resident: they need nothing more */
    available = (int)(live_bytes + VJO_GAME_RESERVE + plain_need + extra1 + 4096u);
    if (!resident.model) available += (int)PAGE_ROUND(vjo_vocr_model(r.model)->bytes);
    vjo_game_ocr_run_request(&r, &out);
    TEST_CHECK(out.need_workspace == plain_need + extra1);
    TEST_MSG("need %u (plain %u, +1 helper %u, +2 %u)", (unsigned)out.need_workspace, plain_need, extra1, extra2);
    available = (int)(live_bytes + VJO_GAME_RESERVE + plain_need + 4096u);
    if (!resident.model) available += (int)PAGE_ROUND(vjo_vocr_model(r.model)->bytes);
    vjo_game_ocr_run_request(&r, &out);
    TEST_CHECK(out.need_workspace == plain_need && out.rc != VJO_E_OOM);
    TEST_MSG("need %u (plain %u, +1 helper %u)", (unsigned)out.need_workspace, plain_need, extra1);
    arm_mhz = 333;
}

/* The step log: a retried job logs its first pass and its signature once (the
 * TEAR step counts the tries), and when the log fills up the other steps make
 * room for the release of every block the worker holds, so the FREE steps that
 * show the job gave its memory back are never the ones lost. */
static void step_log_keeps_the_releases(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    unsigned pass = 0, sig = 0, tear = 0, frees = 0;
    line();
    change_at_rows = 2; /* during the finder's second pass of the first try */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == 0 || out.rc == VJO_E_OCR_MODEL || !VJO_VOCR_MODEL_DIR[0]);
    for (unsigned i = 0; i < out.n_steps; i++) {
        pass += out.step[i].id == VJO_GSTEP_PASS && !out.step[i].rc;
        sig += out.step[i].id == VJO_GSTEP_SIG;
        if (out.step[i].id == VJO_GSTEP_TEAR) { tear++; TEST_CHECK(out.step[i].rc >= 2); }
        frees += out.step[i].id == VJO_GSTEP_FREE;
    }
    TEST_CHECK(tear == 1 && pass == 1 && sig == 1 && frees == 2 && !memory_live && !capture_live);
    TEST_MSG("%u steps: %u PASS, %u SIG, %u TEAR, %u FREE", out.n_steps, pass, sig, tear, frees);
    /* every block the job took is back, and counted back */
    TEST_CHECK(blocks_held == (unsigned)(weights_live ? 1 : 0));
    /* a full log: with two blocks held, the 19th and 20th steps are the two FREEs */
    VjoGameOcrResult log;
    memset(&log, 0, sizeof(log));
    log_to = &log;
    blocks_held = 2;
    for (unsigned i = 0; i < VJO_GAME_OCR_STEPS; i++) step_add(VJO_GSTEP_ALLOC, 0, 0, 0, 0, 0, 1, 0);
    TEST_CHECK(log.n_steps == VJO_GAME_OCR_STEPS - 2);
    TEST_CHECK(step_add(VJO_GSTEP_FREE, 0, 1, 0, 4096, 0, 1, 0) && step_add(VJO_GSTEP_FREE, 0, 2, 0, 4096, 0, 1, 0));
    TEST_CHECK(log.n_steps == VJO_GAME_OCR_STEPS && !step_add(VJO_GSTEP_FREE, 0, 3, 0, 4096, 0, 1, 0));
    TEST_CHECK(log.step[VJO_GAME_OCR_STEPS - 1].id == VJO_GSTEP_FREE && log.step[VJO_GAME_OCR_STEPS - 1].arg == 2);
    log_to = NULL;
    blocks_held = 0;
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live);
}

/* The self-test's fallback stage: the memory check refuses the model on
 * purpose and the fallback runs. */
static void forced_fallback_runs_the_fallback(void)
{
    VjoGameOcrRequest r = setup(); VjoGameOcrResult out;
    snprintf(r.model, sizeof(r.model), "H15_w8.vocr");
    snprintf(r.fallback, sizeof(r.fallback), "FL10_w8.vocr");
    r.flags = VJO_GAME_OCR_F_FORCE_FALLBACK;
    TEST_CHECK(!vjo_game_ocr_run_request(&r, &out) && out.fallback == 1);
    int forced = 0;
    for (unsigned i = 0; i < out.n_steps; i++)
        if (out.step[i].id == VJO_GSTEP_FALLBACK && out.step[i].arg == 1) forced = 1;
    TEST_CHECK(forced);
}
TEST_LIST = {
    {"100 real vocr jobs and game USER reserve", repeated_jobs_and_reserve},
    {"allocation base free failure and retry", memory_failure_and_retry},
    {"cancel before allocation and wire validation", cancel_and_wire},
    {"real model bounded short reads errors and cancellation", real_model_io_and_cancellation},
    {"model callbacks without weights timeouts errors and saved cleanup sequence", model_callbacks_timeouts_errors_and_cleanup},
    {"ALL system gate and retained thread joins", system_app_gate_and_failed_joins},
    {"capture memory ladder USER CDRAM PHYCONT each logged", capture_memory_ladder_is_logged},
    {"the kernel strip after a failed hook write; timeouts are not retried", hook_failure_falls_back_to_the_kernel_strip},
    {"BUSY rows calls: asked again before arming, the block kept after", busy_rows_calls},
    {"the self-test probe tries every rung", probe_tries_every_rung},
    {"the probe's user-mode frame read stays inside the frame's block", probe_user_read_stays_in_the_frame_block},
    {"the self-test fallback stage forces the fallback", forced_fallback_runs_the_fallback},
    {"the job policy rides in the flags: the wait, its step, unknown bits refused", policy_in_the_flags},
    {"the request's flags become the job's policy", request_flags_become_the_policy},
    {"the step log logs a retried pass once and keeps the releases", step_log_keeps_the_releases},
    {"API 21: helpers and the ARM clock ride in the flags, counted and restored", helpers_and_clock_in_the_flags},
    {NULL, NULL}
};
