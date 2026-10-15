"""The device default is VOCR3_H15_w8.vocr from bee-san/vita-vn-ocr v0.2.5-vocr3.1, with
VOCR3_FL10_w8.vocr as its fallback; v0.2.0's H15/FL10 stay pinned and shipped; every pinned
model fits the console budget ceiling; and the game worker's, the kernel's, the model metadata's
(core/vocr_models.c, with each model's generation) and the Python pins agree. Each C table is
read entry by entry, with a macro or a literal size; an entry the parsers cannot read fails."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import prepare_ocr  # noqa: E402

# The sha256s in the v0.2.0 release notes and in v0.2.5-vocr3.1's SHA256SUMS (the VOCR3_H15 and
# VOCR3_FL10 arms of vita-vn-ocr eval/vocr3_report_spec_20261013.json).
H15_V020_SHA256 = "316ebc377ec34520440991c5085199d8e74031f01a867938be90a60c1e3f03cd"
VOCR3_H15_SHA256 = "cb3c61a8f91f9aeeb3efcc0e79313f664112f25c4bd55865bae9df52522b360c"
VOCR3_FL10_SHA256 = "87fb4d0c18416f5f3771cecd274c8db3cf69b8b4a4ee9f204edebb4b82079154"
# docs/vita-memory-budget.md in bee-san/vita-vn-ocr: BUDGET_READY weights<=1600000 arena<=262144.
CEILING_WEIGHTS, CEILING_ARENA = 1_600_000, 262_144


def macros():
    """#define NAME value (number or string) of include/vjo_vocr_sizes.h."""
    text = (ROOT / "include/vjo_vocr_sizes.h").read_text()
    out = {m.group(1): int(m.group(2)) for m in re.finditer(r"#define (VJO_\w+) (\d+)u", text)}
    out.update({m.group(1): m.group(2) for m in re.finditer(r'#define (VJO_\w+) "([^"]+)"', text)})
    for m in re.finditer(r"#define (VJO_\w+) (VJO_\w+)\s*$", text, re.M):  # an alias of another macro
        if m.group(2) in out:
            out[m.group(1)] = out[m.group(2)]
    return out


def number(token, sizes):
    """A size or an arena: a macro of include/vjo_vocr_sizes.h, or a decimal literal (1155968u)."""
    return sizes[token] if token.startswith("VJO_") else int(token)


def without_comments(source):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)


def array_body(source, declaration):
    """The initialisers of the C array declared by `declaration`, between its outer braces."""
    body = source[source.index(declaration):]
    body = body[body.index("{") + 1:]
    return without_comments(body[:body.index("\n};")])


def entries(body):
    """How many initialisers an array body holds, counted by brace depth: every entry, however it is
    written, so that one the patterns below do not read is noticed."""
    depth = count = 0
    for ch in body:
        if ch == "{":
            count += depth == 0
            depth += 1
        elif ch == "}":
            depth -= 1
    return count


def every_pin(where, pins, expected):
    """name -> value of the pins read from `where`, which must be all `expected` of its entries, each
    name once: a pin in a form the pattern misses fails here instead of escaping the comparisons."""
    names = [name for name, _ in pins]
    if len(pins) != expected or len(set(names)) != len(names):
        raise AssertionError(f"{where}: read {len(pins)} pins with {len(set(names))} names of {expected} entries")
    return dict(pins)


SIZE = r"(VJO_\w+|\d+)[uU]?"  # a macro or a decimal literal, as either table may spell it


def c_table(source=None):
    """name -> (bytes, sha256 hex, arena) from core/vocr_ocr.c's release table."""
    if source is None:
        source = (ROOT / "core/vocr_ocr.c").read_text()
    sizes = macros()
    body = array_body(source, "#else\nstatic const VjoVocrModel models[]")
    pins = []
    for m in re.finditer(r'\{"?([\w.]+)"?, ' + SIZE + r',\s*\{([^}]*)\},\s*' + SIZE + r', VOCR_LINES_\w+\}', body):
        name = sizes.get(m.group(1), m.group(1))  # a name macro (VJO_VOCR3_H15_NAME) or a literal
        digest = "".join("%02x" % int(b, 16) for b in re.findall(r"0x([0-9a-f]{2})", m.group(3)))
        pins.append((name, (number(m.group(2), sizes), digest, number(m.group(4), sizes))))
    return every_pin("core/vocr_ocr.c models[]", pins, entries(body))


