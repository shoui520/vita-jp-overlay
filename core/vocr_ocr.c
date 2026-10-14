/* vita-vn-ocr backend (vocr_ocr.h): region strips -> vocr_find_lines_rows ->
 * verified weights (resident between jobs, or one-shot) -> vocr_run per line,
 * straight from the strip that holds it. No copy of the whole region is made.
 * The runtime (vocr.c, vocr_lines.c, vocr_lines_rows.c) is bee-san/vita-vn-ocr's,
 * at the commit cmake/VocrOCR.cmake pins. */
#include "vocr_ocr.h"
#include "../include/vjo_vocr_sizes.h"

#include <bearssl_hash.h>
#include <string.h>

#include "fbgrey.h"
#include "vocr.h"
#include "vocr_lines.h"
#include "vocr_lines_rows.h"

typedef char vjo_vocr_max_lines_matches_runtime[VJO_VOCR_MAX_LINES == VOCR_LINES_MAX ? 1 : -1];
/* The int8 assets of github.com/bee-san/vita-vn-ocr releases v0.2.5-vocr3.1
 * (VOCR3, the default and its fallback) and v0.2.0 (docs/vita-vn-ocr.md).
 * Arena bytes: vocr_arena_bytes() in W8I mode with the default 4-frame tiles
 * and 16-frame head batches, any line width. */
#ifdef VJO_VOCR_TEST_MODELS
/* Host lifecycle tests only (host/tests/test_game_resident.c): synthetic pins. */
#include VJO_VOCR_TEST_MODELS
#else
static const VjoVocrModel models[] = {
    /* VOCR3 (vita-vn-ocr #67): H15's and FL10's shapes retrained, the same sizes and arenas. */
    {VJO_VOCR3_H15_NAME, VJO_VOCR_H15_BYTES,
     {0xcb,0x3c,0x61,0xa8,0xf9,0x1f,0x9a,0xee,0xb3,0xef,0xcc,0x0e,0x79,0x31,0x3f,0x66,
      0x41,0x12,0xf2,0x5c,0x4b,0xd5,0x58,0x65,0xba,0xe9,0xdf,0x52,0x52,0x2b,0x36,0x0c},
     182944, VOCR_LINES_GRAD},
    {VJO_VOCR3_FL10_NAME, VJO_VOCR_FL10_BYTES,
     {0x87,0xfb,0x4d,0x0c,0x18,0x41,0x6f,0x5f,0x37,0x71,0xce,0xcd,0x27,0x4c,0x8d,0xb3,
      0xcf,0x69,0xb8,0xb4,0xa4,0xee,0x9f,0x20,0x4e,0xde,0xbb,0x4b,0x82,0x07,0x91,0x54},
     124832, VOCR_LINES_GRAD},
    /* v0.2.0, the earlier defaults: kept as a fallback (README 'Settings'). */
    {"H15_w8.vocr", VJO_VOCR_H15_BYTES,
     {0x31,0x6e,0xbc,0x37,0x7e,0xc3,0x45,0x20,0x44,0x09,0x91,0xc5,0x08,0x51,0x99,0xd8,
      0xe7,0x40,0x31,0xf0,0x1a,0x86,0x79,0x38,0xbe,0x90,0xa6,0x0c,0x1e,0x3f,0x03,0xcd},
     182944, VOCR_LINES_GRAD},
    {"FL10_w8.vocr", VJO_VOCR_FL10_BYTES,
     {0x50,0x0e,0x0b,0x84,0x2e,0x5f,0x0c,0xb3,0x26,0xf5,0x5e,0x7d,0x99,0x1a,0x52,0xab,
      0x66,0xdd,0x24,0x97,0x7c,0xcc,0x89,0xbd,0x18,0x82,0x28,0xae,0xdc,0x82,0x5e,0x84},
     124832, VOCR_LINES_GRAD},
    {"F20_w8.vocr", VJO_VOCR_F20_BYTES,
     {0xf8,0xc1,0xab,0xe4,0x74,0x3a,0x71,0xa7,0x42,0xe7,0x18,0x52,0x3f,0x42,0x06,0xf9,
      0x92,0xd3,0xbd,0x4f,0xb6,0x9d,0xe3,0x21,0x99,0x7b,0x2b,0xd6,0xf1,0x67,0x79,0xc0},
     241056, VOCR_LINES_GRAD},
#if VJO_VOCR2_ENABLED
    /* Experimental trained FL10-sized recognizer and native textbox finder.
     * The strict per-title guard failed; this backend is explicitly opt-in. */
    {VJO_VOCR2_ARTIFACT_NAME, VJO_VOCR2_BYTES,
     {0xd3,0xad,0xc5,0x2e,0x5c,0xf6,0x2b,0x54,0x8a,0xf0,0x4c,0x5a,0xb0,0x7a,0x2f,0x57,
      0xcf,0x04,0x19,0x38,0x6b,0xbd,0xf4,0x9b,0xa1,0x9c,0x97,0x03,0xbe,0x6e,0x17,0x66},
     124832, VOCR_LINES_TEXTBOX},
#endif
};
#endif /* VJO_VOCR_TEST_MODELS */
#define N_MODELS (sizeof(models) / sizeof(models[0]))

#define ALIGN16(x) (((x) + (size_t)15) & ~(size_t)15)
/* Weights are read and hashed in pieces of this size: the game worker's kernel
 * broker stages exactly one piece per request (VJO_GAME_OCR_MODEL_CHUNK). */
#define READ_CHUNK 32768u

const VjoVocrModel *vjo_vocr_models(unsigned *count)
{
    if (count) *count = (unsigned)N_MODELS;
    return models;
}

const VjoVocrModel *vjo_vocr_model(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < N_MODELS; i++)
        if (!strcmp(name, models[i].name)) return &models[i];
    return NULL;
}

/* Job workspace: [finder, then recognizer arena | vocr_model, vocr_ctx], and with
 * helpers (VjoVocrPool) [the caller's line text | per helper: arena, vocr_ctx,
 * line pixels, line text]. The one-shot form puts the weights in front of it.
 * The region's pixels are not in it: they live one strip at a time in the
 * capture block. */
typedef struct {
    size_t scratch_off, structs_off, total;
    size_t scratch, finder;
    unsigned helpers;        /* helper slots laid out */
    size_t text0_off, helper_off, helper_bytes;
} Layout;

/* A helper's part of the workspace: [arena | vocr_ctx | pixels | text]. */
#define H_ARENA(m) ALIGN16((size_t)(m)->arena_bytes)
#define H_CTX ALIGN16(sizeof(vocr_ctx))
#define H_PIXELS ALIGN16((size_t)VJO_VOCR_STRIP_BYTES)
#define H_TEXT ALIGN16((size_t)VJO_OCR_TEXT_CAP)

static int region_ok(unsigned w, unsigned h)
{
    return w && h && w <= VJO_OCR_MAX_WIDTH && h <= VJO_OCR_MAX_HEIGHT;
}

static int layout_helpers(const VjoVocrModel *m, unsigned w, unsigned h, unsigned helpers, Layout *l)
{
    if (!m || !region_ok(w, h) || helpers > VJO_VOCR_HELPERS_MAX) return -1;
    l->finder = vocr_lines_arena_bytes(w, h);
    l->scratch = ALIGN16(l->finder > m->arena_bytes ? l->finder : m->arena_bytes);
    l->scratch_off = 0;
    l->structs_off = l->scratch;
    l->total = l->structs_off + ALIGN16(sizeof(vocr_model)) + ALIGN16(sizeof(vocr_ctx));
    l->helpers = helpers;
    l->helper_bytes = H_ARENA(m) + H_CTX + H_PIXELS + H_TEXT;
    l->text0_off = l->total;
    l->helper_off = l->text0_off + H_TEXT;
    if (helpers) l->total = l->helper_off + (size_t)helpers * l->helper_bytes;
    return 0;
}

