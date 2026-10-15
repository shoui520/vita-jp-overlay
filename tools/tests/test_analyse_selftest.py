"""tools/analyse_selftest.py against reports shaped like shell/selftest.c writes them.

v1 (API 18): reports built here, and #74's own report from the PCH-1001 on 3.65
(tools/tests/fixtures/selftest-74-report.txt, verbatim from the issue).
v2 (API 19): tools/tests/fixtures/selftest-v2-*.txt, written by the real
shell/selftest.c on the host (host/tests/selftest_writer.c, synthetic figures);
with VJO_SELFTEST_WRITER set the writer runs again and must reproduce them.
"""
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/analyse_selftest.py"
FIXTURES = Path(__file__).resolve().parent / "fixtures"
TIERS_JSON = ROOT / "tools/model_tiers.json"
sys.path.insert(0, str(ROOT / "tools"))
import analyse_selftest as a  # noqa: E402

HEADER = """selftest v1 api=18
boot seq=7 mode=0 prev=0/4/0/0 failures=0 write=0x00000000
config backend=2 model=H15_w8.vocr fallback=FL10_w8.vocr dictionary=3 anki=0
"""


def sample(step, shell_main=1_200_000, paf=2_557_952, kd_free=9_000_000, kd_total=37_748_736, capture=0, pages=540672, pafl=8192):
    return (f"mem {step} shell_main={shell_main} shell_cdram=4194304 shell_phycont=0 rc=0x00000000 paf_free={paf}\n"
            f"ledger {step} pages={pages}/{pages} paf={pafl}/{pafl}\n"
            f"kernel {step} kd_total={kd_total} kd_free={kd_free} kd_rc=0x00000000 shell_total=52428800 "
            f"shell_free={shell_main + 65536} shell_rc=0x00000000 ledger=131072/131072 limit=2220032 capture={capture}\n")


def full_run(rc=0, free_user=91_226_112, title="PCSG00415", enabled=1, fallback=0):
    return (HEADER + "step boot t=12\n" + sample("boot") + "step ok t=60012\n" + sample("ok")
            + f"step game t=95000 title={title} enabled={enabled} mode=1 region=full\n" + sample("game", shell_main=98304)
            + f"step ocr t=99000 rc={rc} model={'FL10_w8.vocr' if fallback else 'H15_w8.vocr'} fallback={fallback} 960x544 lines=3 recognized=3 frames=330\n"
            + f"game free_user={free_user} reserve=4194304 need_workspace=712704 need_weights=1159168 release=0 workspace=709936 "
              f"resident=1159168 weights=1 io=0x00000000 stat=0x00000000 cleanup=0 capture_rc=0x00000000 find_ms=120 load_ms=900 recognize_ms=640\n"
            + sample("ocr", shell_main=98304, paf=2_490_000, kd_free=6_900_000, capture=2088960) + "step done t=99100\n")


def run(text, *args):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(text)
    try:
        return subprocess.run([sys.executable, str(SCRIPT), f.name, *args], capture_output=True, text=True)
    finally:
        Path(f.name).unlink()


def failed_cells(stdout):
    """Table rows or memory-route cells whose result is FAIL."""
    return re.findall(r"^ {2}\S+ +FAIL .*$|^ {4}.{40} FAIL .*$", stdout, re.M)


def rows(stdout):
    """stage -> result from the printed table."""
    out = {}
    for line in stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and line.startswith("  ") and not line.startswith("    ") and parts[1] in ("PASS", "FAIL", "WARN", "NOTEXT", "-"):
            out[parts[0]] = parts[1]
    return out