def metadata_table(source=None):
    """name -> (generation, shape, bytes) from core/vocr_models.c, the Overlay app's model metadata."""
    if source is None:
        source = (ROOT / "core/vocr_models.c").read_text()
    sizes = macros()
    body = array_body(source, "#else\nstatic const VjoVocrModelInfo infos[]")
    pins = [(sizes.get(m.group(1), m.group(1)), (m.group(2), m.group(3), number(m.group(4), sizes)))
            for m in re.finditer(r'\{"?([\w.]+)"?, "(\w+)", "(\w+)", ' + SIZE + r'\}', body)]
    return every_pin("core/vocr_models.c infos[]", pins, entries(body))


def kernel_names(source=None):
    """name -> bytes the kernel's model broker accepts (kernel/game_ocr.c model_size). Every return but
    the last (return 0) and every name comparison there must be one of the pins read."""
    if source is None:
        source = (ROOT / "kernel/game_ocr.c").read_text()
    body = source[source.index("static uint32_t model_size(const char *name)"):]
    body = without_comments(body[:body.index("\n}\n")])
    sizes = macros()
    pins = [(sizes.get(n, n.strip('"')), number(b, sizes))
            for n, b in re.findall(r'!strcmp\(name, ("[\w.]+"|VJO_\w+)\)\) return ' + SIZE + ';', body)]
    if not re.search(r"\breturn 0;\s*$", body) or body.count("strcmp(") != len(pins):
        raise AssertionError(f"kernel/game_ocr.c model_size: {body.count('strcmp(')} comparisons, "
                             f"{len(pins)} pins read, last return {body.split('return')[-1].strip()!r}")
    return every_pin("kernel/game_ocr.c model_size", pins, len(re.findall(r"\breturn\b", body)) - 1)


def python_pins():
    """name -> (url, bytes, sha256, release tag) of every release's asset table in tools/prepare_ocr.py:
    VOCR_ASSETS (VOCR_TAG), VOCR3_ASSETS (VOCR3_TAG) and any later VOCRn_ASSETS (VOCRn_TAG), each name in
    one table only."""
    out = {}
    for key, assets in sorted(vars(prepare_ocr).items()):
        release = re.fullmatch(r"(VOCR\d*)_ASSETS", key)
        if not release:
            continue
        tag = getattr(prepare_ocr, release.group(1) + "_TAG")
        for name, (url, size, sha) in assets.items():
            if name in out:
                raise AssertionError(f"tools/prepare_ocr.py: {name} is in two release tables")
            out[name] = (url, size, sha, tag)
    return out


def c_lm_table():
    """name -> (bytes, sha256 hex, tier) from core/vocr_ocr.c's LM table."""
    source = (ROOT / "core/vocr_ocr.c").read_text()
    sizes = macros()
    table = source[source.index("static const VjoVocrLm lms[VJO_VOCR_LM_TIERS]"):]
    table = table[:table.index("};")]
    out = {}
    for m in re.finditer(r"\{(VJO_VOCR_LM\d_NAME), (VJO_VOCR_LM\d_BYTES),\s*\{([^}]*)\},\s*(\d+), (\d)\}", table):
        digest = "".join("%02x" % int(b, 16) for b in re.findall(r"0x([0-9a-f]{2})", m.group(3)))
        out[sizes[m.group(1)]] = (sizes[m.group(2)], digest, int(m.group(5)))
    return out


