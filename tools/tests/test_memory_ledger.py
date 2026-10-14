"""tools/check_memory_ledger.py: the ledger check must pass on this tree and
fail on each kind of regression it exists for."""
import json
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/check_memory_ledger.py"
sys.path.insert(0, str(ROOT / "tools"))
import check_memory_ledger as ledger  # noqa: E402


def run(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *args], capture_output=True, text=True)


def fake_self(memsz_list):
    """A minimal SELF: header offsets to an ELF header and program headers."""
    elf_off, phdr_off = 0x100, 0x140
    phentsize = 32
    data = bytearray(b"SCE\0" + b"\0" * (phdr_off + phentsize * len(memsz_list) - 4))
    struct.pack_into("<Q", data, 0x40, elf_off)
    struct.pack_into("<Q", data, 0x48, phdr_off)
    struct.pack_into("<H", data, elf_off + 0x2A, phentsize)
    struct.pack_into("<H", data, elf_off + 0x2C, len(memsz_list))
    for i, (memsz, flags) in enumerate(memsz_list):
        struct.pack_into("<8I", data, phdr_off + i * phentsize, 1, 0, 0, 0, 0, memsz, flags, 0x1000)
    return bytes(data)


class MemoryLedgerCheck(unittest.TestCase):
    def copy_tree(self):
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp, True)
        for d in ledger.SCAN_DIRS:
            shutil.copytree(ROOT / d, tmp / d, ignore=shutil.ignore_patterns("*.o", "*.obj"))
        (tmp / "tools").mkdir()
        shutil.copyfile(SCRIPT, tmp / "tools/check_memory_ledger.py")
        return tmp

    def test_this_tree_passes_and_prints_the_table(self):
        result = run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("memory ledger: pass", result.stdout)
        for pool in ("shell pages", "shell paf", "kernel", "game"):
            self.assertIn(pool, result.stdout)
        self.assertIn("allocation sites: 9, all in ledger wrappers", result.stdout)
        as_json = json.loads(run("--json").stdout)
        self.assertEqual(as_json["result"], "pass")
        self.assertEqual({p["pool"] for p in as_json["pools"]},
                         {"shell pages", "shell paf", "shell paf (network dictionary)", "kernel", "game"})
        for pool in as_json["pools"]:
            if not pool.get("soft"):
                self.assertLessEqual(pool["worst"], pool["limit"])
        # Anki cards are text-only (VJO_ANKI_SCREENSHOT 0): the opt-in network configuration fits too
        self.assertEqual(as_json["warnings"], [])
        self.assertNotIn("WARNING", result.stderr)
        net = next(p for p in as_json["pools"] if p["pool"] == "shell paf (network dictionary)")
        self.assertEqual(net["worst"], 8192 + 571888 + 131072 + 20480)

    def test_a_soft_overshoot_is_a_warning_with_the_bytes(self):
        # a picture card's 384 KiB arena (VJO_ANKI_SCREENSHOT 1) put the network configuration over
        tmp = self.copy_tree()
        budget = tmp / "include/vjo_ledger_budget.h"
        budget.write_text(re.sub(r"#define VJO_BUDGET_PAF_ANKI \d+u", "#define VJO_BUDGET_PAF_ANKI 393216u",
                                 budget.read_text()))
        result = run("--root", str(tmp))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("WARNING: shell paf (network dictionary): worst case 993,776 B exceeds the limit 985,088 B "
                      "by 8,688 B", result.stderr)

    def test_an_unaccounted_allocation_fails(self):
        tmp = self.copy_tree()
        worker = tmp / "shell/worker.c"
        worker.write_text(worker.read_text() + "\nstatic int sneaky(void) { return sceKernelAllocMemBlock(\"x\", 0, 4096, NULL); }\n")
        result = run("--root", str(tmp))
        self.assertEqual(result.returncode, 1)
        self.assertIn("sceKernelAllocMemBlock at shell/worker.c:", result.stderr)
        self.assertIn("route it through a ledger wrapper", result.stderr)
        # a thread, a PAF allocation and a libc malloc are caught too, also in C++ and in core/
        for rel, call in (("kernel/scene.c", "ksceKernelCreateThread(\"t\", 0, 0, 0, 0, 0, NULL)"),
                          ("shell/overlay.cpp", "sce_paf_malloc(16)"), ("core/arena.c", "malloc(16)"),
                          ("shell/strip.cpp", "new int[4]")):
            tmp = self.copy_tree()
            path = tmp / rel
            path.write_text(path.read_text() + f"\nstatic void *sneaky_{path.stem}(void) {{ return (void *){call}; }}\n")
            result = run("--root", str(tmp))
            self.assertEqual(result.returncode, 1, rel)
            self.assertIn(rel, result.stderr)

    def test_a_call_inside_a_comment_or_string_is_not_a_site(self):
        tmp = self.copy_tree()
        worker = tmp / "shell/worker.c"
        worker.write_text(worker.read_text() + "\n/* sceKernelAllocMemBlock(\"x\", 0, 1, NULL) */\n"
                          "// malloc(3)\nstatic const char *note = \"sceKernelCreateThread(\";\n")
        result = run("--root", str(tmp))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_removed_wrapper_fails(self):
        tmp = self.copy_tree()
        path = tmp / "shell/ledger.c"
        path.write_text(path.read_text().replace("sceKernelCreateThread(", "sceKernelCreateThreadX("))
        result = run("--root", str(tmp))
        self.assertEqual(result.returncode, 1)
        self.assertIn("no longer holds its sceKernelCreateThread wrapper", result.stderr)

    def test_a_worst_case_over_its_limit_fails(self):
        tmp = self.copy_tree()
        budget = tmp / "include/vjo_ledger_budget.h"
        text = budget.read_text()
        text = re.sub(r"#define VJO_BUDGET_PAF_ANKI \d+u", "#define VJO_BUDGET_PAF_ANKI 1048576u", text)
        budget.write_text(text)
        result = run("--root", str(tmp))
        self.assertEqual(result.returncode, 1)
        self.assertRegex(result.stderr, r"pool shell paf: worst case 1,142,784 B exceeds its limit 985,088 B by 157,696 B")
        # a per-item limit too
        tmp = self.copy_tree()
        budget = tmp / "include/vjo_ledger_budget.h"
        budget.write_text(re.sub(r"#define VJO_BUDGET_GAME_WEIGHTS \d+u", "#define VJO_BUDGET_GAME_WEIGHTS 1605632u", budget.read_text()))
        result = run("--root", str(tmp))
        self.assertEqual(result.returncode, 1)
        self.assertIn("VJO_BUDGET_GAME_WEIGHTS 1,605,632 B exceeds VJO_LIMIT_GAME_WEIGHTS 1,601,536 B", result.stderr)

    def test_a_budget_header_that_is_not_arithmetic_is_rejected(self):
        tmp = self.copy_tree()
        budget = tmp / "include/vjo_ledger_budget.h"
        budget.write_text(budget.read_text() + "\n#define VJO_SNEAKY (sizeof(int))\n")
        result = run("--root", str(tmp))
        self.assertEqual(result.returncode, 2)
        self.assertIn("only integer arithmetic", result.stderr)

    def test_module_pages_over_the_declared_budget_fail(self):
        tmp = self.copy_tree()
        values = ledger.parse_budget(ROOT / "include/vjo_ledger_budget.h")
        shell_budget = values["VJO_BUDGET_SHELL_MODULE_PAGES"]
        rw = 180_224  # the data/bss segment's pages; the text segment takes the rest of the budget
        text = shell_budget - rw
        fits = tmp / "shell.suprx"
        fits.write_bytes(fake_self([(text - 40, 5), (rw - 1_000, 6)]))
        result = run("--root", str(tmp), "--shell", str(fits))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"{shell_budget:,} B declared", result.stdout)
        grown = tmp / "shell_grown.suprx"
        grown.write_bytes(fake_self([(text + 1, 5), (rw - 1_000, 6)]))  # one byte into the next page
        result = run("--root", str(tmp), "--shell", str(grown))
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"VitaJPOverlay_Shell: {shell_budget + 4096:,} B of pages, budget VJO_BUDGET_SHELL_MODULE_PAGES is {shell_budget:,} B", result.stderr)
        kernel_budget = values["VJO_BUDGET_KERNEL_MODULE_PAGES"]
        kernel = tmp / "kernel.skprx"
        kernel.write_bytes(fake_self([(32_768, 5), (kernel_budget - 32_768 + 1, 6)]))
        result = run("--root", str(tmp), "--kernel", str(kernel))
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"VitaJPOverlay_Kernel: {kernel_budget + 4096:,} B of pages", result.stderr)
        not_self = tmp / "junk.suprx"
        not_self.write_bytes(b"ELF junk")
        result = run("--root", str(tmp), "--worker", str(not_self))
        self.assertEqual(result.returncode, 2)
        self.assertIn("not a SELF", result.stderr)

    def test_the_real_packaged_modules_when_present(self):
        built = ROOT / "build/vita"
        paths = {k: built / n for k, n in (("kernel", "VitaJPOverlay_Kernel.skprx"), ("shell", "VitaJPOverlay_Shell.suprx"),
                                            ("worker", "VitaJPOverlay_OCR.suprx"))}
        if not all(p.is_file() for p in paths.values()):
            self.skipTest("no Vita build in build/vita")
        result = run(*[a for k, p in paths.items() for a in (f"--{k}", str(p))])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
