"""The docs list exactly the files the kernel's model broker opens (audit-test-build-2 N3): the names
README.md and docs/vita-vn-ocr.md give are tools/docs_model_names.py's, which reads them from
kernel/game_ocr.c, and they are the same as the pin tables'."""
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools/tests"))
import docs_model_names as names  # noqa: E402
import prepare_ocr  # noqa: E402
import test_vocr_defaults as pins  # noqa: E402


class DocsModelNames(unittest.TestCase):
    def test_the_docs_list_what_the_kernel_opens(self):
        self.assertEqual(names.stale(), [], "run python3 tools/docs_model_names.py")
        want = names.block()
        for doc in names.DOCS:
            text = (ROOT / doc).read_text(encoding="utf-8")
            found = names.BLOCK.findall(text)
            self.assertTrue(found, doc)
            self.assertEqual(set(found), {want}, doc)
            # every name the kernel opens is in the text, once in each block
            models, lms = names.kernel_names()
            for name in models + lms:
                self.assertEqual(want.count(f"`{name}`"), 1, (doc, name))

    def test_the_kernels_names_are_the_pin_tables(self):
        models, lms = names.kernel_names()
        self.assertEqual(models, list(pins.kernel_names()))  # the test of the pins reads the same function
        self.assertEqual(set(models), set(pins.c_table()))   # the game worker's pins
        self.assertEqual(lms, sorted(pins.c_lm_table(), key=lambda n: pins.c_lm_table()[n][2]))
        self.assertEqual(set(lms), set(prepare_ocr.VOCR_LM_ASSETS))
        # VOCR3, the originals and VOCR4-W20, and the four LMs (the audit's list)
        self.assertEqual(sorted(models), sorted(["VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr", "FL10_w8.vocr",
                                                 "H15_w8.vocr", "F20_w8.vocr", "VOCR4_W20_w8.vocr"]))
        self.assertEqual(lms, ["vocr_lm_o3_500k.vlm", "vocr_lm_o3_1000k.vlm", "vocr_lm_o3_2000k.vlm",
                               "vocr_lm_o3_4000k.vlm"])
        text = names.names_text(models, lms)
        for generation, tag in (("VOCR3", "v0.2.5-vocr3.1"), ("VOCR1", "v0.2.0"), ("VOCR4", "v0.2.5-vocr4.1")):
            self.assertIn(f"({generation}, release {tag})", text)
        self.assertIn("(release v0.2.6-ctclm.2)", text)

    def test_a_name_the_kernel_adds_makes_the_docs_stale(self):
        kernel = (ROOT / "kernel/game_ocr.c").read_text(encoding="utf-8")
        w20 = "    if (!strcmp(name, VJO_VOCR4_W20_NAME)) return VJO_VOCR_CASCADE_BYTES;\n"
        self.assertEqual(kernel.count(w20), 1)
        added = kernel.replace(w20, w20 + '    if (!strcmp(name, "VOCR5_N_w8.vocr")) return VJO_VOCR_H15_BYTES;\n')
        assets = {**prepare_ocr.VOCR4_ASSETS, "VOCR5_N_w8.vocr": ("https://fixture.invalid/N", 1, "00" * 32)}
        real = names.kernel_names
        with mock.patch.object(names, "kernel_names", lambda: real(added)), \
             mock.patch.object(prepare_ocr, "VOCR4_ASSETS", assets):
            self.assertIn("`VOCR5_N_w8.vocr`", names.block())
            self.assertEqual(names.stale(), list(names.DOCS))
            with tempfile.TemporaryDirectory() as d:  # writing them brings them back in line
                for doc in names.DOCS:
                    (Path(d) / doc).parent.mkdir(parents=True, exist_ok=True)
                    (Path(d) / doc).write_text((ROOT / doc).read_text(encoding="utf-8"), encoding="utf-8")
                with mock.patch("builtins.print"):
                    names.write(d)
                self.assertEqual(names.stale(d), [])
        # a pin the tool cannot read, or that no release pins, fails rather than being left out
        for line in ('    if (!memcmp(name, "VOCR5", 5)) return VJO_VOCR_H15_BYTES;\n',
                     '    if (!strcmp(name, VJO_VOCR5_N_NAME)) return VJO_VOCR_H15_BYTES;\n',
                     '    if (!strcmp(name, "H15_w8.vocr")) return VJO_VOCR_H15_BYTES;\n'):
            with self.assertRaises(ValueError, msg=line):
                names.kernel_names(kernel.replace(w20, w20 + line))
        with self.assertRaises(ValueError):
            names.names_text(*real(added))  # VOCR5_N_w8.vocr is in no release table
        lm = "VJO_VOCR_LM4_NAME}"
        self.assertEqual(kernel.count(lm), 1)
        with self.assertRaises(ValueError):
            names.kernel_names(kernel.replace(lm, "VJO_VOCR_LM3_NAME}"))

    def test_a_missing_or_edited_block_is_stale(self):
        with tempfile.TemporaryDirectory() as d:
            for doc in names.DOCS:
                (Path(d) / doc).parent.mkdir(parents=True, exist_ok=True)
                (Path(d) / doc).write_text((ROOT / doc).read_text(encoding="utf-8"), encoding="utf-8")
            readme = Path(d) / "README.md"
            text = readme.read_text(encoding="utf-8")
            readme.write_text(text.replace("`F20_w8.vocr`", "`F21_w8.vocr`"), encoding="utf-8")
            self.assertEqual(names.stale(d), ["README.md"])
            readme.write_text(names.BLOCK.sub("the two VOCR3 model names", text), encoding="utf-8")
            self.assertEqual(names.stale(d), ["README.md"])
            with self.assertRaises(ValueError):
                names.write(d)


if __name__ == "__main__":
    unittest.main()
