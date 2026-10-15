/* The actual game worker (game_ocr/main.c) and core adapter (core/vocr_ocr.c)
 * with synthetic pinned weights (test_resident_models.h), so the resident
 * weights' lifecycle runs where the release files are absent (CI): read and
 * SHA-256 checked once, kept while the file's size and modification time stay
 * the same, released on change, failure, model change, refused preflight,
 * inactive game and stop. The line finder and recognizer are spies: this
 * makes no inference, accuracy or console-timing claim. */
#include "acutest.h"
#include <psp2/host_stubs.h>
#include "vocr.h"
#include "vocr_lines.h"
#include "vocr_lines_rows.h"
#include "test_resident_weights.h"
#include "../../game_ocr/main.c"
#include <bearssl_hash.h>

#define W 200u
#define H 60u
static unsigned char job_memory[1024*1024] __attribute__((aligned(64)));
static unsigned char weights_memory[VJO_VOCR_F20_BYTES + 4096] __attribute__((aligned(64)));
static unsigned char pixels[W*H*4];
static unsigned char capture_mem_block[64*1024] __attribute__((aligned(64)));
static int capture_live;
static unsigned capture_block, capture_allocs;
static int64_t available;
static unsigned live_bytes, peak_bytes, job_block, weights_block;
static int job_live, weights_live, job_alloc_failure, weights_alloc_failure, weights_free_failure;
static unsigned job_allocs, job_frees, weights_allocs, weights_frees;
static int registered_rc, quit_after_delay, cancel_now, cancel_at_open, cancel_after_reads;
static const char *served;        /* the model the kernel would open for this job */
static int open_failure, size_delta, stat_failure, corrupt;
static uint64_t mtime;
static unsigned opens, closes, reads, bytes_read, loads, delays;
static int file_open_now;
static uint64_t clock_value;
static unsigned spy_lines = 1;
static int free_query_error;
static uint32_t opened_which;

/* ---- the Vita SDK and kernel transport, as the worker sees them ---- */
SceUID sceKernelGetProcessId(void) { return 7; }
uint64_t sceKernelGetProcessTimeWide(void) { return clock_value++; }
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out)
{ out->size_user = (int)(available - live_bytes); return free_query_error; }
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned size, void *opts)
{
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && !(size & 4095));
    if (!strcmp(name, "VjoGameVocrCapture")) {
        TEST_CHECK(!capture_live && size <= sizeof(capture_mem_block));
        if (job_alloc_failure) return -1;
        capture_live = 1; capture_block = size; capture_allocs++;
        live_bytes += size; if (live_bytes > peak_bytes) peak_bytes = live_bytes;
        return 11;
    }
    if (!strcmp(name, "VjoGameVocrWeights")) {
        TEST_CHECK(!weights_live && size <= sizeof(weights_memory)); /* never two weight blocks */
        weights_allocs++;
        if (weights_alloc_failure) return -1;
        weights_live = 1; weights_block = size;
    } else {
        TEST_CHECK(!strcmp(name, "VjoGameVocr") && !job_live && size <= sizeof(job_memory));
        job_allocs++;
        if (job_alloc_failure) return -1;
        job_live = 1; job_block = size;
    }
    live_bytes += size;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return !strcmp(name, "VjoGameVocrWeights") ? 10 : 9;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid == 11) { TEST_CHECK(capture_live); *base = capture_mem_block; return 0; }
    TEST_CHECK(uid == 10 ? weights_live : uid == 9 && job_live);
    *base = uid == 10 ? (void *)weights_memory : (void *)job_memory; return 0;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    if (uid == 11) { TEST_CHECK(capture_live); capture_live = 0; live_bytes -= capture_block; return 0; }
    if (uid == 10) {
        TEST_CHECK(weights_live);
        if (weights_free_failure) return -1;
        weights_live = 0; weights_frees++; live_bytes -= weights_block; return 0;
    }
    TEST_CHECK(uid == 9 && job_live);
    job_live = 0; job_frees++; live_bytes -= job_block; return 0;
}
SceUID sceKernelCreateThread(const char *n, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu, void *opt) { return 20; }
int sceKernelStartThread(SceUID uid, SceSize size, void *args) { return 0; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { return 0; }
int sceKernelDeleteThread(SceUID uid) { return 0; }
int sceKernelDelayThread(unsigned us)
{ delays++; clock_value += us; if (quit_after_delay) quitting = 1; return 0; }
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size) { snprintf(out, size, "PCSG00415"); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize cap) { return -1; }
SceUID sceIoOpen(const char *path, int flags, unsigned mode) { return (int)0x80010013u; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
int vjoBootInfo(VjoBootInfo *out) { out->mode = VJO_BOOT_NORMAL; return 0; }
int vjoOcrRegister(void) { return registered_rc; }
/* take_wait_us: the kernel's idle vjoOcrTake waits this long for a job before
 * answering 1 (100 ms since the hand-off waits; 0: an older kernel, at once);
 * take_job: the first call claims a job (an empty request: the worker refuses it
 * at once and completes it); take_quit: the worker stops after this many calls. */
static unsigned take_calls, take_wait_us, take_quit;
static int take_job, take_perm; /* take_perm: every take answers PERM (the game stopped being the active one) */
int vjoOcrTake(VjoGameOcrRequest *out)
{
    take_calls++;
    if (take_quit && take_calls >= take_quit) quitting = 1;
    if (take_job && take_calls == 1) { memset(out, 0, sizeof(*out)); return 0; }
    clock_value += take_wait_us;
    return take_perm ? VJO_ERR_PERM : 1;
}
int vjoOcrCancelled(uint32_t seq)
{ return cancel_now || (cancel_after_reads && reads >= (unsigned)cancel_after_reads); }
static unsigned complete_failures; /* the next completions the kernel refuses (BUSY) */
int vjoOcrComplete(const VjoGameOcrResult *out)
{
    TEST_CHECK(!job_live && !capture_live);
    if (complete_failures) { complete_failures--; return VJO_ERR_BUSY; }
    return 0;
}
/* The kernel's rows: grey of the pixels, straight into the worker's capture block. */
int vjoOcrCaptureRows(const VjoGameOcrRows *req, void *dst, VjoGameOcrRowsInfo *info)
{
    TEST_CHECK(req->seq == 12 && req->y < H && req->n && req->n <= H - req->y && capture_live);
    for (unsigned r = 0; r < req->n; r++)
        for (unsigned x = 0; x < W; x++) {
            const unsigned char *p = pixels + ((req->y + r) * W + x) * 4;
            ((unsigned char *)dst)[r * req->dst_stride + x] = (unsigned char)((77u*p[0] + 150u*p[1] + 29u*p[2]) >> 8);
        }
    info->fb_width = W; info->fb_height = H; info->fb_pitch = W;
    return (int)req->n;
}
int sceDisplayGetFrameBuf(SceDisplayFrameBuf *fb, int sync) { return -1; }
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info) { return -1; }

