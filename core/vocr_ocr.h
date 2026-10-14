/* vita-vn-ocr backend: bee-san/vita-vn-ocr's polarity-agnostic dialogue line
 * finder and tiny CTC recognizer (plain C99, no malloc), with the int8 weights
 * published in its v0.2.5-vocr3.1 (VOCR3) and v0.2.0 releases. Built with
 * -DVJO_WITH_VOCR=ON; see docs/vita-vn-ocr.md.
 *
 * API 19: a job never holds the region. It reads the region in strips of grey
 * rows (VjoGreySource: in the game, the display hook copies each strip from the
 * game's framebuffer), finds the lines with the runtime's row-callback finder
 * (vocr_lines_rows.h) and recognizes each line straight from the strip that
 * holds it. Its memory is a workspace (the finder's arena, then the
 * recognizer's, and the runtime's two structs: vjo_vocr_job_bytes) and a
 * capture block (one strip, the rows' checksums: vjo_vocr_capture_bytes, under
 * 64 KiB page-rounded). Every row's checksum is kept the first time it is
 * read and compared whenever it is read again; a row that changed means the
 * screen changed while it was read, and the job starts over (VJO_VOCR_TRIES in
 * all, then VJO_E_SCREEN_CHANGED). A row that goes back to a state it had a
 * little earlier in the job (a blinking or bobbing "next" icon, a looping
 * background) is not a change: each row keeps its recent states. The weights live in a separate caller-owned
 * block (VjoVocrResident) that is read and checked once and then kept for later
 * jobs while the file's size and stamp stay the same; nothing else is
 * allocated. vjo_vocr_ocr() is the one-shot form: weights and job share one
 * workspace (vjo_vocr_workspace_bytes()) and are read for every job.
 *
 * Two optional checks (VjoVocrPolicy; vjo_vocr_ocr_policy): the stable-text
 * wait recognizes nothing until every row of the region has held still for
 * settle_us (text that types out character by character is read once it is
 * whole, never half-typed), and the blank-line check skips a found line whose
 * crop holds no text-like ink (vjo_vocr_ink) instead of recognizing it. */
#ifndef VJO_VOCR_OCR_H
#define VJO_VOCR_OCR_H

#include <stddef.h>
#include <stdint.h>

#include "local_ocr.h" /* VJO_OCR_MAX_WIDTH/HEIGHT, VJO_OCR_TEXT_CAP */
#include "net.h"       /* VjoPlatform.file_open, now_us */