static int layout(const VjoVocrModel *m, unsigned w, unsigned h, Layout *l)
{
    return layout_helpers(m, w, h, 0, l);
}

size_t vjo_vocr_job_bytes(const char *model, unsigned width, unsigned height)
{
    Layout l;
    return layout(vjo_vocr_model(model), width, height, &l) < 0 ? 0 : l.total;
}

size_t vjo_vocr_helper_bytes(const char *model)
{
    const VjoVocrModel *m = vjo_vocr_model(model);
    return m ? H_ARENA(m) + H_CTX + H_PIXELS + H_TEXT : 0;
}

size_t vjo_vocr_job_bytes_helpers(const char *model, unsigned width, unsigned height, unsigned helpers)
{
    Layout l;
    return layout_helpers(vjo_vocr_model(model), width, height, helpers, &l) < 0 ? 0 : l.total;
}

size_t vjo_vocr_workspace_bytes(const char *model, unsigned width, unsigned height)
{
    const VjoVocrModel *m = vjo_vocr_model(model);
    Layout l;
    return layout(m, width, height, &l) < 0 ? 0 : ALIGN16(m->bytes) + l.total;
}

#define ALIGN4(x) (((x) + (size_t)3) & ~(size_t)3)

unsigned vjo_vocr_strip_rows(unsigned width, unsigned height)
{
    unsigned rows;
    if (!region_ok(width, height)) return 0;
    rows = VJO_VOCR_STRIP_BYTES / width;
    if (rows > VJO_VOCR_STRIP_MAX_ROWS) rows = VJO_VOCR_STRIP_MAX_ROWS;
    if (rows > height) rows = height;
    return rows ? rows : 1u;
}

/* Capture block: [strip: rows x width | sums: 4 x height | ring: slots x 2 x
 * height | seen, cycled: 1 bit per row each]. slots: as many earlier states
 * as fit. */
#define BIT_BYTES(h) (((size_t)(h) + 7u) / 8u)
static size_t capture_fixed(unsigned width, unsigned height, unsigned rows)
{
    return ALIGN4((size_t)rows * width) + 4u * (size_t)height + ALIGN4(2u * BIT_BYTES(height));
}

unsigned vjo_vocr_state_slots(unsigned width, unsigned height)
{
    unsigned rows = vjo_vocr_strip_rows(width, height), slots;
    size_t fixed, room;
    if (!rows) return 0;
    fixed = capture_fixed(width, height, rows);
    room = fixed < VJO_VOCR_CAPTURE_LIMIT ? VJO_VOCR_CAPTURE_LIMIT - fixed : 0;
    slots = (unsigned)(room / (2u * (size_t)height));
    while (slots > 2u && ALIGN4((size_t)slots * 2u * height) > room) slots--;
    return slots < 2u ? 2u : slots > VJO_VOCR_STATES_MAX ? VJO_VOCR_STATES_MAX : slots;
}

size_t vjo_vocr_capture_bytes(unsigned width, unsigned height)
{
    unsigned rows = vjo_vocr_strip_rows(width, height);
    if (!rows) return 0;
    return capture_fixed(width, height, rows) + ALIGN4((size_t)vjo_vocr_state_slots(width, height) * 2u * height);
}

static int cancelled(const VjoGreySource *im)
{
    return im->cancelled && im->cancelled(im->ud);
}

static int run_cancel(void *user)
{
    return cancelled((const VjoGreySource *)user);
}

static uint32_t clock_us(const VjoPlatform *p)
{
    return p->now_us ? (uint32_t)p->now_us(p->ud) : 0;
}

/* The region, a strip at a time, for the finder and the recognizer. Every row
 * keeps the checksum it had when first read; a later read that differs means
 * the screen changed under us (err = VJO_E_SCREEN_CHANGED), unless the row went
 * back to one of its recent states (an animated icon; cycle_cap rows at most:
 * new text, or a box that clears, changes more rows than an icon). While
 * settling, a change is only noted (changed). */
typedef struct {
    const VjoGreySource *src;
    const VjoPlatform *p;
    const VjoVocrPolicy *pol; /* never NULL: a zeroed one is API 19's job */
    VjoVocrStats *st;
    unsigned char *strip;   /* rows [y0, y0 + rows) of the region, width bytes each */
    uint32_t *sums;         /* per region row, valid where seen */
    uint16_t *ring;         /* per region row, slots earlier states (folded), the latest first */
    unsigned char *seen, *cyc; /* 1 bit per region row each; cyc: it went back to an earlier state */
    unsigned w, h, strip_rows, y0, rows, slots;
    unsigned first_next;    /* the first pass's next row (h once it is in) */
    unsigned cycle_cap;     /* rows that may go back to an earlier state without being a change */
    int learn;              /* the stable-text wait is on: such rows are learned */
    int settling, changed;  /* a pass that waits: changes are noted, not errors */
    int retract;            /* the next read from row 0 starts a first pass it does not finish */
    int published;          /* a first pass of this job has ended (the Shell was told of it) */
    int held;               /* the rows have read the same since pass_from (a wait ended settled) */
    int verified;           /* verified_from is set */
    uint32_t pass_from;     /* since when the rows have read the same (this try's first read, or the wait's) */
    uint32_t verified_from; /* start of the latest re-read of every row (a whole pass, in order) */
    uint32_t cover_from;    /* a re-read from row 0 in progress: its start, and its next row (0: none) */
    unsigned cover_next;
    uint32_t settle_left;   /* the job's waiting budget left */
    int err;
} Stream;

static void bits_clear(Stream *s)
{
    memset(s->seen, 0, 2u * BIT_BYTES(s->h));
}

static uint16_t fold16(uint32_t sum)
{
    return (uint16_t)(sum ^ (sum >> 16));
}

/* Checks (or records) the checksum of region row y, whose grey bytes are at p. */
static void stream_check(Stream *s, unsigned y, const unsigned char *p, int first)
{
    uint32_t sum = vjo_fnv1a(VJO_FNV_INIT, p, s->w);
    unsigned i = y >> 3, k, slots = s->slots;
    unsigned char bit = (unsigned char)(1u << (y & 7u));
    uint16_t *ring = s->ring + (size_t)y * slots, now = fold16(sum);
    if (first) s->st->region_sum = vjo_fnv1a(s->st->region_sum, (const unsigned char *)&sum, 4);
    if (!(s->seen[i] & bit)) {
        s->seen[i] |= bit;
        s->sums[y] = sum;
        for (k = 0; k < slots; k++) ring[k] = now;
        return;
    }
    if (s->sums[y] == sum) return;
    /* A state folding to the current one's 16 bits is a new one (the sums
     * differ), whatever copies of that fold the ring holds. */
    k = now == fold16(s->sums[y]) ? slots : 0;
    while (k < slots && ring[k] != now) k++;
    /* Back to a recent state: an animated icon, not text (text being typed
     * only ever moves on to new states). With the stable-text wait on, up to
     * cycle_cap rows of a job are taken for such rows; a page of new text,
     * or a box of several lines clearing back to what it showed before,
     * changes more rows than that. Without the wait every change is one, as
     * in API 19. */
    if (k < slots && ((s->cyc[i] & bit) || (s->learn && s->st->cycling_rows < s->cycle_cap))) {
        if (!(s->cyc[i] & bit)) {
            s->cyc[i] |= bit;
            s->st->cycling_rows++;
        }
        memmove(ring + 1, ring, k * sizeof(*ring));
        ring[0] = fold16(s->sums[y]);
        s->sums[y] = sum;
        s->st->cycles++;
        return;
    }
    if (k == slots) k = slots - 1; /* a new state: the oldest leaves */
    memmove(ring + 1, ring, k * sizeof(*ring));
    ring[0] = fold16(s->sums[y]);
    s->sums[y] = sum;
    if (s->settling) {
        s->st->settle_changes++;
        s->changed = 1;
        return;
    }
    s->st->tears++;
    s->err = VJO_E_SCREEN_CHANGED;
}