static const VjoVocrModel *served_model(void) { return vjo_vocr_model(served); }
static uint32_t served_seed(void) { return !strcmp(served, "FL10_w8.vocr") ? TEST_FL10_SEED : TEST_H15_SEED; }
static VjoGameOcrRequest current; /* the claimed request, as the kernel holds it */
int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info)
{
    TEST_CHECK(seq == 12 && !file_open_now);
    /* The kernel opens one of the request's two pinned names, never a path. */
    TEST_ASSERT(which == VJO_GAME_OCR_OPEN_MODEL || which == VJO_GAME_OCR_OPEN_FALLBACK);
    served = which == VJO_GAME_OCR_OPEN_FALLBACK ? current.fallback : current.model;
    TEST_ASSERT(served[0] && vjo_vocr_model(served));
    opened_which = which;
    opens++;
    /* The Shell cancels while the kernel opens the file (it answers CANCELLED). */
    if (cancel_at_open) { cancel_now = 1; return VJO_ERR_CANCELLED; }
    if (open_failure) return open_failure;
    file_open_now = 1;
    uint32_t size = served_model()->bytes + (uint32_t)size_delta;
    *info = (VjoGameOcrModelInfo){sizeof(*info), size, stat_failure, 0, stat_failure ? 0 : mtime};
    return (int)size;
}
int vjoOcrModelRead(uint32_t seq, uint32_t off, uint32_t n, void *dst)
{
    TEST_CHECK(seq == 12 && file_open_now && n && n <= VJO_GAME_OCR_MODEL_CHUNK);
    reads++;
    for (uint32_t i = 0; i < n; i++)
        ((unsigned char *)dst)[i] = test_weight_byte(off + i, served_seed()) ^ (corrupt && off + i == 777 ? 1 : 0);
    bytes_read += n;
    return (int)n;
}
int vjoOcrModelClose(uint32_t seq) { TEST_CHECK(seq == 12); if (file_open_now) closes++; file_open_now = 0; return 0; }

