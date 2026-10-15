/* shell/log_vita.c, the real file, on an in-memory file system: batched lines
 * (vjo_log_batch_begin/end) leave log.txt, log.old.txt and status.txt byte for
 * byte as the previous logger (main bcb780e's, unchanged since 4a7ae4d,
 * verbatim below as ref_*) leaves them line by line, through status.txt's
 * 64 KiB cap (a line that does not fit is skipped, a later shorter one is not),
 * log.txt's 1 MiB rotation, another thread logging inside a batch, a batch
 * fuller than its buffer, and logging before vjo_log_configure; with one open
 * per file per batch. Failed opens and writes lose lines but never the
 * logger's count of what each file holds. */
#include "acutest.h"

#include <stdlib.h>

#include <psp2/log_stubs.h>
#include "../../shell/log_vita.c"

/* ---- the in-memory file system ---- */

#define FILES 8
#define FDS 8
typedef struct {
    char path[96];
    char *data;
    size_t size, cap;
    int used;
} File;
static File files[FILES];
static struct { int file, open; } fds[FDS];
static unsigned opens[FILES], writes, renames, thread = 1, formats, log_path_opens, status_path_opens, status_writes;
static int fail_writes, fail_opens;

static File *lookup(const char *path, int create)
{
    for (int i = 0; i < FILES; i++)
        if (files[i].used && !strcmp(files[i].path, path)) return &files[i];
    if (!create) return NULL;
    for (int i = 0; i < FILES; i++)
        if (!files[i].used) {
            memset(&files[i], 0, sizeof(files[i]));
            files[i].used = 1;
            snprintf(files[i].path, sizeof(files[i].path), "%s", path);
            return &files[i];
        }
    return NULL;
}

SceUID sceIoOpen(const char *path, int flags, int mode)
{
    File *f;
    (void)mode;
    if (fail_opens) return (int)0x80010018u; /* EMFILE */
    f = lookup(path, (flags & SCE_O_CREAT) != 0);
    if (!f) return -1;
    log_path_opens += !strcmp(path, VJO_LOG_PATH);
    status_path_opens += !strcmp(path, VJO_STATUS_PATH);
    if (flags & SCE_O_TRUNC) f->size = 0;
    for (int i = 0; i < FDS; i++)
        if (!fds[i].open) {
            fds[i].open = 1;
            fds[i].file = (int)(f - files);
            opens[f - files]++;
            return i + 3;
        }
    return -1;
}
int sceIoClose(SceUID fd) { fds[fd - 3].open = 0; return 0; }
int sceIoWrite(SceUID fd, const void *data, SceSize len)
{
    File *f = &files[fds[fd - 3].file];
    writes++;
    status_writes += !strcmp(f->path, VJO_STATUS_PATH);
    if (fail_writes) return -1;
    if (f->size + len > f->cap) {
        f->cap = (f->size + len) * 2;
        f->data = realloc(f->data, f->cap);
    }
    memcpy(f->data + f->size, data, len);
    f->size += len;
    return (int)len;
}
int sceIoGetstat(const char *path, SceIoStat *st)
{
    File *f = lookup(path, 0);
    if (!f) return (int)0x80010002u;
    st->st_mode = 0;
    st->st_size = (SceOff)f->size;
    return 0;
}
int sceIoRemove(const char *path)
{
    File *f = lookup(path, 0);
    if (!f) return (int)0x80010002u;
    free(f->data);
    memset(f, 0, sizeof(*f));
    return 0;
}
int sceIoRename(const char *from, const char *to)
{
    File *f = lookup(from, 0);
    if (!f || lookup(to, 0)) return -1;
    renames++;
    snprintf(f->path, sizeof(f->path), "%s", to);
    return 0;
}
int sceIoMkdir(const char *path, int mode) { (void)path; (void)mode; return 0; }

int sceClibVsnprintf(char *dst, SceSize cap, const char *fmt, va_list ap)
{
    formats++;
    return vsnprintf(dst, cap, fmt, ap);
}
/* The two mutexes (VjoLog, VjoStatus), each held by at most one thread: a lock of a mutex
 * the caller holds already (the SDK's are not recursive), an unlock of one it does not hold,
 * or one still held when a logger call returns (lock_check) fails the test. */
