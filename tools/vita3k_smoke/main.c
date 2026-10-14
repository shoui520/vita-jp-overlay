/* Vita3K smoke test (#74 preflight; docs/offline-testing.md section 5).
 *
 * Vita3K cannot load the kernel plugin, so none of the three plugins runs
 * there as it is. This app runs the part that can: the real game worker
 * (game_ocr/main.c, built for the Vita with VitaSDK) doing whole OCR jobs in a
 * Vita user process, its memory ladder on the emulator's memblock types, its
 * probe's sceDisplayGetFrameBuf read, the real core strips, finder and
 * recognizer with the release weights, on a 32 KiB thread stack as in a game.
 * The kernel's side (vjoOcr*) is an in-process stand-in that converts the
 * frame's rows with the same core/fbgrey.h the display hook uses.
 *
 * It catches crashes only: Vita3K has no ScePhyMemPartKD, no SceShell and no
 * taiHEN kernel, and its timings are the emulator's. AGENTS.md rule 5. */
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>

#include "../../core/boot_guard.h"
#include "../../core/fbgrey.h"
#include "../../game_ocr/main.h"
#include "../../include/vjo_api.h"
#include "../../include/vjo_game_ocr.h"

#define DIR "ux0:data/vjo_smoke"
#define W 960u
#define H 544u

static FILE *out;
static void *frame;           /* the display framebuffer, CDRAM */
static uint32_t frame_fmt = VJO_FB_A8B8G8R8;
static VjoGameOcrRequest job;
static uint32_t job_seq = 40;
static SceUID model_fd = -1;

/* ---- the kernel, in process ---- */
int vjoGetVersion(void) { return VJO_API_VERSION; }
int vjoBootInfo(VjoBootInfo *b)
{
    memset(b, 0, sizeof(*b));
    b->size = sizeof(*b);
    b->mode = VJO_BOOT_NORMAL;
    return 0;
}
int vjoOcrRegister(void) { return 0; }
int vjoOcrTake(VjoGameOcrRequest *o) { (void)o; return VJO_ERR_BUSY; }
int vjoOcrCancelled(uint32_t seq) { (void)seq; return 0; }
int vjoOcrComplete(const VjoGameOcrResult *r) { (void)r; return 0; }
int vjoOcrCaptureRows(const VjoGameOcrRows *req, void *dst, VjoGameOcrRowsInfo *info)
{
    unsigned bpp = frame_fmt == VJO_FB_A1B5G5R5 ? 2u : 4u;
    if (!req || req->size != sizeof(*req) || req->seq != job.seq || !req->n || req->n > VJO_GAME_OCR_ROWS_MAX ||
        req->y + req->n > job.height || req->dst_stride < job.width || !dst)
        return VJO_ERR_ARG;
    for (uint32_t r = 0; r < req->n; r++) {
        const uint8_t *src = (const uint8_t *)frame + ((size_t)(job.y + req->y + r) * W + job.x) * bpp;
        uint8_t *d = (uint8_t *)dst + (size_t)r * req->dst_stride;
        for (uint32_t x = 0; x < job.width; x++) {
            uint32_t abgr;
            vjo_fb_to_abgr(&abgr, src + (size_t)x * bpp, 1, frame_fmt);
            d[x] = (uint8_t)vjo_grey(abgr & 0xFFu, (abgr >> 8) & 0xFFu, (abgr >> 16) & 0xFFu);
        }
    }
    if (info && info->size == sizeof(*info)) {
        info->fb_width = W; info->fb_height = H; info->fb_pitch = W; info->fb_format = frame_fmt;
        info->wait_us = 16667; info->kstrip_bytes = req->mode == VJO_GAME_OCR_SRC_KSTRIP ? req->n * job.width : 0;
        info->rc = (int32_t)req->n;
    }
    return (int)req->n;
}
int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info)
{
    char path[160];
    SceIoStat st;
    const char *name = which == VJO_GAME_OCR_OPEN_FALLBACK ? job.fallback : job.model;
    if (seq != job.seq || model_fd >= 0) return VJO_ERR_ARG;
    snprintf(path, sizeof(path), DIR "/%s", name);
    model_fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (model_fd < 0) return model_fd;
    memset(&st, 0, sizeof(st));
    sceIoGetstat(path, &st);
    if (info) { /* as kernel/game_ocr.c model_info_out_locked writes it */
        memset(info, 0, sizeof(*info));
        info->size = sizeof(*info);
        info->bytes = (uint32_t)st.st_size;
        info->mtime = 0x2026101300000000ull;
    }
    return (int)st.st_size;
}
int vjoOcrModelRead(uint32_t seq, uint32_t off, uint32_t bytes, void *dst)
{
    if (seq != job.seq || model_fd < 0 || bytes > VJO_GAME_OCR_MODEL_CHUNK) return VJO_ERR_ARG;
    return sceIoPread(model_fd, dst, bytes, off);
}
int vjoOcrModelClose(uint32_t seq)
{
    (void)seq;
    if (model_fd >= 0) sceIoClose(model_fd);
    model_fd = -1;
    return 0;
}