/* ---- the runtime: spies, each checking it got the verified resident bytes ---- */
int vocr_find_lines_rows(const VocrLinesRows *src, int mode, void *arena, size_t bytes, VocrLineSet *out)
{
    memset(out, 0, sizeof(*out));
    out->count = spy_lines;
    for (unsigned i = 0; i < spy_lines; i++) out->lines[i] = (VocrLine){10, 2 + 3*i, 180 - 10*i, 3, 0};
    return VOCR_LINES_OK;
}
int vocr_load(vocr_model *m, const void *blob, size_t bytes)
{
    const VjoVocrModel *pin = served_model();
    loads++;
    TEST_CHECK(blob == (const void *)weights_memory && bytes == pin->bytes && weights_live);
    for (uint32_t i = 0; i < bytes; i += 997)
        if (((const unsigned char *)blob)[i] != test_weight_byte(i, served_seed())) { TEST_CHECK(!"resident bytes changed"); break; }
    memset(m, 0, sizeof(*m)); m->blob = blob; m->bytes = bytes;
    return VOCR_OK;
}
size_t vocr_arena_bytes(const vocr_model *m, const vocr_opts *o) { return served_model()->arena_bytes; }
int vocr_init(vocr_ctx *c, const vocr_model *m, const vocr_opts *o, void *arena, size_t bytes) { return VOCR_OK; }
/* Frames as the runtime counts them: resized width / 8; 1 ms per frame. */
int vocr_run(vocr_ctx *c, const vocr_image *img, char *text, size_t cap, vocr_extra *extra)
{
    snprintf(text, cap, "猫"); extra->width = img->width; extra->frames = img->width / 8;
    clock_value += 1000u * extra->frames; return VOCR_OK;
}

/* ---- helpers ---- */
static VjoGameOcrRequest request_with(const char *name, const char *fallback)
{
    VjoGameOcrRequest r = {.size=sizeof(r), .seq=12, .capture_seq=11, .fb_width=W, .fb_height=H, .width=W, .height=H};
    snprintf(r.model, sizeof(r.model), "%s", name);
    snprintf(r.fallback, sizeof(r.fallback), "%s", fallback);
    snprintf(r.model_dir, sizeof(r.model_dir), "%s", VJO_VOCR_MODEL_ROOT);
    served = name;
    current = r;
    return r;
}
static VjoGameOcrRequest request(const char *name) { return request_with(name, ""); }
static void reset(void)
{
    if (weights_live) { weights_free_failure = 0; TEST_CHECK(!drop_weights()); }
    worker = workspace_uid = weights_uid = capture_uid = line_uid = -1;
    capture_live = 0; capture_block = capture_allocs = 0;
    resident.model = NULL; resident.data = NULL; resident.capacity = 0; resident.size = resident.stamp = 0;
    model_owned = 0; model_seq = 0;
    quitting = active_seq = 0; worker_started = claim_outstanding = blocked = io_error = stat_error = 0;
    available = 64*1024*1024; live_bytes = peak_bytes = 0;
    job_live = weights_live = job_alloc_failure = weights_alloc_failure = weights_free_failure = 0;
    job_allocs = job_frees = weights_allocs = weights_frees = 0;
    registered_rc = quit_after_delay = cancel_now = cancel_at_open = cancel_after_reads = 0;
    take_calls = take_wait_us = take_quit = 0; take_job = take_perm = 0; complete_failures = 0;
    open_failure = size_delta = stat_failure = corrupt = 0; mtime = 0x12345678;
    opens = closes = reads = bytes_read = loads = delays = 0; file_open_now = 0;
    spy_lines = 1; free_query_error = 0;
    clock_value = 0;
    memset(pixels, 0, sizeof(pixels));
}
static int run_with(const char *name, const char *fallback, VjoGameOcrResult *out)
{
    VjoGameOcrRequest r = request_with(name, fallback);
    int rc = vjo_game_ocr_run_request(&r, out);
    TEST_CHECK(!job_live && !capture_live && !file_open_now && !model_owned && !out->cleanup_status);
    TEST_CHECK(weights_live == (resident.model != NULL));
    TEST_CHECK(out->resident_bytes == (weights_live ? weights_block : 0));
    return rc;
}
static int run(const char *name, VjoGameOcrResult *out)
{
    VjoGameOcrRequest r = request(name);
    int rc = vjo_game_ocr_run_request(&r, out);
    TEST_CHECK(!job_live && !file_open_now && !model_owned && !out->cleanup_status);
    /* Between jobs a weights block is held only for verified weights. */
    TEST_CHECK(weights_live == (resident.model != NULL));
    TEST_CHECK(out->resident_bytes == (weights_live ? weights_block : 0));
    return rc;
}