class VocrDefaults(unittest.TestCase):
    def test_cascade_model_is_the_published_vocr4_w20(self):
        self.assertEqual(prepare_ocr.VOCR4_TAG, "v0.2.5-vocr4.1")
        url, size, sha = prepare_ocr.VOCR4_ASSETS[prepare_ocr.VOCR_CASCADE_MODEL]
        self.assertTrue(url.endswith("/releases/download/v0.2.5-vocr4.1/VOCR4_W20_w8.vocr"), url)
        # v0.2.5-vocr4.1's SHA256SUMS; F20's shape
        self.assertEqual((size, sha), (1558016, "2745e69699ad7b2aba3092f83e5ddc1912f405f54b9df8a2e55be82c385b716e"))
        sizes = macros()
        self.assertEqual(sizes["VJO_VOCR_CASCADE_NAME"], prepare_ocr.VOCR_CASCADE_MODEL)
        self.assertEqual(sizes["VJO_VOCR_CASCADE_BYTES"], size)
        self.assertEqual(c_table()[prepare_ocr.VOCR_CASCADE_MODEL][:2], (size, sha))
        import package_release  # noqa: PLC0415
        roles = {name: role for name, _, _, role in package_release.shipped_models()}
        self.assertIn(prepare_ocr.VOCR_CASCADE_MODEL, roles)
        self.assertIn("\nocr_cascade_ms = 2000\n", (ROOT / "docs/config.example.ini").read_text())

    def test_lms_and_their_attribution_come_pinned_from_a_local_copy(self):
        import hashlib  # noqa: PLC0415
        import tempfile  # noqa: PLC0415
        from unittest import mock  # noqa: PLC0415
        with tempfile.TemporaryDirectory() as tmp:
            src, out = Path(tmp) / "src", Path(tmp) / "out"
            src.mkdir()
            pins, data = {}, {}
            for name in prepare_ocr.VOCR_LM_ASSETS:
                data[name] = ("fixture " + name).encode()
                pins[name] = ("https://fixture.invalid/" + name, len(data[name]), hashlib.sha256(data[name]).hexdigest())
                (src / name).write_bytes(data[name])
            att = b"CC BY-SA 4.0 fixture"
            (src / prepare_ocr.VOCR_LM_ATTRIBUTION).write_bytes(att)
            att_pin = {prepare_ocr.VOCR_LM_ATTRIBUTION: ("https://fixture.invalid/a", len(att),
                                                         hashlib.sha256(att).hexdigest())}
            with mock.patch.object(prepare_ocr, "VOCR_LM_ASSETS", pins), \
                 mock.patch.object(prepare_ocr, "VOCR_LM_ATTRIBUTION_PIN", att_pin), \
                 mock.patch.object(prepare_ocr.urllib.request, "urlopen", side_effect=AssertionError("no network")):
                paths = prepare_ocr.fetch_lms(out, src)
                self.assertEqual(sorted(p.name for p in paths), sorted([*pins, prepare_ocr.VOCR_LM_ATTRIBUTION]))
                (src / prepare_ocr.VOCR_LM_ATTRIBUTION).write_bytes(b"something else")
                (out / prepare_ocr.VOCR_LM_ATTRIBUTION).unlink()
                with self.assertRaises(RuntimeError):
                    prepare_ocr.fetch_lms(out, src)
        # the real pin is the release's file
        url, size, sha = prepare_ocr.VOCR_LM_ATTRIBUTION_PIN[prepare_ocr.VOCR_LM_ATTRIBUTION]
        self.assertTrue(url.endswith("/releases/download/v0.2.6-ctclm.2/ATTRIBUTION.md"))
        self.assertEqual((size, sha), (2538, "1de91ab9c181e204c53ae7c2796af47366026901fc8b2c268cbcef820ebaf19c"))

    def test_lm_pins_agree_c_kernel_python_and_the_ledger(self):
        table = c_lm_table()
        self.assertEqual(set(table), set(prepare_ocr.VOCR_LM_ASSETS))
        for name, (size, sha, tier) in table.items():
            url, want_size, want_sha = prepare_ocr.VOCR_LM_ASSETS[name]
            self.assertTrue(url.endswith("/releases/download/v0.2.6-ctclm.2/" + name), url)
            self.assertEqual((size, sha), (want_size, want_sha), name)
        self.assertEqual(sorted(t for _, _, t in table.values()), [1, 2, 3, 4])
        # the kernel knows the same names and sizes by tier
        kernel = (ROOT / "kernel/game_ocr.c").read_text()
        for n in range(1, 5):
            self.assertIn(f"VJO_VOCR_LM{n}_NAME", kernel)
            self.assertIn(f"VJO_VOCR_LM{n}_BYTES", kernel)
        # the ledger's LM block holds the largest, page-rounded
        ledger = (ROOT / "include/vjo_ledger_budget.h").read_text()
        budget = int(re.search(r"#define VJO_BUDGET_GAME_LM (\d+)u", ledger).group(1))
        largest = max(size for size, _, _ in table.values())
        self.assertEqual(budget, (largest + 4095) // 4096 * 4096)
        self.assertIn("\nocr_lm = 1000k\n", (ROOT / "docs/config.example.ini").read_text())


    def test_vocr3_assets_are_the_published_release(self):
        self.assertEqual(prepare_ocr.VOCR3_TAG, "v0.2.5-vocr3.1")
        for name, size, sha in (("VOCR3_H15_w8.vocr", 1155968, VOCR3_H15_SHA256),
                                ("VOCR3_FL10_w8.vocr", 657664, VOCR3_FL10_SHA256)):
            url, got_size, got_sha = prepare_ocr.VOCR3_ASSETS[name]
            self.assertTrue(url.endswith("/releases/download/v0.2.5-vocr3.1/" + name), url)
            self.assertEqual((got_size, got_sha), (size, sha))
        url, size, sha = prepare_ocr.VOCR_ASSETS["H15_w8.vocr"]
        self.assertEqual(prepare_ocr.VOCR_TAG, "v0.2.0")
        self.assertTrue(url.endswith("/releases/download/v0.2.0/H15_w8.vocr"))
        self.assertEqual((size, sha), (1155968, H15_V020_SHA256))

    def test_c_kernel_and_python_pins_agree_and_fit_the_ceiling(self):
        table = c_table()
        python = python_pins()
        self.assertEqual(set(table), set(python))
        for name, (size, sha, arena) in table.items():
            _, want_size, want_sha, _ = python[name]
            self.assertEqual((size, sha), (want_size, want_sha), name)
            self.assertLessEqual(size, CEILING_WEIGHTS, name)
            self.assertLessEqual(arena, CEILING_ARENA, name)
        # the kernel opens exactly the pinned names, at their pinned sizes
        self.assertEqual(kernel_names(), {name: size for name, (size, _, _) in table.items()})
        # the model metadata names exactly the same files, at the same sizes
        self.assertEqual({name: size for name, (_, _, size) in metadata_table().items()},
                         {name: size for name, (size, _, _) in table.items()})
        # VOCR3 has the shapes of v0.2.0's H15 and FL10: the same bytes and arena, other weights
        for new, old in (("VOCR3_H15_w8.vocr", "H15_w8.vocr"), ("VOCR3_FL10_w8.vocr", "FL10_w8.vocr")):
            self.assertEqual(table[new][0], table[old][0])
            self.assertEqual(table[new][2], table[old][2])
            self.assertNotEqual(table[new][1], table[old][1])

    def test_a_pin_in_any_spelling_is_read_or_fails(self):
        """A model pinned in one table only cannot escape the comparison above: the parsers read it
        (a literal size as well as a macro), and an entry they cannot read fails the entry count."""
        def add(source, after, line):
            self.assertEqual(source.count(after), 1, after)
            return source.replace(after, after + line)
        kernel = (ROOT / "kernel/game_ocr.c").read_text()
        f20 = '    if (!strcmp(name, "F20_w8.vocr")) return VJO_VOCR_F20_BYTES;\n'
        literal = kernel_names(add(kernel, f20, '    if (!strcmp(name, "VOCR5_N_w8.vocr")) return 1155968u;\n'))
        self.assertEqual(literal["VOCR5_N_w8.vocr"], 1155968)
        self.assertNotEqual(literal, kernel_names())
        for line in ('    if (!strcmp(name, "VOCR5_N_w8.vocr")) return 0x11a380u;\n',  # a hex size
                     '    if (!memcmp(name, "VOCR5", 5)) return VJO_VOCR_H15_BYTES;\n',  # another comparison
                     '    if (!strcmp(name, "a.vocr") || !strcmp(name, "VOCR5_N_w8.vocr")) return VJO_VOCR_H15_BYTES;\n',
                     '    if (!strcmp(name, "H15_w8.vocr")) return VJO_VOCR_F20_BYTES;\n'):  # a name twice
            with self.assertRaises(AssertionError, msg=line):
                kernel_names(add(kernel, f20, line))
        worker = (ROOT / "core/vocr_ocr.c").read_text()
        end = ("      0x12,0xf4,0x05,0xf5,0x4b,0x9d,0xf8,0xa2,0xe5,0x5b,0xe8,0x2c,0x38,0x5b,0x71,0x6e},\n"
               "     241056, VOCR_LINES_GRAD},\n")  # VOCR4-W20's, the release table's last entry
        sha = "{" + ",".join(["0x00"] * 32) + "}"
        literal = c_table(add(worker, end, f'    {{"VOCR5_N_w8.vocr", 1155968u, {sha}, 182944u, VOCR_LINES_GRAD}},\n'))
        self.assertEqual(literal["VOCR5_N_w8.vocr"], (1155968, "00" * 32, 182944))
        with self.assertRaises(AssertionError):
            c_table(add(worker, end, f'    {{.name = "VOCR5_N_w8.vocr", .bytes = 1155968u, .sha256 = {sha}}},\n'))
        meta = (ROOT / "core/vocr_models.c").read_text()
        f20 = '    {"F20_w8.vocr", "VOCR1", "F20", VJO_VOCR_F20_BYTES},\n'
        literal = metadata_table(add(meta, f20, '    {"VOCR5_N_w8.vocr", "VOCR5", "N", 3000000u},\n'))
        self.assertEqual(literal["VOCR5_N_w8.vocr"], ("VOCR5", "N", 3000000))
        for line in ('    {"VOCR5_N_w8.vocr", "VOCR5", "N", 0x2dc6c0},\n', f20):  # a hex size; a name twice
            with self.assertRaises(AssertionError, msg=line):
                metadata_table(add(meta, f20, line))

    def test_each_model_generation_is_its_releases(self):
        """The generation the Overlay app names the local engine after: the same in the C metadata
        and in the packager's pins (each release's), VOCR3 for the default, and the VOCRn of a file
        named VOCRn_...; the removed VOCR2 trial is pinned nowhere."""
        meta = metadata_table()
        python = python_pins()
        self.assertEqual(len(meta), 6)
        for name, (generation, shape, _) in meta.items():
            self.assertIn(name, python)
            self.assertEqual(generation, prepare_ocr.VOCR_GENERATION[python[name][3]], name)
            if name.startswith("VOCR"):
                self.assertTrue(name.startswith(generation + "_"), name)
            self.assertIn(shape, name, name)
        self.assertEqual(prepare_ocr.VOCR_GENERATION, {"v0.2.0": "VOCR1", "v0.2.5-vocr3.1": "VOCR3",
                                                       "v0.2.5-vocr4.1": "VOCR4"})
        self.assertEqual(meta[prepare_ocr.VOCR_DEFAULT_MODEL][0], "VOCR3")
        self.assertEqual(meta[prepare_ocr.VOCR_FALLBACK_MODEL][0], "VOCR3")
        for path in ("core/vocr_ocr.c", "core/vocr_models.c", "kernel/game_ocr.c", "include/vjo_vocr_sizes.h",
                     "tools/prepare_ocr.py", "tools/package_release.py"):
            text = (ROOT / path).read_text()
            for pin in ('"VOCR2_FL10_w8.vocr"', "'VOCR2_FL10_w8.vocr'", "VJO_VOCR2", "VOCR2_ASSETS"):
                self.assertNotIn(pin, text, path)

    def test_defaults_everywhere_are_vocr3_h15_with_vocr3_fl10_fallback(self):
        self.assertEqual((prepare_ocr.VOCR_DEFAULT_MODEL, prepare_ocr.VOCR_FALLBACK_MODEL),
                         ("VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr"))
        config = (ROOT / "core/config.c").read_text()
        self.assertIn('c->vocr_model, sizeof(c->vocr_model), "VOCR3_H15_w8.vocr"', config)
        self.assertIn('c->vocr_fallback_model, sizeof(c->vocr_fallback_model), "VOCR3_FL10_w8.vocr"', config)
        self.assertIn('#define VJO_VOCR_DEFAULT_MODEL "VOCR3_H15_w8.vocr"', (ROOT / "core/vocr_ocr.h").read_text())
        example = (ROOT / "docs/config.example.ini").read_text()
        self.assertIn("\nvocr_model = VOCR3_H15_w8.vocr\n", example)
        self.assertIn("\nvocr_fallback_model = VOCR3_FL10_w8.vocr\n", example)
        self.assertIn("\nlog_file = on\n", example)
        installer = (ROOT / "tools/install-vita.sh").read_text()
        self.assertIn("--set vocr_model=VOCR3_H15_w8.vocr", installer)
        self.assertIn("--set vocr_fallback_model=VOCR3_FL10_w8.vocr", installer)
        package = (ROOT / "tools/package_release.py").read_text()
        self.assertNotIn('"vocr_model": "', package)  # no model name of its own: the template's
        # VOCR3 and the v0.2.0 fallback are both shipped, each with its own release's pins
        import package_release  # noqa: E402
        generations = {name: {"generation": prepare_ocr.VOCR_GENERATION[tag]}
                       for name, _, tag, _ in package_release.shipped_models() if name.endswith(".vocr")}
        defaults = package_release.manifest_defaults(example, generations)
        self.assertEqual((defaults["vocr_model"], defaults["vocr_fallback_model"], defaults["ocr_engine"]),
                         (prepare_ocr.VOCR_DEFAULT_MODEL, prepare_ocr.VOCR_FALLBACK_MODEL, "VOCR3"))
        shipped = {name: (pins[name], tag, role) for name, pins, tag, role in package_release.shipped_models()}
        self.assertEqual(list(shipped)[:4], ["VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr", "H15_w8.vocr", "FL10_w8.vocr"])
        self.assertEqual(shipped["VOCR3_H15_w8.vocr"][1:], ("v0.2.5-vocr3.1", "default"))
        self.assertEqual(shipped["VOCR3_FL10_w8.vocr"][1:], ("v0.2.5-vocr3.1", "fallback"))
        self.assertEqual(shipped["H15_w8.vocr"][0][2], H15_V020_SHA256)
        self.assertEqual(shipped["H15_w8.vocr"][1], "v0.2.0")


if __name__ == "__main__":
    unittest.main()