/* Rows [y, y + n) through the source into dst (stride w), checked. */
static int stream_read(Stream *s, unsigned y, unsigned n, unsigned char *dst)
{
    unsigned flags = 0;
    uint32_t t0;
    int got, first = 0;
    if (s->err) return -1;
    if (cancelled(s->src)) { s->err = VJO_E_CANCELLED; return -1; }
    if (s->retract && y == 0) {
        flags = VJO_STRIP_FIRST; /* a first pass begun and left: the earlier capture is void */
    } else if (!s->settling && y == s->first_next) {
        flags = VJO_STRIP_FIRST | (y + n == s->h ? VJO_STRIP_LAST : 0u);
        first = 1;
    }
    t0 = clock_us(s->p);
    got = s->src->strip(s->src->ud, y, n, dst, s->w, flags);
    s->st->capture_us += clock_us(s->p) - t0;
    s->st->strips++;
    if (got != (int)n) {
        s->err = got < 0 ? got : VJO_E_SOURCE;
        return -1;
    }
    if (flags == VJO_STRIP_FIRST && !first) s->retract = 0;
    if (first) {
        if (!y && !s->held) s->pass_from = t0;
        s->first_next = y + n;
        if (flags & VJO_STRIP_LAST) s->published = 1;
    } else {
        /* Reads from row 0 that follow each other to the last row re-read every
         * row since the first of them (the finder's second pass, a wait's pass). */
        if (!y) {
            s->cover_from = t0;
            s->cover_next = n;
        } else if (s->cover_next && y <= s->cover_next) {
            if (y + n > s->cover_next) s->cover_next = y + n;
        } else {
            s->cover_next = 0;
        }
    }
    for (unsigned r = 0; r < n; r++) stream_check(s, y + r, dst + (size_t)r * s->w, first);
    if (!s->err && !first && s->cover_next >= s->h) {
        s->verified_from = s->cover_from;
        s->verified = 1;
        s->cover_next = 0;
    }
    return s->err ? -1 : 0;
}

/* The strip from row y: during the first pass the strips follow each other
 * from row 0 (the kernel builds the capture's scene signature from them, and
 * the last one publishes it), otherwise it is moved up to stay inside the region. */
static int stream_fetch(Stream *s, unsigned y)
{
    unsigned n = s->strip_rows;
    if (y == s->first_next && s->first_next < s->h) {
        if (n > s->h - y) n = s->h - y;
    } else if (y + n > s->h) {
        y = s->h - n;
    }
    s->rows = 0;
    if (stream_read(s, y, n, s->strip) < 0) return -1;
    s->y0 = y;
    s->rows = n;
    return 0;
}

/* One whole pass over the region from row 0, a strip at a time, outside the
 * first pass: it re-reads (verifies) every row (stream_read records it). */
static int stream_pass(Stream *s)
{
    for (unsigned y = 0; y < s->h; y += s->strip_rows)
        if (stream_fetch(s, y) < 0) return -1;
    return 0;
}

static int settle_on(const Stream *s)
{
    return s->pol->settle_us && s->p->now_us && s->settle_left;
}

/* Spends us of the job's waiting budget; over: it ran out before the rows held
 * still, so the job goes on without waiting (now and on later tries). */
static void settle_spend(Stream *s, uint32_t us, int over)
{
    s->st->settle_us += us;
    s->settle_left = !over && us < s->settle_left ? s->settle_left - us : 0;
    if (over) s->st->settle_timeout = 1;
}

/* Waits for the rows to hold still: passes that only read the region, until
 * one that saw no change started settle_us after the last pass that did (or the
 * wait's first). A job that already told the Shell of a capture first voids it
 * (retract), so the Shell does not take the screen it read before for this
 * job's. Settled, the rows have read the same since pass_from. */
static int settle_wait(Stream *s)
{
    uint32_t t0 = clock_us(s->p), stable_since = t0;
    int done = 0, over = 0;
    s->settling = 1;
    s->retract = s->published;
    while (!done) {
        uint32_t ps = clock_us(s->p);
        if (ps - t0 >= s->settle_left || s->st->strips >= VJO_VOCR_SETTLE_READS) { over = 1; break; }
        s->changed = 0;
        if (stream_pass(s) < 0) break;
        s->st->settle_passes++;
        if (s->changed) stable_since = ps;
        else if (ps - stable_since >= s->pol->settle_us) done = 1;
    }
    s->settling = 0;
    if (done) {
        s->pass_from = stable_since;
        s->held = 1;
        s->st->settled = 1;
    }
    settle_spend(s, clock_us(s->p) - t0, over);
    return s->err ? -1 : 0;
}

/* After the finder: re-reads the region until a whole pass started settle_us
 * after the rows were first read the same (pass_from). A change is a tear: the
 * lines found are not the screen's any more. Once one re-read has matched, a
 * pass that would start too early is not read: the job sleeps until it is due
 * (with a sleep callback). */
static int settle_verify(Stream *s)
{
    uint32_t t0 = clock_us(s->p);
    int over = 0;
    while (!s->verified || s->verified_from - s->pass_from < s->pol->settle_us) {
        uint32_t now = clock_us(s->p), due = s->pass_from + s->pol->settle_us;
        if (now - t0 >= s->settle_left || s->st->strips >= VJO_VOCR_SETTLE_READS) { over = 1; break; }
        if (s->verified && s->pol->sleep && (int32_t)(due - now) > 0) {
            uint32_t nap = due - now, left = s->settle_left - (now - t0);
            s->pol->sleep(nap < left ? nap : left);
            if (cancelled(s->src)) { s->err = VJO_E_CANCELLED; break; }
        }
        if (stream_pass(s) < 0) break;
        s->st->settle_passes++;
    }
    if (!s->err && !over) s->st->settled = 1;
    settle_spend(s, clock_us(s->p) - t0, over);
    return s->err ? -1 : 0;
}

static const unsigned char *stream_row(void *user, unsigned y)
{
    Stream *s = (Stream *)user;
    if (s->err || y >= s->h) return NULL;
    if (!s->rows || y < s->y0 || y >= s->y0 + s->rows) {
        /* rows run in order within a pass: the strip starts at the row asked for,
         * except during the first pass, whose strips must follow each other */
        if (stream_fetch(s, y) < 0) return NULL;
    }
    return s->strip + (size_t)(y - s->y0) * s->w;
}

/* rc for a finder or row failure: the source's own error first. */
static int stream_error(const Stream *s, int fallback)
{
    return s->err ? s->err : fallback;
}

int vjo_vocr_resident_drop(VjoVocrResident *r)
{
    if (!r) return 0;
    r->model = NULL;
    r->size = r->stamp = 0;
    if (!r->data) return 0;
    if (r->release && r->release(r->ud)) return -1;
    r->data = NULL;
    r->capacity = 0;
    return 0;
}

/* rc, after releasing the resident memory; VJO_E_OCR_UNAVAILABLE if that failed. */
static int drop_with(VjoVocrResident *r, int rc)
{
    return vjo_vocr_resident_drop(r) ? VJO_E_OCR_UNAVAILABLE : rc;
}

/* Reads the whole open file into dst and checks its SHA-256 while reading;
 * cancellation is checked between pieces. */
static int read_verified(const VjoFile *file, const VjoVocrModel *m, unsigned char *dst,
                         const VjoGreySource *im)
{
    br_sha256_context hash;
    unsigned char digest[32];
    br_sha256_init(&hash);
    for (size_t off = 0; off < m->bytes;) {
        size_t n = m->bytes - off < READ_CHUNK ? m->bytes - off : READ_CHUNK;
        if (cancelled(im)) return VJO_E_CANCELLED;
        if (file->read(file->ctx, off, dst + off, n) < 0)
            return cancelled(im) ? VJO_E_CANCELLED : VJO_E_OCR_MODEL;
        br_sha256_update(&hash, dst + off, n);
        off += n;
    }
    br_sha256_out(&hash, digest);
    return memcmp(digest, m->sha256, sizeof(digest)) ? VJO_E_OCR_MODEL : VJO_OK;
}