/* ---- tests ---- */
static void synthetic_pins_match_their_bytes(void)
{
    const char *names[] = {"FL10_w8.vocr", "H15_w8.vocr"};
    for (unsigned k = 0; k < 2; k++) {
        const VjoVocrModel *m = vjo_vocr_model(names[k]);
        uint32_t seed = k ? TEST_H15_SEED : TEST_FL10_SEED;
        br_sha256_context h; unsigned char digest[32], byte;
        br_sha256_init(&h);
        for (uint32_t i = 0; i < m->bytes; i++) { byte = test_weight_byte(i, seed); br_sha256_update(&h, &byte, 1); }
        br_sha256_out(&h, digest);
        TEST_CHECK(!memcmp(digest, m->sha256, 32));
    }
}
static void weights_are_read_once_and_kept(void)
{
    reset(); VjoGameOcrResult out;
    const uint32_t n = TEST_FL10_BYTES, pieces = (n + VJO_GAME_OCR_MODEL_CHUNK - 1) / VJO_GAME_OCR_MODEL_CHUNK;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && !strcmp(out.text, "猫"));
    TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_READ && bytes_read == n && reads == pieces);
    TEST_CHECK(weights_allocs == 1 && weights_live && out.resident_bytes == PAGE_ROUND(n));
    TEST_CHECK(out.workspace_bytes == vjo_vocr_job_bytes("FL10_w8.vocr", W, H));
    for (unsigned i = 0; i < 50; i++) {
        TEST_CHECK(!run("FL10_w8.vocr", &out) && !strcmp(out.text, "猫"));
        TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
    }
    /* Each job checks the file's size and time (Open/Close), none reads it again. */
    TEST_CHECK(bytes_read == n && reads == pieces && opens == 51 && closes == 51 && loads == 51);
    TEST_CHECK(weights_allocs == 1 && !weights_frees && job_allocs == 51 && job_frees == 51);
    TEST_CHECK(!delays); /* answered model calls never make the worker sleep */
}
static void changed_file_is_read_again(void)
{
    reset(); VjoGameOcrResult out;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
    mtime++; /* replaced, same size */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
    TEST_CHECK(bytes_read == 2*TEST_FL10_BYTES && weights_allocs == 1 && !weights_frees); /* same block reused */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
    size_delta = 1; /* a different length is never this model: refused, weights released */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OCR_MODEL && !out.text[0]);
    TEST_CHECK(!weights_live && weights_frees == 1 && !out.resident_bytes && bytes_read == 2*TEST_FL10_BYTES);
    size_delta = 0;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ && weights_allocs == 2);
}
static void unknown_modification_time_reads_every_job(void)
{
    reset(); VjoGameOcrResult out; stat_failure = (int)0x80010016u;
    for (unsigned i = 0; i < 3; i++) {
        TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
        TEST_CHECK(out.stat_error == stat_failure);
    }
    TEST_CHECK(bytes_read == 3*TEST_FL10_BYTES && weights_allocs == 1);
    stat_failure = 0; /* known again: the next job keys on it, then reuses */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ && !out.stat_error);
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
}
static void missing_or_corrupt_file_releases_weights(void)
{
    reset(); VjoGameOcrResult out;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_live);
    loads = 0;
    open_failure = (int)0x80010002u; /* deleted while the game runs */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OCR_MODEL && out.io_error == open_failure);
    TEST_CHECK(!weights_live && !out.resident_bytes);
    open_failure = 0; corrupt = 1; mtime++;
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OCR_MODEL && !weights_live && !loads);
    corrupt = 0;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_live && loads == 1);
    corrupt = 1; /* same size and time: the verified copy is used, the card is not read */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
}
static void cancellation_keeps_verified_and_drops_partial_weights(void)
{
    reset(); VjoGameOcrResult out;
    TEST_CHECK(!run("FL10_w8.vocr", &out));
    cancel_at_open = 1; /* cancelled while checking the file: proves nothing about it */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_CANCELLED && weights_live && resident.model);
    TEST_CHECK(out.resident_bytes == weights_block && !out.text[0]);
    cancel_at_open = cancel_now = 0;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
    mtime++; cancel_after_reads = (int)reads + 2; /* cancelled half-way through a new read */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_CANCELLED && !weights_live && !resident.model);
    cancel_after_reads = 0;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
}
static void model_change_releases_the_old_weights_first(void)
{
    reset(); VjoGameOcrResult out;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_block == PAGE_ROUND(TEST_FL10_BYTES));
    peak_bytes = live_bytes;
    TEST_CHECK(!run("H15_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
    TEST_CHECK(weights_frees == 1 && weights_allocs == 2 && weights_block == PAGE_ROUND(TEST_H15_BYTES));
    /* The alloc stub asserts one weights block at a time; the peak is H15's job. */
    TEST_CHECK(peak_bytes == PAGE_ROUND(TEST_H15_BYTES) + PAGE_ROUND(vjo_vocr_job_bytes("H15_w8.vocr", W, H)) +
                             PAGE_ROUND(vjo_vocr_capture_bytes(W, H)));
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_frees == 2 && weights_block == PAGE_ROUND(TEST_FL10_BYTES));
}
static void peak_is_the_old_single_block_within_a_page(void)
{
    const char *names[] = {"FL10_w8.vocr", "H15_w8.vocr"};
    for (unsigned k = 0; k < 2; k++) {
        reset(); VjoGameOcrResult out;
        TEST_CHECK(!run(names[k], &out));
        uint32_t old_block = PAGE_ROUND(vjo_vocr_workspace_bytes(names[k], W, H));
        TEST_CHECK(peak_bytes <= old_block + PAGE_ROUND(vjo_vocr_capture_bytes(W, H)) + 4096u);
        TEST_MSG("%s: peak %u, one-block %u", names[k], peak_bytes, old_block);
        TEST_CHECK(live_bytes == PAGE_ROUND(vjo_vocr_model(names[k])->bytes)); /* kept between jobs */
    }
}
static void preflight_counts_resident_weights_and_refusal_releases_them(void)
{
    reset(); VjoGameOcrResult out;
    const uint32_t job = PAGE_ROUND(vjo_vocr_job_bytes("FL10_w8.vocr", W, H)) + PAGE_ROUND(vjo_vocr_capture_bytes(W, H)),
                   weights = PAGE_ROUND(TEST_FL10_BYTES);
    available = GAME_RESERVE + job + weights - 1; /* the first job also needs the weights */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OOM && !job_allocs && !weights_allocs);
    available = GAME_RESERVE + job + weights;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_live);
    /* Resident weights are not counted twice: the reserve and the job fit exactly. */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
    available--; /* the game took a byte: the job no longer fits beside its reserve */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OOM && !weights_live && weights_frees == 1);
    TEST_CHECK(job_allocs == 2 && !out.resident_bytes); /* refused before allocating */
    weights_alloc_failure = 1; available = 64*1024*1024; /* the weights block itself is refused */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OOM && !weights_live && !out.text[0]);
    weights_alloc_failure = 0;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
}
static void inactive_game_and_stop_release_weights(void)
{
    reset(); VjoGameOcrResult out;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_live);
    registered_rc = VJO_ERR_BUSY; quit_after_delay = 1; /* another claim pending: keep them */
    worker_main(0, NULL);
    TEST_CHECK(weights_live && !blocked);
    quitting = 0; registered_rc = VJO_ERR_NO_GAME; /* suspended, backgrounded or disabled */
    worker_main(0, NULL);
    TEST_CHECK(!weights_live && !resident.model && !blocked);
    quitting = 0; registered_rc = 0; quit_after_delay = 0;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_live && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live && !resident.model);
}
/* The worker's loop keeps its pace whatever the kernel does: after an idle
 * vjoOcrTake that answered at once (an older kernel) it sleeps its 100 ms, so it
 * never spins; after one the kernel held for its 100 ms wait it asks again at
 * once (the wait is the pace, and a job submitted meanwhile is claimed when it
 * comes, not at the next poll); after a job it asks again at once. */
