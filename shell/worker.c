/* Control thread (kernel events, overlay state, auto/on-press policy) and
 * network thread (capture -> JPEG -> Lens, or capture -> local vita-vn-ocr;
 * then the dictionary). */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include "../core/boot_guard.h"
#include "../include/vjo_ledger_budget.h"
#include "../core/regions.h"
#include "../core/local_dict.h"
#include "../core/textfilter.h"
#include "../core/utf.h"
#ifdef VJO_WITH_VOCR
#include "../core/local_ocr.h"
#include "game_ocr_client.h"
#include "../include/vjo_vocr_sizes.h"
#endif
#include "shell.h"

/* Network memory: two result arenas (the overlay shows one while the worker
 * fills the other) and kept connections. Local lookup retires its prior view
 * before OCR, so it needs only one arena. OCR-only local jobs use bounded
 * static text. Control-thread config/region/messages use separate scratch. */
#define RESULT_ARENA_SIZE (256 * 1024)
#define LOCAL_RESULT_ARENA_SIZE (64 * 1024)
#define SCRATCH_SIZE      (24 * 1024)
typedef char config_fits_scratch[(SCRATCH_SIZE >= VJO_CONFIG_TEXT_MAX + 1) ? 1 : -1];
#define POOL_SLOTS        2 /* Lens and the dictionary */
#define POOL_SIZE         ((vjo_net_pool_size(POOL_SLOTS) + 15) & ~(size_t)15)
#define MEM_SIZE          (2 * RESULT_ARENA_SIZE + POOL_SIZE)
/* Well within the servers' (jiten closes an idle connection after 3
 * minutes, Lens after 4), and short enough that one a network dropped
 * silently (a hotspot's NAT) is rarely met: that costs a read timeout. */
#define NET_IDLE_US       (60 * 1000 * 1000)
/* Connections are readied when the region starts changing (for this long),
 * this often at most: a region that never settles readies them once. */
#define WARM_WINDOW_MS    3000u
#define WARM_GAP_US       2000000LL
#define WARM_CONNECT_US   5000000

#define POLL_TIMEOUT_US   50000
#define BACKOFF_MIN_US    2000000LL
#define BACKOFF_MAX_US    60000000LL
#define JPEG_QUALITY      80
#define SUBTITLE_GAP_US   1000000LL /* between subtitle jobs */
#define UNSETTLED_US      2000000LL /* subtitles: a region that keeps changing is recognized this often */

VjoView g_view;

static SceUID view_lock = -1;
static SceUID cmd_lock = -1;
static SceUID ctl_thread = -1, net_thread = -1;
static int threads_started; /* bits: network=1, control=2 */
static SceUID net_evf = -1;
static volatile int running = 1;

static SceUID mem_uid = -1;
static void *mem_heap; /* bounded allocation from ScePaf's existing heap */
static size_t mem_bytes; /* what either holds, for the ledger */
static int mem_local;
static VjoArena results[2];
static VjoArena scratch; /* static; control thread only */
static uint8_t scratch_mem[SCRATCH_SIZE];
static int active = -1;               /* results[] index shown/cached */
static uint32_t cache_scene; /* VjoState.scene of the cached result, 0 = unknown */
static uint32_t cache_seq;   /* VjoState.region_seq of its capture */
static int cache_ok;
static int cache_dictionary; /* the cached lookup's VJO_DICT_* */
static VjoOverlayData cache_data[2];
/* Local text outlives the single dictionary arena. Only an OCR job rewrites it,
 * after start_job has detached its prior view. Cursor jobs only read it. */
static char local_text[VJO_STRIP_MAX];
static VjoEntryList local_header;
static volatile int local_text_valid;
static uint32_t local_text_generation, local_text_game_generation;
static uint32_t local_text_scene, local_text_capture_seq;
static uint32_t local_cursor_byte, local_cursor_seq;
static int cursor_waiting;
static int job_cursor_only;
static uint32_t job_cursor_byte, job_cursor_seq, job_text_generation;
/* Dictionary paging at the cursor (local_dictionary_results > 1): the
 * requested page shares the cursor sequence. Page locators live at the start
 * of the single result arena (cache_data[0].pages); they are reused only for
 * the text generation and result limit that built them (network thread). */
static int local_page, job_page;
static uint32_t pages_generation;
static int pages_limit;

static VjoConfig cfg;      /* control thread */
static VjoConfig job_cfg;  /* snapshot used by the network thread */
static int anki_on;        /* vjo_anki_configure(&cfg): × saves cards, △ sends them */
static VjoPlatform plat;
static VjoNetPool pool; /* plat's; network thread only, except mem_free while it idles */
static char title_id[12];   /* set while a game is active ("" otherwise) */
/* Scene IDs survive kernel resets. A foreground boundary must also reject
 * results captured for the preceding game, even before a new scene arrives. */
static uint32_t game_generation;
static uint32_t job_generation;

/* Control-thread state. The overlay and the network job are independent: an
 * auto job runs while the overlay is closed, the overlay can open while a
 * job runs, and a result can be shown while the next job runs.
 *   overlay: OV_CLOSED, OV_OPEN, or OV_OPEN_OLD_JOB (opened while a job was
 *            already running: its result may be for an earlier screen, which
 *            is checked when it ends)
 *   job:     job_running (set here, finished via job_done from the network
 *            thread)
 *   subtitles: on/off (the strip shows each job's sentence while the
 *            overlay is closed)
 * plus inputs to the background policy: stable_pending (a REGION_STABLE
 * or REGION_QUIET not acted on yet) and the error backoffs, one per stage (a
 * failing dictionary does not stop the subtitles, which need only OCR).
 * Each event has one handler below (on_*). */
enum { OV_CLOSED, OV_OPEN, OV_OPEN_OLD_JOB };
static int ov = OV_CLOSED;
static int subtitles; /* written by set_subtitles only */
static int input_block_error;
static int stable_pending;
/* The overlay waits for the running job ("Recognizing…"), or shows its
 * sentence before the lookup: job_text_list, in the job's arena, which
 * must not be shown once another job starts in it. */
static int view_waiting, view_job_text;
static VjoEntryList job_text_list;
typedef struct {
    int64_t until, us;
} Backoff;
static Backoff ocr_backoff, dict_backoff;
static int64_t job_started_us;

/* network job */
#define NET_EV_JOB  1u
#define NET_EV_QUIT 2u
#define NET_EV_WARM 4u
static volatile int job_running;
static volatile int job_done;
static volatile int job_idx;
static volatile int job_rc;
static volatile int job_lookup;     /* the job looks the words up (else OCR only) */
static int job_prepares;            /* ...the first word, before the overlay opens (log) */
/* The cached result's text when the job started (NULL: no usable lookup).
 * Its arena is not touched while the job runs (the job fills the other). */
static const char *job_cached_text;
/* The job's text is job_cached_text: no lookup, the cached result stays
 * (and is now for the job's screen). */
static volatile int job_same_text;
/* The job's capture is done (job_capture_scene, job_capture_seq: its
 * VjoState.capture_scene and region_seq). */
static volatile int job_captured;
static volatile uint32_t job_capture_scene, job_capture_seq;
/* The job is for an earlier screen: its requests were aborted, its result
 * is dropped. */
static volatile int job_cancelled;
static VjoNetCancel net_cancel = {-1, -1, 0}; /* the network thread's requests */
/* The network thread opens the connections for the next job (warm_dictionary:
 * VJO_DICT_*, or -1 for Lens only). */
static volatile int warm_running;
static volatile int warm_dictionary;
static int64_t warm_asked_us;
/* cache_data[job_idx].sentence is ready (the lookup may still run): the
 * net thread only appends to the arena, so the control thread may read it */
static volatile int job_text_ready;

static volatile int pending_cmd;
static VjoRect pending_rect;
static uint32_t pending_cursor_byte, pending_cursor_generation;
static void start_cursor_job(void);
static void note_pages(int rc, const VjoOverlayData *out);

static uint32_t advance_generation(uint32_t *value)
{
    if (++*value == 0) *value = 1;
    return *value;
}

static void retire_local_text(int new_generation)
{
    sceKernelLockMutex(cmd_lock, 1, NULL);
    vjo_view_lock();
    local_text_valid = 0;
    cursor_waiting = 0;
    local_page = 0;
    if (new_generation) advance_generation(&local_text_generation);
    g_view.list = NULL;
    g_view.local_cursor_mode = 0;
    g_view.dict_page = g_view.dict_pages = 0;
    g_view.version++;
    vjo_view_unlock();
    sceKernelUnlockMutex(cmd_lock, 1);
}

void vjo_view_lock(void)
{
    sceKernelLockMutex(view_lock, 1, NULL);
}

void vjo_view_unlock(void)
{
    sceKernelUnlockMutex(view_lock, 1);
}

void vjo_post_command(int cmd, const VjoRect *rect)
{
    sceKernelLockMutex(cmd_lock, 1, NULL);
    pending_cmd = cmd;
    if (rect)
        pending_rect = *rect;
    sceKernelUnlockMutex(cmd_lock, 1);
}

/* Under cmd_lock and the view lock: queues the cursor job for the current
 * local_cursor_byte/local_page and detaches the shown entry before its arena
 * is reused. */
static void post_local_lookup(uint32_t text_generation)
{
    advance_generation(&local_cursor_seq);
    pending_cursor_byte = local_cursor_byte;
    pending_cursor_generation = text_generation;
    pending_cmd = VJO_CMD_CURSOR;
    local_header = (VjoEntryList){.header = local_text};
    g_view.list = &local_header; /* detach entries before their arena is reset */
    g_view.cursor_byte = local_cursor_byte;
    g_view.dict_page = local_page;
    g_view.list_seq++;
    sceClibSnprintf(g_view.status, sizeof(g_view.status), "Looking up words…");
    g_view.status_is_error = 0;
    g_view.version++;
}

void vjo_post_cursor(uint32_t byte_offset, uint32_t text_generation)
{
    /* This order is also used by cursor completion: a newer request cannot
     * race past the stale-result check and republish the old definition. */
    sceKernelLockMutex(cmd_lock, 1, NULL);
    if (pending_cmd != VJO_CMD_NONE && pending_cmd != VJO_CMD_CURSOR) {
        sceKernelUnlockMutex(cmd_lock, 1);
        return;
    }
    vjo_view_lock();
    if (!g_view.open || !g_view.local_cursor_mode || !local_text_valid ||
        !text_generation || text_generation != local_text_generation) {
        vjo_view_unlock();
        sceKernelUnlockMutex(cmd_lock, 1);
        return;
    }
    size_t len = sceClibStrnlen(local_text, sizeof(local_text));
    if ((len ? byte_offset >= len : byte_offset != 0) ||
        vjo_utf8_cursor_step(local_text, len, byte_offset, 0) != byte_offset ||
        (cfg.japanese_only_selection &&
         (byte_offset >= len || vjo_utf8_japanese_cursor_step(local_text, len, byte_offset, 0) != byte_offset))) {
        vjo_view_unlock();
        sceKernelUnlockMutex(cmd_lock, 1);
        return;
    }
    local_cursor_byte = byte_offset;
    local_page = 0; /* a new word starts at its best match */
    g_view.dict_pages = 0;
    post_local_lookup(text_generation);
    vjo_view_unlock();
    sceKernelUnlockMutex(cmd_lock, 1);
}