static SceUID next_mutex = 41;
static struct { SceUID uid; SceUID holder; } mutexes[4];
static unsigned mutex_errors;
static int mutex_slot(SceUID uid)
{
    for (int i = 0; i < 4; i++)
        if (mutexes[i].uid == uid) return i;
    return -1;
}
SceUID sceKernelCreateMutex(const char *name, unsigned attr, int count, void *opt)
{
    (void)name; (void)attr; (void)count; (void)opt;
    for (int i = 0; i < 4; i++)
        if (!mutexes[i].uid) { mutexes[i].uid = next_mutex; mutexes[i].holder = 0; return next_mutex++; }
    return -1;
}
int sceKernelDeleteMutex(SceUID uid)
{
    int i = mutex_slot(uid);
    if (i < 0 || mutexes[i].holder) mutex_errors++;
    if (i >= 0) mutexes[i].uid = 0;
    return 0;
}
int sceKernelLockMutex(SceUID uid, int count, unsigned *timeout)
{
    int i = mutex_slot(uid);
    (void)count; (void)timeout;
    if (i < 0 || mutexes[i].holder) { mutex_errors++; return -1; } /* a deadlock on the console */
    mutexes[i].holder = (SceUID)thread;
    return 0;
}
int sceKernelUnlockMutex(SceUID uid, int count)
{
    int i = mutex_slot(uid);
    (void)count;
    if (i < 0 || mutexes[i].holder != (SceUID)thread) { mutex_errors++; return -1; }
    mutexes[i].holder = 0;
    return 0;
}
/* After every logger call: nothing held, nothing misused. */
static void lock_check(void)
{
    for (int i = 0; i < 4; i++)
        if (mutexes[i].uid && mutexes[i].holder) mutex_errors++;
    TEST_CHECK(mutex_errors == 0);
    TEST_MSG("%u mutex errors", mutex_errors);
}
SceUID sceKernelGetThreadId(void) { return (SceUID)thread; }