static void worker_loop_keeps_its_pace(void)
{
    reset(); take_quit = 5; /* an older kernel */
    worker_main(0, NULL);
    TEST_CHECK(take_calls == 5 && delays == 5 && clock_value >= 5 * 100000u);
    TEST_MSG("%u takes, %u sleeps", take_calls, delays);
    reset(); take_quit = 5; take_wait_us = 100000; /* the kernel waits */
    worker_main(0, NULL);
    TEST_CHECK(take_calls == 5 && delays == 0 && clock_value >= 5 * 100000u && clock_value < 6 * 100000u);
    TEST_MSG("%u takes, %u sleeps", take_calls, delays);
    reset(); take_quit = 3; take_wait_us = 100000; take_job = 1; /* a job, then idle */
    worker_main(0, NULL);
    TEST_CHECK(take_calls == 3 && delays == 0 && !claim_outstanding);
    reset(); take_quit = 3; take_job = 1; /* a job on an older kernel: the next take at once, then its sleeps */
    worker_main(0, NULL);
    TEST_CHECK(take_calls == 3 && delays == 2);
    /* a job whose completion the kernel refuses: the claim is retried after 50 ms, never at once */
    reset(); take_quit = 2; take_wait_us = 100000; take_job = 1; complete_failures = 1;
    worker_main(0, NULL);
    TEST_CHECK(take_calls == 2 && delays == 2 && !claim_outstanding);
    TEST_MSG("%u takes, %u sleeps", take_calls, delays);
    /* a take the kernel held and then refused (the game stopped being the active one during the
     * wait): the next round at once, as after any held take; refused at once: the 100 ms sleep */
    reset(); take_quit = 3; take_wait_us = 100000; take_perm = 1;
    worker_main(0, NULL);
    TEST_CHECK(take_calls == 3 && delays == 0);
    reset(); take_quit = 3; take_perm = 1;
    worker_main(0, NULL);
    TEST_CHECK(take_calls == 3 && delays == 3);
    /* a wait cut short (a job submitted and cancelled before it was taken): the 100 ms sleep */
    reset(); take_quit = 2; take_wait_us = 20000;
    worker_main(0, NULL);
    TEST_CHECK(take_calls == 2 && delays == 2);
    reset();
}
static void failed_release_blocks_until_stop_retries(void)
{
    reset(); VjoGameOcrResult out;
    TEST_CHECK(!run("FL10_w8.vocr", &out));
    weights_free_failure = 1; size_delta = 1; /* the refusal must release them, and cannot */
    VjoGameOcrRequest r = request("FL10_w8.vocr");
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE && blocked && weights_live);
    TEST_CHECK(out.cleanup_status && !out.text[0] && !resident.model);
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_E_OCR_UNAVAILABLE && weights_live);
    weights_free_failure = 0; size_delta = 0;
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live && !blocked);
    TEST_CHECK(!vjo_game_ocr_worker_start()); /* a restarted worker loads them again */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_live && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live);
}