int vjo_post_page(int page, uint32_t text_generation)
{
    int accepted = 0;
    sceKernelLockMutex(cmd_lock, 1, NULL);
    if (pending_cmd == VJO_CMD_NONE || pending_cmd == VJO_CMD_CURSOR) {
        vjo_view_lock();
        /* Only pages of the published (or already pending) result for this
         * text and cursor; dict_pages is reset whenever either changes. */
        if (g_view.open && g_view.local_cursor_mode && local_text_valid && text_generation &&
            text_generation == local_text_generation && page >= 0 && page < g_view.dict_pages &&
            page != g_view.dict_page) {
            local_page = page;
            post_local_lookup(text_generation);
            accepted = 1;
        }
        vjo_view_unlock();
    }
    sceKernelUnlockMutex(cmd_lock, 1);
    return accepted;
}

static int64_t now_us(void)
{
    return (int64_t)sceKernelGetProcessTimeWide();
}

/* Dictionary file reads since the last lookup began (platform_vita.c counts). */
uint32_t vjo_dict_reads, vjo_dict_read_bytes;

/* Up to `chars` characters of s (the line at most) into out, for the log. */
static void log_text(char *out, size_t cap, const char *s, size_t len, int chars)
{
    size_t i = 0;
    while (i < len && chars-- > 0) {
        size_t j = i;
        uint32_t cp = vjo_utf8_next(s, len, &j);
        if (cp < 0x20 || j >= cap) break;
        i = j;
    }
    sceClibMemcpy(out, s, i);
    out[i] = 0;
}

/* One log.txt line per dictionary lookup: why (first word during OCR, on
 * open, cursor or tap, dictionary page), the cursor byte, the matched span
 * in characters, the dictionary file, the page, the file reads, rc and the
 * time; then the text at the cursor, the entry and the source named in its
 * definition. The numbers come first and the texts are bounded (32 + 31 + 31
 * + 23 bytes, whole characters), so the line fits vjo_log's 254 bytes. */
static void log_lookup(const char *why, const VjoOverlayData *out, uint32_t cursor, int rc, int64_t t0)
{
    char query[33], head[32], reading[32], source[24], dict[8];
    const VjoEntry *e = out->list.n_entries > 0 ? &out->list.entries[0] : NULL;
    const char *text = out->sentence ? out->sentence : "";
    size_t len = sceClibStrnlen(text, VJO_LOCAL_MAX_TEXT);
    int chars = 0, page = out->page, files = vjo_local_names_count(job_cfg.local_dictionaries);
    /* Which file: known with one, or from the page locators (2-8 shown). */
    int file = files == 1 ? 1 : out->pages && page >= 0 && page < out->pages->count ? out->pages->page[page].dict + 1 : 0;
    query[0] = head[0] = reading[0] = source[0] = 0;
    if (cursor <= len) log_text(query, sizeof(query), text + cursor, len - cursor, 8);
    if (e) {
        size_t i = (size_t)e->hl_start;
        while (i < (size_t)e->hl_end && i < len) { vjo_utf8_next(text, len, &i); chars++; }
        log_text(head, sizeof(head), e->vocab->spelling, sceClibStrnlen(e->vocab->spelling, 256), 10);
        if (e->vocab->reading != e->vocab->spelling)
            log_text(reading, sizeof(reading), e->vocab->reading, sceClibStrnlen(e->vocab->reading, 256), 10);
        if (e->vocab->n_meanings && e->vocab->meanings[0][0] == '[') { /* "[Daijirin4 Plain]\n..." */
            const char *m = e->vocab->meanings[0] + 1;
            size_t n = 0;
            while (n + 1 < sizeof(source) && m[n] && m[n] != ']') n++;
            if (m[n] == ']') { sceClibMemcpy(source, m, n); source[n] = 0; }
        }
    }
    if (!e) sceClibSnprintf(dict, sizeof(dict), "-");
    else if (file) sceClibSnprintf(dict, sizeof(dict), "%d/%d", file, files);
    else sceClibSnprintf(dict, sizeof(dict), "?/%d", files);
    vjo_log("lookup %s at %u: %d chars, dictionary %s, page %d/%d, %u reads %u B, rc=%d, %u ms; \"%s\" -> %s%s%s%s%s%s%s",
            why, (unsigned)cursor, chars, dict, page + 1, vjo_overlay_page_count(out),
            (unsigned)vjo_dict_reads, (unsigned)vjo_dict_read_bytes, rc, (unsigned)((now_us() - t0) / 1000),
            query, e ? head : "no entry", reading[0] ? " (" : "", reading, reading[0] ? ")" : "",
            source[0] ? " [" : "", source, source[0] ? "]" : "");
}

/* Does a result for scene `a` show the current scene `b`? */
static int same_scene(uint32_t a, uint32_t b)
{
    return a != 0 && a == b;
}

/* Does a result for `scene`, from region capture `seq`, show the screen in
 * st? Its scene is the current one, or its capture still shows the screen
 * (checked now): an icon that kept changing when it was taken got masked
 * since, or is all that changes. */
static int shows_screen(uint32_t scene, uint32_t seq, const VjoState *st)
{
    return same_scene(scene, st->scene) ||
           (seq && seq == st->region_seq && same_scene(st->region_scene, st->scene));
}

/* ---------------- memory ---------------- */

static void mem_free(void);

static int has_result_memory(void)
{
    return mem_heap != NULL || mem_uid >= 0;
}

static int game_ocr_backend(int backend)
{
    return backend == VJO_OCR_VOCR || backend == VJO_OCR_VOCR2;
}

static int local_pipeline(const VjoConfig *c)
{
    return game_ocr_backend(c->ocr_backend) &&
           c->dictionary == VJO_DICT_LOCAL;
}

static int mem_alloc_for(const VjoConfig *c)
{
    void *base = NULL;
    uint8_t *p;
    int local = local_pipeline(c);
    size_t arena_size = local ? LOCAL_RESULT_ARENA_SIZE : RESULT_ARENA_SIZE;
    size_t bytes = local ? arena_size : MEM_SIZE;
    if (has_result_memory() && mem_local != local)
        mem_free();
    if (has_result_memory())
        return 0;
#ifdef VJO_PAF_ALLOC
    /* Games can exhaust USER while SceShell still has reserved heap pages.
     * Result arenas and kept TLS sessions have one bounded owner. */
    mem_heap = vjo_shell_heap_alloc_for(VJO_SL_PAF_RESULTS, bytes);
    if (!mem_heap) {
        vjo_log("result workspace Paf allocation failed (%u KiB)", (unsigned)(bytes >> 10));
        return -1;
    }
    base = mem_heap;
    mem_bytes = bytes;
#else
    /* Host tests only: the Vita build never takes partition pages here. */
    bytes = (bytes + 0xFFF) & ~(size_t)0xFFF;
    mem_uid = vjo_shell_memblock_alloc(VJO_SL_USERRW_RESULTS, "VjoShellMem", bytes, &base);
    if (mem_uid < 0)
        return mem_uid;
    mem_bytes = bytes;
#endif
    p = (uint8_t *)base;
    vjo_arena_init(&results[0], p, arena_size);
    vjo_arena_init(&results[1], local ? NULL : p + arena_size, local ? 0 : arena_size);
    vjo_net_pool_init(&pool, local ? NULL : p + 2 * arena_size,
                      local ? 0 : POOL_SIZE, NET_IDLE_US);
    mem_local = local;
    vjo_log("%s workspace: %u bytes, %u %u-byte results, %u-byte connection pool",
            local ? "local dictionary" : "network", (unsigned)bytes,
            (unsigned)(local ? 1 : 2), (unsigned)arena_size, (unsigned)(local ? 0 : POOL_SIZE));
    active = -1;
    cache_ok = 0;
    return 0;
}

static int mem_alloc(void) { return mem_alloc_for(&cfg); }

/* Called by the idle control thread or by the network thread owning its job. */
static void mem_free(void)
{
    if (has_result_memory()) {
        vjo_view_lock();
        g_view.list = NULL;
        vjo_view_unlock();
        vjo_net_pool_close(&plat);
        vjo_net_pool_init(&pool, NULL, 0, NET_IDLE_US);
        if (mem_heap)
            vjo_shell_heap_free(VJO_SL_PAF_RESULTS, mem_heap, mem_bytes);
        else
            vjo_shell_memblock_free(VJO_SL_USERRW_RESULTS, mem_uid, mem_bytes);
    }
    mem_heap = NULL;
    mem_uid = -1;
    mem_bytes = 0;
    vjo_arena_init(&results[0], NULL, 0);
    vjo_arena_init(&results[1], NULL, 0);
    cache_data[0].pages = cache_data[1].pages = NULL; /* their locators were in the arena */
    active = -1;
    cache_ok = 0;
}

/* ---------------- view ---------------- */

static void view_publish(int open, const VjoEntryList *list, const char *status, int is_error)
{
    vjo_view_lock();
    __atomic_store_n(&g_view.open, open, __ATOMIC_RELEASE); /* also read unlocked (overlay frame) */
    g_view.list = list;
    g_view.list_seq++;
    sceClibSnprintf(g_view.status, sizeof(g_view.status), "%s", status ? status : "");
    g_view.status_is_error = is_error;
    g_view.local_cursor_mode = local_pipeline(&cfg) && local_text_valid && list &&
                               list->header == local_text;
    g_view.cursor_byte = local_cursor_byte;
    /* The static header placeholder keeps the requested page while its
     * lookup runs; a published result carries its own page count. */
    if (list != &local_header) {
        const VjoOverlayData *shown = list && active >= 0 && list == &cache_data[active].list
                                      ? &cache_data[active] : NULL;
        g_view.dict_page = shown && g_view.local_cursor_mode ? shown->page : 0;
        g_view.dict_pages = shown && g_view.local_cursor_mode ? vjo_overlay_page_count(shown) : 0;
    }
    g_view.japanese_only_selection = cfg.japanese_only_selection;
    g_view.highlight_mode = cfg.highlight_mode;
    g_view.text_generation = local_text_generation;
    g_view.font_size_ja = cfg.font_size_ja;
    g_view.font_size_en = cfg.font_size_en;
    g_view.anki_enabled = anki_on;
    g_view.n_warnings = cfg.n_warnings;
    sceClibMemcpy(g_view.warnings, cfg.warnings, sizeof(g_view.warnings));
    g_view.version++;
    vjo_view_unlock();
}

static void view_show_cache(void)
{
    view_waiting = view_job_text = 0;
    VjoOverlayData *d = &cache_data[active];
    size_t mark = vjo_arena_mark(&scratch);
    const char *err = d->err.rc ? vjo_err_text(&scratch, d->failed_stage, &d->err) : NULL;
    view_publish(1, &d->list, err, err != NULL);
    vjo_arena_release(&scratch, mark);
}

/* ---------------- network thread ---------------- */

typedef struct {
    const uint8_t *data;
} MemJpeg;

static int mem_jpeg_read(void *ud, uint32_t off, void *dst, uint32_t len)
{
    sceClibMemcpy(dst, ((MemJpeg *)ud)->data + off, len);
    return 0;
}

/* ---- local OCR: vita-vn-ocr (ocr_backend = vocr) ---- */

/* Out-of-memory advice, by where the memory ran out (#53). Each fits the
 * status line after "Text recognition failed: not enough memory. ". */
#define OOM_KERNEL_CAPTURE "The kernel could not allocate this region's screen capture (kernel memory " \
    "shared by all plugins); the OCR model does not affect it. Select a tighter dialogue region with Square, " \
    "or restart the Vita."