/* ---- the frame: a VN screen with a dark textbox and three lines of strokes ---- */
static uint32_t seed = 74;
static unsigned rnd(unsigned n) { seed = seed * 1103515245u + 12345u; return (seed >> 8) % n; }
static void put(unsigned x, unsigned y, unsigned r, unsigned g, unsigned b)
{
    if (frame_fmt == VJO_FB_A1B5G5R5)
        ((uint16_t *)frame)[y * W + x] = (uint16_t)(0x8000u | (b >> 3) << 10 | (g >> 3) << 5 | (r >> 3));
    else if (frame_fmt == VJO_FB_A2B10G10R10)
        ((uint32_t *)frame)[y * W + x] = 3u << 30 | (b << 2 | b >> 6) << 20 | (g << 2 | g >> 6) << 10 | (r << 2 | r >> 6);
    else
        ((uint32_t *)frame)[y * W + x] = 0xFF000000u | b << 16 | g << 8 | r;
}
static void box(unsigned x0, unsigned y0, unsigned x1, unsigned y1, unsigned r, unsigned g, unsigned b)
{
    for (unsigned y = y0; y < y1; y++)
        for (unsigned x = x0; x < x1; x++) put(x, y, r, g, b);
}
static void glyphs(unsigned x0, unsigned y0, unsigned x1, unsigned gh, unsigned r, unsigned g, unsigned b)
{
    for (unsigned x = x0; x + gh <= x1; x += gh + gh / 6) {
        unsigned strokes = 3 + rnd(4), t = gh / 10 ? gh / 10 : 1;
        for (unsigned k = 0; k < strokes; k++) {
            if (rnd(2)) { unsigned yy = y0 + rnd(gh - t); box(x, yy, x + gh, yy + t, r, g, b); }
            else { unsigned xx = x + rnd(gh - t); box(xx, y0, xx + t, y0 + gh, r, g, b); }
        }
    }
}
static void draw(uint32_t fmt)
{
    frame_fmt = fmt;
    seed = 74;
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < W; x++) put(x, y, 70 + y * 100 / H + rnd(9), 100 + x * 60 / W, 150);
    box(20, 380, 940, 536, 18, 24, 34);
    glyphs(44, 404, 900, 22, 240, 240, 240);
    glyphs(44, 440, 760, 22, 250, 236, 150);
    glyphs(44, 476, 620, 22, 228, 228, 236);
}

