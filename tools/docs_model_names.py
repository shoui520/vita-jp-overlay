#!/usr/bin/env python3
"""Writes the files the kernel's model broker opens, read from kernel/game_ocr.c, into the docs.

    python3 tools/docs_model_names.py           # rewrite the names in README.md and docs/vita-vn-ocr.md
    python3 tools/docs_model_names.py --check   # exit 1 if a document's names are not the kernel's

The names are kernel/game_ocr.c's own: model_size() compares every recognizer file the broker opens and
lm_name() lists every LM tier's, both through include/vjo_vocr_sizes.h's macros. tools/prepare_ocr.py's
pins say which release, and so which generation, each one comes from. Each document holds the text
between BEGIN and END, wrapped at 80 columns (tools/tests/test_docs_model_names.py checks it).
"""
import argparse
import re
import sys
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import prepare_ocr  # noqa: E402

DOCS = ("README.md", "docs/vita-vn-ocr.md")
# Inline HTML comments: GitHub shows neither, and neither may contain two hyphens in a row.
BEGIN = "<!-- names from kernel/game_ocr.c, written by tools/docs_model_names.py -->"
END = "<!-- end of the names -->"
BLOCK = re.compile(re.escape(BEGIN) + r"\n.*?" + re.escape(END), re.S)
WIDTH = 80


def macros(header):
    """The string and number macros of include/vjo_vocr_sizes.h, aliases resolved."""
    out = {m.group(1): m.group(2) for m in re.finditer(r'#define (VJO_\w+) "([^"]+)"', header)}
    out.update({m.group(1): int(m.group(2)) for m in re.finditer(r"#define (VJO_\w+) (\d+)u\b", header)})
    for m in re.finditer(r"#define (VJO_\w+) (VJO_\w+)\s*$", header, re.M):
        if m.group(2) in out:
            out[m.group(1)] = out[m.group(2)]
    return out


def function(source, signature):
    """A C function from its signature to its closing brace, without comments."""
    text = source[source.index(signature):]
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text[:text.index("\n}\n")], flags=re.S)


def kernel_names(source=None, header=None):
    """(recognizers, LMs) the broker opens, in kernel/game_ocr.c's order. Every comparison and return of
    model_size() and every entry of lm_name()'s table must be read, each name once: anything else is a
    ValueError, so no file it opens can be left out of the docs."""
    if source is None:
        source = (ROOT / "kernel/game_ocr.c").read_text(encoding="utf-8")
    sizes = macros(header if header is not None else (ROOT / "include/vjo_vocr_sizes.h").read_text(encoding="utf-8"))
    sizer = function(source, "static uint32_t model_size(const char *name)")
    read = re.findall(r'if \(!strcmp\(name, ("[\w.]+"|VJO_\w+)\)\) return \w+;', sizer)
    models = [n.strip('"') if n.startswith('"') else sizes.get(n) for n in read]
    if (None in models or len(set(models)) != len(models) or sizer.count("strcmp(") != len(models)
            or len(re.findall(r"\breturn\b", sizer)) != len(models) + 1 or not re.search(r"\breturn 0;\s*$", sizer)):
        raise ValueError("kernel/game_ocr.c model_size(): a name comparison this tool cannot read")
    table = re.search(r"names\[VJO_VOCR_LM_TIERS\] = \{([^}]*)\};", function(source, "static const char *lm_name("))
    lms = [sizes.get(entry.strip()) for entry in table.group(1).split(",")] if table else []
    if not table or None in lms or len(set(lms)) != len(lms) or len(lms) != sizes.get("VJO_VOCR_LM_TIERS"):
        raise ValueError("kernel/game_ocr.c lm_name(): not one pinned name for each of VJO_VOCR_LM_TIERS")
    return models, lms


def release(name):
    """The release tag of a pinned recognizer: the VOCRn_TAG of the one VOCRn_ASSETS table holding it."""
    tags = [getattr(prepare_ocr, key[:-len("ASSETS")] + "TAG") for key in sorted(vars(prepare_ocr))
            if re.fullmatch(r"VOCR\d*_ASSETS", key) and name in getattr(prepare_ocr, key)]
    if len(tags) != 1:
        raise ValueError(f"{name}: the kernel opens it, and tools/prepare_ocr.py pins it in {len(tags)} releases")
    return tags[0]


def listing(items):
    return items[0] if len(items) == 1 else ", ".join(items[:-1]) + " and " + items[-1]


def names_text(models, lms):
    """The recognizers by generation and release, then the LMs, as the docs say them."""
    groups = {}
    for name in models:
        tag = release(name)
        groups.setdefault((prepare_ocr.VOCR_GENERATION[tag], tag), []).append(f"`{name}`")
    for name in lms:
        if name not in prepare_ocr.VOCR_LM_ASSETS:
            raise ValueError(f"{name}: the kernel opens it, and tools/prepare_ocr.py does not pin it")
    recognizers = "; ".join(f"{listing(names)} ({generation}, release {tag})"
                            for (generation, tag), names in groups.items())
    return (f"the recognizers {recognizers}; and the character LMs {listing([f'`{n}`' for n in lms])} "
            f"(release {prepare_ocr.VOCR_LM_TAG})")


def block():
    """What sits between the markers: a line break, then the text wrapped at WIDTH, then END."""
    text = textwrap.fill(names_text(*kernel_names()), WIDTH, break_long_words=False, break_on_hyphens=False)
    return BEGIN + "\n" + text + END


def stale(root=ROOT, docs=DOCS):
    """The documents whose names are not the kernel's (or that hold no names at all)."""
    want = block()
    out = []
    for doc in docs:
        found = BLOCK.findall((Path(root) / doc).read_text(encoding="utf-8"))
        if not found or any(b != want for b in found):
            out.append(doc)
    return out


def write(root=ROOT, docs=DOCS):
    want = block()
    for doc in docs:
        path = Path(root) / doc
        text = path.read_text(encoding="utf-8")
        new, n = BLOCK.subn(lambda _: want, text)
        if not n:
            raise ValueError(f"{doc}: no names between {BEGIN} and {END}")
        if new != text:
            path.write_text(new, encoding="utf-8")
        print(f"{doc}: {'updated' if new != text else 'up to date'} ({n} place{'s' if n > 1 else ''})")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="exit 1 if a document's names are not the kernel's")
    args = parser.parse_args()
    if args.check:
        bad = stale()
        for doc in bad:
            print(f"{doc}: its model names are not kernel/game_ocr.c's; run python3 tools/docs_model_names.py",
                  file=sys.stderr)
        return 1 if bad else 0
    write()
    return 0


if __name__ == "__main__":
    sys.exit(main())