#define OOM_SHELL_RESULTS "The Shell has too little memory for the overlay's results; the region and " \
    "the OCR model do not affect it. Restart the Vita and try again."
#define OOM_SCREENSHOT "The screenshot did not fit the overlay's result memory. Select a tighter " \
    "dialogue region with Square."
#define OOM_TEXT_BUFFER "The recognized text did not fit the overlay's text buffer. Select a tighter " \
    "dialogue region with Square."
#define OOM_GAME_REGION "The game has too little free memory for OCR of this region. Select a tighter " \
    "dialogue region with Square."
#define OOM_GAME_NONE "The game has too little free memory for any OCR right now (it keeps a 4 MiB " \
    "reserve). Try again later in the game, or restart it."
#define OOM_GAME_MODEL_FITS "The game has too little free memory for this OCR model's weights; " \
    "VOCR3_FL10_w8.vocr, the smallest model, would fit this region."
#define OOM_GAME_MODEL "The game has too little free memory for this OCR model's weights. Select " \
    "VOCR3_FL10_w8.vocr, the smallest model, and a tighter dialogue region with Square."
#define OOM_GAME_SMALLEST "The game has too little free memory for the OCR model's weights, already " \
    "the smallest. Select a tighter dialogue region with Square, or try again later."
#define OOM_GAME_ALLOCATION "The game's memory could not be allocated although the memory check " \
    "passed. Try again."
#define OOM_GAME_UNKNOWN "The game's free memory could not be checked (see log.txt), so OCR did not start."
/* Any other OCR-stage shortage (Lens result arena, recognized text). */
#define OOM_OCR_OTHER "Select a tighter dialogue region with Square."

#ifdef VJO_WITH_VOCR
/* Which part of the game worker's memory check failed: no OCR can fit
 * beside the reserve; the region's job workspace; the model's weights (the
 * smallest model's, or a larger one's, and whether FL10's would fit); an
 * allocation after a passed check; or the check itself. */
static const char *game_oom_detail(const VjoGameOcrResult *r)
{
    int64_t job, free_bytes, fl10 = ((int64_t)VJO_VOCR_FL10_BYTES + 4095) & ~(int64_t)4095;
    if (!r->reserve_bytes) return NULL; /* not from the memory check: no specific advice */
    if (r->free_user < 0) return OOM_GAME_UNKNOWN;
    /* With a fallback, need_* are the fallback's: the last, smallest check. */
    free_bytes = (int64_t)r->free_user + r->release_bytes;
    job = (int64_t)r->reserve_bytes + r->need_workspace;
    if (free_bytes >= job + r->need_weights) return OOM_GAME_ALLOCATION;
    if (free_bytes < (int64_t)r->reserve_bytes) return OOM_GAME_NONE;
    if (free_bytes < job) return OOM_GAME_REGION;
    if (r->need_weights <= fl10) return OOM_GAME_SMALLEST;
    return free_bytes >= job + fl10 ? OOM_GAME_MODEL_FITS : OOM_GAME_MODEL;
}
#endif

#ifdef VJO_WITH_VOCR
static int vocr_cancelled(void *ud)
{
    (void)ud;
    __sync_synchronize();
    return !running || job_cancelled;
}
static int ocr_submit(void *ud, const VjoGameOcrRequest *r) { (void)ud; return vjoOcrSubmit(r); }
static int ocr_read(void *ud, uint32_t seq, VjoGameOcrResult *r) { (void)ud; return vjoOcrRead(seq, r); }
static int ocr_cancel(void *ud, uint32_t seq) { (void)ud; return vjoOcrCancel(seq); }
static int64_t ocr_clock(void *ud) { (void)ud; return now_us(); }
static void ocr_delay(void *ud, uint32_t us) { (void)ud; sceKernelDelayThread(us); }

/* The job's headroom: free USER (+ another model's resident weights, released
 * first) minus the reserve and what the job needs; negative means refused.
 * Clamped to int32 for the log. */
static int32_t vocr_headroom(const VjoGameOcrResult *r)
{
    int64_t h = (int64_t)r->free_user + r->release_bytes - (int64_t)r->reserve_bytes -
                (int64_t)r->need_workspace - (int64_t)r->need_weights;
    return h > INT32_MAX ? INT32_MAX : h < INT32_MIN ? INT32_MIN : (int32_t)h;
}

/* The game worker's memory check and per-line recognition, after the job
 * line: free USER against what the job needed (also for a refused job), its
 * headroom, and each line's crop, CTC frames and time, i.e. ms per frame on
 * this console. */
static void log_vocr_measurements(const VjoGameOcrResult *r)
{
    char buf[224];
    size_t n = 0;
    if (r->reserve_bytes && r->free_user < 0)
        vjo_log("game vocr memory: free USER unknown (error 0x%08X), needs workspace %u + weights %u + "
                "reserve %u: check refused", (unsigned)r->free_user, (unsigned)r->need_workspace,
                (unsigned)r->need_weights, (unsigned)r->reserve_bytes);
    else if (r->reserve_bytes && r->release_bytes)
        /* Another model's resident weights, released before this job's. */
        vjo_log("game vocr memory: free USER=%d + own weights %u, needs workspace %u + weights %u + "
                "reserve %u: check %s, headroom %d", (int)r->free_user, (unsigned)r->release_bytes,
                (unsigned)r->need_workspace, (unsigned)r->need_weights, (unsigned)r->reserve_bytes,
                vocr_headroom(r) >= 0 ? "passed" : "refused", (int)vocr_headroom(r));
    else if (r->reserve_bytes)
        vjo_log("game vocr memory: free USER=%d, needs workspace %u + weights %u + reserve %u: check %s, headroom %d",
                (int)r->free_user, (unsigned)r->need_workspace, (unsigned)r->need_weights,
                (unsigned)r->reserve_bytes, vocr_headroom(r) >= 0 ? "passed" : "refused", (int)vocr_headroom(r));
    if (r->frames)
        vjo_log("game vocr speed: %u frames in %u us, %u us per frame", (unsigned)r->frames,
                (unsigned)r->recognize_us, (unsigned)(r->recognize_us / r->frames));
    /* Crop WxH, CTC frames and time of each line; 6 per log line (256 bytes each). */
    for (uint32_t i = 0; i < r->recognized && i < VJO_GAME_OCR_LINES; i++) {
        const VjoGameOcrLine *l = &r->line[i];
        int w;
        if (!n) {
            w = sceClibSnprintf(buf, sizeof(buf), i ? "game vocr lines (cont):" : "game vocr lines:");
            n = w > 0 && (size_t)w < sizeof(buf) ? (size_t)w : 0;
        }
        w = sceClibSnprintf(buf + n, sizeof(buf) - n, " %ux%u %uf %u.%ums", l->width, l->height,
                            l->frames, (unsigned)(l->us / 1000), (unsigned)(l->us % 1000 / 100));
        n = w > 0 && n + (size_t)w < sizeof(buf) ? n + (size_t)w : sizeof(buf) - 1;
        if (i % 6 == 5 || i + 1 == r->recognized || i + 1 == VJO_GAME_OCR_LINES) {
            vjo_log("%s", buf);
            n = 0;
        }
    }
}

/* The game worker's memory check for the job it reported (with a fallback,
 * the fallback's) refused it. */
static int fallback_check_refused(const VjoGameOcrResult *r)
{
    return r->reserve_bytes && (r->free_user < 0 ||
           (int64_t)r->free_user + r->release_bytes <
               (int64_t)r->need_workspace + r->need_weights + r->reserve_bytes);
}

/* The configured VOCR model failed to open or verify in this game (missing,
 * wrong or corrupt file): later jobs of the same game ask for the fallback. */
static char vocr_unusable[VJO_GAME_OCR_MODEL_BYTES];
static uint32_t vocr_unusable_generation;
static int vocr_fallback_requested;  /* the last request carried a fallback */
static int vocr_retry_with_fallback; /* this job's retry: request the fallback itself */

/* The capture of the job in flight: the kernel publishes it (VjoState.done_seq
 * moves on, with capture_scene and region_seq) once the game has read its
 * first pass over the region. A try after the screen changed under the job
 * voids it (region_seq 0 while the rows settle and are read again) and
 * publishes its own, so the job's capture is always its latest one. Polled
 * from the exchange's cancel callback. */
static uint32_t vocr_done_before;
static int vocr_capture_fixed; /* full-frame and probe jobs: theirs is not the region's */

static void vocr_note_capture(void)
{
    VjoState st;
    if (vocr_capture_fixed) return;
    st.size = sizeof(st);
    if (vjoGetState(&st) < 0 || st.done_seq == vocr_done_before) return;
    if (!st.region_seq) {
        job_captured = 0;
        return;
    }
    job_capture_scene = st.capture_scene;
    job_capture_seq = st.region_seq;
    __sync_synchronize(); /* the scene is visible before job_captured */
    job_captured = 1;
}

static int vocr_poll_cancelled(void *ud)
{
    vocr_note_capture();
    return vocr_cancelled(ud);
}

/* The game worker's step log: one log.txt line per step (`step/rc/bytes/free`). */
static void log_vocr_steps(const char *who, const VjoGameOcrResult *r)
{
    char line[224];
    for (uint32_t i = 0; i < r->n_steps && i < VJO_GAME_OCR_STEPS; i++)
        if (vjo_gstep_format(line, sizeof(line), who, &r->step[i]) > 0)
            vjo_log("%s", line);
}

/* One job: request (flags: VJO_GAME_OCR_F_*; force_model: this model, no
 * fallback), exchange, logs. */