static void memory_check_is_reported_also_when_refused(void)
{
    reset(); VjoGameOcrResult out;
    const uint32_t job = PAGE_ROUND(vjo_vocr_job_bytes("FL10_w8.vocr", W, H)) + PAGE_ROUND(vjo_vocr_capture_bytes(W, H)),
                   weights = PAGE_ROUND(TEST_FL10_BYTES);
    available = GAME_RESERVE + job + weights - 4096;
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OOM && !job_allocs);
    TEST_CHECK(out.free_user == (int32_t)available && out.reserve_bytes == GAME_RESERVE);
    TEST_CHECK(out.need_workspace == job && out.need_weights == weights);
    TEST_CHECK(out.workspace_bytes == vjo_vocr_job_bytes("FL10_w8.vocr", W, H)); /* was 0 */
    available = 64*1024*1024;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.free_user == (int32_t)available && out.need_weights == weights);
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.need_weights == 0); /* resident: only the job */
    TEST_CHECK(out.free_user == (int32_t)(available - weights));
    free_query_error = (int)0x80020001u; /* the query itself failed: refused, its error reported */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OOM && out.free_user == free_query_error && !weights_live);
    free_query_error = 0;
    VjoGameOcrRequest bad = request("FL10_w8.vocr"); bad.width = 961; /* ends before the check */
    TEST_CHECK(vjo_game_ocr_run_request(&bad, &out) == VJO_E_SOURCE && !out.reserve_bytes && !out.free_user);
}
static void frames_and_lines_are_reported(void)
{
    reset(); VjoGameOcrResult out;
    spy_lines = VJO_GAME_OCR_LINES;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.lines == VJO_GAME_OCR_LINES);
    TEST_CHECK(out.recognized == VJO_GAME_OCR_LINES);
    uint32_t frames = 0;
    for (unsigned i = 0; i < out.recognized; i++) {
        TEST_CHECK(out.line[i].width == 180 - 10*i && out.line[i].height == 3);
        TEST_CHECK(out.line[i].frames == (180 - 10*i) / 8 && out.line[i].us >= 1000u * out.line[i].frames);
        frames += out.line[i].frames;
    }
    TEST_CHECK(out.frames == frames && out.recognize_us >= 1000u * frames);
    TEST_MSG("frames %u recognize_us %u", out.frames, out.recognize_us);
    spy_lines = 0; /* nothing found: no frames, no lines, the file is not opened */
    unsigned before = opens;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && !out.frames && !out.recognized && opens == before);
}

