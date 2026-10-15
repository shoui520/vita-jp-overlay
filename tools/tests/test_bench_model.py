"""tools/bench_stages.py and tools/bench_model.py: the source constants the job model quotes are the
source's, every workload has a recorded output checksum, and the report renders from a minimal input."""
import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import bench_model
import bench_stages

# Where each constant of bench_model.SOURCE lives, and the pattern that holds its value.
WHERE = {
    "take_wait_ms": ("kernel/game_ocr.c", r"#define TAKE_WAIT_US (\d+)u", 1000),
    "read_wait_ms": ("kernel/game_ocr.c", r"#define READ_WAIT_US (\d+)u", 1000),
    "worker_nap_ms": ("game_ocr/main.c", r"sceKernelDelayThread\(claim_outstanding \? \d+ : (\d+)\)", 1000),
    "log_batch_lines": ("shell/shell.h", r"#define VJO_LOG_BATCH_LINES (\d+)", 1),
    "control_poll_ms": ("shell/worker.c", r"#define POLL_TIMEOUT_US\s+(\d+)", 1000),
    "kernel_tick_ms": ("kernel/main.c", r"#define POLL_US\s+(\d+)", 1000),
    "signature_every_frames": ("kernel/vjo_kernel.h", r"#define VJO_CHECK_EVERY_FRAMES (\d+)", 1),
    "settle_ms": ("kernel/scene.h", r"#define SCENE_STABLE_US\s+(\d+)", 1000),
}


class BenchModelTest(unittest.TestCase):
    def assert_close(self, got, want, path="jobs"):
        """Equal structure and strings; numbers within 1e-9 relative (float sums differ in the last
        bit between Python versions and libms)."""
        if isinstance(want, dict):
            self.assertEqual(set(got), set(want), path)
            for k in want:
                self.assert_close(got[k], want[k], f"{path}.{k}")
        elif isinstance(want, list):
            self.assertEqual(len(got), len(want), path)
            for i, (g, w) in enumerate(zip(got, want)):
                self.assert_close(g, w, f"{path}[{i}]")
        elif isinstance(want, float) or isinstance(got, float):
            self.assertAlmostEqual(got, want, delta=1e-9 * max(1.0, abs(want)), msg=path)
        else:
            self.assertEqual(got, want, path)

    def test_source_constants_are_the_sources(self):
        self.assertEqual(set(WHERE), set(bench_model.SOURCE))
        for key, (path, pattern, scale) in WHERE.items():
            m = re.search(pattern, (ROOT / path).read_text())
            self.assertIsNotNone(m, f"{key}: {pattern} not in {path}")
            self.assertEqual(int(m.group(1)) // scale, bench_model.SOURCE[key][0], f"{key} in {path}")

    def test_every_workload_has_a_recorded_checksum(self):
        checks = json.loads(bench_stages.EXPECTED.read_text())["checks"]
        self.assertEqual({w["name"] for w in bench_stages.WORKLOADS}, set(checks))
        for name, value in checks.items():
            self.assertRegex(value, r"^[0-9a-f]{16}$", name)

    def test_report_renders(self):
        host = {"workloads": {"config": {"ops": 1, "ms_per_op": 0.0123, "counters": {"bytes": 4702, "warnings": 0}}}}
        report, md = bench_model.build_report(host, None, None, bench_stages.WORKLOADS)
        self.assertEqual([r["workload"] for r in report["stages"]], ["config"])
        self.assertIn("| `config` | parse | 12.3 µs | – | – | – | – | – |", md)

    def test_job_log_lines(self):
        # API 19's one-line job: job start, 4 result lines + 1 "lines", 11 steps, 4 after, subtitle; the
        # Shell's own job time holds 19 of them (no start, done or subtitle), as #81's console jobs logged
        self.assertEqual(bench_model.job_log_lines("api19", 1), 22)
        self.assertEqual(bench_model.job_log_lines("api19", 1) - 3, 19)
        self.assertAlmostEqual(bench_model.CONSOLE["jobs"]["log_lines_mean"], 19.1, places=1)
        # main's: 15 steps, a text line per line, the cascade's speed line when it read a line again
        self.assertEqual(bench_model.job_log_lines("default", 1), 27)
        self.assertEqual(bench_model.job_log_lines("default", 1, cascade_jobs=1), 28)
        # appends: the baseline's one a line; main's (#100) the result's and the text's lines as one batch each
        self.assertEqual(bench_model.job_log_appends("baseline", 1), 27)
        self.assertEqual(bench_model.job_log_appends("api19", 1), 22)
        self.assertEqual(bench_model.job_log_appends("default", 1, cascade_jobs=1), 8)
        self.assertEqual(bench_model.job_log_appends("default", 0), 7)  # no line: no text batch
        # the result's batch fits one batch (shell/shell.h VJO_LOG_BATCH_LINES)
        self.assertLessEqual(4 + 3 + 15 + 1, bench_model.SOURCE["log_batch_lines"][0])

    def test_the_fixes_in_the_model(self):
        # main's job against the baseline: #100's appends and #102's polls, nothing else
        bundle = json.loads((ROOT / "docs/benchmarks/pipeline-stages-20261015.json").read_text())
        jobs = {(j["kind"], j["mhz"], j["fps"], j["first_job"]): j for j in bundle["report"]["jobs"]}
        for mhz in (444, 333):
            for fps in (60, 30):
                new, old = jobs[("default", mhz, fps, False)], jobs[("baseline", mhz, fps, False)]
                fs = bench_model.CONSOLE["fs_append_ms"][2]
                saved = (old["log_appends"] - new["log_appends"]) * fs + old["mean_polling_ms"]
                self.assertAlmostEqual(old["estimate"] - new["estimate"], saved, delta=1e-6)
                self.assertEqual(new["mean_polling_ms"], 0)
                self.assertEqual(old["mean_polling_ms"], 75)

    def test_committed_baseline_reproduces_its_console_check(self):
        bundle = json.loads((ROOT / "docs/benchmarks/pipeline-stages-20261015.json").read_text())
        dev = {"summary": bundle["dev"]}
        report, md = bench_model.build_report(bundle["host"], bundle["a9"], dev, bench_stages.WORKLOADS)
        self.assert_close(report["jobs"], bundle["report"]["jobs"])
        chk = report["console_check"]
        measured = chk["measured"]["shell_ms_mean"]
        # the model of API 19's job at the console jobs' own means: its range holds what they took,
        # and its estimate explains more than half of it, never more than all
        lo, hi = chk["model"]["total"]
        self.assertLess(lo, measured)
        self.assertGreater(hi, measured)
        self.assertGreater(chk["unexplained_ms"], 0)
        self.assertLess(chk["unexplained_ms"], measured / 2)
        self.assertIn("| **total** |", md)


if __name__ == "__main__":
    unittest.main()