static int vocr_run_one(VjoGameOcrResult *result, uint32_t flags, const char *force_model, const char **ran)
{
    VjoGameOcrRequest request = {.size=sizeof(request), .flags=flags};
    VjoGameOcrClient client = {NULL, ocr_submit, ocr_read, ocr_cancel, ocr_clock, ocr_delay};
    VjoState st0;
    sceClibSnprintf(request.model_dir, sizeof(request.model_dir), "%s", job_cfg.ocr_model_dir);
    const char *model = job_cfg.ocr_backend == VJO_OCR_VOCR2 ? VJO_VOCR2_MODEL_NAME : job_cfg.vocr_model;
    /* VOCR2's pinned model is already FL10-sized: no fallback. */
    const char *fallback = job_cfg.ocr_backend == VJO_OCR_VOCR2 ? "" : job_cfg.vocr_fallback_model;
    vocr_fallback_requested = 0;
    if (fallback[0] && !sceClibStrcmp(fallback, model)) fallback = "";
    if (force_model) {
        model = force_model;
        fallback = "";
    } else if (fallback[0] && (vocr_retry_with_fallback ||
                               (vocr_unusable_generation == job_generation && !sceClibStrcmp(vocr_unusable, model)))) {
        model = fallback;
        fallback = "";
    }
    size_t model_len = sceClibStrnlen(model, sizeof(job_cfg.vocr_model));
    size_t fallback_len = sceClibStrnlen(fallback, sizeof(job_cfg.vocr_fallback_model));
    if (!model_len || model_len >= sizeof(request.model) || fallback_len >= sizeof(request.fallback))
        return VJO_E_OCR_MODEL;
    sceClibMemcpy(request.model, model, model_len+1);
    sceClibMemcpy(request.fallback, fallback, fallback_len+1);
    vocr_fallback_requested = fallback_len > 0;
    st0.size = sizeof(st0);
    if (vjoGetState(&st0) < 0) st0.done_seq = st0.stable = 0;
    vocr_done_before = st0.done_seq;
    vocr_capture_fixed = (flags & (VJO_GAME_OCR_F_FULL | VJO_GAME_OCR_F_PROBE)) != 0;
    /* The job's policy (API 20). The config keeps both waits in range, so the
     * counts fit their 8 bits (10 and 50 ms steps, rounded; a wait of 1-4 ms is
     * one step, not none); a screen not settled now is waited for first. */
    if (job_cfg.ocr_skip_blank_lines) request.flags |= VJO_GAME_OCR_F_BLANK;
    if (job_cfg.ocr_settle_ms > 0) {
        uint32_t settle = (uint32_t)(job_cfg.ocr_settle_ms + 5) / 10u;
        request.flags |= (settle ? settle : 1u) << VJO_GAME_OCR_SETTLE_SHIFT |
                         (uint32_t)(job_cfg.ocr_settle_max_ms + 25) / 50u << VJO_GAME_OCR_SETTLE_MAX_SHIFT;
        if (!st0.stable) request.flags |= VJO_GAME_OCR_F_SETTLE_FIRST;
    }
    /* API 21 (ocr-speed): helper threads for parallel lines, the ARM clock. */
    if (job_cfg.ocr_threads > 1) request.speed |= (uint32_t)(job_cfg.ocr_threads - 1) & VJO_GAME_OCR_S_HELPERS_MASK;
    if (job_cfg.ocr_boost_clock) request.speed |= VJO_GAME_OCR_S_BOOST;
    vjo_capture_hold(); /* no Lens/Anki capture while the game captures for this job */
    int rc = vjo_game_ocr_exchange(&client, &request, result, vocr_poll_cancelled, NULL, 60000000);
    vjo_capture_release();
    vocr_note_capture();
    /* weights: read = from the card and SHA-256 checked, resident = kept from
     * an earlier job (file unchanged); resident = bytes the game keeps after it. */
    static const char *const weights[] = {"none", "read", "resident"};
    static const char *const sources[] = {"none", "hook", "kernel strip", "user"};
    static const char *const memories[] = {"none", "USER", "CDRAM", "PHYCONT", "kernel"};
    *ran = result->fallback ? request.fallback : model;
    vjo_log("game vocr rc=%d %s%s: %ux%u at %u,%u of %ux%u fmt 0x%08X, lines=%u USER workspace=%u weights=%s resident=%u, "
            "find=%u ms load=%u ms recognize=%u ms frames=%u io=0x%08X stat=0x%08X cleanup=%d",
            rc, *ran, !result->fallback ? "" : fallback_check_refused(result) ? " (fallback, refused too)" : " (fallback)",
            result->width, result->height, result->region_x, result->region_y, result->fb_width, result->fb_height,
            (unsigned)result->fb_format, result->lines, result->workspace_bytes,
            result->weights < 3 ? weights[result->weights] : "?", result->resident_bytes,
            result->find_us/1000, result->load_us/1000, result->recognize_us/1000, result->frames,
            (unsigned)result->io_error, (unsigned)result->stat_error, result->cleanup_status);
    vjo_log("game vocr capture: %s into %s, %u B block, %u-row strips, %u strips in %u ms, %u rows changed, sum 0x%08X",
            result->source < 4 ? sources[result->source] : "?",
            result->capture_memory < 5 ? memories[result->capture_memory] : "?", (unsigned)result->capture_bytes,
            (unsigned)result->strip_rows, (unsigned)result->captures, (unsigned)(result->capture_us / 1000),
            (unsigned)result->tears, (unsigned)result->region_sum);
    if (result->fallback)
        vjo_log("game vocr fallback: %s needs workspace %u + weights %u + reserve %u, more than free "
                "USER %d + own weights %u", model, (unsigned)result->model_need_workspace,
                (unsigned)result->model_need_weights, (unsigned)result->reserve_bytes,
                (int)result->free_user, (unsigned)result->model_release_bytes);
    log_vocr_measurements(result);
    log_vocr_steps(flags & VJO_GAME_OCR_F_PROBE ? "probe" : "job", result);
    return rc;
}

/* The self-test (docs/self-test.md): every stage on this settled screen in
 * turn, each with its record, a failed one included. They share the job's one
 * result (the Shell's pages may only shrink): the last stage, the user's
 * region again, leaves the job's text. */
static int run_selftest_stages(VjoGameOcrResult *result)
{
    unsigned n = vjo_selftest_stages();
    int rc = VJO_E_SOURCE;
    for (unsigned i = 0; i < n && !vocr_cancelled(NULL); i++) {
        const char *ran = "?";
        int64_t t = now_us();
        vjo_selftest_stage_begin(i, job_cfg.vocr_model, job_cfg.vocr_fallback_model);
        sceClibMemset(result, 0, sizeof(*result));
        rc = vocr_run_one(result, vjo_selftest_stage_flags(i), NULL, &ran);
        vjo_selftest_stage_end(i, rc, result, (uint32_t)((now_us() - t) / 1000), ran);
    }
    if (vocr_cancelled(NULL)) return VJO_E_CANCELLED; /* the stages run again on the next settled screen */
    vjo_selftest_done();
    return rc;
}

static int run_vocr(VjoArena *a, VjoOverlayData *out)
{
    static VjoGameOcrResult result;
    const char *ran = "?";
    int local = local_pipeline(&job_cfg);
    if (job_cfg.ocr_backend == VJO_OCR_VOCR2 && !VJO_VOCR2_MODEL_NAME[0]) {
        out->err.detail = "VOCR2 weights are unavailable in this build.";
        return out->err.rc = VJO_E_OCR_MODEL;
    }
    sceClibMemset(&result, 0, sizeof(result));
    vocr_fallback_requested = 0;
    if (vjo_selftest_stages())
        out->err.rc = run_selftest_stages(&result);
    else
        out->err.rc = vocr_run_one(&result, 0, NULL, &ran);
    /* The configured model failed to open or verify: keep OCR working with
     * the fallback for this job. Only a file that is absent, the wrong
     * length or fails its hash (io 0: rejected after reading) makes the
     * switch stick for this game; a timeout or unavailable storage does not. */
    if (out->err.rc == VJO_E_OCR_MODEL && vocr_fallback_requested && !result.fallback &&
        !vocr_cancelled(NULL)) {
        int file_bad = !result.io_error || result.io_error == VJO_ERR_FORMAT ||
                       (uint32_t)result.io_error == 0x80010002u; /* ENOENT */
        vjo_log("game vocr: %s is unusable here (io=0x%08X), using %s%s", job_cfg.vocr_model,
                (unsigned)result.io_error, job_cfg.vocr_fallback_model,
                file_bad ? " for this game" : " for this job");
        if (file_bad) {
            sceClibSnprintf(vocr_unusable, sizeof(vocr_unusable), "%.*s", (int)sizeof(vocr_unusable) - 1,
                            job_cfg.vocr_model);
            vocr_unusable_generation = job_generation;
        }
        vocr_retry_with_fallback = 1;
        sceClibMemset(&result, 0, sizeof(result));
        out->err.rc = vocr_run_one(&result, 0, NULL, &ran);
        vocr_retry_with_fallback = 0;
    }
    if (out->err.rc != VJO_OK) {
        if (out->err.rc == VJO_E_OCR_MODEL) {
            /* Vita errno ENODEV and EACCES are distinct from absent/corrupt weights. */
            if ((uint32_t)result.io_error == 0x80010013u)
                out->err.detail = "The local OCR model storage is unavailable to this game. See log.txt for details.";
            else if ((uint32_t)result.io_error == 0x8001000Du)
                out->err.detail = "Access to the local OCR model was denied. See log.txt for details.";
        }
        if (out->err.rc == VJO_E_OOM)
            out->err.detail = game_oom_detail(&result);
        if (out->err.rc == VJO_E_SOURCE)
            out->err.detail = "The game's screen could not be read (see log.txt).";
        return out->err.rc;
    }
    if (local) {
        VjoArena text_arena;
        vjo_arena_init(&text_arena, local_text, sizeof(local_text));
        out->ocr_text = result.text;
        out->filtered = vjo_filter_lines(&text_arena, result.text, job_cfg.non_japanese_filter);
        /* Filtering only removes bytes, so one complete worker text fits. */
        out->sentence = out->filtered ? vjo_java_trim((char *)out->filtered) : NULL;
        if (!out->sentence) {
            out->failed_stage = VJO_STAGE_OCR;
            out->err.detail = OOM_TEXT_BUFFER;
            return out->err.rc = VJO_E_OOM;
        }
        /* Cursor byte0 is the trimmed sentence, not discarded whitespace. */
        size_t len = sceClibStrnlen(out->sentence, sizeof(local_text) - 1);
        sceClibMemmove(local_text, out->sentence, len + 1);
        out->filtered = out->sentence = local_text;
        out->list.header = out->sentence ? out->sentence : "";
        sceKernelLockMutex(cmd_lock, 1, NULL);
        job_text_generation = advance_generation(&local_text_generation);
        job_cursor_seq = advance_generation(&local_cursor_seq);
        local_page = job_page = 0;
        local_cursor_byte = job_cursor_byte = job_cfg.japanese_only_selection
            ? (uint32_t)vjo_utf8_japanese_cursor_step(local_text, len, 0, 0) : 0;
        local_text_game_generation = job_generation;
        local_text_scene = job_capture_scene;
        local_text_capture_seq = job_capture_seq;
        __sync_synchronize();
        local_text_valid = 1;
        sceKernelUnlockMutex(cmd_lock, 1);
        if (!job_lookup || !len || job_cursor_byte >= len || mem_alloc_for(&job_cfg) == 0)
            return VJO_OK;
        out->failed_stage = VJO_STAGE_DICT;
        out->err.dict = VJO_DICT_LOCAL;
        out->err.detail = "Text was recognized, but Shell UI memory is too low for dictionary results. Restart the Vita and try again.";
        return out->err.rc = VJO_E_OOM;
    }
    return vjo_overlay_ocr_text(a, &job_cfg, result.text, out);
}
#else
static int run_vocr(VjoArena *a, VjoOverlayData *out)
{
    (void)a;
    out->err.detail = "This build has no vita-vn-ocr backend. Install a build made with VJO_WITH_VOCR, "
                      "or set ocr_backend = lens.";
    return out->err.rc = VJO_E_OCR_UNAVAILABLE;
}
#endif