/* Bytes the memory check counts for a model's job and weights (page-rounded). */
static uint32_t capture_of(void) { return PAGE_ROUND(vjo_vocr_capture_bytes(W, H)); }
/* The job's need: its workspace and its capture block (API 19). */
static uint32_t job_of(const char *name) { return PAGE_ROUND(vjo_vocr_job_bytes(name, W, H)) + capture_of(); }
static uint32_t weights_of(const char *name) { return PAGE_ROUND(vjo_vocr_model(name)->bytes); }

static void fallback_runs_only_when_the_model_does_not_fit(void)
{
    reset(); VjoGameOcrResult out;
    const uint32_t h15 = job_of("H15_w8.vocr") + weights_of("H15_w8.vocr");
    const uint32_t fl10 = job_of("FL10_w8.vocr") + weights_of("FL10_w8.vocr");
    TEST_ASSERT(fl10 < h15);
    /* Room for H15: it runs, the fallback is not used. */
    available = GAME_RESERVE + h15;
    TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && !strcmp(out.text, "猫"));
    TEST_CHECK(!out.fallback && opened_which == VJO_GAME_OCR_OPEN_MODEL && resident.model == vjo_vocr_model("H15_w8.vocr"));
    TEST_CHECK(out.need_workspace == job_of("H15_w8.vocr") && out.need_weights == weights_of("H15_w8.vocr"));
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live);
    /* One byte short for H15 but room for FL10: FL10 runs in the same job. */
    reset(); available = GAME_RESERVE + h15 - 1;
    TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && !strcmp(out.text, "猫"));
    TEST_CHECK(out.fallback == 1 && opened_which == VJO_GAME_OCR_OPEN_FALLBACK);
    TEST_CHECK(out.model_need_workspace == job_of("H15_w8.vocr") && out.model_need_weights == weights_of("H15_w8.vocr"));
    TEST_CHECK(out.need_workspace == job_of("FL10_w8.vocr") && out.need_weights == weights_of("FL10_w8.vocr"));
    TEST_CHECK(out.workspace_bytes == vjo_vocr_job_bytes("FL10_w8.vocr", W, H));
    TEST_CHECK(resident.model == vjo_vocr_model("FL10_w8.vocr") && weights_block == weights_of("FL10_w8.vocr"));
    TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_READ && bytes_read == TEST_FL10_BYTES);
    /* No room for FL10 either: refused, with FL10's numbers (the smallest check). */
    reset(); available = GAME_RESERVE + fl10 - 1;
    TEST_CHECK(run_with("H15_w8.vocr", "FL10_w8.vocr", &out) == VJO_E_OOM && !job_allocs && !weights_allocs);
    TEST_CHECK(out.fallback == 1 && out.need_weights == weights_of("FL10_w8.vocr") && !out.text[0]);
}
static void resident_fallback_is_kept_and_upgraded_when_memory_returns(void)
{
    reset(); VjoGameOcrResult out;
    const uint32_t h15 = job_of("H15_w8.vocr") + weights_of("H15_w8.vocr");
    available = GAME_RESERVE + h15 - 1; /* the game's free memory without the worker's blocks */
    TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && out.fallback);
    /* Same memory: H15 still does not fit even counting FL10's block as
     * released, so FL10 stays resident and is not read again. */
    for (unsigned i = 0; i < 5; i++) {
        TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && out.fallback);
        TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT && out.need_weights == 0);
        TEST_CHECK(out.model_release_bytes == weights_of("FL10_w8.vocr"));
    }
    TEST_CHECK(bytes_read == TEST_FL10_BYTES && weights_allocs == 1 && !weights_frees);
    /* The game frees memory: H15 now fits once FL10's block is released, so the
     * job releases FL10 first (never two weight blocks) and reads H15. */
    available += 1;
    TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && !out.fallback);
    TEST_CHECK(out.release_bytes == weights_of("FL10_w8.vocr") && opened_which == VJO_GAME_OCR_OPEN_MODEL);
    TEST_CHECK(weights_frees == 1 && weights_allocs == 2 && resident.model == vjo_vocr_model("H15_w8.vocr"));
    TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_READ && bytes_read == TEST_FL10_BYTES + TEST_H15_BYTES);
    TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && !out.fallback); /* stays on H15 */
    TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
    /* Memory shrinks again: H15 resident no longer fits beside the job; FL10
     * fits once H15's block is released: switch down within one job. */
    available -= 1 + weights_of("FL10_w8.vocr");
    TEST_CHECK(!run_with("H15_w8.vocr", "FL10_w8.vocr", &out) && out.fallback);
    TEST_CHECK(out.release_bytes == weights_of("H15_w8.vocr") && resident.model == vjo_vocr_model("FL10_w8.vocr"));
    TEST_CHECK(weights_frees == 2 && weights_allocs == 3);
}
static void fallback_must_be_another_smaller_pinned_model(void)
{
    reset(); VjoGameOcrResult out;
    available = GAME_RESERVE + job_of("FL10_w8.vocr") + weights_of("FL10_w8.vocr");
    /* FL10 configured with a larger "fallback": ignored, FL10 runs. */
    TEST_CHECK(!run_with("FL10_w8.vocr", "H15_w8.vocr", &out) && !out.fallback);
    TEST_CHECK(opened_which == VJO_GAME_OCR_OPEN_MODEL && resident.model == vjo_vocr_model("FL10_w8.vocr"));
    /* An unknown, the same or no fallback: the configured model only. */
    const char *ignored[] = {"F99_w8.vocr", "H15_w8.vocr", ""};
    for (unsigned i = 0; i < 3; i++) {
        reset(); available = GAME_RESERVE + job_of("FL10_w8.vocr") + weights_of("FL10_w8.vocr");
        TEST_CHECK(run_with("H15_w8.vocr", ignored[i], &out) == VJO_E_OOM && !out.fallback && !job_allocs);
        TEST_CHECK(out.need_weights == weights_of("H15_w8.vocr"));
    }
    /* An unterminated fallback field is a wire error before anything else. */
    reset();
    VjoGameOcrRequest bad = request_with("H15_w8.vocr", "FL10_w8.vocr");
    memset(bad.fallback, 'x', sizeof(bad.fallback));
    TEST_CHECK(vjo_game_ocr_run_request(&bad, &out) == VJO_E_SOURCE && !out.reserve_bytes && !job_allocs);
}
static void fallback_file_failure_and_other_resident_model(void)
{
    reset(); VjoGameOcrResult out;
    const uint32_t h15 = job_of("H15_w8.vocr") + weights_of("H15_w8.vocr");
    available = GAME_RESERVE + h15 - 1;
    open_failure = (int)0x80010002u; /* FL10 is missing too: the fallback's own file error */
    TEST_CHECK(run_with("H15_w8.vocr", "FL10_w8.vocr", &out) == VJO_E_OCR_MODEL && out.fallback);
    TEST_CHECK(opened_which == VJO_GAME_OCR_OPEN_FALLBACK && out.io_error == open_failure && !weights_live);
    open_failure = 0;
    /* Weights of a model this job can use neither as model nor fallback are released first. */
    reset(); available = 64*1024*1024;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && resident.model == vjo_vocr_model("FL10_w8.vocr"));
    TEST_CHECK(!run_with("H15_w8.vocr", "", &out) && resident.model == vjo_vocr_model("H15_w8.vocr"));
    TEST_CHECK(weights_frees == 1 && weights_allocs == 2 && !out.release_bytes);
}