static char udp[1 << 20];
static size_t udp_used;
static unsigned datagrams;
int sceNetSocket(const char *name, int domain, int type, int protocol)
{
    (void)name; (void)domain; (void)type; (void)protocol;
    return 77;
}
int sceNetSocketClose(int fd) { (void)fd; return 0; }
int sceNetSendto(int fd, const void *buf, unsigned len, int flags, const SceNetSockaddr *to, unsigned tolen)
{
    (void)fd; (void)flags; (void)to; (void)tolen;
    memcpy(udp + udp_used, buf, len);
    udp_used += len;
    datagrams++;
    return (int)len;
}
uint16_t sceNetHtons(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
int sceNetInetPton(int af, const char *src, void *dst) { (void)af; (void)src; memset(dst, 0, 4); return 1; }

/* ---- the reference: shell/log_vita.c at 4a7ae4d, the file and status paths, verbatim ---- */

static SceOff ref_file_size = -1;
static int ref_file_on, ref_status_size = -1;

static void ref_file_append(const char *line, int len)
{
    SceUID fd;
    if (ref_file_size < 0) {
        SceIoStat st;
        ref_file_size = sceIoGetstat(VJO_LOG_PATH, &st) >= 0 ? st.st_size : 0;
    }
    if (ref_file_size + len > LOG_FILE_CAP) {
        sceIoRemove(VJO_LOG_OLD_PATH);
        sceIoRename(VJO_LOG_PATH, VJO_LOG_OLD_PATH);
        ref_file_size = 0;
    }
    fd = sceIoOpen(VJO_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0)
        return;
    if (sceIoWrite(fd, line, len) == len)
        ref_file_size += len;
    sceIoClose(fd);
}

static void ref_status_append(const char *line, int len)
{
    SceUID fd;
    if (ref_status_size < 0 || ref_status_size + len > STATUS_CAP)
        return;
    fd = sceIoOpen(VJO_STATUS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) {
        if (sceIoWrite(fd, line, len) == len)
            ref_status_size += len;
        sceIoClose(fd);
    }
}

static void ref_log_raw(const char *line, int len, int configured)
{
    if (len <= 0)
        return;
    ref_status_append(line, len);
    if (!configured)
        return;
    sceNetSendto(77, line, len, 0, NULL, 0);
    if (ref_file_on)
        ref_file_append(line, len);
}

/* ---- the scenario ---- */

typedef struct {
    char *log, *old, *status, *udp;
    size_t log_n, old_n, status_n, udp_n;
    unsigned log_opens, status_opens, datagrams;
} Snapshot;

static void fs_reset(void)
{
    for (int i = 0; i < FILES; i++) {
        free(files[i].data);
        memset(&files[i], 0, sizeof(files[i]));
    }
    memset(fds, 0, sizeof(fds));
    memset(opens, 0, sizeof(opens));
    memset(mutexes, 0, sizeof(mutexes));
    mutex_errors = 0;
    udp_used = datagrams = writes = renames = formats = log_path_opens = status_path_opens = status_writes = 0;
    fail_writes = fail_opens = 0;
    thread = 1;
}

static char *copy(const File *f, size_t *n)
{
    char *p = malloc(f && f->size ? f->size : 1);
    *n = f ? f->size : 0;
    if (f && f->size) memcpy(p, f->data, f->size);
    return p;
}

static void snap(Snapshot *s)
{
    File *l = lookup(VJO_LOG_PATH, 0), *o = lookup(VJO_LOG_OLD_PATH, 0), *st = lookup(VJO_STATUS_PATH, 0);
    s->log = copy(l, &s->log_n);
    s->old = copy(o, &s->old_n);
    s->status = copy(st, &s->status_n);
    s->udp = malloc(udp_used ? udp_used : 1);
    memcpy(s->udp, udp, udp_used);
    s->udp_n = udp_used;
    s->log_opens = l ? opens[l - files] : 0;
    s->status_opens = st ? opens[st - files] : 0;
    s->datagrams = datagrams;
}

static int same(const char *a, size_t an, const char *b, size_t bn)
{
    return an == bn && !memcmp(a, b, an);
}

static void snap_free(Snapshot *s)
{
    free(s->log); free(s->old); free(s->status); free(s->udp);
}

/* A log line of length len (2..255) with its index in it. */
static int make_line(char *buf, unsigned i, unsigned len)
{
    int n = snprintf(buf, 256, "line %u:", i);
    while ((unsigned)n < len - 1) {
        buf[n] = (char)('a' + (i + (unsigned)n) % 26);
        n++;
    }
    buf[len - 1] = '\n';
    return (int)len;
}

static uint32_t rng = 1;
static unsigned next(unsigned mod)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng % mod;
}

/* The same lines through the reference line by line, and through the new
 * logger with random batches (and threads); prefill: bytes already in log.txt. */
static void run(unsigned lines, size_t prefill, int file_on_, int udp_on, int threads, int bcap, Snapshot *ref,
                Snapshot *got)
{
    char line[256];
    char *fill = calloc(1, prefill + 1);
    memset(fill, 'x', prefill);
    /* reference */
    fs_reset();
    if (prefill) {
        File *f = lookup(VJO_LOG_PATH, 1);
        f->data = malloc(prefill); memcpy(f->data, fill, prefill); f->size = f->cap = prefill;
    }
    ref_file_size = -1; ref_file_on = file_on_; ref_status_size = 0;
    { SceUID fd = sceIoOpen(VJO_STATUS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666); sceIoClose(fd); }
    rng = 7;
    for (unsigned i = 0; i < lines; i++) {
        int n = make_line(line, i, 2 + next(254));
        (void)next(6); (void)next(3); /* keep the stream aligned with the batched run */
        if (udp_on) ref_log_raw(line, n, 1);
        else { ref_status_append(line, n); if (ref_file_on) ref_file_append(line, n); }
    }
    snap(ref);
    /* the new logger */
    fs_reset();
    if (prefill) {
        File *f = lookup(VJO_LOG_PATH, 1);
        f->data = malloc(prefill); memcpy(f->data, fill, prefill); f->size = f->cap = prefill;
    }
    {
        VjoConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.log_file = file_on_;
        if (udp_on) snprintf(cfg.log_host, sizeof(cfg.log_host), "192.168.1.2");
        log_mutex = -1; udp_fd = -1; batch = NULL;
        vjo_status_reset();
        vjo_log_configure(&cfg);
    }
    rng = 7;
    {
        VjoLogBatch b;
        static char buf[4096];
        int open = 0;
        for (unsigned i = 0; i < lines; i++) {
            int n = make_line(line, i, 2 + next(254));
            unsigned what = next(6), who = next(3);
            if (!open && what == 0) open = vjo_log_batch_begin(&b, buf, bcap);
            else if (open && what == 1) { vjo_log_batch_end(&b); open = 0; }
            thread = threads && who == 0 ? 2 : 1; /* thread 2 never owns the batch */
            vjo_log_raw(line, n);
            thread = 1;
        }
        if (open) vjo_log_batch_end(&b);
    }
    snap(got);
    free(fill);
    lock_check();
}

