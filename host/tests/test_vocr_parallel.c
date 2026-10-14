/* ocr-speed: parallel lines and the line cache (core/vocr_ocr.h VjoVocrPool,
 * VjoVocrCache) with real vocr_run calls. The test builds a tiny valid weights
 * blob (the same layer kinds as the release models: stem, depthwise, pointwise,
 * the 2x1 collapse, 1x5 temporal depthwise with a residual pointwise, the head),
 * pins it in the model table (test_parallel_models.h), and runs the job on a
 * synthetic region of several lines:
 *  - serial against 1 and 2 helpers, with real threads, with a pool that runs
 *    each line at once, and with one whose threads never start: the same text,
 *    frames and per-line results in line order, every helper joined;
 *  - the cache: a second job on the same screen runs no recognizer and gives
 *    the same text; one changed line is the only one recognized again; weights
 *    read from the file again empty it;
 *  - cancellation and a failed join while helpers run; fewer helpers when the
 *    workspace holds fewer; the busy hook around the recognitions only. */
#include "acutest.h"
#include "net_posix.h"
#include "vocr.h"
#include "vocr_ocr.h"
#include "../../core/fbgrey.h"
#include <bearssl_hash.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void test_pin_model(uint32_t bytes, const unsigned char sha256[32], uint32_t arena_bytes);

/* ------------------------------------------------------------ the tiny blob
 * (vita-vn-ocr runtime/tests/test_vocr.c's builder: height 8, stride 4) */
static unsigned char blob[1 << 16] __attribute__((aligned(16)));
static size_t blob_pos;
static uint32_t lcg = 12345u;
enum { CLASSES = 12 };

