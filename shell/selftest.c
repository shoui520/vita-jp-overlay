/* The on-device self-test (docs/self-test.md). Armed by one file,
 * ux0:data/VitaJPOverlay/selftest.txt (tools/install_ftp.sh --self-test),
 * it follows the next boot and the first game started: at each step it
 * writes lines to ux0:data/VitaJPOverlay/selftest-report.txt, with the free
 * memory of every pool we can read, by every route, and the ledger. The report
 * is appended line by line, so a crash leaves the steps reached; the report is
 * what the owner copies back for tools/analyse_selftest.py.
 *
 * v2 (API 19): on the first settled screen of the game, one job runs every
 * stage in turn: the user's region with the fallback model forced (FL10 read
 * from the card), the user's region with the configured model (H15 read), the
 * full screen through the strips (weights kept), the capture probe (each
 * capture path and memory type, one strip each), and the user's region again
 * (the warm time; its text is the job's). Each stage ends with its own record,
 * passed or not, then the next one starts; after the last the test is done and
 * the marker removed, so a failure never leaves it armed
 * (#74: the API 18 test stayed armed after a failed capture and kept forcing
 * the full-screen region). It changes one thing while armed: OCR runs
 * automatically. The region is the user's. */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#include "../core/boot_guard.h"
#include "../include/vjo_import.h"
#include "../include/vjo_ledger_budget.h"
#include "shell.h"

#define MARKER VJO_DATA_DIR "/selftest.txt"
#define REPORT VJO_DATA_DIR "/selftest-report.txt"
#define REPORT_CAP (48 * 1024)

#ifdef VJO_HOST
#define IMPORT_READY(fn) 1
#else
#define IMPORT_READY(fn) VJO_IMPORT_READY(fn)
#endif

static int armed;        /* the marker was present at this boot */
static int finished;     /* every stage has its record: the test is complete */
static int report_bytes; /* -1: the report could not be opened */
static int64_t t0;

static void report(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    SceUID fd;
    int n;
    if (!armed || report_bytes < 0 || report_bytes > REPORT_CAP)
        return;
    va_start(ap, fmt);
    n = sceClibVsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    fd = sceIoOpen(REPORT, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0)
        return;
    if (sceIoWrite(fd, line, n) == n)
        report_bytes += n;
    sceIoClose(fd);
    vjo_log_raw(line, n); /* status.txt too */
}

static int64_t ms_since_start(void)
{
    return ((int64_t)sceKernelGetProcessTimeWide() - t0) / 1000;
}

/* One memory sample. SceShell's budget as the user API reports it: each
 * figure is the free size minus a system margin [SDK: Kernel-Reference], so a
 * negative one is under the margin (valid=0: not a free size; #74 read
 * -2097152, i.e. nothing free above a 2 MiB margin), not an error. Then the
 * PAF heap, the Shell ledger, and every kernel route (kernel/meminfo.c), each
 * with its own result. */