static int run_job(VjoArena *a, VjoOverlayData *out)
{
    VjoState st;
    VjoJpegSource src;
    VjoBuf jb;
    MemJpeg mem;

    sceClibMemset(out, 0, sizeof(*out));
    out->list.header = "";
    /* Warm connections may still own the old pool when a configuration change
     * requests a job. Only this thread can retire it once that work finishes. */
    if (local_pipeline(&job_cfg)) {
        mem_free();
    } else if (mem_alloc_for(&job_cfg) < 0) {
        out->failed_stage = VJO_STAGE_OCR;
        out->err.detail = OOM_SHELL_RESULTS;
        return out->err.rc = VJO_E_OOM;
    }
    vjo_arena_reset(a);
    if (game_ocr_backend(job_cfg.ocr_backend)) {
        /* On the console only: no image or connection leaves it. */
        if (run_vocr(a, out) != VJO_OK) {
            if (!out->failed_stage)
                out->failed_stage = VJO_STAGE_OCR;
            if (out->err.rc == VJO_E_OCR_UNAVAILABLE)
                out->err.detail = "The game OCR worker is unavailable. Register it for this game (install --title) and reboot.";
            /* The text filter's arena (network dictionaries): the text was too long. */
            if (out->err.rc == VJO_E_OOM && out->failed_stage == VJO_STAGE_OCR && !out->err.detail &&
                out->ocr_text)
                out->err.detail = OOM_TEXT_BUFFER;
            return out->err.rc;
        }
        goto recognized;
    }
    vjo_buf_init(&jb, a);
    st.capture_scene = 0;
    st.capture_result = 0;
    if ((out->err.rc = vjo_capture_jpeg(a, 0, JPEG_QUALITY, &jb, &st)) != VJO_OK) {
        out->failed_stage = VJO_STAGE_OCR;
        if (out->err.rc == VJO_E_OOM)
            out->err.detail = st.capture_result == VJO_ERR_NO_MEMORY ? OOM_KERNEL_CAPTURE : OOM_SCREENSHOT;
        return out->err.rc;
    }
    job_capture_scene = st.capture_scene;
    job_capture_seq = st.region_seq;
    __sync_synchronize(); /* the scene is visible before job_captured */
    job_captured = 1;
    sceClibMemset(&src, 0, sizeof(src));
    src.width = st.width;
    src.height = st.height;
    mem.data = jb.data;
    src.ud = &mem;
    src.read = mem_jpeg_read;
    src.size = (uint32_t)jb.len;
    if (vjo_overlay_ocr(a, &plat, &job_cfg, &src, out)) {
        if (out->err.rc == VJO_E_OOM && out->failed_stage == VJO_STAGE_OCR && !out->err.detail)
            out->err.detail = OOM_OCR_OTHER;
        return out->err.rc;
    }
recognized:
    /* The screen changed only in pixels (a mark, a cursor, an effect):
     * the cached lookup is this text's. */
    job_same_text = job_cached_text && out->filtered && !sceClibStrcmp(out->filtered, job_cached_text);
    __sync_synchronize(); /* the sentence is visible before job_text_ready */
    job_text_ready = 1;
    if (!job_lookup || job_same_text) return VJO_OK;
    if (local_pipeline(&job_cfg) &&
        job_cursor_byte >= sceClibStrnlen(local_text, sizeof(local_text))) return VJO_OK;
    if (!local_pipeline(&job_cfg))
        return vjo_overlay_lookup(a, &plat, &job_cfg, out);
    int64_t t0 = now_us();
    vjo_dict_reads = vjo_dict_read_bytes = 0;
    int rc = vjo_overlay_lookup_at(a, &plat, &job_cfg, job_cursor_byte, out);
    note_pages(rc, out);
    log_lookup(job_prepares ? "first word" : "on open", out, job_cursor_byte, rc, t0);
    return rc;
}

/* Records which text/limit the locators in this arena belong to. */
static void note_pages(int rc, const VjoOverlayData *out)
{
    pages_generation = !rc && out->pages ? job_text_generation : 0;
    pages_limit = job_cfg.local_dictionary_results;
}

/* No capture, model, inference or sentence tokenization for cursor moves.
 * Another dictionary page at the same cursor reads only that page's entry
 * into the same arena, after the locators (no probing, no new allocation). */
static int run_cursor_job(VjoArena *a, VjoOverlayData *out)
{
    const VjoLocalPages *pages = out->pages;
    int64_t t0 = now_us();
    int rc;
    vjo_dict_reads = vjo_dict_read_bytes = 0;
    if (pages && has_result_memory() && pages_generation == job_text_generation &&
        pages_limit == job_cfg.local_dictionary_results &&
        pages->cursor_byte == job_cursor_byte && job_page < pages->count) {
        /* post_local_lookup already published the static header, so the
         * shown entry can be released without the view lock. */
        rc = vjo_overlay_lookup_page(a, &plat, &job_cfg, job_page, out);
        if (rc) pages_generation = 0; /* e.g. a replaced file: locate again next time */
        log_lookup("page", out, job_cursor_byte, rc, t0);
        return rc;
    }
    sceClibMemset(out, 0, sizeof(*out));
    out->ocr_text = out->filtered = out->sentence = local_text;
    out->list.header = local_text;
    if (job_cursor_byte >= sceClibStrnlen(local_text, sizeof(local_text))) return VJO_OK;
    if (mem_alloc_for(&job_cfg) < 0) {
        out->failed_stage = VJO_STAGE_DICT;
        out->err.dict = VJO_DICT_LOCAL;
        out->err.detail = "Text was recognized, but Shell UI memory is too low for dictionary results. Restart the Vita and try again.";
        return out->err.rc = VJO_E_OOM;
    }
    vjo_view_lock(); /* the published list is the static header, never old entries */
    vjo_arena_reset(a);
    vjo_view_unlock();
    rc = vjo_overlay_lookup_at(a, &plat, &job_cfg, job_cursor_byte, out);
    note_pages(rc, out);
    /* Locators were stale (another limit): show the requested page if it exists. */
    if (!rc && job_page > 0 && out->pages && job_page < out->pages->count)
        rc = vjo_overlay_lookup_page(a, &plat, &job_cfg, job_page, out);
    log_lookup("cursor", out, job_cursor_byte, rc, t0);
    return rc;
}

static int net_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (running) {
        unsigned int bits = 0;
        sceKernelWaitEventFlag(net_evf, NET_EV_JOB | NET_EV_WARM | NET_EV_QUIT,
                               SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, NULL);
        if (bits & NET_EV_QUIT)
            break;
        if ((bits & NET_EV_WARM) && !(bits & NET_EV_JOB)) /* a job opens them itself */
            vjo_overlay_warm(&plat, warm_dictionary, WARM_CONNECT_US);
        if (bits & NET_EV_WARM) {
            __sync_synchronize();
            warm_running = 0;
        }
        if (bits & NET_EV_JOB) {
            int idx = job_idx;
            int64_t t0 = now_us();
            job_rc = job_cursor_only ? run_cursor_job(&results[idx], &cache_data[idx])
                                     : run_job(&results[idx], &cache_data[idx]);
            vjo_log("job %d done rc=%d in %d ms, arena peak %u, dictionary page %d/%d", idx, job_rc,
                    (int)((now_us() - t0) / 1000), (unsigned)results[idx].peak,
                    cache_data[idx].page + 1, vjo_overlay_page_count(&cache_data[idx]));
            __sync_synchronize(); /* results are visible before job_done */
            job_done = 1;
        }
    }
    return 0;
}

/* The network thread is at work (its memory must stay). */
static int net_busy(void)
{
    __sync_synchronize();
    return job_running || warm_running;
}

/* lookup = 0: OCR only (the subtitles' sentence). */
static void start_job(const char *why, int lookup)
{
    if (job_running)
        return;
    if (local_pipeline(&cfg)) {
        /* Retire the previous lookup before OCR, then recreate result arenas
         * after all capture/model/inference allocations have been freed. */
        retire_local_text(0); /* atomic request exclusion and view detachment */
        if (ov != OV_CLOSED)
            view_publish(1, NULL, "Recognizing…", 0);
        if (!warm_running)
            mem_free();
    }
    vjo_log("job start (%s)%s", why, lookup ? "" : ", OCR only");
    job_lookup = lookup;
    job_prepares = ov == OV_CLOSED;
    job_cursor_only = 0;
    job_text_generation = 0;
    job_idx = local_pipeline(&cfg) ? 0 : active == 0 ? 1 : 0;
    job_cfg = cfg; /* the control thread may reload cfg while the job runs */
    job_generation = game_generation;
    job_cached_text = !local_pipeline(&cfg) && cache_ok && cache_dictionary == cfg.dictionary
                      ? cache_data[active].filtered : NULL;
    job_same_text = 0;
    job_captured = 0;
    job_capture_scene = 0;
    job_capture_seq = 0;
    job_cancelled = 0;
    vjo_net_cancel_clear(&net_cancel);
    if (view_job_text) { /* its arena may be the new job's */
        view_job_text = 0;
        view_publish(1, NULL, "Recognizing…", 0);
    }
    job_done = 0;
    job_text_ready = 0;
    job_running = 1;
    job_started_us = now_us();
    __sync_synchronize();
    sceKernelSetEventFlag(net_evf, NET_EV_JOB);
    if (subtitles)
        vjo_strip_set_busy(1);
}

static void start_cursor_job(void)
{
    VjoState st = {.size = sizeof(st)};
    if (job_running) { cursor_waiting = 1; return; }
    if (ov == OV_CLOSED || !title_id[0] || !local_pipeline(&cfg) || !local_text_valid ||
        local_text_game_generation != game_generation) {
        cursor_waiting = 0;
        return;
    }
    if (vjoGetState(&st) < 0 || !shows_screen(local_text_scene, local_text_capture_seq, &st)) {
        retire_local_text(1);
        view_publish(1, NULL, "The dialogue changed. Close and reopen the overlay.", 1);
        return;
    }
    sceKernelLockMutex(cmd_lock, 1, NULL);
    job_cursor_only = 1;
    cache_ok = 0; /* this job will overwrite the single result arena */
    job_lookup = 1;
    job_idx = 0;
    job_cfg = cfg;
    job_generation = game_generation;
    job_text_generation = local_text_generation;
    job_cursor_byte = local_cursor_byte;
    job_cursor_seq = local_cursor_seq;
    job_page = local_page;
    cursor_waiting = 0;
    job_cached_text = NULL;
    job_same_text = job_cancelled = job_done = job_text_ready = 0;
    job_captured = 1;
    job_capture_scene = local_text_scene;
    job_capture_seq = local_text_capture_seq;
    vjo_net_cancel_clear(&net_cancel);
    local_header = (VjoEntryList){.header = local_text};
    view_publish(1, &local_header, "Looking up words…", 0);
    job_running = 1;
    job_started_us = now_us();
    __sync_synchronize();
    sceKernelSetEventFlag(net_evf, NET_EV_JOB);
    sceKernelUnlockMutex(cmd_lock, 1);
}

/* ---------------- control thread ---------------- */

static int same_pipeline(const VjoConfig *a, const VjoConfig *b)
{
    if (a->dictionary != b->dictionary || a->ocr_backend != b->ocr_backend ||
        a->non_japanese_filter != b->non_japanese_filter ||
        a->japanese_only_selection != b->japanese_only_selection)
        return 0;
    if (game_ocr_backend(a->ocr_backend) &&
        (sceClibStrcmp(a->ocr_model_dir, b->ocr_model_dir) || a->ocr_skip_blank_lines != b->ocr_skip_blank_lines ||
         (a->ocr_backend == VJO_OCR_VOCR && (sceClibStrcmp(a->vocr_model, b->vocr_model) ||
                                             sceClibStrcmp(a->vocr_fallback_model, b->vocr_fallback_model)))))
        return 0;
    if (a->dictionary == VJO_DICT_LOCAL)
        return !sceClibStrcmp(a->local_dictionary_dir, b->local_dictionary_dir) &&
               !sceClibStrcmp(a->local_dictionaries, b->local_dictionaries);
    if (a->dictionary == VJO_DICT_HACHIDORI)
        return !sceClibStrcmp(a->hachidori_host, b->hachidori_host);
    return !sceClibStrcmp(vjo_config_api_key(a), vjo_config_api_key(b));
}