static float frand(void)
{
    lcg = lcg * 1664525u + 1013904223u;
    return (float)((lcg >> 8) & 0xFFFF) / 32768.0f - 1.0f;
}
static void wr16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void wr32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static uint32_t put(const void *data, size_t n)
{
    uint32_t off = (uint32_t)blob_pos;
    memcpy(blob + blob_pos, data, n);
    blob_pos = (blob_pos + n + 15u) & ~(size_t)15u;
    return off;
}
static uint32_t put_floats(unsigned n, float scale)
{
    uint32_t off = (uint32_t)blob_pos;
    for (unsigned i = 0; i < n; i++) {
        float v = frand() * scale;
        memcpy(blob + blob_pos + 4u * i, &v, sizeof v);
    }
    blob_pos = (blob_pos + 4u * n + 15u) & ~(size_t)15u;
    return off;
}
static uint32_t crc32_ref(const unsigned char *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
static void op_rec(unsigned i, unsigned type, unsigned flags, unsigned kh, unsigned kw, unsigned sh, unsigned sw,
                   unsigned ph, unsigned pw, unsigned cin, unsigned cout, unsigned hin, unsigned hout, uint32_t w_off,
                   uint32_t w_bytes, uint32_t b_off, uint32_t s_off, unsigned k, unsigned cout_pad)
{
    unsigned char *r = blob + 64 + 64 * i;
    memset(r, 0, 64);
    r[0] = (unsigned char)type; r[1] = (unsigned char)flags; r[2] = (unsigned char)kh; r[3] = (unsigned char)kw;
    r[4] = (unsigned char)sh; r[5] = (unsigned char)sw; r[6] = (unsigned char)ph; r[7] = (unsigned char)pw;
    wr16(r + 8, cin); wr16(r + 10, cout); wr16(r + 12, hin); wr16(r + 14, hout);
    wr32(r + 16, w_off); wr32(r + 20, w_bytes); wr32(r + 24, b_off); wr32(r + 28, s_off);
    wr16(r + 32, k); wr16(r + 34, cout_pad);
}
static void mat_op(unsigned i, unsigned type, unsigned flags, unsigned kh, unsigned cin, unsigned cout, unsigned hin,
                   unsigned hout)
{
    unsigned k = kh * cin, cout_pad = (cout + 7u) & ~7u, n = cout_pad * k;
    uint32_t w_off = (uint32_t)blob_pos, s_off, b_off = 0;
    for (unsigned j = 0; j < n; j++) blob[blob_pos + j] = (unsigned char)(signed char)(int)(frand() * 127.0f);
    blob_pos = (blob_pos + n + 15u) & ~(size_t)15u;
    s_off = (uint32_t)blob_pos;
    for (unsigned j = 0; j < cout_pad; j++) {
        float sc = 0.01f + 0.004f * (float)(j % 5);
        memcpy(blob + blob_pos + 4u * j, &sc, sizeof sc);
    }
    blob_pos = (blob_pos + 4u * cout_pad + 15u) & ~(size_t)15u;
    if (flags & 4) b_off = put_floats(cout_pad, 0.1f);
    op_rec(i, type, flags | 8, kh, 1, 1, 1, 0, 0, cin, cout, hin, hout, w_off, n, b_off, s_off, k, cout_pad);
}
static size_t build_blob(void)
{
    unsigned char vocab[2 * (CLASSES + 1) + CLASSES];
    unsigned pos = 0, n_ops = 7;
    uint32_t off;
    memset(blob, 0, sizeof blob);
    blob_pos = 64 + 64 * n_ops;
    lcg = 12345u;
    off = put_floats(9 * 8, 0.6f);
    op_rec(0, 1, 1 | 4, 3, 3, 2, 2, 1, 1, 1, 8, 8, 4, off, 9 * 8 * 4, put_floats(8, 0.1f), 0, 9, 8);
    off = put_floats(9 * 8, 0.6f);
    op_rec(1, 2, 1 | 4, 3, 3, 2, 2, 1, 1, 8, 8, 4, 2, off, 9 * 8 * 4, put_floats(8, 0.1f), 0, 9, 8);
    mat_op(2, 3, 1 | 4, 1, 8, 16, 2, 2);
    mat_op(3, 3, 1 | 4 | 16, 2, 16, 16, 2, 1);
    off = put_floats(5 * 16, 0.6f);
    op_rec(4, 2, 1 | 4, 1, 5, 1, 1, 0, 2, 16, 16, 1, 1, off, 5 * 16 * 4, put_floats(16, 0.1f), 0, 5, 16);
    mat_op(5, 3, 1 | 2 | 4, 1, 16, 16, 1, 1);
    mat_op(6, 4, 4, 1, 16, CLASSES, 1, 1);
    wr16(vocab, 0);
    wr16(vocab + 2, 0);
    for (unsigned i = 1; i < CLASSES; i++) {
        vocab[2 * (CLASSES + 1) + pos++] = (unsigned char)('a' + i - 1);
        wr16(vocab + 2 * (i + 1), pos);
    }
    off = put(vocab, 2 * (CLASSES + 1) + pos);
    memcpy(blob, "VOCR", 4);
    wr16(blob + 4, 1); wr16(blob + 6, 64); wr32(blob + 8, (uint32_t)blob_pos); wr32(blob + 12, 1u);
    wr16(blob + 16, 8); wr16(blob + 18, 256); wr16(blob + 20, 4); wr16(blob + 22, n_ops);
    wr32(blob + 24, CLASSES); wr32(blob + 28, 64); wr32(blob + 32, off); wr32(blob + 36, pos);
    wr32(blob + 40, crc32_ref(blob + 64, blob_pos - 64));
    return blob_pos;
}

static char model_dir[256];

/* The blob in a file, pinned in the model table. */
static void setup_model(void)
{
    static int done;
    unsigned char sha[32];
    br_sha256_context h;
    vocr_model m;
    vocr_opts o;
    size_t n;
    FILE *f;
    char path[300];
    if (done) return;
    n = build_blob();
    TEST_ASSERT(vocr_load(&m, blob, n) == VOCR_OK);
    memset(&o, 0, sizeof o);
    o.mode = VOCR_MODE_W8I;
    snprintf(model_dir, sizeof model_dir, "/tmp/vjo-par-%d", (int)getpid());
    mkdir(model_dir, 0700);
    snprintf(path, sizeof path, "%s/TINY_w8.vocr", model_dir);
    f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fwrite(blob, 1, n, f) == n);
    fclose(f);
    br_sha256_init(&h);
    br_sha256_update(&h, blob, n);
    br_sha256_out(&h, sha);
    test_pin_model((uint32_t)n, sha, (uint32_t)vocr_arena_bytes(&m, &o));
    done = 1;
}

/* ------------------------------------------------------------ the region */
#define W 420
#define H 200
#define LINES 5
static unsigned char pixels[W * H * 4];
static unsigned strips_read, cancel_after;
static int cancel_now;