static void sample(const char *step)
{
    SceKernelFreeMemorySizeInfo f = {.size = sizeof(f)};
    VjoMemInfo m;
    const VjoLedger *l = &g_shell_ledger;
    int rc = sceKernelGetFreeMemorySize(&f);
    report("mem %s rc=0x%08X main=%d cdram=%d phycont=%d valid=%d paf_free=%u", step, (unsigned)rc,
           rc < 0 ? 0 : f.size_user, rc < 0 ? 0 : f.size_cdram, rc < 0 ? 0 : f.size_phycont,
           rc >= 0 && f.size_user >= 0 && f.size_cdram >= 0 && f.size_phycont >= 0,
           (unsigned)vjo_shell_heap_free_size());
    report("ledger %s pages=%u/%u paf=%u/%u", step,
           (unsigned)l->pool_current[VJO_LEDGER_POOL_SHELL_PAGES], (unsigned)l->pool_peak[VJO_LEDGER_POOL_SHELL_PAGES],
           (unsigned)l->pool_current[VJO_LEDGER_POOL_SHELL_PAF], (unsigned)l->pool_peak[VJO_LEDGER_POOL_SHELL_PAF]);
    sceClibMemset(&m, 0, sizeof(m));
    m.size = sizeof(m);
    if (!IMPORT_READY(vjoMemInfo) || (rc = vjoMemInfo(&m)) < 0) {
        report("kmem %s route=none rc=0x%08X", step, (unsigned)rc);
        return;
    }
    report("kmem %s route=block part=kernel rc=0x%08X step=%d total=%u free=%u", step, (unsigned)m.kernel_rc,
           (int)m.kernel_step, (unsigned)m.kernel_total, (unsigned)m.kernel_free);
    report("kmem %s route=block part=shell rc=0x%08X step=%d total=%u free=%u", step, (unsigned)m.caller_rc,
           (int)m.caller_step, (unsigned)m.caller_total, (unsigned)m.caller_free);
    report("kmem %s route=cas part=shell rc=0x%08X step=%d main=%u/%u cdram=%u/%u phycont=%u/%u", step,
           (unsigned)m.cas_rc, (int)m.cas_step, (unsigned)m.cas_main_free, (unsigned)m.cas_main_total,
           (unsigned)m.cas_cdram_free, (unsigned)m.cas_cdram_total, (unsigned)m.cas_phycont_free,
           (unsigned)m.cas_phycont_total);
    {
        char parts[160];
        size_t n = 0;
        parts[0] = 0;
        for (uint32_t i = 0; i < m.as_parts && i < VJO_MEMINFO_PARTS; i++) {
            int w = sceClibSnprintf(parts + n, sizeof(parts) - n, " p%u=%u/%u", (unsigned)i, (unsigned)m.as_free[i],
                                    (unsigned)m.as_total[i]);
            if (w < 0 || (size_t)w >= sizeof(parts) - n) break;
            n += (size_t)w;
        }
        report("kmem %s route=as part=%s rc=0x%08X step=%d parts=%u skipped=%u%s", step,
               m.as_caller ? "shell" : "kernel", (unsigned)m.as_rc, (int)m.as_step, (unsigned)m.as_parts,
               (unsigned)m.as_skipped, parts);
    }
    report("kernel %s ledger=%u/%u limit=%u capture=%u", step, (unsigned)m.ledger_current, (unsigned)m.ledger_peak,
           (unsigned)m.ledger_limit, (unsigned)m.capture_bytes);
}

int vjo_selftest_armed(void)
{
    return armed && !finished;
}

void vjo_selftest_init(const VjoConfig *cfg)
{
    SceIoStat st;
    VjoBootInfo b = {.size = sizeof(b)};
    SceUID fd;
    t0 = (int64_t)sceKernelGetProcessTimeWide();
    if (sceIoGetstat(MARKER, &st) < 0)
        return;
    armed = 1;
    fd = sceIoOpen(REPORT, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        report_bytes = -1;
        vjo_log("self-test: report not writable 0x%08X", (unsigned)fd);
        return;
    }
    sceIoClose(fd);
    report("selftest v2 api=%d", VJO_API_VERSION);
    if (IMPORT_READY(vjoBootInfo) && vjoBootInfo(&b) == 0)
        report("boot seq=%u mode=%d prev=%d/%d/%d/%d failures=%d write=0x%08X", (unsigned)b.seq, b.mode,
               b.prev_mode, b.prev_stage, b.prev_inert, b.prev_fault, b.failures, (unsigned)b.write_status);
    report("config backend=%d model=%s fallback=%s dictionary=%d anki=%d", cfg->ocr_backend, cfg->vocr_model,
           cfg->vocr_fallback_model, cfg->dictionary, cfg->anki_mining);
    report("step boot t=%lld", (long long)ms_since_start());
    sample("boot");
}