static void apply_config(void)
{
    VjoConfig previous = cfg;
    vjo_config_load(&cfg, &scratch);
    if (vjo_selftest_armed())
        cfg.ocr_mode = VJO_OCR_AUTO; /* the first settled screen is recognized unasked */
    if (!same_pipeline(&previous, &cfg)) {
        cache_ok = 0;
        retire_local_text(1);
        if (job_running && !same_pipeline(&job_cfg, &cfg)) {
            job_cancelled = 1;
            vjo_net_cancel(&net_cancel);
        }
    } else if (previous.local_dictionary_results != cfg.local_dictionary_results) {
        /* A presentation of the same recognized text: look the word up again
         * (from its best match) without recapturing or rerunning OCR. */
        cache_ok = 0;
        sceKernelLockMutex(cmd_lock, 1, NULL);
        vjo_view_lock();
        local_page = 0;
        g_view.dict_page = g_view.dict_pages = 0;
        vjo_view_unlock();
        sceKernelUnlockMutex(cmd_lock, 1);
    }
    vjo_log_configure(&cfg);
    vjo_strip_configure(subtitles, &cfg);
    anki_on = vjo_anki_configure(&cfg);
    if (vjoSetTriggers(cfg.toggle_button, cfg.subtitle_button) < 0)
        vjo_log("kernel refused the buttons: keeping the previous ones");
    for (int i = 0; i < cfg.n_warnings; i++)
        vjo_log("config warning: %s", cfg.warnings[i]);
}

/* The region in force for this game (w == 0: none, the whole frame). The
 * self-test uses it as it is; its full-screen stage asks for the whole frame
 * itself (VJO_GAME_OCR_F_FULL). */
static VjoRect game_region;

static void push_region(void)
{
    VjoRect r;
    int found = vjo_region_load(title_id, &r, &scratch);
    vjoSetRegion(found && r.w ? &r : NULL);
    if (found && r.w) game_region = r;
    else sceClibMemset(&game_region, 0, sizeof(game_region));
    vjo_log("region: %s%s", found == VJO_REGION_GAME ? "this game's" : found ? "all games'" : "none",
            found && r.w ? "" : ", full screen");
}

static int game_active(void)
{
    return title_id[0] != '\0';
}

/* The running job captured another screen than the one settled now: it is
 * abandoned (Lens and the dictionary take seconds), so the job for this
 * screen can start at once. Returns 1 if it was. */
static int cancel_outdated_job(const VjoState *st)
{
    __sync_synchronize(); /* pairs with the network thread's barrier */
    if (!job_running || job_cancelled || !job_captured || !(st->stable || st->quiet) ||
        shows_screen(job_capture_scene, job_capture_seq, st))
        return 0;
    job_cancelled = 1;
    vjo_net_cancel(&net_cancel);
    vjo_log("job cancelled: the screen changed since its capture");
    return 1;
}

/* Is the cached result for the screen in st? If so it is retagged with
 * st's scene, which stays valid through later captures. */
static int claim_cache_for(const VjoState *st)
{
    if (!cache_ok || active < 0 || !shows_screen(cache_scene, cache_seq, st))
        return 0;
    if (cache_scene != st->scene) {
        vjo_view_lock();
        cache_scene = st->scene;
        vjo_view_unlock();
    }
    return 1;
}

static void clear_input_block_error(void)
{
    if (input_block_error) {
        input_block_error = 0;
        vjo_strip_show("", VJO_STRIP_STATUS, 0);
    }
}

static void open_overlay(void)
{
    VjoState st;
    int block_rc = vjoSetInputBlock(1);
    vjo_log("input block on rc=%d (%08X)", block_rc, block_rc);
    if (block_rc < 0) {
        input_block_error = 1;
        ov = OV_CLOSED;
        view_publish(0, NULL, NULL, 0);
        vjo_strip_show("Overlay unavailable: cannot block game controls.", VJO_STRIP_ERROR, 0);
        vjo_view_lock();
        __atomic_store_n(&g_view.strip_on, 1, __ATOMIC_RELEASE);
        vjo_view_unlock();
        return;
    }
    clear_input_block_error();
    ov = OV_OPEN;
    apply_config(); /* settings are re-read on every open */
    if (anki_on)
        vjo_anki_post_count(); /* the queue line's count (Anki's thread, no allocation) */
    if (!net_busy() && !local_pipeline(&cfg) && mem_alloc() < 0) {
        view_publish(1, NULL, "Not enough memory for the overlay", 1);
        return;
    }
    if (!vjo_config_dict_ready(&cfg)) {
        VjoErr e = {cfg.dictionary == VJO_DICT_HACHIDORI ? VJO_E_NO_HOST : VJO_E_NO_KEY,
                    0, 0, NULL, cfg.dictionary};
        size_t mark = vjo_arena_mark(&scratch);
        view_publish(1, NULL, vjo_err_text(&scratch, VJO_STAGE_DICT, &e), 1);
        vjo_arena_release(&scratch, mark);
        return;
    }
    st.size = sizeof(st);
    vjoGetState(&st);
    if (local_pipeline(&cfg) && local_text_valid &&
        local_text_game_generation == game_generation &&
        shows_screen(local_text_scene, local_text_capture_seq, &st)) {
        /* Reopening a completed lookup must not replace its word with the
         * pending character and repeat disk I/O. Cursor/text generations
         * exclude a result superseded before its next job has started. */
        if (!job_running && !cursor_waiting && job_cursor_seq == local_cursor_seq &&
            job_text_generation == local_text_generation && claim_cache_for(&st)) {
            vjo_log("open: saved local word");
            view_show_cache();
            return;
        }
        local_header = (VjoEntryList){.header = local_text};
        view_publish(1, &local_header, "Looking up words…", 0);
        /* Opening while the prepared first lookup is still finishing can
         * show that result directly; don't queue the same lookup again. */
        if (job_running && job_lookup && !job_cancelled &&
            job_text_generation == local_text_generation && job_cursor_seq == local_cursor_seq)
            return;
        start_cursor_job();
        return;
    }
    if (st.alloc_status == VJO_ALLOC_FAIL) {
        view_publish(1, NULL, "Not enough memory to capture the screen", 1);
        return;
    }
    if (claim_cache_for(&st)) { /* a background job's, or the subtitles' */
        vjo_log("open: background result");
        view_show_cache();
        return;
    }
    /* Why the background result can't be used (tuning). */
    vjo_log("open: %s (stable %u, quiet %u)",
            job_running ? "background job still running"
            : !cache_ok || active < 0 ? "no background result"
                                      : "screen changed since the background result",
            st.stable, st.quiet);
    view_publish(1, NULL, "Recognizing…", 0);
    view_waiting = 1;
    if (job_running) {
        ov = OV_OPEN_OLD_JOB; /* a cancelled one is followed by a new one */
        cancel_outdated_job(&st);
    } else {
        start_job("overlay opened", 1);
    }
}

static void close_overlay(void)
{
    if (ov == OV_CLOSED)
        return;
    ov = OV_CLOSED;
    cursor_waiting = 0;
    if (job_running && job_cursor_only) job_cancelled = 1;
    view_waiting = view_job_text = 0;
    vjoSetInputBlock(0);
    vjo_view_lock();
    __atomic_store_n(&g_view.open, 0, __ATOMIC_RELEASE);
    /* The next job may reuse the shown arena; an open republishes it. */
    g_view.list = NULL;
    g_view.local_cursor_mode = 0;
    g_view.version++;
    vjo_view_unlock();
}

static void backoff_note(Backoff *b, int failed)
{
    if (!failed) {
        b->us = 0;
        b->until = 0;
        return;
    }
    b->us = b->us ? b->us * 2 : BACKOFF_MIN_US;
    if (b->us > BACKOFF_MAX_US)
        b->us = BACKOFF_MAX_US;
    b->until = now_us() + b->us;
}

/* The lookup of the finished job's text: the cached result's when it was
 * the same text (which needs a usable cache: active is set), else its own
 * if it made one. NULL if none. */
static const VjoEntryList *job_words(const VjoOverlayData *d)
{
    if (job_same_text)
        return &cache_data[active].list;
    return job_lookup && d->err.rc == VJO_OK ? &d->list : NULL;
}

/* The strip for a finished job: its sentence with its readings, else its
 * error. */
static void strip_show_result(const VjoOverlayData *d)
{
    size_t mark = vjo_arena_mark(&scratch);
    if (d->sentence) {
        const VjoEntryList *words = job_words(d);
        vjo_strip_show(d->sentence, VJO_STRIP_SENTENCE, 0);
        if (words)
            vjo_strip_show_readings(&scratch, words);
    } else {
        vjo_strip_show(vjo_err_text(&scratch, d->failed_stage, &d->err), VJO_STRIP_ERROR, 0);
    }
    vjo_arena_release(&scratch, mark);
}

/* The overlay was opened while the job ran: is the result for the screen
 * shown now, with its lookup? Else a new job. Returns 1 if one started. */
static int restart_old_job(int has_lookup)
{
    VjoState st;
    if (ov != OV_OPEN_OLD_JOB)
        return 0;
    ov = OV_OPEN;
    st.size = sizeof(st);
    vjoGetState(&st);
    if (shows_screen(job_capture_scene, job_capture_seq, &st) && has_lookup)
        return 0;
    if (local_pipeline(&cfg) && local_text_valid &&
        local_text_game_generation == game_generation &&
        shows_screen(local_text_scene, local_text_capture_seq, &st)) {
        start_cursor_job();
        return 1;
    }
    start_job(has_lookup ? "result was for an earlier screen" : "the running job was OCR only", 1);
    return 1;
}

/* The network thread finished the job. */
static void on_cursor_done(void)
{
    VjoState st = {.size = sizeof(st)};
    int same_screen = vjoGetState(&st) >= 0 && shows_screen(job_capture_scene, job_capture_seq, &st);
    job_running = job_done = job_text_ready = 0;
    sceKernelLockMutex(cmd_lock, 1, NULL);
    int current = job_generation == game_generation &&
                local_text_valid && job_text_generation == local_text_generation &&
                same_pipeline(&job_cfg, &cfg) && same_screen && ov != OV_CLOSED;
    int valid = current && !job_cancelled;
    int newer = valid && (job_cursor_seq != local_cursor_seq ||
                          job_cfg.local_dictionary_results != cfg.local_dictionary_results);
    if (valid && !newer) {
        active = 0;
        cache_ok = cache_data[0].err.rc == VJO_OK;
        cache_dictionary = job_cfg.dictionary;
        cache_scene = job_capture_scene;
        cache_seq = job_capture_seq;
        local_page = cache_data[0].page; /* a missing page falls back to the best match */
        backoff_note(&dict_backoff, cache_data[0].err.rc != VJO_OK);
        view_show_cache();
    }
    job_cancelled = 0;
    sceKernelUnlockMutex(cmd_lock, 1);
    if (!same_screen && local_text_valid) {
        retire_local_text(1);
        if (ov != OV_CLOSED)
            view_publish(1, NULL, "The dialogue changed. Close and reopen the overlay.", 1);
    }
    /* A close cancels the old result; a reopen may already be waiting for
     * another lookup of this still-current saved text. */
    if (newer || (current && cursor_waiting)) start_cursor_job();
    else cursor_waiting = 0;
}