static int strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    (void)ud; (void)flags;
    if (y + n > H) return VJO_E_SOURCE;
    strips_read++;
    if (cancel_after && strips_read >= cancel_after) __atomic_store_n(&cancel_now, 1, __ATOMIC_RELEASE);
    for (unsigned r = 0; r < n; r++)
        for (unsigned x = 0; x < W; x++) {
            const unsigned char *p = pixels + ((size_t)(y + r) * W + x) * 4;
            dst[(size_t)r * stride + x] = (unsigned char)vjo_grey(p[0], p[1], p[2]);
        }
    return (int)n;
}
static int is_cancelled(void *ud) { (void)ud; return __atomic_load_n(&cancel_now, __ATOMIC_ACQUIRE); }
static VjoGreySource im = {NULL, W, H, strip, is_cancelled, NULL};
static unsigned char capture[64 * 1024];

/* LINES lines of glyph-like strokes, each with its own spacing and length (seed
 * moves line `which`'s strokes, so its pixels change and the others' do not). */
static void draw(unsigned which, unsigned seed)
{
    for (size_t i = 0; i < sizeof(pixels); i += 4) {
        pixels[i] = pixels[i + 1] = pixels[i + 2] = 20;
        pixels[i + 3] = 255;
    }
    for (unsigned l = 0; l < LINES; l++) {
        unsigned y0 = 12 + l * 38, step = 5 + l, len = W - 30 - 40 * l, shift = l == which ? seed : 0;
        for (unsigned y = y0; y < y0 + 24; y++)
            for (unsigned x = 15 + shift; x < len; x += step)
                for (unsigned k = 0; k < 2 + (x / 7 + l) % 2; k++) {
                    unsigned char *p = pixels + ((size_t)y * W + x + k) * 4;
                    p[0] = p[1] = p[2] = (unsigned char)(200 + 7 * ((x / 11 + y) % 5));
                }
    }
}

/* ------------------------------------------------------------ pools */
typedef struct {
    pthread_t t[1 + VJO_VOCR_HELPERS_MAX];
    void (*fn[1 + VJO_VOCR_HELPERS_MAX])(void *);
    void *arg[1 + VJO_VOCR_HELPERS_MAX];
    int live[1 + VJO_VOCR_HELPERS_MAX];
    int mode;               /* 0 threads, 1 at once (in run), 2 never starts, 3 join fails */
    unsigned runs, joins, max_live, now_live;
    pthread_mutex_t mu;
} Pool;