static void check_same(const Snapshot *ref, const Snapshot *got, int udp_on)
{
    TEST_CHECK(same(ref->log, ref->log_n, got->log, got->log_n));
    TEST_CHECK(same(ref->old, ref->old_n, got->old, got->old_n));
    TEST_CHECK(same(ref->status, ref->status_n, got->status, got->status_n));
    if (udp_on) TEST_CHECK(same(ref->udp, ref->udp_n, got->udp, got->udp_n) && ref->datagrams == got->datagrams);
    TEST_MSG("log %zu/%zu old %zu/%zu status %zu/%zu", ref->log_n, got->log_n, ref->old_n, got->old_n, ref->status_n,
             got->status_n);
}

static void batches_write_the_same_bytes(void)
{
    Snapshot ref, got;
    /* Across status.txt's 64 KiB cap (about 500 lines), with threads. */
    run(1500, 0, 1, 0, 1, 4096, &ref, &got);
    check_same(&ref, &got, 0);
    TEST_CHECK(ref.status_n <= STATUS_CAP && ref.status_n > STATUS_CAP - 256);
    TEST_CHECK(got.log_opens < ref.log_opens);
    TEST_MSG("log.txt opened %u times line by line, %u batched", ref.log_opens, got.log_opens);
    snap_free(&ref); snap_free(&got);
    /* log.txt rotates inside a batch: 300 lines into a file 20 KiB under its cap. */
    run(300, LOG_FILE_CAP - 20000, 1, 0, 0, 4096, &ref, &got);
    check_same(&ref, &got, 0);
    TEST_CHECK(ref.old_n > LOG_FILE_CAP - 20000 && ref.log_n > 0);
    snap_free(&ref); snap_free(&got);
    /* A small buffer (lines often do not fit), UDP on: the same datagrams too. */
    run(800, 0, 1, 1, 1, 300, &ref, &got);
    check_same(&ref, &got, 1);
    snap_free(&ref); snap_free(&got);
    /* log_file = off: status.txt only. */
    run(800, 0, 0, 0, 1, 4096, &ref, &got);
    check_same(&ref, &got, 0);
    TEST_CHECK(got.log_n == 0);
    snap_free(&ref); snap_free(&got);
    lock_check();
}

static void a_batch_is_one_open_per_file(void)
{
    VjoConfig cfg;
    VjoLogBatch b;
    char buf[2048], line[256];
    fs_reset();
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_file = 1;
    log_mutex = -1; udp_fd = -1; batch = NULL;
    vjo_status_reset();
    vjo_log_configure(&cfg);
    TEST_CHECK(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 1);
    TEST_CHECK(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 0); /* one batch at a time */
    for (unsigned i = 0; i < 16; i++) vjo_log_raw(line, make_line(line, i, 100));
    TEST_CHECK(opens[lookup(VJO_STATUS_PATH, 0) - files] == 1); /* only vjo_status_reset's */
    TEST_CHECK(!lookup(VJO_LOG_PATH, 0));                      /* nothing written yet */
    vjo_log_batch_end(&b);
    TEST_CHECK(opens[lookup(VJO_LOG_PATH, 0) - files] == 1);
    TEST_CHECK(opens[lookup(VJO_STATUS_PATH, 0) - files] == 2);
    TEST_CHECK(lookup(VJO_LOG_PATH, 0)->size == 1600);
    /* more lines than the batch holds: written as it fills, nothing lost */
    TEST_CHECK(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 1);
    for (unsigned i = 0; i < 2 * VJO_LOG_BATCH_LINES + 3; i++) vjo_log_raw(line, make_line(line, i, 10));
    vjo_log_batch_end(&b);
    TEST_CHECK(lookup(VJO_LOG_PATH, 0)->size == 1600 + 10 * (2 * VJO_LOG_BATCH_LINES + 3));
    TEST_CHECK(opens[lookup(VJO_LOG_PATH, 0) - files] == 1 + 3);
    lock_check();
}