/* Verified weights of m in r->data. The file is always opened (its size and
 * stamp decide); it is read and hashed again only if they changed, the stamp
 * is unknown, or no verified copy is held. */
static int resident_weights(const VjoPlatform *p, const char *dir, const VjoVocrModel *m,
                            VjoVocrResident *r, const VjoGreySource *im, VjoVocrStats *st)
{
    char path[256];
    size_t dn = strlen(dir), fn = strlen(m->name);
    VjoFile file;
    int rc;
    /* Another model's weights: never hold two. */
    if (r->model != m && vjo_vocr_resident_drop(r)) return VJO_E_OCR_UNAVAILABLE;
    if (!dn || dn + 1 + fn >= sizeof(path)) return drop_with(r, VJO_E_OCR_MODEL);
    memcpy(path, dir, dn);
    path[dn] = '/';
    memcpy(path + dn + 1, m->name, fn + 1);
    memset(&file, 0, sizeof(file));
    if (!p->file_open || p->file_open(p->ud, path, &file) < 0)
        /* A cancelled check proves nothing about the file: keep verified weights. */
        return cancelled(im) ? VJO_E_CANCELLED : drop_with(r, VJO_E_OCR_MODEL);
    if (file.size != m->bytes || !file.read || !file.close) {
        if (file.close) file.close(file.ctx);
        return drop_with(r, VJO_E_OCR_MODEL);
    }
    if (r->model == m && r->data && file.stamp && file.stamp == r->stamp && file.size == r->size) {
        file.close(file.ctx);
        st->model_cached = 1;
        return VJO_OK;
    }
    /* From here until the hash matches, the memory holds no weights. */
    r->model = NULL;
    if (!r->data) {
        unsigned char *data = r->alloc ? (unsigned char *)r->alloc(r->ud, m->bytes) : NULL;
        if (!data) {
            file.close(file.ctx);
            return VJO_E_OOM;
        }
        r->data = data;
        r->capacity = m->bytes;
        if ((uintptr_t)data & 15u) {
            file.close(file.ctx);
            return drop_with(r, VJO_E_OOM);
        }
    }
    rc = read_verified(&file, m, r->data, im);
    file.close(file.ctx);
    if (rc) return drop_with(r, rc);
    r->model = m;
    r->size = file.size;
    r->stamp = file.stamp;
    st->model_loaded = 1;
    return VJO_OK;
}

static int run_error(int rc)
{
    switch (rc) {
    case VOCR_E_TEXT: return VJO_E_TOO_LARGE;
    case VOCR_E_CANCELLED: return VJO_E_CANCELLED;
    default: return VJO_E_OCR_INFERENCE;
    }
}

/* Line ln's grey pixels and their stride: from the strip (stride s->w), read
 * again from its top row if the strip does not hold the whole line; a line
 * taller than a strip goes through the strip a strip at a time (each row checked
 * as always) and only its own columns are kept, in the source's line buffer
 * (stride ln->w: ln->w x ln->h bytes). NULL: the line is left out (tall, no
 * buffer) or the read failed (s->err). */
static const unsigned char *line_pixels(Stream *s, const VocrLine *ln, size_t *stride)
{
    if (ln->h <= s->strip_rows) {
        if (!s->rows || ln->y < s->y0 || ln->y + ln->h > s->y0 + s->rows) {
            if (stream_fetch(s, ln->y) < 0) return NULL;
        }
        *stride = s->w;
        return s->strip + (size_t)(ln->y - s->y0) * s->w + ln->x;
    }
    {
        size_t bytes = (size_t)ln->w * ln->h;
        unsigned char *buf = bytes <= VJO_VOCR_LINE_MAX_BYTES && s->src->line_buffer
                             ? (unsigned char *)s->src->line_buffer(s->src->ud, bytes) : NULL;
        s->st->tall++;
        if (!buf) {
            s->st->tall_skipped++;
            return NULL;
        }
        if (bytes > s->st->line_bytes) s->st->line_bytes = bytes;
        for (unsigned y = 0; y < ln->h;) {
            unsigned n = ln->h - y < s->strip_rows ? ln->h - y : s->strip_rows;
            s->rows = 0;
            if (stream_read(s, ln->y + y, n, s->strip) < 0) return NULL;
            for (unsigned r = 0; r < n; r++)
                memcpy(buf + (size_t)(y + r) * ln->w, s->strip + (size_t)r * s->w + ln->x, ln->w);
            s->y0 = ln->y + y; /* the strip now holds these rows */
            s->rows = n;
            y += n;
        }
        *stride = ln->w;
        return buf;
    }
}

/* The weights resident and verified, the runtime's model parsed and its arena
 * planned in the scratch (the finder is done with it). */
static int load_model(const VjoPlatform *p, const char *dir, const VjoVocrModel *mi, const VjoGreySource *im,
                      VjoVocrResident *res, unsigned char *ws, const Layout *l, VjoVocrStats *st,
                      VjoVocrCache *cache)
{
    vocr_model *model = (vocr_model *)(void *)(ws + l->structs_off);
    vocr_ctx *ctx = (vocr_ctx *)(void *)(ws + l->structs_off + ALIGN16(sizeof(vocr_model)));
    vocr_opts opts;
    uint32_t t0 = clock_us(p);
    int rc = resident_weights(p, dir, mi, res, im, st);
    if (rc) return rc;
    /* Weights read from the file again (another file, or the first job): no
     * earlier line's text is known to be theirs. */
    if (st->model_loaded && cache) memset(cache, 0, sizeof(*cache));
    /* Verified bytes the runtime refuses would fail every job: release them. */
    if (vocr_load(model, res->data, mi->bytes) != VOCR_OK) return drop_with(res, VJO_E_OCR_MODEL);
    memset(&opts, 0, sizeof(opts));
    opts.mode = VOCR_MODE_W8I;
    {
        size_t need = vocr_arena_bytes(model, &opts);
        if (!need || need > l->scratch) return drop_with(res, VJO_E_OCR_MODEL);
        if (vocr_init(ctx, model, &opts, ws + l->scratch_off, need) != VOCR_OK) return VJO_E_OCR_INFERENCE;
    }
    st->load_us = clock_us(p) - t0;
    return VJO_OK;
}

/* ---- ocr-speed: the line cache ---- */

/* 64-bit FNV-1a of a crop's rows, then its width, height, polarity and the job's
 * cache variant. Never 0 (an empty cache entry). */
static uint64_t line_key(const unsigned char *px, size_t stride, unsigned w, unsigned h, unsigned dark,
                         uint32_t variant)
{
    const uint64_t prime = 0x100000001b3ull;
    uint64_t k = 0xcbf29ce484222325ull;
    const uint32_t tail[4] = {w, h, dark, variant};
    for (unsigned y = 0; y < h; y++) {
        const unsigned char *r = px + (size_t)y * stride;
        for (unsigned x = 0; x < w; x++) {
            k ^= r[x];
            k *= prime;
        }
    }
    for (unsigned i = 0; i < sizeof(tail); i++) {
        k ^= ((const unsigned char *)tail)[i];
        k *= prime;
    }
    return k ? k : 1u;
}

static VjoVocrCacheLine *cache_find(VjoVocrCache *c, const VjoVocrModel *m, uint64_t key)
{
    for (unsigned i = 0; i < VJO_VOCR_CACHE_LINES; i++)
        if (c->line[i].key == key && c->line[i].model == m) return &c->line[i];
    return NULL;
}