void vjo_selftest_step(const char *step)
{
    if (!armed || finished)
        return;
    report("step %s t=%lld", step, (long long)ms_since_start());
    sample(step);
}

void vjo_selftest_game(const char *title, int enabled, int mode, const VjoRect *region)
{
    if (!armed || finished)
        return;
    report("step game t=%lld title=%s enabled=%d mode=%d region=%s rect=%u,%u,%u,%u", (long long)ms_since_start(),
           title, enabled, mode, region && region->w ? "user" : "none", region ? region->x : 0u,
           region ? region->y : 0u, region ? region->w : 0u, region ? region->h : 0u);
    sample("game");
}

/* ---- the stages ---- */
static const struct {
    const char *name;
    uint32_t flags;
} stages[] = {
    /* In this order: the last stage's result is the job's (shell/worker.c); the
     * probe, which also reads the frame from user mode, after the main stages. */
    {"fallback", VJO_GAME_OCR_F_FORCE_FALLBACK}, /* the user's region with the fallback model (FL10 read) */
    {"textbox", 0},                           /* the user's region; the configured model's weights read (H15) */
    {"fullscreen", VJO_GAME_OCR_F_FULL},      /* the whole frame, through the strips; the weights kept */
    {"probe", VJO_GAME_OCR_F_PROBE},          /* each capture path and memory type, one strip each */
    {"textbox2", 0},                          /* the user's region again: the warm time; the job's text */
};

unsigned vjo_selftest_stages(void)
{
    return vjo_selftest_armed() ? (unsigned)(sizeof(stages) / sizeof(stages[0])) : 0u;
}

const char *vjo_selftest_stage_name(unsigned i)
{
    return i < sizeof(stages) / sizeof(stages[0]) ? stages[i].name : "?";
}

uint32_t vjo_selftest_stage_flags(unsigned i)
{
    return i < sizeof(stages) / sizeof(stages[0]) ? stages[i].flags : 0u;
}

void vjo_selftest_stage_begin(unsigned i, const char *model, const char *fallback)
{
    if (!vjo_selftest_armed())
        return;
    report("stage %s begin t=%lld flags=0x%X model=%s fallback=%s", vjo_selftest_stage_name(i),
           (long long)ms_since_start(), (unsigned)vjo_selftest_stage_flags(i), model, fallback[0] ? fallback : "-");
}

static const char *const step_names[VJO_GSTEP_COUNT] = {
    "none", "mem", "check", "alloc", "free", "pass", "tear", "sig", "find", "load", "recognize", "fallback",
    "fb", "probe", "settle", "blank", "par", "clock", "pace"};
static const char *const mem_names[] = {"-", "user", "cdram", "phycont", "kernel"};
static const char *const src_names[] = {"-", "hook", "kstrip", "user"};
static const char *const item_names[] = {"-", "capture", "workspace", "weights", "probe", "line"};

/* One step of the game worker's log as `step/rc/bytes/free` text: what the
 * step was, its native result, its bytes, the game's free memory after it. */
int vjo_gstep_format(char *buf, size_t cap, const char *who, const VjoGameOcrStep *s)
{
    const char *name = s->id < VJO_GSTEP_COUNT ? step_names[s->id] : "?";
    const char *kind = "-", *arg = "";
    char argbuf[16];
    if (s->id == VJO_GSTEP_ALLOC || s->id == VJO_GSTEP_FREE) {
        kind = s->kind < 5 ? mem_names[s->kind] : "?";
        arg = s->arg < 6 ? item_names[s->arg] : "?";
    } else if (s->id == VJO_GSTEP_PROBE) {
        kind = s->kind < 4 ? src_names[s->kind] : "?";
        arg = s->arg < 5 ? mem_names[s->arg] : "?";
    } else if (s->id == VJO_GSTEP_PASS || s->id == VJO_GSTEP_SIG || s->id == VJO_GSTEP_FB) {
        kind = s->kind < 4 ? src_names[s->kind] : "?";
        sceClibSnprintf(argbuf, sizeof(argbuf), "%u", (unsigned)s->arg);
        arg = argbuf;
    } else {
        sceClibSnprintf(argbuf, sizeof(argbuf), "%u", (unsigned)s->arg);
        arg = argbuf;
    }
    return sceClibSnprintf(buf, cap, "gstep %s %s kind=%s arg=%s rc=0x%08X bytes=%u sum=0x%08X main=%d cdram=%d "
                           "phycont=%d us=%u", who, name, kind, arg, (unsigned)s->rc, (unsigned)s->bytes,
                           (unsigned)s->sum, (int)s->free_main, (int)s->free_cdram, (int)s->free_phycont,
                           (unsigned)s->us);
}