static void before_configure_and_without_sinks(void)
{
    VjoLogBatch b;
    char buf[512];
    fs_reset();
    log_mutex = -1; udp_fd = -1; batch = NULL; file_on = 0;
    vjo_status_reset();
    TEST_CHECK(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 0); /* no batches before configure */
    vjo_log("boot %d", 1);
    TEST_CHECK(lookup(VJO_STATUS_PATH, 0)->size == 7 && !lookup(VJO_LOG_PATH, 0));
    /* status.txt full or closed, no file, no UDP: nothing is even formatted */
    vjo_status_close();
    formats = 0;
    vjo_log("not formatted %d", 2);
    TEST_CHECK(formats == 0);
    lock_check();
}

/* A fresh logger (log_file on) after fs_reset, log.txt holding `prefill` bytes. */
static void fresh_logger(size_t prefill)
{
    VjoConfig cfg;
    fs_reset();
    if (prefill) {
        File *f = lookup(VJO_LOG_PATH, 1);
        f->data = malloc(prefill);
        memset(f->data, 'x', prefill);
        f->size = f->cap = prefill;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_file = 1;
    log_mutex = -1; udp_fd = -1; batch = NULL;
    vjo_status_reset();
    vjo_log_configure(&cfg);
}

/* lens[0..n): line lengths; lines [from, to) in one batch, the rest one by one. Afterwards
 * log.txt, log.old.txt and status.txt must equal the line-by-line reference's. */
static void edge_run(const unsigned *lens, unsigned n, unsigned from, unsigned to, size_t prefill)
{
    Snapshot ref, got;
    char line[256 + 64];
    VjoLogBatch b;
    static char buf[8192];
    fresh_logger(prefill);
    ref_file_size = -1; ref_file_on = 1; ref_status_size = 0;
    for (unsigned i = 0; i < n; i++) {
        int k = make_line(line, i, lens[i]);
        ref_status_append(line, k);
        ref_file_append(line, k);
    }
    snap(&ref);
    fresh_logger(prefill);
    for (unsigned i = 0; i < n; i++) {
        int k = make_line(line, i, lens[i]);
        if (i == from) TEST_CHECK(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 1);
        vjo_log_raw(line, k);
        if (i + 1 == to) vjo_log_batch_end(&b);
    }
    snap(&got);
    check_same(&ref, &got, 0);
    snap_free(&ref); snap_free(&got);
}

/* Lines that bring log.txt or status.txt to exactly their cap are written (not rotated,
 * not skipped); the line after rotates or is skipped; a later line that fits again is
 * written; a batch writes log.txt once per run between rotations. */
static void lines_at_the_caps(void)
{
    static unsigned lens[300];
    unsigned n = 0;
    /* log.txt 80 B under its cap: a batch of 30 + 50 (exactly the cap) + 1 (rotates) + 20 */
    unsigned l1[] = {30, 50, 1, 20};
    edge_run(l1, 4, 0, 4, LOG_FILE_CAP - 80);
    TEST_CHECK(log_path_opens == 2); /* the run before the rotation and the one after */
    TEST_MSG("log.txt opened %u times", log_path_opens);
    TEST_CHECK(lookup(VJO_LOG_OLD_PATH, 0) && lookup(VJO_LOG_OLD_PATH, 0)->size == LOG_FILE_CAP);
    /* a single line, outside a batch, that fills log.txt exactly: no rotation */
    unsigned l2[] = {50, 2};
    edge_run(l2, 2, 2, 2, LOG_FILE_CAP - 50);
    TEST_CHECK(lookup(VJO_LOG_OLD_PATH, 0) && lookup(VJO_LOG_OLD_PATH, 0)->size == LOG_FILE_CAP);
    /* status.txt: 255 lines of 255 B and one of 211 bring it 300 B under its cap; then, in a
     * batch, 250 (fits), 60 (skipped), 50 (fits: exactly the cap), 1 (skipped) */
    for (n = 0; n < 255; n++) lens[n] = 255;
    lens[n++] = 211;
    lens[n++] = 250; lens[n++] = 60; lens[n++] = 50; lens[n++] = 1;
    edge_run(lens, n, n - 4, n, 0);
    TEST_CHECK(lookup(VJO_STATUS_PATH, 0)->size == STATUS_CAP);
    /* the same lines one by one */
    edge_run(lens, n, n, n, 0);
    TEST_CHECK(lookup(VJO_STATUS_PATH, 0)->size == STATUS_CAP);
    /* 100 B under the cap (256 x 255 + 156 = 65,436), then a batch of 60 + 40: exactly the cap */
    for (n = 0; n < 256; n++) lens[n] = 255;
    lens[n++] = 156;
    lens[n++] = 60; lens[n++] = 40;
    edge_run(lens, n, n - 2, n, 0);
    TEST_CHECK(lookup(VJO_STATUS_PATH, 0)->size == STATUS_CAP);
    lock_check();
}

/* A batch whose run fails to write just under the cap: the next line is judged against
 * what the file really holds, as line by line, so log.txt does not rotate early. */
static void a_failed_run_at_the_cap(void)
{
    VjoLogBatch b;
    static char buf[1024];
    char line[256];
    fresh_logger(LOG_FILE_CAP - 100);
    TEST_ASSERT(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 1);
    vjo_log_raw(line, make_line(line, 0, 10));
    vjo_log_raw(line, make_line(line, 1, 100));
    fail_writes = 1;
    vjo_log_batch_end(&b); /* 10 + 100 pass the cap: the 10 is written (fails), then the 100 */
    fail_writes = 0;
    /* line by line, 10 and then 100 would both fail and the file stay 100 B under its cap:
     * 100 more fit exactly, no rotation */
    TEST_CHECK(!lookup(VJO_LOG_OLD_PATH, 0));
    TEST_CHECK(file_size == LOG_FILE_CAP - 100 && lookup(VJO_LOG_PATH, 0)->size == LOG_FILE_CAP - 100);
    vjo_log_raw(line, make_line(line, 2, 100));
    TEST_CHECK(!lookup(VJO_LOG_OLD_PATH, 0) && lookup(VJO_LOG_PATH, 0)->size == LOG_FILE_CAP);
    lock_check();
}

/* status.txt: a batch that reaches the cap exactly is one write, not one per line. */
static void status_runs_are_one_write(void)
{
    VjoLogBatch b;
    static char buf[1024];
    char line[256];
    fresh_logger(0);
    for (unsigned i = 0; i < 256; i++) vjo_log_raw(line, make_line(line, i, 255)); /* 65,280 B */
    vjo_log_raw(line, make_line(line, 256, 156)); /* 100 B under the cap */
    status_writes = 0;
    TEST_ASSERT(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 1);
    vjo_log_raw(line, make_line(line, 257, 60));
    vjo_log_raw(line, make_line(line, 258, 40));
    vjo_log_batch_end(&b);
    TEST_CHECK(lookup(VJO_STATUS_PATH, 0)->size == STATUS_CAP && status_writes == 1);
    TEST_MSG("status.txt %zu B in %u writes", lookup(VJO_STATUS_PATH, 0)->size, status_writes);
    lock_check();
}

/* A configuration change inside a batch: the lines logged before it go out under the old
 * settings (log_file on), as line by line; the ones after under the new (off). */
static void configure_inside_a_batch(void)
{
    VjoConfig cfg;
    VjoLogBatch b;
    char buf[1024], line[256];
    fresh_logger(0);
    TEST_ASSERT(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 1);
    vjo_log_raw(line, make_line(line, 0, 40));
    vjo_log_raw(line, make_line(line, 1, 40));
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_file = 0;
    vjo_log_configure(&cfg);
    vjo_log_raw(line, make_line(line, 2, 40));
    vjo_log_batch_end(&b);
    TEST_CHECK(lookup(VJO_LOG_PATH, 0) && lookup(VJO_LOG_PATH, 0)->size == 80);
    TEST_CHECK(lookup(VJO_STATUS_PATH, 0)->size == 120);
    lock_check();
}

/* Another thread's line during a batch is written at once, after the batch's lines so far;
 * the batch goes on. */
static void another_thread_is_not_held(void)
{
    VjoLogBatch b;
    char buf[1024], line[256];
    fresh_logger(0);
    TEST_ASSERT(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 1);
    for (unsigned i = 0; i < 3; i++) vjo_log_raw(line, make_line(line, i, 30));
    TEST_CHECK(!lookup(VJO_LOG_PATH, 0));
    thread = 2;
    vjo_log_raw(line, make_line(line, 3, 30));
    thread = 1;
    TEST_CHECK(lookup(VJO_LOG_PATH, 0) && lookup(VJO_LOG_PATH, 0)->size == 120);
    vjo_log_raw(line, make_line(line, 4, 30));
    TEST_CHECK(lookup(VJO_LOG_PATH, 0)->size == 120); /* held again */
    vjo_log_batch_end(&b);
    TEST_CHECK(lookup(VJO_LOG_PATH, 0)->size == 150);
    lock_check();
}

/* The batch never writes past its line table or its buffer: with guards after both, more
 * lines than VJO_LOG_BATCH_LINES, lines that fill the buffer and a line longer than it. */
static void a_batch_stays_inside_its_memory(void)
{
    struct { VjoLogBatch b; uint32_t guard[16]; } tb;
    struct { char buf[200]; unsigned char guard[300]; } mem;
    char line[256];
    fresh_logger(0);
    memset(tb.guard, 0xA5, sizeof(tb.guard));
    memset(mem.guard, 0x5A, sizeof(mem.guard));
    /* 25 short lines: the table holds 24, so two appends */
    TEST_ASSERT(vjo_log_batch_begin(&tb.b, mem.buf, sizeof(mem.buf)) == 1);
    for (unsigned i = 0; i < VJO_LOG_BATCH_LINES + 1; i++) vjo_log_raw(line, make_line(line, i, 5));
    vjo_log_batch_end(&tb.b);
    TEST_CHECK(log_path_opens == 2);
    TEST_MSG("log.txt opened %u times for %d lines", log_path_opens, VJO_LOG_BATCH_LINES + 1);
    /* 60-byte lines: three fill 180 of the 200 bytes, the fourth starts the next append */
    log_path_opens = 0;
    TEST_ASSERT(vjo_log_batch_begin(&tb.b, mem.buf, sizeof(mem.buf)) == 1);
    for (unsigned i = 0; i < 7; i++) vjo_log_raw(line, make_line(line, i, 60));
    vjo_log_batch_end(&tb.b);
    TEST_CHECK(log_path_opens == 3);
    /* a 250-byte line, longer than the buffer: written alone, at once */
    log_path_opens = 0;
    TEST_ASSERT(vjo_log_batch_begin(&tb.b, mem.buf, sizeof(mem.buf)) == 1);
    vjo_log_raw(line, make_line(line, 0, 20));
    vjo_log_raw(line, make_line(line, 1, 250));
    TEST_CHECK(log_path_opens == 2 && lookup(VJO_LOG_PATH, 0)->size == 25 * 5 + 7 * 60 + 270);
    vjo_log_batch_end(&tb.b);
    for (unsigned i = 0; i < 16; i++) TEST_CHECK(tb.guard[i] == 0xA5A5A5A5u);
    for (unsigned i = 0; i < sizeof(mem.guard); i++)
        if (mem.guard[i] != 0x5A) { TEST_CHECK(mem.guard[i] == 0x5A); TEST_MSG("guard byte %u", i); break; }
    lock_check();
}

/* Writes and opens that fail for a while, inside and outside batches: the
 * lines written meanwhile are lost (each batch's at most), the logger's sizes
 * stay the files' real sizes (so rotation and the 64 KiB cap stay right), and
 * every line after the failures is written. */
static void failures_lose_lines_but_not_the_sizes(void)
{
    VjoConfig cfg;
    VjoLogBatch b;
    static char buf[4096];
    char line[256];
    fs_reset();
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_file = 1;
    log_mutex = -1; udp_fd = -1; batch = NULL;
    vjo_status_reset();
    vjo_log_configure(&cfg);
    rng = 11;
    for (unsigned i = 0; i < 1200; i++) {
        int n = make_line(line, i, 2 + next(254));
        unsigned what = next(10);
        fail_writes = i >= 200 && i < 260;
        fail_opens = i >= 700 && i < 730;
        if (what == 0) {
            if (vjo_log_batch_begin(&b, buf, sizeof(buf))) {
                for (unsigned k = 0; k < 8; k++) vjo_log_raw(line, n);
                vjo_log_batch_end(&b);
            }
        } else {
            vjo_log_raw(line, n);
        }
        File *l = lookup(VJO_LOG_PATH, 0), *st = lookup(VJO_STATUS_PATH, 0);
        TEST_CHECK(file_size == (SceOff)(l ? l->size : 0));
        TEST_CHECK(status_size == (int)(st ? st->size : 0) && status_size <= STATUS_CAP);
    }
    fail_writes = fail_opens = 0;
    size_t before = lookup(VJO_LOG_PATH, 0)->size;
    vjo_log_raw("after\n", 6);
    TEST_CHECK(lookup(VJO_LOG_PATH, 0)->size == before + 6);
    lock_check();
}

/* Whether vjo_log formats a line: with log.txt on, or UDP on, it does even once status.txt is
 * full; with neither and status.txt full (or closed) it does not; with one byte of status.txt
 * left it still does (audit-overlay-perf-3: dropping file_on or udp_fd from the check silenced
 * log.txt and UDP once status.txt was full). */
static void sinks_decide_whether_vjo_log_formats(void)
{
    VjoConfig cfg;
    fresh_logger(0); /* log_file on, no UDP */
    status_size = STATUS_CAP; /* full */
    formats = 0;
    vjo_log("to log.txt %d", 1);
    TEST_CHECK(formats == 1 && lookup(VJO_LOG_PATH, 0) && lookup(VJO_LOG_PATH, 0)->size == 13);
    TEST_CHECK(lookup(VJO_STATUS_PATH, 0)->size == 0);
    memset(&cfg, 0, sizeof(cfg)); /* log_file off, UDP on */
    snprintf(cfg.log_host, sizeof(cfg.log_host), "192.168.1.2");
    vjo_log_configure(&cfg);
    formats = 0; datagrams = 0;
    vjo_log("to udp %d", 2);
    TEST_CHECK(formats == 1 && datagrams == 1 && lookup(VJO_LOG_PATH, 0)->size == 13);
    memset(&cfg, 0, sizeof(cfg)); /* neither */
    vjo_log_configure(&cfg);
    formats = 0; datagrams = 0;
    vjo_log("nowhere %d", 3);
    TEST_CHECK(formats == 0 && datagrams == 0);
    status_size = STATUS_CAP - 1; /* one byte left: a sink still */
    vjo_log("%s", "");
    TEST_CHECK(formats == 1 && lookup(VJO_STATUS_PATH, 0)->size == 1);
    lock_check();
}

TEST_LIST = {
    {"batches_write_the_same_bytes", batches_write_the_same_bytes},
    {"a_batch_is_one_open_per_file", a_batch_is_one_open_per_file},
    {"before_configure_and_without_sinks", before_configure_and_without_sinks},
    {"failures_lose_lines_but_not_the_sizes", failures_lose_lines_but_not_the_sizes},
    {"lines_at_the_caps", lines_at_the_caps},
    {"configure_inside_a_batch", configure_inside_a_batch},
    {"another_thread_is_not_held", another_thread_is_not_held},
    {"a_batch_stays_inside_its_memory", a_batch_stays_inside_its_memory},
    {"a_failed_run_at_the_cap", a_failed_run_at_the_cap},
    {"status_runs_are_one_write", status_runs_are_one_write},
    {"sinks_decide_whether_vjo_log_formats", sinks_decide_whether_vjo_log_formats},
    {NULL, NULL},
};
