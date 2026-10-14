/* vjo-vocr-parallel-bench REGIONS.bin MODEL_DIR MODEL [REPEAT]
 *
 * ocr-speed: the game worker's job (core/vocr_ocr.c) on real regions with real
 * weights, serially and with 1 and 2 helper threads (pthreads), and again with
 * the line cache: the texts must be identical, and the wall time per region is
 * printed for each. REGIONS.bin (tools/vocr_regions_pack.py): u32 count, then
 * per region u32 width, u32 height and width x height RGBA bytes. Host only:
 * x86 cores and threads say nothing about the Vita's, only that the texts are
 * the same and the work divides. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "net_posix.h"
#include "vocr_ocr.h"

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

typedef struct {
    pthread_t t[1 + VJO_VOCR_HELPERS_MAX];
    void (*fn[1 + VJO_VOCR_HELPERS_MAX])(void *);
    void *arg[1 + VJO_VOCR_HELPERS_MAX];
} Pool;
static void *tramp(void *p)
{
    void **a = (void **)p;
    Pool *pl = (Pool *)a[0];
    unsigned slot = (unsigned)(uintptr_t)a[1];
    free(p);
    pl->fn[slot](pl->arg[slot]);
    return NULL;
}
static int pool_run(void *ud, unsigned slot, void (*fn)(void *), void *arg)
{
    Pool *pl = (Pool *)ud;
    void **a = malloc(2 * sizeof(void *));
    if (!a) return -1;
    pl->fn[slot] = fn;
    pl->arg[slot] = arg;
    a[0] = pl;
    a[1] = (void *)(uintptr_t)slot;
    if (pthread_create(&pl->t[slot], NULL, tramp, a)) {
        free(a);
        return -1;
    }
    return 0;
}
static int pool_join(void *ud, unsigned slot)
{
    return pthread_join(((Pool *)ud)->t[slot], NULL) ? -1 : 0;
}
static uint64_t thread_us(void *ud)
{
    struct timespec ts;
    (void)ud;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void *res_alloc(void *ud, size_t bytes)
{
    void *p = NULL;
    (void)ud;
    return posix_memalign(&p, 16, bytes) ? NULL : p;
}
static VjoVocrResident res;
static int res_release(void *ud)
{
    (void)ud;
    free(res.data);
    return 0;
}

int main(int argc, char **argv)
{
    FILE *f;
    uint32_t count;
    unsigned repeat = argc > 4 ? (unsigned)atoi(argv[4]) : 1u;
    PosixPlatform pp;
    VjoPlatform plat;
    static char ref[4096], text[4096];
    static unsigned char capture[64 * 1024];
    static VjoVocrCache cache;
    double total[5] = {0, 0, 0, 0, 0};
    unsigned diffs = 0, lines = 0, regions = 0, dispatched = 0, cached = 0, frames = 0;
    if (argc < 4) {
        fprintf(stderr, "usage: %s REGIONS.bin MODEL_DIR MODEL [REPEAT]\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f || fread(&count, 4, 1, f) != 1) return 2;
    posix_platform_init(&pp, &plat);
    res.alloc = res_alloc;
    res.release = res_release;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t wh[2];
        unsigned char *px;
        VjoRgbaImage img;
        VjoGreySource src;
        if (fread(wh, 4, 2, f) != 2) return 2;
        px = malloc((size_t)wh[0] * wh[1] * 4);
        if (!px || fread(px, 4, (size_t)wh[0] * wh[1], f) != (size_t)wh[0] * wh[1]) return 2;
        img.pixels = px;
        img.width = wh[0];
        img.height = wh[1];
        img.stride = (size_t)wh[0] * 4;
        vjo_grey_source_rgba(&img, &src);
        /* 0: serial, 1: one helper, 2: two helpers, 3: two helpers + cache (filling), 4: the cache again */
        for (int run = 0; run < 5; run++) {
            unsigned helpers = run == 0 ? 0u : run == 1 ? 1u : 2u;
            Pool pl;
            VjoVocrPool pool = {helpers, pool_run, pool_join, thread_us, &pl};
            VjoVocrPolicy pol;
            VjoVocrStats st;
            size_t need = vjo_vocr_job_bytes_helpers(argv[3], wh[0], wh[1], helpers);
            void *ws = NULL;
            double t0;
            int rc = 0;
            memset(&pol, 0, sizeof pol);
            pol.pool = helpers ? &pool : NULL;
            pol.cache = run >= 3 ? &cache : NULL;
            if (run == 3) memset(&cache, 0, sizeof cache);
            if (!need || posix_memalign(&ws, 64, need)) return 3;
            t0 = now_s();
            for (unsigned k = 0; k < (run >= 3 ? 1u : repeat); k++)
                rc = vjo_vocr_ocr_policy(&plat, argv[2], argv[3], &src, &res, ws, need, capture,
                                         vjo_vocr_capture_bytes(wh[0], wh[1]), &pol, run ? text : ref, 4096, &st);
            total[run] += (now_s() - t0) / (run >= 3 ? 1u : repeat);
            free(ws);
            if (rc) {
                fprintf(stderr, "region %u run %d: rc %d\n", i, run, rc);
                return 4;
            }
            if (run && strcmp(text, ref)) diffs++;
            if (!run) {
                lines += st.recognized;
                frames += st.frames;
            }
            if (run == 2) dispatched += st.dispatched;
            if (run == 4) cached += st.cached;
        }
        regions++;
        free(px);
    }
    fclose(f);
    printf("{\"regions\": %u, \"lines\": %u, \"frames\": %u, \"text_differences\": %u, \"dispatched_2\": %u, "
           "\"cached_again\": %u, \"ms_per_region\": {\"serial\": %.3f, \"helpers_1\": %.3f, \"helpers_2\": %.3f, "
           "\"cache_first\": %.3f, \"cache_again\": %.3f}}\n",
           regions, lines, frames, diffs, dispatched, cached, 1e3 * total[0] / regions, 1e3 * total[1] / regions,
           1e3 * total[2] / regions, 1e3 * total[3] / regions, 1e3 * total[4] / regions);
    return diffs ? 1 : 0;
}