static void cache_put(VjoVocrCache *c, const VjoVocrModel *m, uint64_t key, const char *text, size_t len,
                      unsigned frames, uint16_t line_flags)
{
    VjoVocrCacheLine *e = cache_find(c, m, key);
    if (len >= VJO_VOCR_CACHE_TEXT) return;
    if (!e) {
        e = &c->line[0];
        for (unsigned i = 1; i < VJO_VOCR_CACHE_LINES; i++)
            if (!c->line[i].key || (e->key && c->line[i].used < e->used)) e = &c->line[i];
    }
    e->key = key;
    e->model = m;
    e->used = ++c->tick;
    e->frames = (uint16_t)(frames > 0xFFFFu ? 0xFFFFu : frames);
    e->len = (uint16_t)len;
    e->line_flags = line_flags;
    memcpy(e->text, text, len);
    e->text[len] = '\0';
}

/* ---- ocr-speed: the slots that recognize lines ---- */

/* Slot 0 is the caller's thread (the job's context in the scratch); slots
 * 1..helpers run on helper threads, each with its own arena and context over
 * the shared model, and a copy of its line's pixels. */
typedef struct {
    vocr_ctx *ctx;
    unsigned char *arena, *pixels;
    char *text;                 /* VJO_OCR_TEXT_CAP bytes */
    int inited;                 /* vocr_init done on ctx */
    int busy;                   /* holds a line not yet in the text */
    int running;                /* a helper thread was started for it: join before reuse */
    int done;                   /* (atomic) the run returned */
    int rc;
    uint32_t us, cpu_us;
    vocr_image img;
    vocr_extra extra;
    /* A helper reads nothing on the caller's stack (it may outlive a failed join):
     * the callbacks it needs are copied here. */
    const VjoVocrPool *pool;    /* the caller's thread only */
    uint64_t (*now_us)(void *ud);
    void *now_ud;
    uint64_t (*thread_us)(void *ud);
    void *thread_ud;
    const VjoGreySource *im;
    const int *abort;           /* the job stops: helpers end at their next tile */
} Slot;

/* The job's abort flag first: once the job has stopped it, a helper that is
 * still running (a failed join) never reads the caller's image again. */
static int slot_cancel(void *user)
{
    const Slot *sl = (const Slot *)user;
    return __atomic_load_n(sl->abort, __ATOMIC_ACQUIRE) || cancelled(sl->im);
}

static uint32_t slot_clock(const Slot *sl)
{
    return sl->now_us ? (uint32_t)sl->now_us(sl->now_ud) : 0;
}

static void slot_run(void *arg)
{
    Slot *sl = (Slot *)arg;
    uint32_t t0 = slot_clock(sl);
    uint64_t c0 = sl->thread_us ? sl->thread_us(sl->thread_ud) : 0;
    sl->rc = vocr_run(sl->ctx, &sl->img, sl->text, VJO_OCR_TEXT_CAP, &sl->extra);
    sl->us = slot_clock(sl) - t0;
    sl->cpu_us = sl->thread_us ? (uint32_t)(sl->thread_us(sl->thread_ud) - c0) : 0;
    __atomic_store_n(&sl->done, 1, __ATOMIC_RELEASE);
}

/* A line between its fetch and its place in the text, in line order. */
enum { LN_SKIP, LN_HIT, LN_SLOT };
typedef struct {
    int kind;
    unsigned slot;              /* LN_SLOT */
    const VjoVocrCacheLine *hit;
    uint64_t key;               /* 0: not cached (no cache, or a tall line) */
    uint16_t w, h;
} Pending;

typedef struct {
    const VjoPlatform *p;
    const VjoVocrModel *mi;
    VjoVocrStats *st;
    VjoVocrCache *cache;
    char *text;
    size_t cap, used;
    Slot slot[1 + VJO_VOCR_HELPERS_MAX];
    unsigned helpers;
    Pending q[VJO_VOCR_MAX_LINES];
    unsigned qh, qt;
    struct { uint64_t key; size_t off, len; unsigned frames, stat; } put[VJO_VOCR_MAX_LINES];
    unsigned n_put;
    int abort, stuck;
} Lines;

static void line_stat(Lines *L, const Pending *e, unsigned frames, uint32_t us, uint32_t cpu_us, unsigned slot,
                      int cached, uint16_t flags)
{
    VjoVocrLineStat *ls;
    if (L->st->recognized >= VJO_VOCR_MAX_LINES) return;
    ls = &L->st->line[L->st->recognized++];
    ls->width = e->w;
    ls->height = e->h;
    ls->frames = (uint16_t)(frames > 0xFFFFu ? 0xFFFFu : frames);
    ls->reserved = flags;
    ls->slot = (uint8_t)slot;
    ls->cached = (uint8_t)(cached != 0);
    ls->pad = 0;
    ls->us = us;
    ls->cpu_us = cpu_us;
}

/* Places the line at the head of the queue into the text, exactly as the serial
 * job does (separators, an empty read, the capacity). Its run is over. */
static int place(Lines *L)
{
    Pending *e = &L->q[L->qh];
    size_t start = L->used ? L->used + 1 : 0, n = 0;
    const char *t = NULL;
    const unsigned stat = L->st->recognized; /* the line's VjoVocrLineStat, if there is room for it */
    L->qh++;
    if (start + 1 >= L->cap) return VJO_E_TOO_LARGE;
    if (e->kind == LN_SKIP) return VJO_OK;
    if (e->kind == LN_HIT) {
        t = e->hit->text;
        n = e->hit->len;
        line_stat(L, e, e->hit->frames, 0, 0, 0, 1, e->hit->line_flags);
        L->st->cached++;
    } else {
        Slot *sl = &L->slot[e->slot];
        sl->busy = 0;
        if (sl->rc != VOCR_OK) return run_error(sl->rc);
        t = sl->text;
        n = strlen(t);
        L->st->frames += sl->extra.frames;
        line_stat(L, e, sl->extra.frames, sl->us, sl->cpu_us, e->slot, 0, 0);
    }
    if (start + n + 1 > L->cap) return VJO_E_TOO_LARGE;
    if (t != L->text + start) memmove(L->text + start, t, n + 1);
    if (e->key && L->n_put < VJO_VOCR_MAX_LINES) {
        L->put[L->n_put].key = e->key;
        L->put[L->n_put].off = start;
        L->put[L->n_put].len = n;
        L->put[L->n_put].frames = e->kind == LN_SLOT ? L->slot[e->slot].extra.frames : 0;
        L->put[L->n_put].stat = stat < L->st->recognized ? stat : VJO_VOCR_MAX_LINES;
        L->n_put++;
    }
    if (!n) { /* nothing read on this line: no separator either */
        L->text[L->used] = '\0';
        return VJO_OK;
    }
    if (start) L->text[L->used] = '\n';
    L->used = start + n;
    return VJO_OK;
}

/* Waits for slot k's helper, if one runs it. */
static int join_slot(Lines *L, unsigned k)
{
    Slot *sl = &L->slot[k];
    if (!sl->running) return VJO_OK;
    if (sl->pool->join(sl->pool->ud, k) < 0) {
        L->stuck = 1; /* it may still run in the workspace: never freed under it */
        return VJO_E_OCR_UNAVAILABLE;
    }
    sl->running = 0;
    return VJO_OK;
}

/* Places every line at the head whose run is over (wait: also the ones still
 * running, in order). */
static int drain(Lines *L, int wait)
{
    while (L->qh < L->qt) {
        Pending *e = &L->q[L->qh];
        int rc;
        if (e->kind == LN_SLOT) {
            Slot *sl = &L->slot[e->slot];
            if (sl->running && !wait && !__atomic_load_n(&sl->done, __ATOMIC_ACQUIRE)) return VJO_OK;
            rc = join_slot(L, e->slot);
            if (rc) return rc;
        }
        rc = place(L);
        if (rc) return rc;
    }
    return VJO_OK;
}

/* Stops the job's helpers and waits for every one: nothing runs in the
 * workspace afterwards (unless a join fails: L->stuck). */
static void stop_all(Lines *L)
{
    __atomic_store_n(&L->abort, 1, __ATOMIC_RELEASE);
    for (unsigned k = 1; k <= L->helpers; k++)
        if (join_slot(L, k)) L->stuck = 1;
    L->qh = L->qt;
}