void vjo_selftest_stage_end(unsigned i, int rc, const VjoGameOcrResult *r, uint32_t ms, const char *model)
{
    static const char *const weights[] = {"none", "read", "resident"};
    const char *name = vjo_selftest_stage_name(i);
    char line[256];
    if (!vjo_selftest_armed())
        return;
    for (uint32_t k = 0; k < r->n_steps && k < VJO_GAME_OCR_STEPS; k++) {
        if (vjo_gstep_format(line, sizeof(line), name, &r->step[k]) > 0)
            report("%s", line);
    }
    for (uint32_t k = 0; k < r->recognized && k < VJO_GAME_OCR_LINES; k++)
        report("line %s i=%u w=%u h=%u frames=%u us=%u", name, (unsigned)k, r->line[k].width, r->line[k].height,
               r->line[k].frames, (unsigned)r->line[k].us);
    report("stage %s end rc=%d t=%lld ms=%u region=%ux%u+%u+%u fb=%ux%u/%u/0x%08X model=%s fallback=%u lines=%u "
           "recognized=%u frames=%u", name, rc, (long long)ms_since_start(), (unsigned)ms, (unsigned)r->width,
           (unsigned)r->height, (unsigned)r->region_x, (unsigned)r->region_y, (unsigned)r->fb_width,
           (unsigned)r->fb_height, (unsigned)r->fb_pitch, (unsigned)r->fb_format, model, (unsigned)r->fallback,
           (unsigned)r->lines, (unsigned)r->recognized, (unsigned)r->frames);
    report("result %s source=%s mem=%s capture=%u strip_rows=%u strips=%u tears=%u capture_ms=%u workspace=%u "
           "resident=%u weights=%s find_ms=%u load_ms=%u recognize_ms=%u", name,
           r->source < 4 ? src_names[r->source] : "?", r->capture_memory < 5 ? mem_names[r->capture_memory] : "?",
           (unsigned)r->capture_bytes, (unsigned)r->strip_rows, (unsigned)r->captures, (unsigned)r->tears,
           (unsigned)(r->capture_us / 1000), (unsigned)r->workspace_bytes, (unsigned)r->resident_bytes,
           r->weights < 3 ? weights[r->weights] : "?", (unsigned)(r->find_us / 1000), (unsigned)(r->load_us / 1000),
           (unsigned)(r->recognize_us / 1000));
    report("memory %s free_user=%d reserve=%u need_workspace=%u need_weights=%u release=%u io=0x%08X stat=0x%08X "
           "cleanup=%d sum=0x%08X", name, (int)r->free_user, (unsigned)r->reserve_bytes, (unsigned)r->need_workspace,
           (unsigned)r->need_weights, (unsigned)r->release_bytes, (unsigned)r->io_error, (unsigned)r->stat_error,
           r->cleanup_status, (unsigned)r->region_sum);
}

void vjo_selftest_done(void)
{
    if (!vjo_selftest_armed())
        return;
    sample("done");
    report("step done t=%lld", (long long)ms_since_start());
    finished = 1;
    sceIoRemove(MARKER); /* one run per arming */
}