static void on_job_done(void)
{
    if (job_cursor_only) { on_cursor_done(); return; }
    int idx = job_idx, ocr_failed;
    VjoOverlayData *d = &cache_data[idx];
    __sync_synchronize(); /* pairs with the network thread's barrier */
    job_running = 0;
    job_done = 0;
    job_text_ready = 0;
    /* A job that read a screen while it changed is as stale as a cancelled one:
     * the next settled screen gets a new job, and no error is shown. */
    if (job_cancelled || job_generation != game_generation || !same_pipeline(&job_cfg, &cfg) ||
        d->err.rc == VJO_E_SCREEN_CHANGED) {
        if (d->err.rc == VJO_E_SCREEN_CHANGED)
            vjo_log("job dropped: the screen changed while the game read it");
        cache_ok = 0;
        job_cancelled = 0;
        if (ov != OV_CLOSED && game_active()) { /* the overlay waits for this screen's */
            ov = OV_OPEN;
            start_job("the screen changed during the last one", 1);
        } else {
            stable_pending = game_active(); /* only the current game's next screen */
            if (subtitles)
                vjo_strip_set_busy(0);
        }
        return;
    }
    ocr_failed = d->failed_stage == VJO_STAGE_OCR;
    backoff_note(&ocr_backoff, ocr_failed);
    if (subtitles)
        strip_show_result(d);
    if (job_same_text) {
        /* The cached result's text: that result (its arena stays active)
         * is this screen's. A region change meanwhile cleared cache_ok, but
         * the lookup depends on the text only. */
        vjo_log("same text as the cached result: kept for this screen");
        vjo_view_lock();
        cache_ok = 1;
        cache_scene = job_capture_scene;
        cache_seq = job_capture_seq;
        vjo_view_unlock();
        if (ov == OV_OPEN && !job_lookup)
            return; /* OCR only: the overlay already shows a result */
        if (!restart_old_job(1) && ov != OV_CLOSED)
            view_show_cache();
        return;
    }
    if (!ocr_failed && job_lookup)
        backoff_note(&dict_backoff, d->err.rc != VJO_OK);
    /* Character moves received while initial lookup was working supersede
     * that definition; the saved OCR sentence is already immutable. So does
     * a local_dictionary_results change loaded by the overlay's opening. */
    if (local_pipeline(&job_cfg) && local_text_valid && ov != OV_CLOSED &&
        (cursor_waiting || (job_lookup && (job_cursor_seq != local_cursor_seq ||
                                           job_cfg.local_dictionary_results != cfg.local_dictionary_results)))) {
        start_cursor_job();
        return;
    }
    if (!job_lookup && ov == OV_OPEN) {
        /* OCR only, while the overlay shows a result or an error: that stays
         * (and its arena stays active); the strip took the sentence */
        return;
    }
    /* Publish the new arena; the old one becomes the next job's target. */
    int local = local_pipeline(&job_cfg);
    if (local) sceKernelLockMutex(cmd_lock, 1, NULL);
    if (local && ov != OV_CLOSED && job_lookup && job_cursor_seq != local_cursor_seq) {
        sceKernelUnlockMutex(cmd_lock, 1);
        start_cursor_job();
        return;
    }
    vjo_view_lock();
    active = idx;
    /* The overlay needs the lookup, made with the current page count. */
    cache_ok = d->err.rc == VJO_OK && job_lookup &&
               (!local || job_cfg.local_dictionary_results == cfg.local_dictionary_results);
    cache_dictionary = job_cfg.dictionary;
    cache_scene = job_capture_scene;
    cache_seq = job_capture_seq;
    vjo_view_unlock();
    int needs_restart = ov == OV_OPEN_OLD_JOB;
    if (!needs_restart && ov != OV_CLOSED)
        view_show_cache();
    if (local) sceKernelUnlockMutex(cmd_lock, 1);
    /* Restart helpers may take cmd_lock; never call them while holding it. */
    if (needs_restart && !restart_old_job(job_lookup) && ov != OV_CLOSED) {
        if (local) sceKernelLockMutex(cmd_lock, 1, NULL);
        int newer = local && job_lookup && job_cursor_seq != local_cursor_seq;
        if (!newer) view_show_cache();
        if (local) sceKernelUnlockMutex(cmd_lock, 1);
        if (newer) start_cursor_job();
    }
}

/* Does a job started now look the words up? Yes when they are shown: the
 * overlay is open, or opens from the background result (auto: a local
 * dictionary prepares only the first whole-word match), or the subtitles
 * show their readings. Only jpdb and jiten (an API key) give readings: local
 * and Hachidori subtitle jobs stay OCR-only and allocate no result arena. */
static int want_lookup(void)
{
    return vjo_config_dict_ready(&cfg) &&
           (ov != OV_CLOSED || (cfg.ocr_mode == VJO_OCR_AUTO &&
              (!local_pipeline(&cfg) || cfg.highlight_mode == VJO_HIGHLIGHT_WORD)) ||
            (subtitles && vjo_config_api_key(&cfg)[0])) &&
           now_us() >= dict_backoff.until;
}

/* The network thread posted the job's sentence (before its lookup). */
static void on_job_text(void)
{
    const char *sentence;
    __sync_synchronize(); /* pairs with the network thread's barrier */
    sentence = cache_data[job_idx].sentence;
    job_text_ready = 0;
    if (job_cancelled || job_generation != game_generation)
        return; /* for an earlier screen */
    vjo_log("subtitle text ready in %d ms", (int)((now_us() - job_started_us) / 1000));
    if (subtitles && sentence)
        vjo_strip_show(sentence, VJO_STRIP_SENTENCE, 1);
    /* The overlay waits for this job: the sentence now, the words after
     * the lookup (if the job is for the screen shown). */
    if (ov != OV_CLOSED && view_waiting && job_lookup && !job_same_text && sentence) {
        VjoState st;
        st.size = sizeof(st);
        vjoGetState(&st);
        if (shows_screen(job_capture_scene, job_capture_seq, &st)) {
            job_text_list.header = sentence;
            job_text_list.entries = NULL;
            job_text_list.n_entries = 0;
            view_publish(1, &job_text_list, "Looking up words…", 0);
            view_job_text = 1;
        }
    }
}

static void set_subtitles(int on)
{
    if (on) apply_config();
    VjoState st;
    const VjoOverlayData *d = active >= 0 ? &cache_data[active] : NULL;
    subtitles = on;
    vjo_strip_configure(on, &cfg);
    vjo_log("subtitles %s", on ? "on" : "off");
    if (!on) {
        vjo_strip_show("", VJO_STRIP_SENTENCE, 0);
        return;
    }
    if (!net_busy() && !local_pipeline(&cfg) && mem_alloc() < 0) {
        vjo_strip_show("Not enough memory for subtitles", VJO_STRIP_ERROR, 0);
        return;
    }
    /* settings were re-read before choosing the backend */
    st.size = sizeof(st);
    vjoGetState(&st);
    if (st.alloc_status == VJO_ALLOC_FAIL) {
        vjo_strip_show("Not enough memory to capture the screen", VJO_STRIP_ERROR, 0);
        return;
    }
    if (!job_running && d && d->sentence && shows_screen(cache_scene, cache_seq, &st)) {
        vjo_strip_show(d->sentence, VJO_STRIP_SENTENCE, 0);
        if (cache_ok)
            vjo_strip_show_readings(&scratch, &d->list);
        return;
    }
    vjo_strip_show("Recognizing…", VJO_STRIP_STATUS, 1);
    start_job("subtitles on", want_lookup()); /* if one runs, its result comes */
}

/* subtitle_button: from the full overlay, everything closes. */
static void on_subtitle(void)
{
    if (!game_active())
        return;
    if (ov != OV_CLOSED) {
        close_overlay();
        if (subtitles)
            set_subtitles(0);
    } else {
        set_subtitles(!subtitles);
    }
}

/* A command from the overlay (paf main thread). */
static void on_command(void)
{
    int cmd;
    VjoRect r;
    uint32_t cursor_byte, cursor_generation;
    sceKernelLockMutex(cmd_lock, 1, NULL);
    cmd = pending_cmd;
    r = pending_rect;
    cursor_byte = pending_cursor_byte;
    cursor_generation = pending_cursor_generation;
    pending_cmd = VJO_CMD_NONE;
    sceKernelUnlockMutex(cmd_lock, 1);

    switch (cmd) {
    case VJO_CMD_CURSOR:
        if (ov == OV_CLOSED || !local_pipeline(&cfg) || !local_text_valid ||
            cursor_generation != local_text_generation || cursor_byte != local_cursor_byte)
            break;
        if (!job_running || !job_cursor_only || job_cursor_seq != local_cursor_seq)
            start_cursor_job();
        break;
    case VJO_CMD_CLOSED:
        close_overlay();
        break;
    case VJO_CMD_SET_REGION:
    case VJO_CMD_CLEAR_REGION:
        if (!game_active()) { /* the game exited meanwhile */
            close_overlay();
            break;
        }
        if (cmd == VJO_CMD_SET_REGION)
            vjo_log("region set for %s: %u,%u,%u,%u", title_id, r.x, r.y, r.w, r.h);
        else
            vjo_log("region for %s: full screen", title_id);
        if (vjo_region_save(title_id, cmd == VJO_CMD_SET_REGION ? &r : NULL, &scratch) < 0)
            vjo_log("region.ini: write failed");
        push_region();
        cache_ok = 0;
        retire_local_text(1);
        close_overlay();
        if (subtitles)
            start_job("region changed", want_lookup());
        break;
    }
}

/* ---- is the new foreground process a game? ----
 * Its title ID is often not readable right when the process starts, so the
 * lookup is retried for a few seconds. Unknown after that = a game. */
#define CLASSIFY_TIMEOUT_US 3000000

static SceUID pending_pid;
static int64_t pending_since;

static void activate_game(SceUID pid, const char *tid, int mode)
{
    if (!vjo_game_enabled(tid, &scratch)) {
        title_id[0] = '\0';
        vjoSetGameActive(pid, VJO_GAME_NONE);
        vjo_log("%s: JP Overlay disabled; game input passes through", tid);
        vjo_selftest_game(tid, 0, mode, NULL);
        return;
    }
    sceClibSnprintf(title_id, sizeof(title_id), "%s", tid);
    {
        char name[128];
        vjo_title_name(pid, tid, name, sizeof(name)); /* "" for an unreadable title */
        vjo_anki_set_game(name);                      /* Anki's MiscInfo */
    }
    apply_config();
    if (!net_busy() && !local_pipeline(&cfg))
        mem_alloc();
    push_region();
    backoff_note(&ocr_backoff, 0);
    backoff_note(&dict_backoff, 0);
    vjoSetGameActive(pid, mode);
    vjo_log("game %s started: dictionary %s (key %s), trigger %s, subtitles %s, ocr_mode %s", title_id,
            vjo_dict_name(cfg.dictionary), vjo_config_api_key(&cfg)[0] ? "set" : "MISSING",
            vjo_trigger_name(cfg.toggle_button), vjo_trigger_name(cfg.subtitle_button),
            cfg.ocr_mode == VJO_OCR_AUTO ? "auto" : "on_press");
    vjo_ledger_shell_log("start");
    vjo_selftest_game(tid, 1, mode, &game_region);
}

