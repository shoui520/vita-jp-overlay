#include "acutest.h"
#include "../../shell/game_ocr_client.h"
#include "../../core/conn.h"
#include <string.h>

typedef struct {
    int submit_rc, read_rc, cancelled, malformed, cleanup, engine_rc;
    int submits, reads, cancels, delays, ready_after, abort_after;
    int busy, cancel_when_busy; /* the first `busy` submissions answer BUSY */
    int read_wait_us; /* a pending read returns after this long (a kernel that waits for the result) */
    int64_t time;
    const char *text;
} Fake;
static int submit(void *ud, const VjoGameOcrRequest *req) {
    Fake *f = ud; f->submits++; TEST_CHECK(req->size == sizeof(*req));
    if (f->submits <= f->busy) {
        if (f->cancel_when_busy) f->cancelled = 1;
        return VJO_ERR_BUSY;
    }
    return f->submit_rc;
}
static int read_result(void *ud, uint32_t seq, VjoGameOcrResult *out) {
    Fake *f = ud; f->reads++;
    TEST_CHECK(seq == (uint32_t)f->submit_rc);
    if (f->read_rc) { memset(out, 0xA5, sizeof(*out)); return f->read_rc; }
    if (f->reads <= f->ready_after) { f->time += f->read_wait_us; return 1; }
    out->size = sizeof(*out); out->seq = seq; out->rc = f->engine_rc;
    out->cleanup_status = f->cleanup;
    memset(out->text, 0xA5, sizeof(out->text));
    const char *text = f->text ? f->text : "test";
    size_t text_bytes = strlen(text)+1;
    TEST_ASSERT(text_bytes <= sizeof(out->text));
    memcpy(out->text, text, text_bytes);
    if (f->malformed == 1) out->seq++;
    if (f->malformed == 2) out->size--;
    if (f->malformed == 3) memset(out->text, 'x', sizeof(out->text));
    return 0;
}
static int cancel(void *ud, uint32_t seq) {
    Fake *f = ud; TEST_CHECK(seq == (uint32_t)f->submit_rc); f->cancels++; return -1;
}
static int64_t now(void *ud) { return ((Fake *)ud)->time; }
static void delay(void *ud, uint32_t us) { Fake *f = ud; f->delays++; f->time += us; }
static int cancelled(void *ud) {
    Fake *f = ud; return f->cancelled || (f->abort_after && f->reads >= f->abort_after);
}
static int exchange(Fake *f, VjoGameOcrResult *out) {
    VjoGameOcrRequest req = {.size = sizeof(req)};
    VjoGameOcrClient c = {f, submit, read_result, cancel, now, delay};
    return vjo_game_ocr_exchange(&c, &req, out, cancelled, f, 150000);
}
static void successful(void) {
    Fake f = {.submit_rc = 17, .ready_after = 2}; VjoGameOcrResult out;
    TEST_CHECK(exchange(&f, &out) == 0 && !strcmp(out.text, "test"));
    TEST_CHECK(f.submits == 1 && f.reads == 3 && f.delays == 2 && !f.cancels);
}
static void refuses_missing_worker(void) {
    Fake f = {.submit_rc = -1}; VjoGameOcrResult out;
    memset(&out, 0xA5, sizeof(out));
    TEST_CHECK(exchange(&f, &out) == VJO_E_OCR_UNAVAILABLE && !out.text[0]);
    TEST_CHECK(!f.reads && !f.cancels);
}
static void timeout_keeps_worker_cleanup_separate(void) {
    Fake f = {.submit_rc = 9, .ready_after = 99}; VjoGameOcrResult out;
    TEST_CHECK(exchange(&f, &out) == VJO_E_OCR_INFERENCE && !out.text[0]);
    TEST_CHECK(f.time == 150000 && f.reads == 3 && f.cancels == 1);
}
static void cancellation(void) {
    Fake f = {.submit_rc = 9, .ready_after = 99, .abort_after = 1}; VjoGameOcrResult out;
    TEST_CHECK(exchange(&f, &out) == VJO_E_CANCELLED && !out.text[0]);
    TEST_CHECK(f.reads == 1 && f.cancels == 1);
    f = (Fake){.submit_rc = 9, .cancelled = 1};
    TEST_CHECK(exchange(&f, &out) == VJO_E_CANCELLED && !f.submits);
}
static void rejects_stale_and_incomplete_results(void) {
    for (int malformed = 1; malformed <= 3; malformed++) {
        Fake f = {.submit_rc = 9, .malformed = malformed}; VjoGameOcrResult out;
        TEST_CHECK(exchange(&f, &out) == VJO_E_OCR_INFERENCE && !out.text[0]);
        TEST_CHECK(f.cancels == 1);
    }
    Fake f = {.submit_rc = 9, .read_rc = -4}; VjoGameOcrResult out;
    TEST_CHECK(exchange(&f, &out) == VJO_E_CANCELLED && !out.text[0] && f.cancels == 1);
}
static void refuses_unclean_or_failed_engine(void) {
    Fake f = {.submit_rc = 9, .cleanup = -1}; VjoGameOcrResult out;
    TEST_CHECK(exchange(&f, &out) == VJO_E_OCR_INFERENCE && !out.text[0] && f.cancels == 1);
    f = (Fake){.submit_rc = 9, .engine_rc = VJO_E_OOM};
    TEST_CHECK(exchange(&f, &out) == VJO_E_OOM && !out.text[0] && !f.cancels);
    f = (Fake){.submit_rc = 9, .engine_rc = 1};
    TEST_CHECK(exchange(&f, &out) == VJO_E_OCR_INFERENCE && !out.text[0] && f.cancels == 1);
}
/* A BUSY kernel (the worker still drains a job cancelled a moment ago) is
 * asked again, 20 ms apart, for 2 s; the job then runs as usual. One that
 * stays BUSY is the unavailable worker it always was, and a cancellation
 * meanwhile ends the wait with nothing submitted. */