#ifdef __cplusplus
extern "C" {
#endif

#define VJO_VOCR_DEFAULT_MODEL "VOCR3_H15_w8.vocr" /* VJO_VOCR3_H15_NAME (include/vjo_vocr_sizes.h) */

/* The capture block: one strip of at most this many bytes of grey rows (the
 * strip is region width x VJO_VOCR_STRIP_ROWS(width) rows), plus per region
 * row its checksum, its earlier states (16-bit, the latest first) and two
 * bits; as many earlier states as fit within VJO_VOCR_CAPTURE_LIMIT, 2 to
 * VJO_VOCR_STATES_MAX (vjo_vocr_state_slots: 16 for a dialogue box, 6 at
 * 960x544, 5 at 896x544, the widest strip). */
#define VJO_VOCR_STRIP_BYTES (56u * 1024u)
#define VJO_VOCR_STRIP_MAX_ROWS 64u /* the kernel copies at most this many rows per call */
#define VJO_VOCR_TRIES 3u           /* captures of one job before VJO_E_SCREEN_CHANGED */
#define VJO_VOCR_CAPTURE_LIMIT 65536u /* include/vjo_ledger_budget.h VJO_BUDGET_GAME_CAPTURE */
#define VJO_VOCR_STATES_MAX 16u
/* With the stable-text wait on, a row that goes back to one of its recent
 * states (an animated icon) is not a change, in this many rows of a job at
 * most: a page of new text, or a box of several lines clearing back to what
 * the job saw before, changes more. A one- or two-line box that clears back to
 * an empty box the same job saw earlier, while the job reads it, can be taken
 * for an icon (the job's text then misses those lines; it is tagged with the
 * screen it read, which is no longer shown). Not learning such rows once the
 * job reads would close that, at a retry whenever a "next" icon that blinks
 * slower than the wait goes back while the job reads (docs/vita-vn-ocr.md).
 * Without the wait every change is one, as in API 19. */
#define VJO_VOCR_CYCLE_ROWS 64u
/* The wait reads at most this many strips a job, whatever the clock says (a
 * real job reads a few hundred at most: its waits end by settle_max_us). */
#define VJO_VOCR_SETTLE_READS 4096u
/* A line taller than a strip is read through the strip and kept whole, its own
 * columns only, in VjoGreySource.line_buffer: line width x line height bytes, up
 * to this many (include/vjo_ledger_budget.h VJO_BUDGET_GAME_LINE), so the
 * recognizer gets the same input as from a copy of the whole region. Dialogue
 * lines are 20-53 px on the shared eval screenshots, inside one strip; the
 * finder can report a far taller band (lines it could not separate, a picture),
 * up to the region: 41 of 372 linefind dev regions have one, the largest
 * 533 x 148 = 78,884 B. A band of more than this many bytes is left out and
 * counted (tall_skipped). */
#define VJO_VOCR_LINE_MAX_BYTES 516096u

/* A region read in strips of grey rows: strip(ud, y, n, dst, stride, flags)
 * fills region rows [y, y + n) as (77 R + 150 G + 29 B) >> 8, width bytes each,
 * stride apart, and returns n, or a negative VJO_E_* / native error (the job
 * then fails with it; VJO_E_CANCELLED for a cancelled one). flags:
 * VJO_STRIP_FIRST when the rows continue the job's first pass from row 0, in
 * order, and VJO_STRIP_LAST on the one that ends it. line_buffer(ud, bytes),
 * optional, gives memory for one line taller than a strip (valid until the next
 * call or the end of the job), or NULL: that line is then left out. */
#define VJO_STRIP_FIRST 0x1u
#define VJO_STRIP_LAST 0x2u
typedef struct {
    void *ud;
    unsigned width, height;
    int (*strip)(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags);
    int (*cancelled)(void *ud); /* optional; checked between strips and lines */
    void *(*line_buffer)(void *ud, size_t bytes);
} VjoGreySource;

/* A weights file this build accepts: exact size and SHA-256 of the release
 * asset, and the recognizer's arena (W8I mode, default tiling). */
typedef struct {
    const char *name;
    uint32_t bytes;
    unsigned char sha256[32];
    uint32_t arena_bytes;
    unsigned finder_mode;     /* vocr_lines.h: GRAD for old models, TEXTBOX for VOCR2 */
} VjoVocrModel;

/* The model named `name` (a file name in ocr_model_dir), or NULL. */
const VjoVocrModel *vjo_vocr_model(const char *name);
/* The supported models, for messages and tools; *count is set. */
const VjoVocrModel *vjo_vocr_models(unsigned *count);

/* The line finder returns at most this many lines (vocr_lines.h VOCR_LINES_MAX). */
#define VJO_VOCR_MAX_LINES 16u

/* One recognized line: its crop, the CTC frames the recognizer ran, and the
 * time it took, so a console log gives ms per frame for each line. */
typedef struct {
    uint16_t width, height;  /* crop in region pixels */
    uint16_t frames;         /* CTC frames (the resized width / 8) */
    uint16_t reserved;       /* kept by the line cache with the line's text (VjoVocrCache) */
    uint32_t us;             /* vocr_run time with VjoPlatform.now_us, else 0 (wall time, start to end) */
    /* ocr-speed */
    uint8_t slot;            /* who recognized it: 0 the caller's thread, 1.. a helper (VjoVocrPool) */
    uint8_t cached;          /* 1: its pixels were unchanged since a job read them (VjoVocrCache): no vocr_run */
    uint16_t pad;
    uint32_t cpu_us;         /* the recognizing thread's run time for it (VjoVocrPool.thread_us), else 0 */
} VjoVocrLineStat;

/* A found line's ink (vjo_vocr_ink): its contrast (the 98th minus the 2nd
 * percentile grey) and its edges: the pixels from which the grey level changes
 * by at least `edge` = max(VJO_VOCR_EDGE_MIN, contrast / VJO_VOCR_EDGE_DIV)
 * within VJO_VOCR_INK_SPAN pixels to the right, and below (at most two a pixel),
 * per 10,000 pixels. Soft edges count: text whose strokes rise over two pixels
 * (a PSP game's 480x272 frame shown 2x with bilinear filtering, a game rendering
 * under 960x544, a blurred font) is strokes too. On the linefind development
 * lines no text line has fewer than 1,553, and in seven soft render paths of the
 * same screenshots (docs/vita-vn-ocr.md) none has fewer than
 * VJO_VOCR_BLANK_EDGES. A crop under that holds no text-like ink: an even area,
 * a smooth part of a picture, the box's gradient, strokes under 24 levels of
 * contrast. A lone small mark ("。") is too few pixels to move the
 * percentiles, but its edges count. */
typedef struct {
    uint8_t lo, hi;          /* 2nd and 98th percentile grey */
    uint8_t edge;            /* the step an edge needs */
    uint8_t reserved;
    uint16_t edges;          /* edges per 10,000 pixels */
    uint16_t reserved2;
} VjoVocrInk;
#define VJO_VOCR_EDGE_MIN 24u
#define VJO_VOCR_EDGE_DIV 4u
#define VJO_VOCR_INK_SPAN 2u
#define VJO_VOCR_BLANK_EDGES 50u
/* Crops of more pixels are never called blank (no line is this large: a strip
 * holds at most 64 rows of 960, a tall line VJO_VOCR_LINE_MAX_BYTES). */
#define VJO_VOCR_INK_PIXELS 4194304u

/* A line the blank-line check skipped, for the log. */
typedef struct {
    uint16_t index;          /* its place among the found lines */
    uint16_t width, height;
    uint16_t x, y;
    VjoVocrInk ink;
} VjoVocrBlankStat;
#define VJO_VOCR_BLANK_LOGGED 4u

typedef struct {
    size_t workspace;        /* bytes of this job's workspace (see vjo_vocr_ocr*) */
    size_t capture;          /* bytes of its capture block (vjo_vocr_capture_bytes) */
    size_t model_bytes;      /* weights, used in place */
    size_t scratch_bytes;    /* the finder's arena, then the recognizer's */
    unsigned width, height;  /* region */
    unsigned strip_rows;     /* rows per strip */
    unsigned strips;         /* strips read (in the game: one frame each) */
    unsigned tears;          /* rows that read differently on a later read */
    unsigned tries;          /* captures of the region (1 unless the screen changed) */
    unsigned tall;           /* lines taller than a strip read into line_buffer (or left out: tall_skipped) */
    unsigned tall_skipped;
    size_t line_bytes;       /* the largest line_buffer asked for (a tall line's width x height), 0 none */
    uint32_t region_sum;     /* FNV-1a of the region's grey rows, first pass of the last try */
    uint32_t capture_us;     /* time inside strip() */
    unsigned lines;          /* lines found */
    unsigned dark;           /* of those, dark text (inverted before recognition) */
    unsigned truncated;      /* more lines than the finder returns (16) */
    unsigned frames;         /* CTC frames recognized, all lines */
    unsigned model_loaded;   /* 1: the weights were read and verified in this job */
    unsigned model_cached;   /* 1: resident verified weights were reused (file unchanged) */
    uint32_t find_us, load_us, recognize_us; /* with VjoPlatform.now_us, else 0 */
    unsigned recognized;     /* entries of line[] filled: lines vocr_run finished */
    VjoVocrLineStat line[VJO_VOCR_MAX_LINES];
    /* The stable-text wait (VjoVocrPolicy.settle_us). */
    uint32_t settle_us;      /* time spent on passes read only to see the rows hold still */
    unsigned settle_passes;  /* those passes */
    unsigned settle_changes; /* row changes to a new state they saw (text being typed) */
    unsigned cycles;         /* rows that went back to their previous state (an icon), all reads */
    unsigned cycling_rows;   /* distinct such rows */
    unsigned settled;        /* 1: the rows held still for settle_us before the lines were read */
    unsigned settle_timeout; /* 1: settle_max_us ran out first, so the job went on without it */
    /* The blank-line check (VjoVocrPolicy.blank). */
    unsigned blank;          /* lines skipped: no text-like ink in their crop */
    uint32_t blank_us;       /* time spent checking */
    VjoVocrBlankStat blank_line[VJO_VOCR_BLANK_LOGGED]; /* the first ones skipped */
    /* Parallel lines and the line cache (ocr-speed). */
    unsigned helpers;        /* helper threads the job could use (VjoVocrPool.helpers, 0 serial) */
    unsigned dispatched;     /* lines a helper recognized */
    unsigned run_failures;   /* helpers that could not start: their line ran on the caller's thread */
    unsigned cached;         /* lines whose text came from the cache */
    uint32_t key_us;         /* time spent hashing lines for the cache */
    unsigned stuck;          /* 1: a helper could not be joined: it may still run in the workspace */
} VjoVocrStats;

/* Each found line, for host tools (VjoVocrPolicy.line): valid during the call. */
typedef struct {
    unsigned index, x, y, w, h, dark;
    const unsigned char *pixels;  /* the crop's grey rows, stride apart */
    size_t stride;
    VjoVocrInk ink;
    int blank;                    /* skipped by the blank-line check */
    const char *text;             /* what the recognizer read ("" when skipped) */
    uint32_t us;                  /* its recognition time */
} VjoVocrLineInfo;

/* What a job does besides finding and recognizing the lines; a zeroed policy
 * (or NULL) is API 19's job exactly.
 *
 * settle_us: before any line is recognized, every row of the region has to
 * read the same (an icon toggling between two states aside) on passes that
 * span at least this long, so a line still typing out is never read half
 * done. A job whose screen was already still when it was asked reads its
 * first pass and finds its lines as before, then re-reads the region until the
 * span is long enough; with settle_first, or once the screen changed under a
 * job, it waits first, re-reading the region without finding lines, and takes
 * its first pass (the capture the Shell is told of) only once the rows hold
 * still. settle_max_us bounds the time spent waiting over the whole job (0:
 * VJO_VOCR_SETTLE_MAX_US): a region that never holds still (a video behind the
 * text) is then read as before. blank: a found line whose crop has no
 * text-like ink is skipped (vjo_vocr_blank; blank_edges, if set, in place of
 * VJO_VOCR_BLANK_EDGES, for tools and tests). sleep, optional: once the region
 * has been re-read without a change, the job sleeps until a re-read can be the
 * last one instead of reading passes that are too early to count. */
/* Parallel lines (ocr-speed): the lines of a region are independent, so a job
 * may recognize up to 1 + helpers of them at once, each on its own vocr_ctx and
 * arena over the one shared, read-only model. The caller's thread keeps all the
 * rest in line order: each line's fetch from the strip and every row check, the
 * blank-line check, the cache, the stats and the text; a line handed to a
 * helper is first copied out of the strip (the next fetch rewrites it), and a
 * line runs on the caller's thread when no helper is free. The text, the
 * frames and every line's result are exactly the serial job's: vocr_run is
 * deterministic and shares nothing but the read-only weights. run(ud, slot,
 * fn, arg) runs fn(arg) on another thread (slot 1..helpers, at most one at a
 * time per slot) and returns 0, or < 0 when it cannot (that line then runs on
 * the caller's thread). join(ud, slot) returns 0 once that fn has returned, or
 * < 0 when it cannot tell: the job then fails with VJO_E_OCR_UNAVAILABLE and the
 * workspace must stay allocated (the helper may still be running in it).
 * thread_us, optional: the calling thread's run time so far in microseconds
 * (each line's cpu_us). A per-line callback (VjoVocrPolicy.line) keeps the job
 * serial. The workspace grows by vjo_vocr_helper_bytes() per helper. */
typedef struct {
    unsigned helpers;        /* 0..VJO_VOCR_HELPERS_MAX */
    int (*run)(void *ud, unsigned slot, void (*fn)(void *arg), void *arg);
    int (*join)(void *ud, unsigned slot);
    uint64_t (*thread_us)(void *ud);
    void *ud;
} VjoVocrPool;
#define VJO_VOCR_HELPERS_MAX 2u

/* The line cache (ocr-speed): a line whose grey pixels, size, polarity and model
 * are those of a line recognized before, under the same VjoVocrPolicy.cache_variant,
 * gets that line's text (and its VjoVocrLineStat.reserved) without running the
 * recognizer (vocr_run is deterministic, so it is exactly the text it would give).
 * The key is a 64-bit FNV-1a of the crop's rows, its size, its polarity and the
 * variant. cache_variant names everything else that decides a line's shown text
 * (a decoder, a re-reading cascade and their settings): jobs that differ in it
 * share no entry. Caller-owned, zero-initialized, kept between jobs; entries of
 * another model never match, and any job that reads the weights from the file
 * again empties it. A job adds its recognized lines once it has succeeded. */
#define VJO_VOCR_CACHE_LINES 8u
#define VJO_VOCR_CACHE_TEXT 368u /* longer line texts are not kept */
typedef struct {
    uint64_t key;            /* 0: empty */
    const VjoVocrModel *model;
    uint32_t used;           /* the cache's tick when last used (least recently used goes first) */
    uint16_t frames, len;
    uint16_t line_flags;     /* the line's VjoVocrLineStat.reserved */
    uint16_t pad;
    char text[VJO_VOCR_CACHE_TEXT];
} VjoVocrCacheLine;
typedef struct {
    VjoVocrCacheLine line[VJO_VOCR_CACHE_LINES];
    uint32_t tick, hits, misses;
} VjoVocrCache;

typedef struct {
    uint32_t settle_us, settle_max_us;
    int settle_first;
    int blank;
    unsigned blank_edges;
    void (*line)(void *ud, const VjoVocrLineInfo *info);
    void *ud;
    void (*sleep)(uint32_t us);
    /* ocr-speed: NULL / 0 is API 20's serial job. */
    const VjoVocrPool *pool;
    VjoVocrCache *cache;
    uint32_t cache_variant;   /* see VjoVocrCache */
    /* busy(ud, 1) just before the first line is recognized, busy(ud, 0) once the
     * last one is (helpers joined); never for a job whose lines all come from the
     * cache or are skipped. The game worker raises the ARM clock in between, when
     * the job asks for it (ocr_boost_clock). */
    void (*busy)(void *ud, int on);
    void *busy_ud;
} VjoVocrPolicy;
#define VJO_VOCR_SETTLE_MAX_US 2000000u

/* The ink of a w x h crop of grey rows, stride apart (either polarity).
 * Returns 1 when it holds no text-like ink (vjo_vocr_blank), else 0. */
int vjo_vocr_ink(const unsigned char *pixels, size_t stride, unsigned w, unsigned h, VjoVocrInk *out);
int vjo_vocr_blank(const VjoVocrInk *ink);

/* Weights kept between jobs. The caller supplies their memory: alloc returns
 * 16-byte aligned memory for `bytes`, or NULL (the job then fails with
 * VJO_E_OOM); release frees it and returns 0, or nonzero if it could not (the
 * memory then stays held but is never used as weights). Zero-initialize the
 * state fields; vocr_ocr.c maintains them. Between jobs, data is non-NULL
 * exactly when model is: memory is held only for verified weights. */
typedef struct {
    void *(*alloc)(void *ud, size_t bytes);
    int (*release)(void *ud);
    void *ud;
    const VjoVocrModel *model;  /* verified weights of this model are in data, else NULL */
    unsigned char *data;        /* the memory held, or NULL */
    size_t capacity;            /* bytes of data */
    uint64_t size, stamp;       /* VjoFile.size and .stamp of the file they were read from */
} VjoVocrResident;

/* Releases the resident weights. 0, or the release callback's failure (the
 * memory is then still held, and model is NULL). */
int vjo_vocr_resident_drop(VjoVocrResident *resident);

/* Exact workspace bytes for a job on a width x height region with `model`:
 * vjo_vocr_job_bytes() without the weights (resident form), or
 * vjo_vocr_workspace_bytes() with them (one-shot form); 0 for an unsupported
 * model or region. Neither includes the capture block. */
size_t vjo_vocr_job_bytes(const char *model, unsigned width, unsigned height);
size_t vjo_vocr_workspace_bytes(const char *model, unsigned width, unsigned height);
/* What each helper (VjoVocrPool) adds to a job's workspace with `model`: its
 * recognizer arena and context, a line's pixels (a line inside a strip is at
 * most VJO_VOCR_STRIP_BYTES) and its text (VJO_OCR_TEXT_CAP); plus, once per
 * job with any helper, the caller's own line text (VJO_OCR_TEXT_CAP). 0 for an
 * unsupported model. vjo_vocr_job_bytes_helpers() is the whole workspace. */
size_t vjo_vocr_helper_bytes(const char *model);
size_t vjo_vocr_job_bytes_helpers(const char *model, unsigned width, unsigned height, unsigned helpers);
/* Rows per strip for a region of this size, and the capture block's exact
 * bytes (0 for an unsupported region), never more than VJO_VOCR_CAPTURE_LIMIT;
 * the recent states it keeps per row. */
unsigned vjo_vocr_strip_rows(unsigned width, unsigned height);
size_t vjo_vocr_capture_bytes(unsigned width, unsigned height);
unsigned vjo_vocr_state_slots(unsigned width, unsigned height);

/* Recognizes the region: lines top to bottom, separated by '\n', in text (at
 * most text_cap bytes with the NUL). workspace: 16-byte aligned, private to
 * the call, at least vjo_vocr_job_bytes(). capture: at least
 * vjo_vocr_capture_bytes(), any alignment.
 *
 * With at least one line, the model file is opened. Weights already resident
 * for this model whose file still reports the same size and a nonzero, equal
 * stamp are used as they are (stats->model_cached); otherwise the file is read
 * into the resident memory (allocated if needed, the old model's released
 * first) and checked against the pinned size and SHA-256 (stats->model_loaded).
 * A missing, wrong or unreadable file, or any failure while loading, releases
 * the resident weights; cancellation during the check keeps verified ones.
 *
 * Returns VJO_OK (also with no line: text "" and no file opened),
 * VJO_E_OCR_MODEL (file missing, wrong size or hash, or refused by the
 * runtime), VJO_E_OOM (workspace or capture too small, or no memory for the
 * weights), VJO_E_OCR_REGION, the source's error (VJO_E_SOURCE when it gave no
 * code), VJO_E_SCREEN_CHANGED, VJO_E_CANCELLED, VJO_E_TOO_LARGE (text),
 * VJO_E_OCR_INFERENCE, or VJO_E_OCR_UNAVAILABLE (resident memory could not be
 * released). text is "" on error. */
int vjo_vocr_ocr_resident(const VjoPlatform *platform, const char *model_dir, const char *model,
                          const VjoGreySource *image, VjoVocrResident *resident,
                          void *workspace, size_t workspace_size, void *capture, size_t capture_size,
                          char *text, size_t text_cap, VjoVocrStats *stats);
/* The same with a policy (NULL: as vjo_vocr_ocr_resident). */
int vjo_vocr_ocr_policy(const VjoPlatform *platform, const char *model_dir, const char *model,
                        const VjoGreySource *image, VjoVocrResident *resident,
                        void *workspace, size_t workspace_size, void *capture, size_t capture_size,
                        const VjoVocrPolicy *policy, char *text, size_t text_cap, VjoVocrStats *stats);

/* The one-shot form: the weights are read into the front of workspace (at
 * least vjo_vocr_workspace_bytes()) for this job only, as the host tools do. */
int vjo_vocr_ocr(const VjoPlatform *platform, const char *model_dir, const char *model,
                 const VjoGreySource *image, void *workspace, size_t workspace_size,
                 void *capture, size_t capture_size, char *text, size_t text_cap, VjoVocrStats *stats);

/* An RGBA image in memory (R, G, B, A bytes) as a VjoGreySource, for the host
 * tools and tests. */
typedef struct {
    const unsigned char *pixels;
    unsigned width, height;
    size_t stride;
} VjoRgbaImage;
void vjo_grey_source_rgba(const VjoRgbaImage *img, VjoGreySource *out);

#ifdef __cplusplus
}
#endif
#endif