TEST_LIST = {
    {"the fallback runs only when the configured model does not fit", fallback_runs_only_when_the_model_does_not_fit},
    {"a resident fallback is kept, and upgraded or downgraded within one job", resident_fallback_is_kept_and_upgraded_when_memory_returns},
    {"a fallback must be another, smaller pinned model", fallback_must_be_another_smaller_pinned_model},
    {"the fallback's own file failure, and another resident model", fallback_file_failure_and_other_resident_model},
    {"the memory check is reported, also when it refuses the job", memory_check_is_reported_also_when_refused},
    {"CTC frames and each line's crop, frames and time are reported", frames_and_lines_are_reported},
    {"synthetic pins match their generated bytes", synthetic_pins_match_their_bytes},
    {"weights are read and hashed once, then kept for 50 jobs", weights_are_read_once_and_kept},
    {"a changed modification time is read again; a changed size is refused", changed_file_is_read_again},
    {"an unknown modification time reads the weights for every job", unknown_modification_time_reads_every_job},
    {"a missing or corrupt file releases the resident weights", missing_or_corrupt_file_releases_weights},
    {"cancellation keeps verified weights and drops a partial read", cancellation_keeps_verified_and_drops_partial_weights},
    {"a model change releases the old weights before allocating the new", model_change_releases_the_old_weights_first},
    {"the job peak stays within a page of the old single block", peak_is_the_old_single_block_within_a_page},
    {"the preflight counts resident weights once; a refusal releases them", preflight_counts_resident_weights_and_refusal_releases_them},
    {"an inactive game and the worker stop release the weights", inactive_game_and_stop_release_weights},
    {"a failed release blocks the worker until stop retries it", failed_release_blocks_until_stop_retries},
    {"the worker's loop keeps its pace with a waiting kernel and an older one", worker_loop_keeps_its_pace},
    {NULL, NULL}
};
