/* The memory ledger's counters (core/ledger.c) and the static budget table
 * (include/vjo_ledger_budget.h) against the code it describes. */
#include "acutest.h"
#include <string.h>
#include "../../core/ledger.h"
#include "../../include/vjo_ledger_budget.h"
#include "../../include/vjo_vocr_sizes.h"
#include "../../core/styled.h"
#ifdef VJO_WITH_VOCR
#include "../../core/vocr_ocr.h"
#endif

enum { A = 0, B, C, N };
static const VjoLedgerItemDef defs[N] = {
    [A] = {"a", VJO_LEDGER_POOL_SHELL_PAGES},
    [B] = {"b", VJO_LEDGER_POOL_SHELL_PAGES},
    [C] = {"c", VJO_LEDGER_POOL_KERNEL},
};

static void test_add_sub_and_peaks(void)
{
    VjoLedger l;
    vjo_ledger_init(&l, defs, N);
    TEST_CHECK(l.n == N && vjo_ledger_pool_current(&l, VJO_LEDGER_POOL_SHELL_PAGES) == 0);
    vjo_ledger_add(&l, A, 100);
    vjo_ledger_add(&l, B, 50);
    vjo_ledger_add(&l, C, 7);
    TEST_CHECK(vjo_ledger_current(&l, A) == 100 && vjo_ledger_current(&l, B) == 50 && vjo_ledger_current(&l, C) == 7);
    TEST_CHECK(vjo_ledger_pool_current(&l, VJO_LEDGER_POOL_SHELL_PAGES) == 150);
    TEST_CHECK(vjo_ledger_pool_peak(&l, VJO_LEDGER_POOL_SHELL_PAGES) == 150);
    TEST_CHECK(vjo_ledger_pool_current(&l, VJO_LEDGER_POOL_KERNEL) == 7);
    vjo_ledger_sub(&l, A, 60);
    TEST_CHECK(vjo_ledger_current(&l, A) == 40 && vjo_ledger_peak(&l, A) == 100);
    TEST_CHECK(vjo_ledger_pool_current(&l, VJO_LEDGER_POOL_SHELL_PAGES) == 90 &&
               vjo_ledger_pool_peak(&l, VJO_LEDGER_POOL_SHELL_PAGES) == 150);
    vjo_ledger_add(&l, A, 200); /* a new peak */
    TEST_CHECK(vjo_ledger_current(&l, A) == 240 && vjo_ledger_peak(&l, A) == 240);
    TEST_CHECK(vjo_ledger_pool_peak(&l, VJO_LEDGER_POOL_SHELL_PAGES) == 290);
    /* never below zero, even when a release is larger than what is held */
    vjo_ledger_sub(&l, A, 1000);
    TEST_CHECK(vjo_ledger_current(&l, A) == 0 && vjo_ledger_pool_current(&l, VJO_LEDGER_POOL_SHELL_PAGES) == 50);
    /* zero bytes, unknown items and pools are ignored */
    vjo_ledger_add(&l, B, 0);
    vjo_ledger_add(&l, -1, 5);
    vjo_ledger_add(&l, N, 5);
    vjo_ledger_sub(&l, 99, 5);
    TEST_CHECK(vjo_ledger_current(&l, B) == 50 && vjo_ledger_current(&l, -1) == 0 && vjo_ledger_current(&l, N) == 0);
    TEST_CHECK(vjo_ledger_pool_current(&l, VJO_LEDGER_POOLS) == 0 && vjo_ledger_pool_current(&l, -1) == 0);
    TEST_CHECK(vjo_ledger_pool_current(&l, VJO_LEDGER_POOL_SHELL_PAGES) == 50);
    /* an uninitialised ledger (defs NULL) ignores everything */
    memset(&l, 0, sizeof(l));
    vjo_ledger_add(&l, A, 5);
    TEST_CHECK(vjo_ledger_current(&l, A) == 0 && !vjo_ledger_known(&l, A));
    /* more items than slots: the extra ones are ignored */
    VjoLedgerItemDef many[VJO_LEDGER_MAX_ITEMS + 3] = {{"x", 0}};
    for (int i = 0; i < VJO_LEDGER_MAX_ITEMS + 3; i++) { many[i].name = "x"; many[i].pool = 0; }
    vjo_ledger_init(&l, many, VJO_LEDGER_MAX_ITEMS + 3);
    TEST_CHECK(l.n == VJO_LEDGER_MAX_ITEMS);
    vjo_ledger_add(&l, VJO_LEDGER_MAX_ITEMS, 5);
    TEST_CHECK(vjo_ledger_pool_current(&l, 0) == 0);
    /* a bad pool in a definition is ignored too */
    VjoLedgerItemDef bad[1] = {{"bad", VJO_LEDGER_POOLS}};
    vjo_ledger_init(&l, bad, 1);
    vjo_ledger_add(&l, 0, 5);
    TEST_CHECK(vjo_ledger_current(&l, 0) == 0);
}

/* The table's worst cases are what the budget check enforces; the
 * arithmetic in the header must hold for the C compiler too. */