static void classify_pending(void)
{
    char tid[32];
    SceUID pid = pending_pid;
    int ret = vjo_title_id(pid, tid, sizeof(tid));
    if (ret == 0) {
        pending_pid = 0;
        int mode = vjo_title_game_mode(tid);
        if (mode == VJO_GAME_NONE) {
            vjo_log("%s: system app, not a game", tid);
            vjoSetGameActive(pid, VJO_GAME_NONE);
        } else {
            activate_game(pid, tid, mode);
        }
    } else if (now_us() - pending_since > CLASSIFY_TIMEOUT_US) {
        pending_pid = 0;
        vjo_log("no title ID for 0x%X (0x%08X): treating it as a game", pid, ret);
        activate_game(pid, "GAME", VJO_GAME);
    }
}

static void invalidate_game_work(void)
{
    if (++game_generation == 0)
        game_generation = 1;
    cache_ok = 0;
    retire_local_text(1);
    stable_pending = 0;
    if (job_running)
        job_cancelled = 1;
    if (net_busy())
        vjo_net_cancel(&net_cancel);
}

static void on_game_exit(void)
{
    invalidate_game_work();
    pending_pid = 0;
    close_overlay();
    if (subtitles)
        set_subtitles(0);
    clear_input_block_error();
    /* Free memory unless the network thread still uses it. */
    if (!net_busy())
        mem_free();
    title_id[0] = '\0';
    vjo_anki_set_game("");
    vjo_ledger_shell_log("exit");
    vjo_selftest_step("exit");
}

/* A new foreground process: classified by classify_pending. */
static void on_game_start(void)
{
    VjoState st;
    invalidate_game_work();
    close_overlay();
    if (subtitles) /* over a game, without its exit: the strip goes */
        set_subtitles(0);
    clear_input_block_error();
    st.size = sizeof(st);
    vjoGetState(&st);
    title_id[0] = '\0';
    pending_pid = st.game_pid;
    pending_since = now_us();
}

static void on_trigger(void)
{
    if (ov != OV_CLOSED)
        close_overlay();
    else if (game_active())
        open_overlay();
}

/* A background job for a settled screen: the auto prefetch (for an instant
 * overlay) and the subtitles' refresh (whatever ocr_mode says). */
static int background_wanted(void)
{
    return subtitles || (cfg.ocr_mode == VJO_OCR_AUTO && (local_pipeline(&cfg) || want_lookup()));
}

static int background_throttled(void)
{
    int64_t t = now_us();
    return t < ocr_backoff.until || (subtitles && t < job_started_us + SUBTITLE_GAP_US);
}

/* A stable event that arrives while busy (or throttled) is kept, so the
 * newest screen is still recognized afterwards. With subtitles, a region
 * that never settles (a video behind the text) is recognized every
 * UNSETTLED_US anyway. */
static void auto_prefetch(void)
{
    VjoState st;
    int unsettled_due;
    if (ov != OV_CLOSED || job_running || !game_active() || !background_wanted() || background_throttled())
        return;
    unsettled_due = subtitles && now_us() - job_started_us >= UNSETTLED_US;
    if (!stable_pending && !unsettled_due)
        return;
    st.size = sizeof(st);
    vjoGetState(&st);
    if (stable_pending) {
        stable_pending = 0;
        if ((st.stable || st.quiet) && !claim_cache_for(&st))
            start_job(st.stable ? "auto: screen settled" : "auto: screen quiet", want_lookup());
    } else if (st.unsettled_ms >= UNSETTLED_US / 1000) {
        start_job("subtitles: screen keeps changing", 0); /* no lookup every 2 s */
    }
}

/* When the region starts changing, the network thread readies the next
 * job's connections (keeping usable ones), so its requests skip the
 * handshakes. */
static void warm_connections(void)
{
    VjoState st;
    int lookup;
    if (!game_active() || !has_result_memory() || net_busy() || now_us() - warm_asked_us < WARM_GAP_US)
        return;
    if (cfg.ocr_backend != VJO_OCR_LENS) /* local OCR opens no Lens connection */
        return;
    lookup = vjo_config_dict_ready(&cfg);
    if (!lookup && !subtitles)
        return;
    st.size = sizeof(st);
    vjoGetState(&st);
    if (!st.unsettled_ms || st.unsettled_ms > WARM_WINDOW_MS)
        return;
    warm_asked_us = now_us();
    warm_dictionary = lookup ? cfg.dictionary : -1;
    vjo_net_cancel_clear(&net_cancel);
    warm_running = 1;
    __sync_synchronize();
    sceKernelSetEventFlag(net_evf, NET_EV_WARM);
}

/* Boot guard: this boot counts as complete once the control thread has run
 * for a minute (core/boot_guard.h). The kernel may answer BUSY while its
 * own uptime is shorter; a few spaced retries cover that. */
#define BOOT_OK_US        (60LL * 1000 * 1000)
#define BOOT_OK_RETRY_US  (5LL * 1000 * 1000)
#define BOOT_OK_ATTEMPTS  12
static int64_t ctl_started_us;
static int boot_ok_attempts; /* -1 once marked (or given up) */

static void boot_guard_ok(void)
{
    static int64_t next_us;
    int64_t now;
    if (boot_ok_attempts < 0)
        return;
    now = now_us();
    if (now - ctl_started_us < BOOT_OK_US || now < next_us)
        return;
    if (vjo_guard_mark(VJO_BOOT_STAGE_OK) == 0) {
        vjo_log("boot guard: ok after a stable minute");
        boot_ok_attempts = -1;
        vjo_selftest_step("ok");
    } else if (++boot_ok_attempts >= BOOT_OK_ATTEMPTS) {
        vjo_log("boot guard: ok not recorded after %d attempts", BOOT_OK_ATTEMPTS);
        boot_ok_attempts = -1;
    } else {
        next_us = now + BOOT_OK_RETRY_US;
    }
}

static int ctl_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    ctl_started_us = now_us();
    boot_ok_attempts = 0;

    /* The order of the handlers within one wakeup is part of the policy
     * (e.g. a trigger and a game start in the same wakeup). */
    while (running) {
        uint32_t bits = 0;
        vjoWaitEvent(VJO_EV_TRIGGER | VJO_EV_SUBTITLE | VJO_EV_REGION_STABLE | VJO_EV_REGION_QUIET |
                         VJO_EV_GAME_START | VJO_EV_GAME_EXIT,
                     &bits, POLL_TIMEOUT_US);

        boot_guard_ok();
        if (job_running) sceKernelPowerTick(0);
        if (bits & VJO_EV_GAME_EXIT)
            on_game_exit();
        if (bits & VJO_EV_GAME_START)
            on_game_start();
        if (pending_pid > 0)
            classify_pending();
        if (bits & VJO_EV_TRIGGER)
            on_trigger();
        if (bits & VJO_EV_SUBTITLE)
            on_subtitle();
        if (pending_cmd != VJO_CMD_NONE)
            on_command();
        if (job_running && job_text_ready)
            on_job_text();
        if (job_running && job_done)
            on_job_done();
        if (bits & (VJO_EV_REGION_STABLE | VJO_EV_REGION_QUIET)) {
            stable_pending = 1;
            if (job_running) {
                VjoState st;
                st.size = sizeof(st);
                vjoGetState(&st);
                cancel_outdated_job(&st);
            }
        }
        auto_prefetch();
        warm_connections();
        vjo_anki_reap(); /* an Anki job's thread and stack go once it is done */
        if (!game_active() && !net_busy() && has_result_memory() && ov == OV_CLOSED)
            mem_free();
    }
    return 0;
}

int vjo_worker_start(void)
{
    int ver = vjoGetVersion(), rc;
    vjo_ledger_shell_init();
    if (ver != VJO_API_VERSION) {
        vjo_log("kernel API version %d, expected %d: not starting", ver, VJO_API_VERSION);
        return -1;
    }
    rc = vjoRegisterShell();
    if (rc < 0) {
        vjo_log("kernel refused the shell registration %d: not starting", rc);
        return -1;
    }
    view_lock = sceKernelCreateMutex("VjoView", 0, 0, NULL);
    cmd_lock = sceKernelCreateMutex("VjoCmd", 0, 0, NULL);
    net_evf = sceKernelCreateEventFlag("VjoNetEv", 0, 0, NULL);
    if (view_lock < 0 || cmd_lock < 0 || vjo_capture_init() < 0 || net_evf < 0 || vjo_net_cancel_init(&net_cancel) < 0) {
        vjo_worker_stop();
        return -1;
    }
    vjo_platform_vita(&plat, &net_cancel);
    plat.pool = &pool; /* no slots until mem_alloc */
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    vjo_arena_init(&results[0], NULL, 0);
    vjo_arena_init(&results[1], NULL, 0);
    if (vjo_anki_start() < 0)
        vjo_log("anki: mailbox unavailable: × saves no cards");
    apply_config(); /* logging, triggers and Anki are set up before the first game */
    vjo_selftest_init(&cfg);
    if (vjo_selftest_armed())
        apply_config(); /* the self-test's forced settings */
    vjo_log("Vita JP Overlay shell started (kernel API %d)", ver);

    net_thread = vjo_shell_thread_create(VJO_SL_NET_STACK, "VjoNet", net_main, VJO_STACK_SHELL_NET);
    ctl_thread = vjo_shell_thread_create(VJO_SL_CTL_STACK, "VjoControl", ctl_main, VJO_STACK_SHELL_CTL);
    if (net_thread < 0 || ctl_thread < 0) {
        vjo_worker_stop();
        return -1;
    }
    if (sceKernelStartThread(net_thread, 0, NULL) < 0) {
        vjo_worker_stop();
        return -1;
    }
    threads_started = 1;
    if (sceKernelStartThread(ctl_thread, 0, NULL) < 0) {
        vjo_worker_stop();
        return -1;
    }
    threads_started |= 2;
    vjo_ledger_shell_log("boot");
    return 0;
}

/* Also undoes a partial vjo_worker_start. */
int vjo_worker_stop(void)
{
    running = 0;
    if (net_cancel.lock >= 0)
        vjo_net_cancel(&net_cancel); /* a request in progress ends now */
    if (net_evf >= 0)
        sceKernelSetEventFlag(net_evf, NET_EV_QUIT);
    if (threads_started) {
        if (((threads_started & 1) && sceKernelWaitThreadEnd(net_thread, NULL, NULL) < 0) ||
            ((threads_started & 2) && sceKernelWaitThreadEnd(ctl_thread, NULL, NULL) < 0)) {
            vjo_log("worker join failed; keeping allocations until stop can be retried");
            return -1;
        }
        threads_started = 0;
    }
    if (net_thread >= 0)
        vjo_shell_thread_delete(VJO_SL_NET_STACK, net_thread, VJO_STACK_SHELL_NET);
    if (ctl_thread >= 0)
        vjo_shell_thread_delete(VJO_SL_CTL_STACK, ctl_thread, VJO_STACK_SHELL_CTL);
    net_thread = ctl_thread = -1;
    /* An Anki job uses the view and capture locks: it is cancelled and
     * joined before they are deleted (the control thread no longer reaps it). */
    if (vjo_anki_stop() < 0)
        return -1;
    mem_free();
    if (net_evf >= 0)
        sceKernelDeleteEventFlag(net_evf);
    if (cmd_lock >= 0)
        sceKernelDeleteMutex(cmd_lock);
    vjo_capture_fini();
    if (view_lock >= 0)
        sceKernelDeleteMutex(view_lock);
    if (net_cancel.lock >= 0)
        sceKernelDeleteMutex(net_cancel.lock);
    net_evf = cmd_lock = view_lock = net_cancel.lock = -1;
    return 0;
}