static int slot_init(Slot *sl, const vocr_model *model)
{
    vocr_opts opts;
    if (sl->inited) return VJO_OK;
    memset(&opts, 0, sizeof(opts));
    opts.mode = VOCR_MODE_W8I;
    if (vocr_init(sl->ctx, model, &opts, sl->arena, vocr_arena_bytes(model, &opts)) != VOCR_OK)
        return VJO_E_OCR_INFERENCE;
    sl->inited = 1;
    return VJO_OK;
}

/* One capture of the region: lines, then (weights) each line recognized. */
static int recognize(const VjoPlatform *p, const char *dir, const VjoVocrModel *mi,
                     const VjoGreySource *im, VjoVocrResident *res, unsigned char *ws, const Layout *l,
                     Stream *s, char *text, size_t cap, VjoVocrStats *st)
{
    const unsigned w = im->width;
    const VjoVocrPolicy *pol = s->pol;
    const VjoVocrPool *pool = pol->pool;
    VocrLinesRows region = {w, im->height, VOCR_LINES_GREY, stream_row, s};
    VocrLineSet set;
    vocr_model *model = (vocr_model *)(void *)(ws + l->structs_off);
    vocr_ctx *ctx = (vocr_ctx *)(void *)(ws + l->structs_off + ALIGN16(sizeof(vocr_model)));
    static Lines lines; /* ~2 KB: never on the worker's stack. One job at a time. */
    Lines *L = &lines;
    uint32_t t0;
    int rc, loaded = 0, busy_on = 0;

    text[0] = '\0';
    st->lines = st->truncated = st->dark = st->frames = st->recognized = 0;
    st->blank = 0;
    st->cached = st->dispatched = st->run_failures = 0;
    t0 = clock_us(p);
    rc = vocr_find_lines_rows(&region, (int)mi->finder_mode, ws + l->scratch_off, l->finder, &set);
    if (rc == VOCR_LINES_E_SOURCE) return stream_error(s, VJO_E_SOURCE);
    if (rc) return rc == VOCR_LINES_E_ARENA ? VJO_E_OOM : VJO_E_OCR_REGION;
    /* A region inside one strip was read once: read it again, so that a screen
     * that changed while it was read is seen (the strips of a taller region are
     * all read twice by the finder's two passes already). */
    if (s->strip_rows >= im->height && stream_fetch(s, 0) < 0) return stream_error(s, VJO_E_SOURCE);
    st->find_us = clock_us(p) - t0;
    st->lines = set.count;
    st->truncated = set.truncated;
    for (unsigned i = 0; i < set.count; i++)
        st->dark += set.lines[i].dark ? 1u : 0u;
    if (!set.count) return VJO_OK; /* nothing to read: the model file is not opened */
    if (cancelled(im)) return VJO_E_CANCELLED;
    /* Nothing is recognized before the rows have held still for settle_us. */
    if (settle_on(s) && settle_verify(s) < 0) return stream_error(s, VJO_E_SOURCE);

    memset(L, 0, sizeof(*L));
    L->p = p;
    L->mi = mi;
    L->st = st;
    L->cache = pol->cache;
    L->text = text;
    L->cap = cap;
    /* Helpers only without a per-line callback (it reads the strip in line order)
     * and for a text the slots can hold. */
    L->helpers = pool && pool->run && pool->join && !pol->line && cap <= VJO_OCR_TEXT_CAP
                 ? (pool->helpers < l->helpers ? pool->helpers : l->helpers) : 0;
    st->helpers = L->helpers;
    for (unsigned k = 0; k <= L->helpers; k++) {
        Slot *sl = &L->slot[k];
        sl->now_us = p->now_us;
        sl->now_ud = p->ud;
        sl->pool = pool;
        sl->thread_us = pool ? pool->thread_us : NULL;
        sl->thread_ud = pool ? pool->ud : NULL;
        sl->im = im;
        sl->abort = &L->abort;
        if (!k) {
            sl->ctx = ctx;
            sl->text = (char *)(ws + l->text0_off); /* only with helpers */
            sl->inited = 1; /* load_model plans ctx */
        } else {
            unsigned char *b = ws + l->helper_off + (size_t)(k - 1) * l->helper_bytes;
            sl->arena = b;
            sl->ctx = (vocr_ctx *)(void *)(b + H_ARENA(mi));
            sl->pixels = b + H_ARENA(mi) + H_CTX;
            sl->text = (char *)(b + H_ARENA(mi) + H_CTX + H_PIXELS);
        }
    }

    /* With the blank-line check, the weights are read at the first line that
     * is not skipped: a box of blank lines opens no model file. */
    if (!pol->blank) {
        rc = load_model(p, dir, mi, im, res, ws, l, st, L->cache);
        if (rc) return rc;
        loaded = 1;
    }

    t0 = clock_us(p);
    for (unsigned i = 0; i < set.count; i++) {
        const VocrLine *ln = &set.lines[i];
        const unsigned char *pixels;
        size_t stride = w;
        VjoVocrInk ink = {0, 0, 0, 0, 0, 0};
        Pending *e;
        Slot *sl = NULL;
        uint64_t key = 0;
        int blank = 0;
        if (cancelled(im)) { rc = VJO_E_CANCELLED; goto fail; }
        /* With nothing queued, the text so far is final: the serial job's check. */
        if (L->qh == L->qt && (L->used ? L->used + 1 : 0) + 1 >= cap) { rc = VJO_E_TOO_LARGE; goto fail; }
        pixels = line_pixels(s, ln, &stride);
        if (s->err) { rc = s->err; goto fail; }
        e = &L->q[L->qt];
        memset(e, 0, sizeof(*e));
        e->w = (uint16_t)ln->w;
        e->h = (uint16_t)ln->h;
        e->kind = LN_SKIP;
        if (!pixels) { /* taller than a strip and no line buffer: left out */
            L->qt++;
            if ((rc = drain(L, 0)) != VJO_OK) goto fail;
            continue;
        }
        if (pol->blank || pol->line) {
            uint32_t b0 = clock_us(p);
            blank = vjo_vocr_ink(pixels, stride, ln->w, ln->h, &ink);
            if (pol->blank_edges) blank = ink.edges < pol->blank_edges;
            blank = blank && pol->blank;
            st->blank_us += clock_us(p) - b0;
        }
        if (blank) {
            if (st->blank < VJO_VOCR_BLANK_LOGGED) {
                VjoVocrBlankStat *b = &st->blank_line[st->blank];
                b->index = (uint16_t)i;
                b->width = (uint16_t)ln->w;
                b->height = (uint16_t)ln->h;
                b->x = (uint16_t)ln->x;
                b->y = (uint16_t)ln->y;
                b->ink = ink;
            }
            st->blank++;
            if (pol->line) {
                VjoVocrLineInfo info = {i, ln->x, ln->y, ln->w, ln->h, ln->dark, pixels, stride, ink, 1, "", 0};
                pol->line(pol->ud, &info);
            }
            L->qt++;
            if ((rc = drain(L, 0)) != VJO_OK) goto fail;
            continue;
        }
        if (!loaded) {
            uint32_t r0 = clock_us(p);
            rc = load_model(p, dir, mi, im, res, ws, l, st, L->cache);
            if (rc) goto fail;
            loaded = 1;
            t0 += clock_us(p) - r0; /* recognize_us leaves the load out */
        }
        if (L->cache && ln->h <= s->strip_rows) {
            uint32_t k0 = clock_us(p);
            const VjoVocrCacheLine *hit;
            key = line_key(pixels, stride, ln->w, ln->h, ln->dark, pol->cache_variant);
            st->key_us += clock_us(p) - k0;
            hit = cache_find(L->cache, mi, key);
            if (hit) {
                ((VjoVocrCacheLine *)hit)->used = ++L->cache->tick;
                e->kind = LN_HIT;
                e->hit = hit;
                e->key = 0; /* already there */
                L->cache->hits++;
                if (pol->line) {
                    VjoVocrLineInfo info = {i, ln->x, ln->y, ln->w, ln->h, ln->dark, pixels, stride, ink, 0,
                                            hit->text, 0};
                    pol->line(pol->ud, &info);
                }
                L->qt++;
                if ((rc = drain(L, 0)) != VJO_OK) goto fail;
                continue;
            }
            L->cache->misses++;
        }
        e->kind = LN_SLOT;
        e->key = key;
        if (!busy_on && pol->busy) {
            pol->busy(pol->busy_ud, 1);
            busy_on = 1;
        }
        /* A helper, when one is free and the line fits its copy; else this thread.
         * The last line always runs here: this thread would only wait otherwise. */
        if (L->helpers && i + 1 < set.count && (size_t)ln->w * ln->h <= VJO_VOCR_STRIP_BYTES) {
            if ((rc = drain(L, 0)) != VJO_OK) goto fail; /* lines that are done free their helpers */
            for (unsigned k = 1; k <= L->helpers && !sl; k++)
                if (!L->slot[k].busy) sl = &L->slot[k];
        }
        if (sl) {
            unsigned k = (unsigned)(sl - L->slot);
            if ((rc = slot_init(sl, model)) != VJO_OK) goto fail;
            for (unsigned r = 0; r < ln->h; r++)
                memcpy(sl->pixels + (size_t)r * ln->w, pixels + (size_t)r * stride, ln->w);
            sl->img.pixels = sl->pixels;
            sl->img.width = ln->w;
            sl->img.height = ln->h;
            sl->img.stride = ln->w;
            sl->img.format = VOCR_GREY8 | (ln->dark ? VOCR_INVERT : 0);
            memset(&sl->extra, 0, sizeof(sl->extra));
            sl->extra.cancel = slot_cancel;
            sl->extra.user = sl;
            sl->busy = 1;
            __atomic_store_n(&sl->done, 0, __ATOMIC_RELEASE);
            e->slot = k;
            L->qt++;
            if (pool->run(pool->ud, k, slot_run, sl) == 0) {
                sl->running = 1;
                st->dispatched++;
            } else {
                st->run_failures++;
                slot_run(sl); /* no thread: here, now */
            }
            if ((rc = drain(L, 0)) != VJO_OK) goto fail;
            continue;
        }
        /* Here, on the caller's thread: straight into the text when nothing is
         * queued before it (the serial job), else into slot 0's text, once
         * slot 0 holds nothing still waiting. */
        if (L->qh < L->qt && L->slot[0].busy && (rc = drain(L, 1)) != VJO_OK) goto fail;
        sl = &L->slot[0];
        {
            size_t start = L->used ? L->used + 1 : 0;
            int direct = L->qh == L->qt;
            uint32_t line_t0 = clock_us(p);
            uint64_t c0 = pool && pool->thread_us ? pool->thread_us(pool->ud) : 0;
            vocr_image img;
            img.pixels = pixels;
            img.width = ln->w;
            img.height = ln->h;
            img.stride = stride;
            img.format = VOCR_GREY8 | (ln->dark ? VOCR_INVERT : 0);
            memset(&sl->extra, 0, sizeof(sl->extra));
            sl->extra.cancel = run_cancel;
            sl->extra.user = (void *)im;
            if (direct) {
                rc = vocr_run(ctx, &img, text + start, cap - start, &sl->extra);
                if (rc != VOCR_OK) { rc = run_error(rc); goto fail; }
            } else {
                sl->rc = vocr_run(ctx, &img, sl->text, VJO_OCR_TEXT_CAP, &sl->extra);
                sl->busy = 1;
            }
            sl->us = clock_us(p) - line_t0;
            sl->cpu_us = pool && pool->thread_us ? (uint32_t)(pool->thread_us(pool->ud) - c0) : 0;
            sl->rc = direct ? VOCR_OK : sl->rc;
            if (direct) {
                char *saved = sl->text;
                sl->text = text + start; /* place() reads the line where it already is */
                e->slot = 0;
                L->qt++;
                rc = place(L);
                sl->text = saved;
                if (rc) goto fail;
                if (pol->line) {
                    VjoVocrLineInfo info = {i, ln->x, ln->y, ln->w, ln->h, ln->dark, pixels, stride, ink, 0,
                                            text + start, sl->us};
                    pol->line(pol->ud, &info);
                }
            } else {
                e->slot = 0;
                L->qt++;
            }
        }
        if ((rc = drain(L, 0)) != VJO_OK) goto fail;
    }
    rc = drain(L, 1);
    if (rc) goto fail;
    if (busy_on) pol->busy(pol->busy_ud, 0);
    st->recognize_us = clock_us(p) - t0;
    /* The job succeeded: its recognized lines become known texts. */
    if (L->cache)
        for (unsigned k = 0; k < L->n_put; k++)
            cache_put(L->cache, mi, L->put[k].key, text + L->put[k].off, L->put[k].len, L->put[k].frames,
                      L->put[k].stat < VJO_VOCR_MAX_LINES ? st->line[L->put[k].stat].reserved : 0u);
    return VJO_OK;
fail:
    stop_all(L);
    if (busy_on) pol->busy(pol->busy_ud, 0);
    if (L->stuck) st->stuck = 1;
    return rc;
}