static void busy_submission_is_repeated_within_its_time(void) {
    Fake f = {.submit_rc = 17, .busy = 2}; VjoGameOcrResult out;
    TEST_CHECK(exchange(&f, &out) == 0 && !strcmp(out.text, "test"));
    TEST_CHECK(f.submits == 3 && f.reads == 1 && f.time == 40000 && !f.cancels);
    f = (Fake){.submit_rc = 17, .busy = 1000};
    memset(&out, 0xA5, sizeof(out));
    TEST_CHECK(exchange(&f, &out) == VJO_E_OCR_UNAVAILABLE && !out.text[0]);
    TEST_CHECK(!f.reads && !f.cancels && f.time == 2000000 && f.submits == 101 && f.delays == 100);
    f = (Fake){.submit_rc = 17, .busy = 1000, .cancel_when_busy = 1};
    TEST_CHECK(exchange(&f, &out) == VJO_E_CANCELLED && f.submits == 1 && !f.reads && !f.cancels);
}
/* A kernel that waits for the result (kernel/game_ocr.c READ_WAIT_US) paces the
 * reads itself: no sleep follows its pending answers, and the timeout and a
 * cancellation still end the exchange. One whose wait was cut short (the
 * result's bit from an earlier job) is followed by the rest of the 50 ms. */
static void waiting_kernel_paces_the_reads(void) {
    Fake f = {.submit_rc = 17, .ready_after = 2, .read_wait_us = 50000}; VjoGameOcrResult out;
    TEST_CHECK(exchange(&f, &out) == 0 && !strcmp(out.text, "test"));
    TEST_CHECK(f.reads == 3 && f.delays == 0 && f.time == 100000);
    f = (Fake){.submit_rc = 9, .ready_after = 99, .read_wait_us = 50000};
    TEST_CHECK(exchange(&f, &out) == VJO_E_OCR_INFERENCE && f.reads == 3 && !f.delays && f.time == 150000);
    f = (Fake){.submit_rc = 9, .ready_after = 99, .read_wait_us = 50000, .abort_after = 2};
    TEST_CHECK(exchange(&f, &out) == VJO_E_CANCELLED && f.reads == 2 && !f.delays);
    f = (Fake){.submit_rc = 17, .ready_after = 2, .read_wait_us = 10000};
    TEST_CHECK(exchange(&f, &out) == 0 && f.delays == 2 && f.time == 2 * 50000);
}
TEST_LIST = {
    {"completed worker", successful}, {"missing worker", refuses_missing_worker},
    {"a waiting kernel paces the reads", waiting_kernel_paces_the_reads},
    {"bounded timeout", timeout_keeps_worker_cleanup_separate},
    {"cancellation", cancellation}, {"stale and incomplete", rejects_stale_and_incomplete_results},
    {"cleanup and engine failure", refuses_unclean_or_failed_engine},
    {"busy submission repeated within its time", busy_submission_is_repeated_within_its_time},
 {NULL, NULL}
};