static void test_budget_table_is_consistent(void)
{
    TEST_CHECK(VJO_WORST_SHELL_PAGES_PERMANENT == VJO_BUDGET_SHELL_MODULE_PAGES + VJO_BUDGET_WORKER_IN_SHELL_PAGES +
                                                  VJO_BUDGET_SHELL_NET_STACK + VJO_BUDGET_SHELL_CTL_STACK);
    TEST_CHECK(VJO_WORST_SHELL_PAGES <= VJO_LIMIT_SHELL_PAGES);
    TEST_CHECK(VJO_WORST_SHELL_PAF <= VJO_LIMIT_SHELL_PAF);
    /* the opt-in network configuration may go over the limit only by the
     * reserve guard's refusal, never by more than one of its items; with
     * text-only Anki cards (shell/anki.c VJO_ANKI_SCREENSHOT 0) it fits */
    TEST_CHECK(VJO_WORST_SHELL_PAF_NET < VJO_LIMIT_SHELL_PAF + VJO_BUDGET_PAF_UI);
    TEST_CHECK(VJO_WORST_SHELL_PAF_NET <= VJO_LIMIT_SHELL_PAF);
    /* the overlay's text arena holds what overlay.cpp allocates from it:
     * header 4096 units + 1 span, body 4096 units + 96 spans, 8-byte aligned */
    {
        uint32_t text = (2u * (4096u + 1u) + 7u) & ~7u, span = (uint32_t)sizeof(VjoSpan);
        uint32_t need = text + ((span * 1u + 7u) & ~7u) + text + ((span * 96u + 7u) & ~7u);
        TEST_CHECK_(need == 17952u && need <= VJO_BUDGET_PAF_UI, "%u", need);
    }
    TEST_CHECK(VJO_WORST_KERNEL <= VJO_LIMIT_KERNEL);
    TEST_CHECK(VJO_WORST_GAME_JOB <= VJO_LIMIT_GAME_JOB);
    TEST_CHECK(VJO_BUDGET_GAME_WEIGHTS <= VJO_LIMIT_GAME_WEIGHTS && VJO_BUDGET_GAME_WORKSPACE <= VJO_LIMIT_GAME_WORKSPACE);
    /* page-rounded figures are page-rounded */
    TEST_CHECK(VJO_BUDGET_SHELL_MODULE_PAGES % VJO_LEDGER_PAGE == 0 && VJO_BUDGET_WORKER_IN_SHELL_PAGES % VJO_LEDGER_PAGE == 0 &&
               VJO_BUDGET_KERNEL_MODULE_PAGES % VJO_LEDGER_PAGE == 0 && VJO_BUDGET_GAME_WEIGHTS % VJO_LEDGER_PAGE == 0 &&
               VJO_BUDGET_GAME_WORKSPACE % VJO_LEDGER_PAGE == 0 && VJO_BUDGET_KERNEL_CAPTURE % VJO_LEDGER_PAGE == 0);
    /* the game weights budget is the largest accepted model, page-rounded */
    TEST_CHECK(VJO_BUDGET_GAME_WEIGHTS == (VJO_VOCR_F20_BYTES + VJO_LEDGER_PAGE - 1) / VJO_LEDGER_PAGE * VJO_LEDGER_PAGE);
    TEST_CHECK(VJO_VOCR_H15_BYTES < VJO_VOCR_F20_BYTES && VJO_VOCR_FL10_BYTES < VJO_VOCR_H15_BYTES);
    /* the PAF limit is the measured free heap minus the runtime reserve */
    TEST_CHECK(VJO_LIMIT_SHELL_PAF == VJO_PAF_FREE_MEASURED - VJO_PAF_RESERVE && VJO_PAF_FREE_MEASURED < VJO_PAF_HEAP);
}

#ifdef VJO_WITH_VOCR
/* The workspace budget is the real layout code's figure for the largest
 * accepted model at the largest region. */
static void test_game_workspace_budget_matches_the_layout(void)
{
    static const char *const models[] = {"FL10_w8.vocr", "H15_w8.vocr", "F20_w8.vocr", VJO_VOCR2_MODEL_NAME};
    size_t largest = 0;
    for (unsigned i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
        /* API 21: with the most helpers a job takes (ocr_threads 3) */
        size_t bytes = vjo_vocr_job_bytes_helpers(models[i], VJO_OCR_MAX_WIDTH, VJO_OCR_MAX_HEIGHT, VJO_VOCR_HELPERS_MAX);
        TEST_CHECK_(bytes > 0, "%s", models[i]);
        if (bytes > largest) largest = bytes;
        const VjoVocrModel *m = vjo_vocr_model(models[i]);
        TEST_CHECK(m && (m->bytes + VJO_LEDGER_PAGE - 1) / VJO_LEDGER_PAGE * VJO_LEDGER_PAGE <= VJO_BUDGET_GAME_WEIGHTS);
    }
    TEST_CHECK_((largest + VJO_LEDGER_PAGE - 1) / VJO_LEDGER_PAGE * VJO_LEDGER_PAGE == VJO_BUDGET_GAME_WORKSPACE,
                "largest full-screen job %zu B", largest);
}
#endif

TEST_LIST = {
    {"add_sub_and_peaks", test_add_sub_and_peaks},
    {"budget_table_is_consistent", test_budget_table_is_consistent},
#ifdef VJO_WITH_VOCR
    {"game_workspace_budget_matches_the_layout", test_game_workspace_budget_matches_the_layout},
#endif
    {NULL, NULL}
};