static const VjoVocrPolicy no_policy;

/* one_shot: the weights occupy the front of workspace for this job only. */
static int run_job(const VjoPlatform *platform, const char *model_dir, const char *model,
                   const VjoGreySource *image, VjoVocrResident *res, void *workspace,
                   size_t workspace_size, void *capture, size_t capture_size, int one_shot,
                   const VjoVocrPolicy *policy, char *text, size_t text_cap, VjoVocrStats *stats)
{
    const VjoVocrModel *mi;
    Layout l;
    Stream st;
    size_t front, need_capture;
    unsigned strip_rows;
    int rc = VJO_OK;
    if (!platform || !model_dir || !model || !image || !res || !text || !text_cap || !stats)
        return VJO_E_SOURCE;
    text[0] = '\0';
    memset(stats, 0, sizeof(*stats));
    mi = vjo_vocr_model(model);
    if (!mi) return VJO_E_OCR_MODEL;
    if (!image->strip || layout(mi, image->width, image->height, &l) < 0) return VJO_E_OCR_REGION;
    front = one_shot ? ALIGN16(mi->bytes) : 0;
    /* As many of the pool's helpers as the workspace holds (each one's arena,
     * context, line copy and text: vjo_vocr_helper_bytes). */
    if (policy && policy->pool && workspace) {
        unsigned helpers = policy->pool->helpers < VJO_VOCR_HELPERS_MAX ? policy->pool->helpers : VJO_VOCR_HELPERS_MAX;
        Layout lh;
        while (helpers && (layout_helpers(mi, image->width, image->height, helpers, &lh) < 0 ||
                           workspace_size < front + lh.total))
            helpers--;
        if (helpers) l = lh;
    }
    strip_rows = vjo_vocr_strip_rows(image->width, image->height);
    need_capture = vjo_vocr_capture_bytes(image->width, image->height);
    stats->workspace = front + l.total;
    stats->capture = need_capture;
    stats->model_bytes = mi->bytes;
    stats->scratch_bytes = l.scratch;
    stats->width = image->width;
    stats->height = image->height;
    stats->strip_rows = strip_rows;
    if (cancelled(image)) return VJO_E_CANCELLED;
    if (!workspace || ((uintptr_t)workspace & 15u) || workspace_size < front + l.total ||
        !capture || capture_size < need_capture) return VJO_E_OOM;
    memset(&st, 0, sizeof(st));
    st.src = image;
    st.p = platform;
    st.pol = policy ? policy : &no_policy;
    st.st = stats;
    st.w = image->width;
    st.h = image->height;
    st.strip_rows = strip_rows;
    st.strip = (unsigned char *)capture;
    st.slots = vjo_vocr_state_slots(st.w, st.h);
    st.sums = (uint32_t *)(void *)((unsigned char *)capture + ALIGN4((size_t)strip_rows * st.w));
    st.ring = (uint16_t *)(void *)(st.sums + st.h);
    st.seen = (unsigned char *)(st.ring + (size_t)st.slots * st.h);
    st.cyc = st.seen + BIT_BYTES(st.h);
    /* An icon is a few dozen rows; a looping background may be more. */
    st.cycle_cap = VJO_VOCR_CYCLE_ROWS;
    st.learn = st.pol->settle_us && platform->now_us;
    st.settle_left = st.pol->settle_max_us ? st.pol->settle_max_us : VJO_VOCR_SETTLE_MAX_US;
    /* The screen may change while it is read (the player advanced the text): then
     * the region is captured again, up to VJO_VOCR_TRIES times. Without the
     * stable-text wait from scratch; with it, after waiting for the rows to hold
     * still (and before the first, with settle_first). */
    for (unsigned attempt = 1; attempt <= VJO_VOCR_TRIES; attempt++) {
        int wait = settle_on(&st) && (attempt > 1 || st.pol->settle_first);
        if (!wait || attempt == 1) bits_clear(&st);
        st.rows = st.y0 = st.first_next = 0;
        st.err = 0;
        st.held = st.verified = 0;
        st.cover_next = 0;
        stats->tries = attempt;
        stats->region_sum = VJO_FNV_INIT;
        if (wait && settle_wait(&st) < 0) {
            rc = stream_error(&st, VJO_E_SOURCE);
            break;
        }
        rc = recognize(platform, model_dir, mi, image, res, (unsigned char *)workspace + front, &l, &st,
                       text, text_cap, stats);
        if (rc != VJO_E_SCREEN_CHANGED || cancelled(image)) break;
    }
    if (rc) text[0] = '\0'; /* never publish a partial recognition */
    return rc;
}

