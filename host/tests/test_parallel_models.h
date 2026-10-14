/* core/vocr_ocr.c's model table for test_vocr_parallel.c only: one model whose
 * size, SHA-256 and arena the test pins at run time (test_pin_model), for a tiny
 * valid weights blob it builds itself, so the parallel and cached paths run real
 * vocr_run calls where the release weights are absent (CI). */
static VjoVocrModel models[] = {
    {"TINY_w8.vocr", 0, {0}, 0, VOCR_LINES_GRAD},
};
void test_pin_model(uint32_t bytes, const unsigned char sha256[32], uint32_t arena_bytes);
void test_pin_model(uint32_t bytes, const unsigned char sha256[32], uint32_t arena_bytes)
{
    models[0].bytes = bytes;
    memcpy(models[0].sha256, sha256, 32);
    models[0].arena_bytes = arena_bytes;
}