/* ---- the stages, as the self-test runs them ---- */
static uint32_t fnv(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

void vjo_game_ocr_cache_clear(void);
static uint32_t stage_speed;     /* VjoGameOcrRequest.speed of the next stage (API 21: helpers, clock) */
static uint32_t last_text_fnv;   /* the last stage's text */

static int stage(const char *name, uint32_t flags, const char *model, const char *fallback, int full)
{
    static VjoGameOcrResult r;
    memset(&job, 0, sizeof(job));
    job.size = sizeof(job);
    job.seq = ++job_seq;
    job.capture_seq = job.seq;
    job.flags = flags;
    job.speed = stage_speed;
    job.fb_width = W; job.fb_height = H;
    job.x = full ? 0 : 22; job.y = full ? 0 : 400; job.width = full ? W : 917; job.height = full ? H : 125;
    snprintf(job.model_dir, sizeof(job.model_dir), "ux0:data/VitaJPOverlay/ocr");
    snprintf(job.model, sizeof(job.model), "%s", model);
    snprintf(job.fallback, sizeof(job.fallback), "%s", fallback);
    memset(&r, 0, sizeof(r));
    int rc = vjo_game_ocr_run_request(&job, &r);
    fprintf(out, "stage %s fmt=0x%08X rc=%d lines=%u recognized=%u fallback=%u source=%u mem=%u capture=%u strips=%u "
                 "tears=%u weights=%u resident=%u text_len=%u text_fnv=0x%08X steps=%u\n",
            name, (unsigned)frame_fmt, rc, (unsigned)r.lines, (unsigned)r.recognized, (unsigned)r.fallback,
            (unsigned)r.source, (unsigned)r.capture_memory, (unsigned)r.capture_bytes, (unsigned)r.captures,
            (unsigned)r.tears, (unsigned)r.weights, (unsigned)r.resident_bytes, (unsigned)strlen(r.text),
            (unsigned)fnv(r.text), (unsigned)r.n_steps);
    last_text_fnv = fnv(r.text);
    for (uint32_t i = 0; i < r.n_steps && i < VJO_GAME_OCR_STEPS; i++)
        fprintf(out, "  step %u kind=%u arg=%u rc=0x%08X bytes=%u main=%d cdram=%d\n", (unsigned)r.step[i].id,
                (unsigned)r.step[i].kind, (unsigned)r.step[i].arg, (unsigned)r.step[i].rc, (unsigned)r.step[i].bytes,
                (int)r.step[i].free_main, (int)r.step[i].free_cdram);
    fflush(out);
    return rc;
}

static int run(SceSize args, void *argp)
{
    static const uint32_t fmts[] = {VJO_FB_A8B8G8R8, VJO_FB_A1B5G5R5, VJO_FB_A2B10G10R10};
    int failures = 0;
    (void)args; (void)argp;
    draw(VJO_FB_A8B8G8R8);
    failures += stage("probe", VJO_GAME_OCR_F_PROBE, "H15_w8.vocr", "FL10_w8.vocr", 0) < 0;
    failures += stage("fallback", VJO_GAME_OCR_F_FORCE_FALLBACK, "H15_w8.vocr", "FL10_w8.vocr", 0) != 0;
    failures += stage("textbox", 0, "H15_w8.vocr", "FL10_w8.vocr", 0) != 0;
    failures += stage("fullscreen", VJO_GAME_OCR_F_FULL, "H15_w8.vocr", "FL10_w8.vocr", 1) != 0;
    {
        /* API 21: the same frame with two helper threads and no cached line: the same text */
        uint32_t serial = last_text_fnv;
        vjo_game_ocr_cache_clear();
        stage_speed = 2;
        failures += stage("fullscreen-threads", VJO_GAME_OCR_F_FULL, "H15_w8.vocr", "FL10_w8.vocr", 1) != 0;
        stage_speed = 0;
        if (last_text_fnv != serial) {
            fprintf(out, "fullscreen-threads text 0x%08X differs from the serial job's 0x%08X\n",
                    (unsigned)last_text_fnv, (unsigned)serial);
            failures++;
        }
        /* and again: every line from the cache, the same text */
        stage_speed = 2;
        failures += stage("fullscreen-cached", VJO_GAME_OCR_F_FULL, "H15_w8.vocr", "FL10_w8.vocr", 1) != 0;
        stage_speed = 0;
        if (last_text_fnv != serial) {
            fprintf(out, "fullscreen-cached text 0x%08X differs from the serial job's 0x%08X\n",
                    (unsigned)last_text_fnv, (unsigned)serial);
            failures++;
        }
        vjo_game_ocr_cache_clear();
    }
    failures += stage("textbox2", 0, "H15_w8.vocr", "FL10_w8.vocr", 0) != 0;
    for (unsigned f = 1; f < 3; f++) {
        draw(fmts[f]);
        failures += stage("textbox-format", 0, "H15_w8.vocr", "FL10_w8.vocr", 0) != 0;
    }
    fprintf(out, "worker stop rc=%d\n", vjo_game_ocr_worker_stop());
    fprintf(out, "done failures=%d\n", failures);
    fclose(out);
    out = NULL;
    return 0;
}

int main(void)
{
    SceUID fbm, th;
    void *base = NULL;
    SceDisplayFrameBuf fb;
    sceIoMkdir(DIR, 0777);
    out = fopen(DIR "/results.txt", "w");
    if (!out) return sceKernelExitProcess(1);
    fprintf(out, "vjo smoke api=%d\n", VJO_API_VERSION);
    /* the framebuffer the probe's user-mode read finds: 960x544, 4 B a pixel at most */
    fbm = sceKernelAllocMemBlock("vjo_smoke_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 4u * 1024u * 1024u, NULL);
    if (fbm < 0 || sceKernelGetMemBlockBase(fbm, &base) < 0) {
        fprintf(out, "framebuffer alloc 0x%08X\n", (unsigned)fbm);
        fclose(out);
        return sceKernelExitProcess(1);
    }
    frame = base;
    draw(VJO_FB_A8B8G8R8);
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb); fb.base = frame; fb.pitch = W; fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = W; fb.height = H;
    fprintf(out, "setframebuf rc=0x%08X\n", (unsigned)sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME));
    fflush(out);
    /* the worker's stack: 32 KiB, as game_ocr/main.c creates its thread */
    th = sceKernelCreateThread("vjo_smoke_job", run, 0x10000100 + 31, 32 * 1024, 0, 0, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        fprintf(out, "thread 0x%08X\n", (unsigned)th);
        fclose(out);
        return sceKernelExitProcess(1);
    }
    sceKernelWaitThreadEnd(th, NULL, NULL);
    /* the framebuffer stays allocated: the display still shows it until the process ends */
    return sceKernelExitProcess(0);
}