class AnalyseV1(unittest.TestCase):
    def test_a_complete_run_passes_and_reports_every_pool(self):
        r = run(full_run())
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("PASS", r.stdout)
        self.assertEqual(rows(r.stdout), {"boot": "PASS", "ok": "PASS", "game": "PASS", "ocr": "PASS", "done": "PASS"})
        self.assertIn("user API (sceKernelGetFreeMemorySize)    PASS  main 98,304 B", r.stdout)
        self.assertIn("kernel block route, KD                   PASS  6,900,000 of 37,748,736 B free", r.stdout)
        self.assertIn("OCR ran in the game at 960x544 with H15_w8.vocr: 3 line(s) recognized, 640 ms", r.stdout)
        self.assertIn("The game had 91,226,112 B of USER free; the job needed 6,066,176 B", r.stdout)
        self.assertIn("check passed", r.stdout)
        # the low SceShell headroom at the game start is called out, with #59
        self.assertIn("only 98,304 B free", r.stdout)
        self.assertIn("#59", r.stdout)
        as_json = json.loads(run(full_run(), "--json").stdout)
        self.assertEqual(as_json["reached"], "done")
        self.assertEqual([s["step"] for s in as_json["steps"]], ["boot", "ok", "game", "ocr", "done"])

    def test_incomplete_runs_name_the_missing_step(self):
        cases = {
            HEADER + "step boot t=12\n" + sample("boot"): ("boot", "never wrote the stable minute (ok)"),
            HEADER + "step boot t=12\n" + sample("boot") + "step ok t=60012\n" + sample("ok"): ("ok", "never wrote a game start"),
            HEADER + "step boot t=12\n" + sample("boot") + "step ok t=60012\n" + sample("ok")
            + "step game t=95000 title=PCSG00415 enabled=1 mode=1 region=full\n" + sample("game"): ("game", "never wrote an OCR result"),
        }
        for text, (reached, phrase) in cases.items():
            r = run(text)
            self.assertEqual(r.returncode, 1, reached)
            self.assertIn(f"INCOMPLETE: the run reached '{reached}'", r.stdout)
            self.assertIn(phrase, r.stdout)
        # an exit event after the game is not progress: never "never wrote ?" (#74)
        r = run(HEADER + "step boot t=12\n" + sample("boot") + "step game t=95000 title=X enabled=1 mode=1 region=full\n"
                + sample("game") + "step exit t=99000\n" + sample("exit"))
        self.assertIn("INCOMPLETE: the run reached 'game'", r.stdout)
        self.assertNotIn("never wrote ?", r.stdout)
        r = run(HEADER)
        self.assertEqual(r.returncode, 1)
        self.assertIn("never wrote a step", r.stdout)

    def test_a_safe_boot_is_explained(self):
        text = HEADER.replace("mode=0 prev=0/4/0/0 failures=0", "mode=1 prev=0/2/0/0 failures=1") + "step boot t=12\n" + sample("boot")
        r = run(text)
        self.assertEqual(r.returncode, 1)
        self.assertIn("SAFE boot", r.stdout)
        self.assertIn("1 normal boot(s) in a row had failed", r.stdout)

    def test_game_out_of_memory_is_blamed_on_the_game_not_the_shell(self):
        r = run(full_run(rc=-103, free_user=5_000_000))
        self.assertEqual(r.returncode, 1)
        self.assertIn("The OCR job failed: out of memory (rc -103)", r.stdout)
        self.assertIn("check refused", r.stdout)
        self.assertIn("Out of memory in the GAME, not in SceShell", r.stdout)
        self.assertIn("FAIL: ocr failed", r.stdout)

    def test_fallback_model_io_errors_and_cleanup_are_named(self):
        text = full_run(fallback=1).replace("io=0x00000000", "io=0x80010013").replace("cleanup=0", "cleanup=1")
        r = run(text)
        self.assertIn("(the fallback model)", r.stdout)
        self.assertIn("Model I/O error 0x80010013 (ENODEV", r.stdout)
        self.assertIn("could not free its memory", r.stdout)

    def test_kernel_capture_failure_and_disabled_game(self):
        text = (HEADER + "step boot t=12\n" + sample("boot") + "step ok t=60012\n" + sample("ok")
                + "step game t=95000 title=PCSG00415 enabled=1 mode=1 region=full\n" + sample("game")
                + "fail capture rc=-1073741826 t=97000\n")
        r = run(text)
        self.assertEqual(r.returncode, 1)
        self.assertIn("capture failed: no memory", r.stdout)
        self.assertIn("ScePhyMemPartKD", r.stdout)
        text = HEADER + "step boot t=12\n" + sample("boot") + "step game t=95000 title=PCSG00415 enabled=0 mode=1 region=full\n" + sample("game")
        r = run(text)
        self.assertIn("disabled in games.ini", r.stdout)
        self.assertEqual(rows(r.stdout)["game"], "FAIL")

    def test_unconfirmed_kd_and_low_paf_and_unavailable_kernel_sample(self):
        text = (HEADER + "step boot t=12\n" + sample("boot", kd_total=16_777_216, paf=1_000_000)
                + "step ok t=60012\nmem ok shell_main=-1 shell_cdram=-1 shell_phycont=-1 rc=0x80020001 paf_free=0\n"
                  "ledger ok pages=540672/540672 paf=8192/8192\nkernel ok unavailable rc=0xBFFFFFF8\n")
        r = run(text)
        self.assertIn("not the 36 MiB of ScePhyMemPartKD", r.stdout)
        self.assertIn("under the 1,572,864 B reserve", r.stdout)
        self.assertIn("sceKernelGetFreeMemorySize failed (0x80020001", r.stdout)
        self.assertIn("kernel memory sample was unavailable (rc 0xBFFFFFF8)", r.stdout)

    def test_garbage_is_rejected(self):
        r = run("hello\nworld\n")
        self.assertEqual(r.returncode, 2)
        self.assertIn("not a self-test report", r.stderr)
        r = subprocess.run([sys.executable, str(SCRIPT), "/nonexistent/report.txt"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)

    def test_parser_keeps_pairs_and_hex(self):
        recs = a.parse("kernel ocr kd_total=37748736 kd_rc=0x80024302 ledger=1/2\n")
        self.assertEqual(recs[0]["fields"], {"kd_total": 37748736, "kd_rc": 0x80024302, "ledger": (1, 2)})
        self.assertEqual(recs[0]["words"], ["ocr"])


class Issue74(unittest.TestCase):
    """The owner's PCH-1001 on 3.65 at 2d1c08c: boot passed, every capture failed."""

    def setUp(self):
        self.report = (FIXTURES / "selftest-74-report.txt").read_text()

    def test_the_74_report_fails_at_the_capture_not_as_a_crash(self):
        r = run(self.report)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertEqual(rows(r.stdout), {"boot": "PASS", "ok": "PASS", "game": "PASS", "ocr": "FAIL", "done": "-"})
        self.assertIn("never reached: the capture failed 5 time(s) (no memory)", r.stdout)
        self.assertIn("2,088,960 B capture buffer", r.stdout)
        self.assertIn("0x80024302", r.stdout)
        self.assertIn("stayed armed and kept forcing the full-screen region", r.stdout)
        self.assertIn("FAIL: ocr failed", r.stdout)
        self.assertNotIn("never wrote ?", r.stdout)
        self.assertNotIn("INCOMPLETE", r.stdout)

    def test_the_74_memory_queries_are_named_as_failures_not_figures(self):
        r = run(self.report)
        # both exact queries returned 0x80020001 at every step; their zeros are not measurements
        self.assertIn("The block kernel memory route failed at every step it was tried (0x80020001", r.stdout)
        self.assertIn("The block shell memory route failed at every step it was tried (0x80020001", r.stdout)
        self.assertNotIn("0 of 0 B free", r.stdout)
        # the user view after boot was under the system margin (rc 0), not an error
        self.assertIn("main -2097152 (under the margin: 0 B free above it)", r.stdout)
        self.assertNotIn("sceKernelGetFreeMemorySize failed", r.stdout)
        # the routes are diagnostics: WARN cells; the verdict is the OCR row's FAIL
        self.assertIn("kernel block route, KD                   WARN  0x80020001", r.stdout)
        self.assertIn("diagnostics: WARN (block kernel route, block shell route)", r.stdout)
        as_json = json.loads(run(self.report, "--json").stdout)
        self.assertEqual((as_json["ocr_result"], as_json["diagnostics_result"]), ("FAIL", "WARN"))
        self.assertEqual(as_json["reached"], "game")
        self.assertEqual(len(as_json["fails"]), 5)


class AnalyseV2(unittest.TestCase):
    def analyse(self, name, *args):
        return run((FIXTURES / f"selftest-v2-{name}.txt").read_text(), *args)

    def test_a_complete_run_passes_every_row(self):
        r = self.analyse("pass")
        self.assertEqual(r.returncode, 0, r.stdout)
        got = rows(r.stdout)
        for name in a.ROWS_V2 + ("capture<64KiB", "kernel-pool", "recognitions", "tearing", "kernel-memory",
                                 "game-memory"):
            self.assertIn(name, got)
        self.assertEqual({k: v for k, v in got.items() if v != "PASS"}, {})
        self.assertIn("worked: hook->user, kstrip->user, user->user", r.stdout)
        self.assertIn("weights read in 1050 ms", r.stdout)       # H15 read from the card (textbox)
        self.assertIn("FL10_w8.vocr, 3 line(s)", r.stdout)        # the forced fallback
        self.assertIn("960x544+0+0 H15_w8.vocr", r.stdout)        # the full screen through the strips
        self.assertIn("largest capture block 61,440 B (limit 65,536 B)", r.stdout)
        self.assertIn("14 line(s) recognized in all", r.stdout)
        self.assertIn("game worker memory", r.stdout)
        self.assertTrue(r.stdout.rstrip().endswith("PASS: every stage completed; the figures above are this console's."))
        self.assertIn("diagnostics: PASS\n", r.stdout)
        as_json = json.loads(self.analyse("pass", "--json").stdout)
        self.assertEqual((as_json["ocr_result"], as_json["diagnostics_result"], as_json["warnings"]),
                         ("PASS", "PASS", []))

    def test_each_stage_has_its_headroom_and_the_free_memory_picks_a_tier(self):
        r = self.analyse("pass")
        # textbox: 91,226,112 + 0 - 4,194,304 - (249,856 + 1,159,168) = 85,622,784
        self.assertIn("textbox    H15_w8.vocr          free   91,226,112 reserve  4,194,304 need  1,409,024 "
                      "headroom   85,622,784 B", r.stdout)
        as_json = json.loads(self.analyse("pass", "--json").stdout)
        heads = {h["stage"]: h["headroom"] for h in as_json["headroom"]}
        self.assertEqual(heads, {"fallback": 86_122_496, "textbox": 85_622_784, "fullscreen": 86_781_952,
                                 "textbox2": 86_781_952})
        self.assertNotIn("probe", heads)  # the probe makes no memory check
        t = as_json["tiers"]
        self.assertEqual((t["free_user"], t["recommendation"]), (91_226_112, "Large"))
        # the formula of vita-vn-ocr docs/model-tiers.md 1.5, at each tier's ceilings of tools/model_tiers.json
        self.assertEqual([row["need"] for row in t["rows"]], [4_395_008, 2_445_312, 1_716_224])
        self.assertEqual([row["weights"] for row in t["rows"]], [2_500_000, 1_600_000, 999_999])
        self.assertEqual([row["reserve"] for row in t["rows"]], [16 << 20, 8 << 20, 4 << 20])
        self.assertEqual([row["box_ms"] for row in t["rows"]], [3000, 2000, 1000])
        self.assertEqual([row["id"] for row in t["rows"]], ["large", "normal", "small"])
        self.assertIn("recommendation: Large", r.stdout)
        self.assertIn("  Large     2,500,000     < 3 s  1,310,720        4,395,008  16,777,216    70,053,888 yes\n",
                      r.stdout)
        self.assertIn("box time: the tier's limit for a typical full textbox (3 lines of about 640 px, 1 core at "
                      "444 MHz), estimated; the recommendation is memory alone", r.stdout)

    def test_the_tier_falls_with_the_free_memory(self):
        # each tier's first job (above) plus its reserve, and one byte less
        for free, want in ((21_172_224, "Large"), (21_172_223, "Normal"), (10_833_920, "Normal"), (10_833_919, "Small"),
                           (5_910_528, "Small"), (5_910_527, None)):
            self.assertEqual(a.tier_table(free)["recommendation"], want, free)
        # a refused stage has a negative headroom and says so: the textbox stage in a game with 5,000,000 B free
        text = (FIXTURES / "selftest-v2-pass.txt").read_text()
        assert "memory textbox free_user=91226112 " in text
        out = a.analyse(text.replace("memory textbox free_user=91226112 ", "memory textbox free_user=5000000 "))
        refused = [h for h in out["headroom"] if h["headroom"] is not None and h["headroom"] < 0]
        self.assertEqual([(h["stage"], h["headroom"]) for h in refused], [("textbox", 5_000_000 - 4_194_304 - 249_856 - 1_159_168)])
        self.assertEqual(refused[0]["headroom"], -603_328)
        self.assertEqual(a.render(out).count("(refused)"), 1)

    def test_collect_needs_a_collect_folder(self):
        with tempfile.TemporaryDirectory() as d:
            r = subprocess.run([sys.executable, str(SCRIPT), "--collect", d], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 2)
        self.assertIn("not a collect folder", r.stderr)

    def test_a_failed_stage_keeps_its_record_and_the_run_goes_on(self):
        r = self.analyse("fail")
        self.assertEqual(r.returncode, 1)
        got = rows(r.stdout)
        self.assertEqual(got["textbox"], "FAIL")
        self.assertEqual((got["fullscreen"], got["textbox2"], got["done"]), ("PASS", "PASS", "PASS"))
        self.assertIn("out of memory (rc -103); failed steps: alloc user/workspace 0x80024302", r.stdout)
        self.assertIn("alloc cdram/workspace 0x80024302", r.stdout)
        self.assertIn("alloc phycont/workspace 0x80024302", r.stdout)
        self.assertIn("FAIL: textbox failed", r.stdout)

    def test_a_stage_without_its_record_is_where_the_vita_stopped(self):
        r = self.analyse("incomplete")
        self.assertEqual(r.returncode, 1)
        got = rows(r.stdout)
        self.assertEqual((got["textbox"], got["fullscreen"], got["textbox2"], got["done"]), ("PASS", "FAIL", "-", "-"))
        self.assertIn("INCOMPLETE: stage 'fullscreen' began and never wrote its record", r.stdout)

    def test_a_failed_memory_route_is_named_and_another_route_still_counts(self):
        r = self.analyse("routes")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("kernel block route, KD                   WARN  0x80020001", r.stdout)
        self.assertIn("at step 2 (block/address-space info)", r.stdout)
        # the kernel's address space refused, the caller's read; a slot outside the kernel half not read
        self.assertIn("kernel process route (faps-coredump)     PASS  2 partitions of shell's address space, "
                      "KD 9,437,184 B free; 1 not read (pointer outside the kernel range)", r.stdout)
        self.assertIn("routes that read nothing: block kernel 0x80020001", r.stdout)
        self.assertEqual(rows(r.stdout)["kernel-memory"], "PASS")
        self.assertEqual(failed_cells(r.stdout), [])

    def test_stages_that_waited_for_text_in_the_region_pass_and_say_so(self):
        r = self.analyse("waited")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertEqual({k: v for k, v in rows(r.stdout).items() if v != "PASS"}, {})
        self.assertIn("FL10_w8.vocr, 3 line(s)", r.stdout)
        self.assertIn("; after 3 settled screen(s) without text in your region", r.stdout)
        self.assertIn("The stages waited for text in your region past 3 settled screen(s) (2 with no line in your "
                      "region, 1 that changed under every read (rc -120)) before they ran.", r.stdout)
        self.assertNotIn("The wait reached its bound", r.stdout)
        self.assertIn("14 line(s) recognized in all, 9 in your region", r.stdout)
        self.assertNotIn("INCOMPLETE", r.stdout)  # each wait closed the begin before it
        self.assertTrue(r.stdout.rstrip().endswith("PASS: every stage completed; the figures above are this console's."))

    def test_region_stages_that_found_no_line_are_not_a_recognition_pass(self):
        """#81: the stages ran on the game's opening notice; the region stages found no line while the full
        screen found five, and the old analyser said PASS. Such rows are NOTEXT, and so is the run."""
        r = self.analyse("notext")
        self.assertEqual(r.returncode, 1, r.stdout)
        got = rows(r.stdout)
        self.assertEqual({k: got[k] for k in ("fallback", "textbox", "fullscreen", "textbox2", "recognitions", "done")},
                         {"fallback": "NOTEXT", "textbox": "NOTEXT", "fullscreen": "PASS", "textbox2": "NOTEXT",
                          "recognitions": "NOTEXT", "done": "PASS"})
        self.assertIn("no text in your region (no line found there): 917x125+22+400 FL10_w8.vocr, 0 line(s)", r.stdout)
        self.assertIn("5 line(s) recognized in all, none in your region", r.stdout)
        self.assertIn("The wait reached its bound (30 screens)", r.stdout)
        verdict = r.stdout.rstrip().splitlines()[-1]
        self.assertTrue(verdict.startswith("NO TEXT: every stage completed, but no line was found in your region in "
                                           "fallback, textbox, textbox2 (the full screen found 5"), verdict)
        as_json = json.loads(self.analyse("notext", "--json").stdout)
        self.assertEqual(as_json["ocr_result"], "NOTEXT")
        # a failure still wins over a missing text
        text = re.sub(r"^(stage textbox end) rc=0 ", r"\1 rc=-103 ", (FIXTURES / "selftest-v2-notext.txt").read_text(),
                      flags=re.M)
        r = run(text)
        self.assertEqual(r.returncode, 1)
        self.assertEqual(rows(r.stdout)["textbox"], "FAIL")
        self.assertTrue(r.stdout.rstrip().splitlines()[-1].startswith("FAIL: textbox failed"))

    def test_no_text_in_some_region_stages_says_what_the_others_read(self):
        """#92's audit, N3: fallback and textbox read lines but textbox2 found none (the game moved on between
        the stages). Still NO TEXT (textbox2's figures are not a recognition), but never 'elsewhere on the
        screen'; the screen changed between the stages only if the region's checksum did."""
        base = (FIXTURES / "selftest-v2-pass.txt").read_text()
        text = re.sub(r"^line textbox2 .*\n", "", base, flags=re.M)
        text, n = re.subn(r"^(stage textbox2 end .*) lines=3 recognized=3 frames=330$", r"\1 lines=0 recognized=0 frames=0",
                          text, flags=re.M)
        self.assertEqual(n, 1)
        changed, n = re.subn(r"^(memory textbox2 .*) sum=0x5EED0003$", r"\1 sum=0x5EED0000", text, flags=re.M)
        self.assertEqual(n, 1)
        r = run(changed)
        self.assertEqual(r.returncode, 1, r.stdout)
        got = rows(r.stdout)
        self.assertEqual((got["fallback"], got["textbox"], got["textbox2"], got["recognitions"]),
                         ("PASS", "PASS", "NOTEXT", "PASS"))
        verdict = r.stdout.rstrip().splitlines()[-1]
        self.assertTrue(verdict.startswith("NO TEXT: every stage completed, but textbox2 found no line in your region "
                                           "while fallback, textbox found 6 there: the screen changed between the "
                                           "stages."), verdict)
        self.assertNotIn("elsewhere", verdict)
        same = run(text).stdout.rstrip().splitlines()[-1]  # the same pixels: no claim that the screen changed
        self.assertTrue(same.startswith("NO TEXT: every stage completed, but textbox2 found no line in your region "
                                        "while fallback, textbox found 6 there. "), same)

    def test_a_run_still_waiting_for_text_is_incomplete_and_says_why(self):
        r = self.analyse("waiting")
        self.assertEqual(r.returncode, 1, r.stdout)
        got = rows(r.stdout)
        self.assertEqual((got["fallback"], got["textbox"], got["done"]), ("-", "-", "-"))
        self.assertIn("waiting for text in your region: 4 settled screen(s) had none, the last at", r.stdout)
        self.assertIn("INCOMPLETE: the stages were still waiting for text in your region: 4 settled screen(s) had none",
                      r.stdout)
        self.assertNotIn("began and never wrote its record", r.stdout)
        self.assertNotIn("The wait reached its bound", r.stdout)
        self.assertEqual(json.loads(self.analyse("waiting", "--json").stdout)["ocr_result"], "INCOMPLETE")
        # a stop in the first stage after the waits is still where the Vita stopped
        text = (FIXTURES / "selftest-v2-waiting.txt").read_text() + \
            "stage fallback begin t=170000 flags=0x2 model=H15_w8.vocr fallback=FL10_w8.vocr\n"
        r = run(text)
        self.assertIn("INCOMPLETE: stage 'fallback' began and never wrote its record", r.stdout)

    @unittest.skipUnless(os.environ.get("VJO_SELFTEST_WRITER"), "vjo-selftest-writer not built")
    def test_the_step_formatter_prints_the_lm_tier_and_the_cascade_bit(self):
        # shell/selftest.c vjo_gstep_format: the kind of the check (the LM tier | 8 with the cascade),
        # lm (the tier) and cascade (lines it read as empty) steps; other steps keep kind=-
        with tempfile.TemporaryDirectory() as tmp:
            done = subprocess.run([os.environ["VJO_SELFTEST_WRITER"], tmp, "gstep"], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        lines = done.stdout.splitlines()
        for start in ("gstep job check kind=10 ", "gstep job lm kind=2 arg=1 ", "gstep job cascade kind=1 arg=1 ",
                      "gstep job recognize kind=- arg=1 ", "gstep job refine kind=2 arg=16 "):
            self.assertTrue(any(line.startswith(start) for line in lines), (start, lines))
        # the refine step as the formatter writes it, read back by --collect: its mode, boxes, splits,
        # the boxes not split for the 16-line cap and the rows read
        rf = a.summarise_log("\n".join(lines) + "\n")["refine"]
        self.assertEqual({k: rf[k] for k in ("jobs", "modes", "boxes", "lines", "split", "capped", "rows", "us")},
                         {"jobs": 1, "modes": {"psp": 1}, "boxes": 9, "lines": 16, "split": 7, "capped": 2,
                          "rows": 410, "us": [1400]})

    @unittest.skipUnless(os.environ.get("VJO_SELFTEST_WRITER"), "vjo-selftest-writer not built")
    def test_the_fixtures_are_what_the_writer_writes(self):
        for name in ("pass", "fail", "incomplete", "routes", "diagnostics", "waited", "notext", "waiting"):
            with tempfile.TemporaryDirectory() as tmp:
                done = subprocess.run([os.environ["VJO_SELFTEST_WRITER"], tmp, name], capture_output=True, text=True)
                self.assertEqual(done.returncode, 0, done.stderr)
                got = (Path(tmp) / "selftest-report.txt").read_text()
                self.assertEqual(got, (FIXTURES / f"selftest-v2-{name}.txt").read_text(),
                                 f"selftest-v2-{name}.txt differs from shell/selftest.c's output: regenerate it with "
                                 f"vjo-selftest-writer DIR {name}")
                # the marker is removed when (and only when) the run finished
                self.assertEqual((Path(tmp) / "selftest.txt").exists(), name in ("incomplete", "waiting"))


class SharedTierTable(unittest.TestCase):
    """tools/model_tiers.json is the only copy of the tiers (audit-test-build-2 N2): the analyser prices
    its tiers and nothing of its own, it holds today's ceilings and box time limits, the Overlay app's
    extra keys change nothing here, and the release ships it beside the analyser."""

    def table(self):
        return json.loads(TIERS_JSON.read_text(encoding="utf-8"))

    def test_the_analyser_prices_the_shared_tables_tiers(self):
        table = self.table()
        self.assertIsNone(a.TIERS_ERROR)
        self.assertEqual(a.TIERS_FILE, TIERS_JSON)
        self.assertEqual(list(a.TIERS), [{k: t[k] for k in ("id", "name", *a.TIER_KEYS)}
                                         for t in reversed(table["tiers"])])
        self.assertEqual((a.TIER_DEFAULT, a.TIER_BOX), (table["default"], table["box"]))
        # no ceiling of its own (the superseded 9 MB / 4 MB tuple included)
        source = SCRIPT.read_text(encoding="utf-8")
        for number in ("999_999", "1_600_000", "2_500_000", "9_000_000", "4_000_000", "1_310_720", "262_144",
                       "131_072"):
            self.assertNotIn(number, source)

    def test_the_tiers_are_todays_ceilings_and_box_time_limits(self):
        # Weights: the owner's ceilings of 2026-10-14 18:09Z, kept by the conductor's correction of
        # 2026-10-15 05:56Z; box time: the owner's limits of 07:33Z, a typical full textbox (3 lines of
        # about 640 px) on one A9 core at 444 MHz; default Normal (owner, 2026-10-15 04:52Z).
        table = self.table()
        self.assertEqual([(t["id"], t["name"], t["weights_max_bytes"], t["box_ms_under"]) for t in table["tiers"]],
                         [("small", "Small", 999_999, 1000), ("normal", "Normal", 1_600_000, 2000),
                          ("large", "Large", 2_500_000, 3000)])
        self.assertEqual(table["box"], {"lines": 3, "line_px": 640, "cores": 1, "mhz": 444})
        self.assertEqual(table["default"], "normal")
        # vita-vn-ocr docs/model-tiers.md 1.3 and 1.5: the W8I arena ceilings and the per-tier reserve
        self.assertEqual([(t["arena_max_bytes"], t["reserve_bytes"]) for t in table["tiers"]],
                         [(131_072, 4 << 20), (262_144, 8 << 20), (1_310_720, 16 << 20)])

    def test_other_keys_are_ignored_and_a_table_it_cannot_use_is_refused(self):
        table = self.table()
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "model_tiers.json"

            def load(t):
                path.write_text(json.dumps(t), encoding="utf-8")
                return a.load_tiers(path)

            # the keys the Overlay app's tier choice adds (overlay-ux-tiers) change nothing here
            extra = json.loads(json.dumps(table))
            extra["est_box_basis"] = "the shipped model at the console's measured ms per CTC frame"
            for t, model in zip(extra["tiers"], ("VOCR3_FL10_w8.vocr", "VOCR3_H15_w8.vocr", "VOCR4_W20_w8.vocr")):
                t.update(model=model, tagline="fastest", est_box_ms=700, need_bytes=6_000_000)
            self.assertEqual(load(extra), (a.TIERS, a.TIER_DEFAULT, a.TIER_BOX))
            for broken in (lambda t: t["tiers"][1].pop("reserve_bytes"),
                           lambda t: t["tiers"][0].update(weights_max_bytes="999999"),
                           lambda t: t["tiers"][0].update(box_ms_under=True),
                           lambda t: t["tiers"][2].update(arena_max_bytes=0),
                           lambda t: t["tiers"].reverse(),               # largest first
                           lambda t: t["tiers"][2].update(box_ms_under=1500),  # a larger tier's limit below Normal's
                           lambda t: t["tiers"][2].update(id="normal"),
                           lambda t: t.update(default="medium"),
                           lambda t: t.update(tiers=[]),
                           lambda t: t["box"].pop("mhz"),
                           lambda t: t.pop("tiers")):
                copy = json.loads(json.dumps(table))
                broken(copy)
                with self.assertRaises(ValueError):
                    load(copy)
            path.write_text("{not json", encoding="utf-8")
            with self.assertRaises(ValueError):
                a.load_tiers(path)
            with self.assertRaises(OSError):
                a.load_tiers(Path(d) / "missing.json")

    def test_without_the_table_the_verdict_stands_and_says_why(self):
        text = (FIXTURES / "selftest-v2-pass.txt").read_text()
        with mock.patch.object(a, "TIERS", ()), mock.patch.object(a, "TIERS_ERROR", "model_tiers.json: missing"):
            out = a.analyse(text)
            shown = a.render(out)
        self.assertEqual((out["ocr_result"], out["diagnostics_result"]), ("PASS", "PASS"))
        self.assertNotIn("tiers", out)
        self.assertIn("The model tiers were not priced: model_tiers.json: missing.", out["findings"])
        self.assertNotIn("model tiers (", shown)
        self.assertTrue(shown.endswith("PASS: every stage completed; the figures above are this console's."))

    def test_the_release_ships_the_table_beside_the_analyser(self):
        cmake = (ROOT / "vita/CMakeLists.txt").read_text(encoding="utf-8")
        copy = [line for line in cmake.splitlines() if "${ROOT}/tools/analyse_selftest.py" in line]
        self.assertEqual(len(copy), 1, copy)
        self.assertIn(" ${ROOT}/tools/model_tiers.json ", copy[0])
        self.assertTrue(copy[0].rstrip().endswith(" ${STAGE}/tools/"), copy[0])

    def test_the_self_test_doc_shows_the_analysers_table(self):
        doc = (ROOT / "docs/self-test.md").read_text(encoding="utf-8")
        for r in a.tier_table(0)["rows"]:
            self.assertIn(f"| {r['tier']} | {r['weights']:,} B | < {r['box_ms'] / 1000:g} s | {r['arena']:,} B | "
                          f"{r['need']:,} B | {r['reserve'] >> 20} MiB | {r['need'] + r['reserve']:,} B |", doc)


# The kernel memory routes exactly as #75's first guard (7bba2aa: only pointers at or above 0x40000000
# were read) answered on a console, where every kernel object is below 0x40000000: the audit's B1.
B1_KMEM = (
    (re.compile(r"^(kmem \w+ route=block part=\w+) rc=0x00000000 step=0 total=\d+ free=\d+$", re.M),
     r"\1 rc=0xBFFFFFFA step=3 total=0 free=0", 8),  # kernel and shell, at boot, ok, game, done
    (re.compile(r"^(kmem \w+ route=cas part=\w+) rc=0x00000000 step=0 .*$", re.M),
     r"\1 rc=0xBFFFFFFA step=3 main=0/0 cdram=0/0 phycont=0/0", 4),
    (re.compile(r"^(kmem \w+ route=as part=\w+) rc=0x00000000 .*$", re.M),
     r"\1 rc=0xBFFFFFFB step=3 parts=0 skipped=2", 4),
)


def with_b1_routes(text):
    for pattern, repl, count in B1_KMEM:
        text, n = pattern.subn(repl, text)
        assert n == count, (pattern.pattern, n)
    return text


class DiagnosticsNeverDecide(unittest.TestCase):
    """A diagnostic that read nothing or did not work is a WARN: it can never turn a working OCR into FAIL
    (#75's first build would have: its kernel memory guard was inverted)."""

    def fixture(self, name):
        return (FIXTURES / f"selftest-v2-{name}.txt").read_text()

    def test_the_row_kinds_are_fixed(self):
        rows_json = json.loads(run(self.fixture("pass"), "--json").stdout)["rows"]
        kinds = {r["row"]: r["kind"] for r in rows_json}
        self.assertEqual({n for n, k in kinds.items() if k == "diagnostic"}, set(a.DIAGNOSTIC_ROWS))
        self.assertEqual({n for n, k in kinds.items() if k == "ocr"},
                         (set(a.ROWS_V2) - {"probe"}) | {"recognitions"})

    def test_every_diagnostic_warns_and_ocr_still_passes(self):
        r = run(self.fixture("diagnostics"))
        self.assertEqual(r.returncode, 0, r.stdout)
        got = rows(r.stdout)
        self.assertEqual({k for k, v in got.items() if v == "WARN"}, {"probe", "capture<64KiB", "kernel-pool",
                                                                     "kernel-memory"})
        self.assertEqual({k for k, v in got.items() if v not in ("PASS", "WARN")}, set())
        self.assertEqual(failed_cells(r.stdout), [])
        self.assertIn("block kernel 0xBFFFFFFA argument at step 3 (pointer outside the kernel range)", r.stdout)
        self.assertIn("fullscreen 262,144 B of cdram (USER_RW refused it there, and CDRAM allocates in 256 KiB units)",
                      r.stdout)
        self.assertIn("the kernel strip (<= 64 KiB of the kernel pool) was used in: fallback, textbox, fullscreen, "
                      "textbox2", r.stdout)
        self.assertIn("the display hook could not write the game's USER memory: worked: kstrip->user, user->user, "
                      "kstrip->cdram; failed: hook->user 0xBFFFFFF9 copy", r.stdout)
        self.assertIn("diagnostics: WARN (probe, capture<64KiB, kernel-pool, kernel-memory)", r.stdout)
        self.assertTrue(r.stdout.rstrip().splitlines()[-1].startswith("PASS: every stage completed"))
        as_json = json.loads(run(self.fixture("diagnostics"), "--json").stdout)
        self.assertEqual((as_json["ocr_result"], as_json["diagnostics_result"]), ("PASS", "WARN"))
        self.assertEqual(as_json["warnings"], ["probe", "capture<64KiB", "kernel-pool", "kernel-memory"])

    def test_warnings_never_soften_an_ocr_failure(self):
        text = re.sub(r"^(stage textbox2 end) rc=0 ", r"\1 rc=-103 ", self.fixture("diagnostics"), flags=re.M)
        r = run(text)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertEqual(rows(r.stdout)["textbox2"], "FAIL")
        self.assertIn("FAIL: textbox2 failed", r.stdout)

    def test_the_b1_console_outcome_is_a_warning_not_a_fail(self):
        """The first audit put 7bba2aa's route results into the pass fixture: exit 1, kernel-memory FAIL."""
        r = run(with_b1_routes(self.fixture("pass")))
        self.assertEqual(r.returncode, 0, r.stdout)
        got = rows(r.stdout)
        self.assertEqual(got["kernel-memory"], "WARN")
        self.assertEqual({k for k, v in got.items() if v != "PASS"}, {"kernel-memory"})
        for route in ("block kernel 0xBFFFFFFA", "block shell 0xBFFFFFFA", "cas 0xBFFFFFFA", "as 0xBFFFFFFB"):
            self.assertIn(route, r.stdout)
        self.assertIn("no KD figure or no user figure at: boot, ok, game, done", r.stdout)
        self.assertTrue(r.stdout.rstrip().splitlines()[-1].startswith("PASS"))

    def test_no_diagnostic_row_can_fail(self):
        base = self.fixture("pass")
        cases = {
            "kernel-memory": [  # vjoMemInfo refused outright at every step (route=none)
                (r"^kmem (\w+) route=block part=kernel .*$", r"kmem \1 route=none rc=0xBFFFFFF8"),
                (r"^kmem \w+ route=(block part=shell|cas|as) .*\n", "")],
            "game-memory": [(r"^(gstep \w+ mem kind=- arg=0) rc=0x00000000 (.*) main=\d+ ",
                             r"\1 rc=0x80020001 \2 main=-1 ")],
            "probe": [(r"^(gstep probe probe kind=\w+ arg=\w+) rc=0x0000003E ", r"\1 rc=0xBFFFFFF9 ")],
            "capture<64KiB": [(r"^(result textbox .* capture=)61440 ", r"\g<1>69632 ")],
            "kernel-pool": [(r"^(result textbox2 source=)hook ", r"\1kstrip ")],
        }
        for name, subs in cases.items():
            text = base
            for pattern, repl in subs:
                text, n = re.subn(pattern, repl, text, flags=re.M)
                self.assertGreater(n, 0, (name, pattern))
            r = run(text)
            with self.subTest(name):
                self.assertEqual(r.returncode, 0, r.stdout)
                self.assertEqual(rows(r.stdout)[name], "WARN", r.stdout)
                self.assertEqual(failed_cells(r.stdout), [])
                self.assertTrue(r.stdout.rstrip().splitlines()[-1].startswith("PASS"))
        # the user API itself failing in SceShell: a WARN cell and a kernel-memory WARN
        text = re.sub(r"^(mem ok) rc=0x00000000 ", r"\1 rc=0x80020001 ", base, flags=re.M)
        r = run(text)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("user API (sceKernelGetFreeMemorySize)    WARN  0x80020001", r.stdout)
        self.assertEqual(rows(r.stdout)["kernel-memory"], "WARN")

    def test_a_missing_ocr_stage_with_done_present_is_a_failure(self):
        """report() drops a line it cannot write (shell/selftest.c): a 'done' run missing an OCR
        stage's records is a failure, not 'not reached'."""
        base = self.fixture("pass")
        # drop every line of the textbox2 stage (begin..its records), keep 'done'
        lines = base.splitlines(keepends=True)
        kept = [ln for ln in lines if " textbox2 " not in ln and not ln.startswith("stage textbox2 ")]
        r = run("".join(kept))
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertEqual(rows(r.stdout)["textbox2"], "FAIL")
        self.assertIn("no record though the run finished", r.stdout)
        self.assertEqual(json.loads(run("".join(kept), "--json").stdout)["ocr_result"], "FAIL")

    def test_a_stop_in_the_probe_is_never_a_warning(self):
        text = self.fixture("pass")
        text = text[:text.index("\n", text.index("stage probe begin")) + 1]  # the Vita stopped in the probe
        r = run(text)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertEqual(rows(r.stdout)["probe"], "FAIL")
        self.assertIn("INCOMPLETE: stage 'probe' began and never wrote its record", r.stdout)
        self.assertIn("diagnostics: not all reached", r.stdout)  # a partial run has no full diagnostic table
        j = json.loads(run(text, "--json").stdout)
        self.assertEqual((j["ocr_result"], j["diagnostics_result"]), ("INCOMPLETE", "INCOMPLETE"))


class OverlayCloseLines(unittest.TestCase):
    """log.txt's "overlay closed" lines (shell/overlay.cpp close_page), before and after row navigation (#81)."""

    OLD = "overlay closed: 1158 frames, 19 word moves, stick 71, slowest frame 2 ms\n"
    NEW = ("overlay closed: 993 frames, 6 word moves, stick 2, slowest frame 36 ms, 4 row moves, 2 wraps, 1 without layout, "
           "slowest key 412 us\n")
    NEW_SLOWER = ("overlay closed: 10 frames, 1 word moves, stick 0, slowest frame 3 ms, 1 row moves, 0 wraps, 0 without layout, "
                  "slowest key 1250 us\n")

    def test_an_older_build_reads_as_before(self):
        lg = a.summarise_log(self.OLD)
        self.assertEqual(lg["overlay"], [2])
        self.assertNotIn("arrows", lg)
        self.assertEqual(a.overlay_summary(lg), "  overlay opened 1x, slowest frame 2 ms")

    def test_the_arrow_keys_are_counted(self):
        lg = a.summarise_log(self.OLD + self.NEW + self.NEW_SLOWER + self.NEW)
        self.assertEqual(lg["overlay"], [2, 36, 3, 36])
        self.assertEqual(lg["arrows"], {"closes": 3, "row_moves": 9, "wraps": 4, "without_layout": 2,
                                        "slowest_key_us": 1250})
        self.assertEqual(a.overlay_summary(lg), "  overlay opened 4x, slowest frame 36 ms; arrows (3 of them): "
                                                "9 row changes, 4 row wraps, 2 before the header's layout, "
                                                "slowest key 1,250 us")


if __name__ == "__main__":
    unittest.main()