int vjo_vocr_ocr_resident(const VjoPlatform *platform, const char *model_dir, const char *model,
                          const VjoGreySource *image, VjoVocrResident *resident,
                          void *workspace, size_t workspace_size, void *capture, size_t capture_size,
                          char *text, size_t text_cap, VjoVocrStats *stats)
{
    return run_job(platform, model_dir, model, image, resident, workspace, workspace_size, capture,
                   capture_size, 0, NULL, text, text_cap, stats);
}

int vjo_vocr_ocr_policy(const VjoPlatform *platform, const char *model_dir, const char *model,
                        const VjoGreySource *image, VjoVocrResident *resident,
                        void *workspace, size_t workspace_size, void *capture, size_t capture_size,
                        const VjoVocrPolicy *policy, char *text, size_t text_cap, VjoVocrStats *stats)
{
    return run_job(platform, model_dir, model, image, resident, workspace, workspace_size, capture,
                   capture_size, 0, policy, text, text_cap, stats);
}

static void *one_shot_alloc(void *ud, size_t bytes)
{
    (void)bytes;
    return ud; /* the front of the caller's workspace, sized by vjo_vocr_workspace_bytes */
}

int vjo_vocr_ocr(const VjoPlatform *platform, const char *model_dir, const char *model,
                 const VjoGreySource *image, void *workspace, size_t workspace_size,
                 void *capture, size_t capture_size, char *text, size_t text_cap, VjoVocrStats *stats)
{
    VjoVocrResident one;
    memset(&one, 0, sizeof(one));
    one.alloc = one_shot_alloc;
    one.ud = workspace;
    return run_job(platform, model_dir, model, image, &one, workspace, workspace_size, capture,
                   capture_size, 1, NULL, text, text_cap, stats);
}

/* ---- the blank-line check ---- */

/* The grey level at or below which `part` per mille of the n pixels lie
 * (n <= VJO_VOCR_INK_PIXELS: 32-bit arithmetic, no 64-bit division helper). */
static unsigned percentile(const uint32_t *hist, uint32_t n, unsigned part)
{
    uint32_t want = (n * part + 999u) / 1000u, acc = 0;
    unsigned v;
    if (!want) want = 1;
    for (v = 0; v < 255u; v++) {
        acc += hist[v];
        if (acc >= want) break;
    }
    return v;
}

static unsigned grey_step(unsigned a, unsigned b)
{
    return a > b ? a - b : b - a;
}

/* floor(n * 10000 / total) for n <= 2 total, total <= VJO_VOCR_INK_PIXELS, in
 * 32-bit steps of 100 (each remainder times 100 stays under 2^32). */
static uint32_t per_10000(uint32_t n, uint32_t total)
{
    uint32_t q = n / total, r = n % total, a, b;
    a = r * 100u / total;
    r = r * 100u % total;
    b = r * 100u / total;
    return q * 10000u + a * 100u + b;
}

int vjo_vocr_ink(const unsigned char *pixels, size_t stride, unsigned w, unsigned h, VjoVocrInk *out)
{
    uint32_t hist[256], total, edges = 0;
    unsigned lo, hi, e;
    memset(out, 0, sizeof(*out));
    if (!pixels || !w || !h || w > VJO_OCR_MAX_WIDTH || h > 0xFFFFu) return 0; /* unknown: never blank */
    total = (uint32_t)w * h;
    if (total > VJO_VOCR_INK_PIXELS) return 0;
    memset(hist, 0, sizeof(hist));
    for (unsigned y = 0; y < h; y++) {
        const unsigned char *r = pixels + (size_t)y * stride;
        for (unsigned x = 0; x < w; x++) hist[r[x]]++;
    }
    lo = percentile(hist, total, 20u);
    hi = percentile(hist, total, 980u);
    e = (hi - lo) / VJO_VOCR_EDGE_DIV < VJO_VOCR_EDGE_MIN ? VJO_VOCR_EDGE_MIN : (hi - lo) / VJO_VOCR_EDGE_DIV;
    /* An edge that scaling or blur softened rises over more than one pixel: the
     * step counts when the grey level gets there within VJO_VOCR_INK_SPAN pixels. */
    for (unsigned y = 0; y < h; y++) {
        const unsigned char *r = pixels + (size_t)y * stride;
        for (unsigned x = 0; x < w; x++) {
            unsigned v = r[x], across = 0, down = 0;
            for (unsigned s = 1; s <= VJO_VOCR_INK_SPAN && !(across && down); s++) {
                if (!across && x + s < w && grey_step(v, r[x + s]) >= e) across = 1;
                if (!down && y + s < h && grey_step(v, r[(size_t)s * stride + x]) >= e) down = 1;
            }
            edges += across + down;
        }
    }
    out->lo = (uint8_t)lo;
    out->hi = (uint8_t)hi;
    out->edge = (uint8_t)e;
    out->edges = (uint16_t)per_10000(edges, total); /* at most 2 per pixel: 20,000 */
    return vjo_vocr_blank(out);
}

int vjo_vocr_blank(const VjoVocrInk *ink)
{
    return ink->edges < VJO_VOCR_BLANK_EDGES;
}

/* ---- an RGBA image in memory as a grey source (host tools and tests) ---- */
static int rgba_strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    const VjoRgbaImage *img = (const VjoRgbaImage *)ud;
    (void)flags;
    if (y >= img->height || n > img->height - y) return VJO_E_SOURCE;
    for (unsigned r = 0; r < n; r++) {
        const unsigned char *p = img->pixels + (size_t)(y + r) * img->stride;
        unsigned char *d = dst + (size_t)r * stride;
        for (unsigned x = 0; x < img->width; x++, p += 4)
            d[x] = (unsigned char)vjo_grey(p[0], p[1], p[2]);
    }
    return (int)n;
}

void vjo_grey_source_rgba(const VjoRgbaImage *img, VjoGreySource *out)
{
    memset(out, 0, sizeof(*out));
    out->ud = (void *)img;
    out->width = img->width;
    out->height = img->height;
    out->strip = rgba_strip;
}