static void *tramp(void *p)
{
    Pool *pl = ((void **)p)[0];
    unsigned slot = (unsigned)(uintptr_t)((void **)p)[1];
    free(p);
    pl->fn[slot](pl->arg[slot]);
    return NULL;
}
static int pool_run(void *ud, unsigned slot, void (*fn)(void *), void *arg)
{
    Pool *pl = (Pool *)ud;
    void **a;
    TEST_CHECK(slot >= 1 && slot <= VJO_VOCR_HELPERS_MAX && !pl->live[slot]);
    if (pl->mode == 2) return -1;
    pl->fn[slot] = fn;
    pl->arg[slot] = arg;
    pl->live[slot] = 1;
    pl->runs++;
    pthread_mutex_lock(&pl->mu);
    if (++pl->now_live > pl->max_live) pl->max_live = pl->now_live;
    pthread_mutex_unlock(&pl->mu);
    if (pl->mode == 1) {
        fn(arg);
        return 0;
    }
    a = malloc(2 * sizeof(void *));
    a[0] = pl;
    a[1] = (void *)(uintptr_t)slot;
    return pthread_create(&pl->t[slot], NULL, tramp, a) ? -1 : 0;
}
static int pool_join(void *ud, unsigned slot)
{
    Pool *pl = (Pool *)ud;
    TEST_CHECK(pl->live[slot]);
    if (pl->mode == 3) return -1;
    if (pl->mode == 0) pthread_join(pl->t[slot], NULL);
    pl->live[slot] = 0;
    pl->joins++;
    pthread_mutex_lock(&pl->mu);
    pl->now_live--;
    pthread_mutex_unlock(&pl->mu);
    return 0;
}
static uint64_t thread_us(void *ud)
{
    struct timespec ts;
    (void)ud;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* ------------------------------------------------------------ the job */
static int busy_on, busy_calls;
static void busy(void *ud, int on)
{
    (void)ud;
    TEST_CHECK(on != busy_on); /* on, then off, never twice */
    busy_on = on;
    busy_calls++;
}

typedef struct {
    int rc;
    char text[VJO_OCR_TEXT_CAP];
    VjoVocrStats st;
} Run;

static PosixPlatform pp;
static VjoPlatform plat;
static VjoVocrResident res;
static void *res_alloc(void *ud, size_t bytes)
{
    void *p = NULL;
    (void)ud;
    return posix_memalign(&p, 16, bytes) ? NULL : p;
}
static void *res_mem;
static int res_release(void *ud)
{
    (void)ud;
    free(res.data);
    return 0;
}

static uint32_t job_variant; /* VjoVocrPolicy.cache_variant of the next jobs */
static void run_job(Run *r, Pool *pl, unsigned helpers, VjoVocrCache *cache, size_t ws_bytes)
{
    VjoVocrPolicy pol;
    VjoVocrPool pool = {helpers, pool_run, pool_join, thread_us, pl};
    size_t need = ws_bytes ? ws_bytes : vjo_vocr_job_bytes_helpers("TINY_w8.vocr", W, H, helpers);
    void *ws = NULL;
    TEST_ASSERT(need > 0 && posix_memalign(&ws, 64, need) == 0);
    memset(&pol, 0, sizeof pol);
    pol.pool = pl ? &pool : NULL;
    pol.cache = cache;
    pol.cache_variant = job_variant;
    pol.busy = busy;
    busy_on = 0;
    strips_read = 0;
    memset(r, 0, sizeof(*r));
    r->rc = vjo_vocr_ocr_policy(&plat, model_dir, "TINY_w8.vocr", &im, &res, ws, need, capture,
                                vjo_vocr_capture_bytes(W, H), &pol, r->text, sizeof(r->text), &r->st);
    TEST_CHECK(!busy_on); /* switched off on every path */
    if (!r->st.stuck) free(ws);
    (void)res_mem;
}

static void setup(void)
{
    setup_model();
    posix_platform_init(&pp, &plat);
    if (!res.alloc) {
        res.alloc = res_alloc;
        res.release = res_release;
    }
    cancel_now = 0;
    cancel_after = 0;
}

static void same_lines(const Run *a, const Run *b)
{
    TEST_CHECK(a->st.recognized == b->st.recognized);
    for (unsigned i = 0; i < a->st.recognized && i < b->st.recognized; i++) {
        TEST_CHECK(a->st.line[i].width == b->st.line[i].width && a->st.line[i].height == b->st.line[i].height);
        TEST_CHECK(a->st.line[i].frames == b->st.line[i].frames);
    }
}

static void test_parallel_equals_serial(void)
{
    static Run serial, par;
    setup();
    draw(LINES, 0);
    run_job(&serial, NULL, 0, NULL, 0);
    TEST_ASSERT(serial.rc == VJO_OK);
    TEST_CHECK(serial.st.lines == LINES && serial.st.recognized == LINES && serial.st.frames > 0);
    TEST_CHECK(serial.st.helpers == 0 && serial.st.dispatched == 0 && serial.text[0]);
    TEST_MSG("serial: %u lines, %u frames, \"%s\"", serial.st.lines, serial.st.frames, serial.text);
    for (int mode = 0; mode < 3; mode++)
        for (unsigned helpers = 1; helpers <= VJO_VOCR_HELPERS_MAX; helpers++) {
            Pool pl;
            memset(&pl, 0, sizeof pl);
            pthread_mutex_init(&pl.mu, NULL);
            pl.mode = mode;
            run_job(&par, &pl, helpers, NULL, 0);
            TEST_CHECK(par.rc == VJO_OK);
            TEST_CHECK(!strcmp(par.text, serial.text));
            TEST_CHECK(par.st.frames == serial.st.frames && par.st.lines == serial.st.lines);
            same_lines(&par, &serial);
            TEST_CHECK(par.st.helpers == helpers);
            TEST_CHECK(pl.runs == pl.joins && pl.now_live == 0);
            if (mode == 2) TEST_CHECK(par.st.dispatched == 0 && par.st.run_failures > 0);
            else TEST_CHECK(par.st.dispatched > 0 && par.st.dispatched == pl.runs);
            if (mode == 0) TEST_CHECK(pl.max_live >= 1 && pl.max_live <= helpers);
            {
                unsigned on_helpers = 0;
                for (unsigned i = 0; i < par.st.recognized; i++) on_helpers += par.st.line[i].slot != 0;
                TEST_CHECK(on_helpers == par.st.dispatched + par.st.run_failures);
            }
            TEST_MSG("mode %d, %u helpers: dispatched %u, run failures %u, max live %u, \"%s\"", mode, helpers,
                     par.st.dispatched, par.st.run_failures, pl.max_live, par.text);
            pthread_mutex_destroy(&pl.mu);
        }
}

static void test_cache(void)
{
    static VjoVocrCache cache;
    static Run a, b, c, ref;
    Pool pl;
    setup();
    memset(&cache, 0, sizeof cache);
    memset(&pl, 0, sizeof pl);
    pthread_mutex_init(&pl.mu, NULL);
    draw(LINES, 0);
    run_job(&a, &pl, 2, &cache, 0);
    TEST_ASSERT(a.rc == VJO_OK);
    TEST_CHECK(a.st.cached == 0 && busy_calls >= 2);
    /* The same screen again: every line from the cache, no recognizer, no busy. */
    busy_calls = 0;
    run_job(&b, &pl, 2, &cache, 0);
    TEST_CHECK(b.rc == VJO_OK && !strcmp(b.text, a.text));
    TEST_CHECK(b.st.cached == LINES && b.st.frames == 0 && b.st.dispatched == 0 && busy_calls == 0);
    TEST_CHECK(!b.st.model_loaded && b.st.model_cached);
    for (unsigned i = 0; i < b.st.recognized; i++) TEST_CHECK(b.st.line[i].cached && b.st.line[i].frames);
    same_lines(&b, &a);
    /* One line changed: only it is recognized, and the text is the serial job's. */
    draw(2, 3);
    run_job(&ref, NULL, 0, NULL, 0);
    run_job(&c, &pl, 2, &cache, 0);
    TEST_CHECK(c.rc == VJO_OK && !strcmp(c.text, ref.text));
    TEST_CHECK(c.st.cached == LINES - 1 && c.st.frames > 0 && c.st.frames < a.st.frames);
    TEST_CHECK(!c.st.line[2].cached && c.st.line[1].cached && c.st.line[3].cached);
    TEST_MSG("changed line 2: \"%s\" vs \"%s\", cached %u", c.text, a.text, c.st.cached);
    /* Another cache variant (another decoder or cascade setting): no line from the cache;
     * the same variant again: every line (the 8 entries hold this job's 5, the least
     * recently used of the other variant's went first). */
    job_variant = 7;
    run_job(&b, &pl, 2, &cache, 0);
    TEST_CHECK(b.rc == VJO_OK && b.st.cached == 0 && !strcmp(b.text, ref.text));
    run_job(&b, &pl, 2, &cache, 0);
    TEST_CHECK(b.rc == VJO_OK && b.st.cached == LINES && !strcmp(b.text, ref.text));
    /* A line's VjoVocrLineStat.reserved comes back with its cached text. */
    for (unsigned i = 0; i < VJO_VOCR_CACHE_LINES; i++)
        if (cache.line[i].key) cache.line[i].line_flags = 0x4000;
    run_job(&b, &pl, 2, &cache, 0);
    TEST_CHECK(b.rc == VJO_OK && b.st.cached == LINES);
    for (unsigned i = 0; i < b.st.recognized; i++) TEST_CHECK(b.st.line[i].reserved == 0x4000);
    /* Weights read from the file again (another file stamp): the cache is emptied. */
    vjo_vocr_resident_drop(&res);
    run_job(&b, &pl, 2, &cache, 0);
    TEST_CHECK(b.rc == VJO_OK && b.st.model_loaded && b.st.cached == 0 && !strcmp(b.text, ref.text));
    pthread_mutex_destroy(&pl.mu);
}

static void test_cancel_and_failed_join(void)
{
    static Run r;
    Pool pl;
    setup();
    draw(LINES, 0);
    memset(&pl, 0, sizeof pl);
    pthread_mutex_init(&pl.mu, NULL);
    /* Cancelled while lines are fetched (helpers running): every helper joined. */
    cancel_after = 6;
    run_job(&r, &pl, 2, NULL, 0);
    TEST_CHECK(r.rc == VJO_E_CANCELLED && !r.text[0] && !r.st.stuck);
    TEST_CHECK(pl.runs == pl.joins && pl.now_live == 0);
    TEST_MSG("cancel: rc %d, runs %u joins %u", r.rc, pl.runs, pl.joins);
    /* A join that fails: the job fails and says the workspace may still be in use. */
    setup();
    memset(&pl, 0, sizeof pl);
    pthread_mutex_init(&pl.mu, NULL);
    pl.mode = 3;
    run_job(&r, &pl, 1, NULL, 0);
    TEST_CHECK(r.rc == VJO_E_OCR_UNAVAILABLE && r.st.stuck && !r.text[0]);
    for (unsigned k = 1; k <= VJO_VOCR_HELPERS_MAX; k++)
        if (pl.live[k]) pthread_join(pl.t[k], NULL);
}

static void test_sizes_and_fewer_helpers(void)
{
    static Run r;
    Pool pl;
    size_t h0, h1, h2, one;
    setup();
    h0 = vjo_vocr_job_bytes_helpers("TINY_w8.vocr", W, H, 0);
    h1 = vjo_vocr_job_bytes_helpers("TINY_w8.vocr", W, H, 1);
    h2 = vjo_vocr_job_bytes_helpers("TINY_w8.vocr", W, H, 2);
    one = vjo_vocr_helper_bytes("TINY_w8.vocr");
    TEST_CHECK(h0 == vjo_vocr_job_bytes("TINY_w8.vocr", W, H));
    TEST_CHECK(h1 == h0 + ((VJO_OCR_TEXT_CAP + 15u) & ~15u) + one && h2 == h1 + one);
    TEST_CHECK(!vjo_vocr_job_bytes_helpers("TINY_w8.vocr", W, H, VJO_VOCR_HELPERS_MAX + 1));
    TEST_CHECK(!vjo_vocr_helper_bytes("other.vocr"));
    /* Arena, context, a strip-sized line and a text each. */
    TEST_CHECK(one == ((vjo_vocr_model("TINY_w8.vocr")->arena_bytes + 15u) & ~(size_t)15u) +
                          ((sizeof(vocr_ctx) + 15u) & ~(size_t)15u) + VJO_VOCR_STRIP_BYTES + VJO_OCR_TEXT_CAP);
    /* A workspace that holds one helper: the job uses one. */
    draw(LINES, 0);
    memset(&pl, 0, sizeof pl);
    pthread_mutex_init(&pl.mu, NULL);
    run_job(&r, &pl, 2, NULL, h1);
    TEST_CHECK(r.rc == VJO_OK && r.st.helpers == 1 && r.st.workspace == h1);
    run_job(&r, &pl, 2, NULL, h1 - 1);
    TEST_CHECK(r.rc == VJO_OK && r.st.helpers == 0 && r.st.dispatched == 0);
    /* A pinned arena smaller than the plan: a helper refuses its context (no overrun) */
    {
        const VjoVocrModel *m = vjo_vocr_model("TINY_w8.vocr");
        uint32_t arena = m->arena_bytes;
        unsigned char sha[32];
        memcpy(sha, m->sha256, 32);
        test_pin_model(m->bytes, sha, arena - 64u);
        run_job(&r, &pl, 2, NULL, 0);
        TEST_CHECK(r.rc == VJO_E_OCR_MODEL || (r.rc == VJO_OK && r.st.dispatched == 0));
        TEST_MSG("rc %d, dispatched %u", r.rc, r.st.dispatched);
        test_pin_model(m->bytes, sha, arena);
        run_job(&r, &pl, 2, NULL, 0);
        TEST_CHECK(r.rc == VJO_OK && r.st.dispatched > 0);
    }
    pthread_mutex_destroy(&pl.mu);
}

TEST_LIST = {
    {"parallel_equals_serial", test_parallel_equals_serial},
    {"cache", test_cache},
    {"cancel_and_failed_join", test_cancel_and_failed_join},
    {"sizes_and_fewer_helpers", test_sizes_and_fewer_helpers},
    {NULL, NULL},
};
