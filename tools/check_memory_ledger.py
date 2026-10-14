#!/usr/bin/env python3
"""Memory ledger check (docs/memory-ledger.md).

Fails when:
  1. a source file of the three plugins calls an allocator or creates a
     thread anywhere but in the wrappers listed in ALLOWED (an unaccounted
     allocation site);
  2. a pool's worst case in include/vjo_ledger_budget.h exceeds its limit;
  3. a packaged module (--kernel/--shell/--worker) has more pages than the
     budget declares for it (module pages may only shrink).

Prints the ledger table either way, so the build log shows it.
"""
import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUDGET = ROOT / "include/vjo_ledger_budget.h"

# Directories whose code is linked into a plugin. settings/ is the standalone
# VPK app and host/ is host-only.
SCAN_DIRS = ("shell", "kernel", "game_ocr", "core", "vita", "include")
SCAN_SUFFIXES = (".c", ".cpp", ".h", ".hpp")

# Calls that take memory from a pool. Matched after comments are stripped.
CALLS = (
    "sceKernelAllocMemBlock", "sceKernelAllocMemBlockForVM", "ksceKernelAllocMemBlock",
    "ksceKernelAllocMemBlockWithInfo", "ksceKernelAllocHeapMemory", "ksceKernelCreateHeap",
    "sce_paf_memalign", "sce_paf_malloc", "sce_paf_calloc", "sce_paf_realloc",
    "sceKernelCreateThread", "ksceKernelCreateThread",
    "malloc", "calloc", "realloc", "aligned_alloc", "posix_memalign",
    "sceClibMspaceCreate", "taiAllocForKernel",
)
CALL_RE = re.compile(r"(?<![A-Za-z0-9_])(" + "|".join(map(re.escape, CALLS)) + r")\s*\(")
NEW_RE = re.compile(r"(?<![A-Za-z0-9_])new\s+[A-Za-z_(]")

# The wrappers: (file, call) -> how many call sites it may hold. Every site
# here is a ledger wrapper: it names its item and calls vjo_ledger_add/sub.
ALLOWED = {
    ("shell/memory.cpp", "sce_paf_memalign"): 1,   # vjo_shell_heap_alloc_for
    ("shell/ledger.c", "sceKernelCreateThread"): 1, # vjo_shell_thread_create
    ("shell/ledger.c", "sceKernelAllocMemBlock"): 1, # vjo_shell_memblock_alloc (host tests' USER_RW path)
    ("kernel/buffers.c", "ksceKernelAllocMemBlock"): 2, # buffers_alloc (Shell JPEG), buffers_kstrip_alloc (OCR ladder's last rung)
    ("kernel/main.c", "ksceKernelCreateThread"): 1,  # the worker thread, counted right after
    ("game_ocr/main.c", "sceKernelAllocMemBlock"): 1, # game_memblock_alloc
    ("game_ocr/main.c", "sceKernelCreateThread"): 2, # vjo_game_ocr_worker_start and pool_run (a helper), each counted right after
}

POOLS = (
    # name, permanent macro, worst-case macro, limit macro, extra per-item limits
    ("shell pages", "VJO_WORST_SHELL_PAGES_PERMANENT", "VJO_WORST_SHELL_PAGES", "VJO_LIMIT_SHELL_PAGES", ()),
    ("shell paf", "VJO_WORST_SHELL_PAF_PERMANENT", "VJO_WORST_SHELL_PAF", "VJO_LIMIT_SHELL_PAF", ()),
    ("kernel", "VJO_WORST_KERNEL_PERMANENT", "VJO_WORST_KERNEL", "VJO_LIMIT_KERNEL", ()),
    ("game", "VJO_WORST_GAME_PERMANENT", "VJO_WORST_GAME_JOB", "VJO_LIMIT_GAME_JOB",
     (("VJO_BUDGET_GAME_WEIGHTS", "VJO_LIMIT_GAME_WEIGHTS"), ("VJO_BUDGET_GAME_WORKSPACE", "VJO_LIMIT_GAME_WORKSPACE"))),
)
# Opt-in configurations whose worst case is checked against the same limit but
# whose overshoot the runtime reserve guard turns into a survivable refusal
# (shell/memory.cpp; host tests): reported as a warning with the exact bytes,
# never hidden, never a build failure.
SOFT = (
    ("shell paf (network dictionary)", "VJO_WORST_SHELL_PAF_NET", "VJO_LIMIT_SHELL_PAF"),
)
ITEMS = {
    "shell pages": ("VJO_BUDGET_SHELL_MODULE_PAGES", "VJO_BUDGET_WORKER_IN_SHELL_PAGES", "VJO_BUDGET_SHELL_NET_STACK",
                    "VJO_BUDGET_SHELL_CTL_STACK", "VJO_BUDGET_SHELL_ANKI_STACK", "VJO_BUDGET_SHELL_USERRW_RESULTS"),
    "shell paf": ("VJO_BUDGET_PAF_PLUGIN", "VJO_BUDGET_PAF_RESULTS_LOCAL", "VJO_BUDGET_PAF_RESULTS_NET", "VJO_BUDGET_PAF_ANKI",
                  "VJO_BUDGET_PAF_UI"),
    # VJO_BUDGET_KERNEL_OCR_STRIP is never held with the capture (kernel/buffers.c): not in the sum
    "kernel": ("VJO_BUDGET_KERNEL_MODULE_PAGES", "VJO_BUDGET_KERNEL_STACK", "VJO_BUDGET_KERNEL_CAPTURE",
               "VJO_BUDGET_KERNEL_OCR_STRIP"),
    "game": ("VJO_BUDGET_GAME_MODULE_PAGES", "VJO_BUDGET_GAME_STACK", "VJO_BUDGET_GAME_WEIGHTS", "VJO_BUDGET_GAME_WORKSPACE",
             "VJO_BUDGET_GAME_CAPTURE", "VJO_BUDGET_GAME_LINE", "VJO_BUDGET_GAME_HELPERS"),
}
# Packaged module -> the macro its pages must not exceed.
MODULE_BUDGETS = {
    "kernel": ("VitaJPOverlay_Kernel", "VJO_BUDGET_KERNEL_MODULE_PAGES"),
    "shell": ("VitaJPOverlay_Shell", "VJO_BUDGET_SHELL_MODULE_PAGES"),
    "worker": ("VitaJPOverlay_OCR", "VJO_BUDGET_WORKER_MODULE_PAGES"),
}
PAGE = 4096


class CheckError(Exception):
    pass


def strip_comments(text):
    """Removes /* */ and // comments and the contents of string literals,
    keeping line structure."""
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("\n" * text[i:end].count("\n"))
            i = end
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
        elif text[i] == '"':
            j = i + 1
            while j < n and text[j] != '"' and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append('""')
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def scan_sources(root):
    """Returns (sites, problems): every allocator call found, and the ones
    that are not allowed."""
    sites, problems = [], []
    counts = {}
    for d in SCAN_DIRS:
        for path in sorted((root / d).rglob("*")):
            if path.suffix not in SCAN_SUFFIXES or not path.is_file():
                continue
            rel = path.relative_to(root).as_posix()
            text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
            for line_no, line in enumerate(text.splitlines(), 1):
                for m in CALL_RE.finditer(line):
                    call = m.group(1)
                    sites.append((rel, line_no, call))
                    counts[(rel, call)] = counts.get((rel, call), 0) + 1
                if path.suffix in (".cpp", ".hpp") and NEW_RE.search(line):
                    sites.append((rel, line_no, "new"))
                    counts[(rel, "new")] = counts.get((rel, "new"), 0) + 1
    for (rel, call), n in sorted(counts.items()):
        allowed = ALLOWED.get((rel, call), 0)
        if n > allowed:
            where = [f"{r}:{l}" for r, l, c in sites if r == rel and c == call]
            problems.append(f"{call} at {', '.join(where)}: {n} site(s), {allowed} allowed; route it through a ledger wrapper")
    for (rel, call), allowed in ALLOWED.items():
        if counts.get((rel, call), 0) < allowed:
            problems.append(f"{rel} no longer holds its {call} wrapper; update ALLOWED in {Path(__file__).name}")
    return sites, problems


def parse_budget(path):
    """Evaluates the #define lines of the budget header (integer arithmetic)."""
    values = {}
    define = re.compile(r"^\s*#define\s+([A-Z_][A-Z0-9_]*)\s+(.*?)\s*(?:/\*.*)?$")
    text = path.read_text(encoding="utf-8")
    # join backslash continuations
    text = re.sub(r"\\\n", " ", text)
    for line in text.splitlines():
        m = define.match(line)
        if not m or m.group(1).endswith("_H"):
            continue
        name, expr = m.groups()
        expr = re.sub(r"/\*.*?\*/", "", expr).strip()
        if not expr:
            continue
        expr = re.sub(r"\b(\d+)[uUlL]+\b", r"\1", expr)
        if not re.fullmatch(r"[A-Z0-9_+\-*/() ]+", expr):
            raise CheckError(f"{path.name}: {name}: only integer arithmetic is allowed, got {expr!r}")
        for sym in re.findall(r"[A-Z_][A-Z0-9_]*", expr):
            if sym not in values:
                raise CheckError(f"{path.name}: {name} uses {sym} before it is defined")
            expr = re.sub(r"\b" + sym + r"\b", str(values[sym]), expr)
        values[name] = int(eval(expr, {"__builtins__": {}}, {}))  # digits and operators only, checked above
    return values


def self_pages(path):
    """Loaded pages of a Vita SELF: each PT_LOAD segment's memsz rounded to 4 KiB."""
    data = path.read_bytes()
    if data[:4] != b"SCE\0":
        raise CheckError(f"{path}: not a SELF")
    elf_offset, = struct.unpack_from("<Q", data, 0x40)
    phdr_offset, = struct.unpack_from("<Q", data, 0x48)
    phnum, = struct.unpack_from("<H", data, elf_offset + 0x2C)
    phentsize, = struct.unpack_from("<H", data, elf_offset + 0x2A)
    pages, segments = 0, []
    for i in range(phnum):
        p_type, _, _, _, _, memsz, flags, _ = struct.unpack_from("<8I", data, phdr_offset + i * phentsize)
        if p_type == 1 and memsz:
            rounded = (memsz + PAGE - 1) // PAGE * PAGE
            segments.append((memsz, flags, rounded))
            pages += rounded
    return pages, segments, hashlib.sha256(data).hexdigest()


def check_budget(values):
    rows, problems, warnings = [], [], []
    for name, perm, worst, limit, extra in POOLS:
        p, w, lim = values[perm], values[worst], values[limit]
        rows.append({"pool": name, "permanent": p, "worst": w, "limit": lim, "headroom": lim - w,
                     "items": {k: values[k] for k in ITEMS[name]}})
        if w > lim:
            problems.append(f"pool {name}: worst case {w:,} B exceeds its limit {lim:,} B by {w - lim:,} B")
        for item, item_limit in extra:
            if values[item] > values[item_limit]:
                problems.append(f"pool {name}: {item} {values[item]:,} B exceeds {item_limit} {values[item_limit]:,} B")
    for name, worst, limit in SOFT:
        w, lim = values[worst], values[limit]
        rows.append({"pool": name, "permanent": None, "worst": w, "limit": lim, "headroom": lim - w, "items": {}, "soft": True})
        if w > lim:
            warnings.append(f"{name}: worst case {w:,} B exceeds the limit {lim:,} B by {w - lim:,} B; the runtime reserve "
                            "guard refuses the last allocation (survived, not a crash)")
    return rows, problems, warnings


def check_modules(values, modules):
    rows, problems = [], []
    for key, path in modules.items():
        if not path:
            continue
        name, macro = MODULE_BUDGETS[key]
        pages, segments, digest = self_pages(path)
        rows.append({"module": name, "pages": pages, "declared": values[macro],
                     "segments": [{"memsz": m, "flags": f, "pages": r} for m, f, r in segments], "sha256": digest})
        if pages > values[macro]:
            problems.append(f"{name}: {pages:,} B of pages, budget {macro} is {values[macro]:,} B (module pages may only shrink; "
                            "lower the footprint or raise the budget deliberately, with the number cited in the PR)")
    return rows, problems


def print_table(budget_rows, module_rows, sites):
    print("memory ledger (include/vjo_ledger_budget.h)")
    print(f"  {'pool':30} {'permanent':>12} {'worst case':>12} {'limit':>12} {'headroom':>12}")
    for r in budget_rows:
        perm = f"{r['permanent']:,}" if r["permanent"] is not None else "(opt-in)"
        print(f"  {r['pool']:30} {perm:>12} {r['worst']:>12,} {r['limit']:>12,} {r['headroom']:>12,}")
        for k, v in r["items"].items():
            print(f"      {k:36} {v:>12,}")
    if module_rows:
        print("  packaged modules (loaded pages vs declared budget)")
        for r in module_rows:
            segs = " + ".join(f"{s['memsz']:,}->{s['pages']:,}" for s in r["segments"])
            print(f"      {r['module']:22} {r['pages']:>10,} B of {r['declared']:>10,} B declared ({segs})")
    print(f"  allocation sites: {len(sites)}, all in ledger wrappers")
    for rel, line, call in sites:
        print(f"      {rel}:{line} {call}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--shell", type=Path)
    parser.add_argument("--worker", type=Path)
    parser.add_argument("--json", action="store_true", help="print the result as JSON")
    args = parser.parse_args()
    problems = []
    try:
        values = parse_budget(args.root / "include/vjo_ledger_budget.h")
        sites, scan_problems = scan_sources(args.root)
        budget_rows, budget_problems, warnings = check_budget(values)
        module_rows, module_problems = check_modules(values, {"kernel": args.kernel, "shell": args.shell, "worker": args.worker})
        problems = scan_problems + budget_problems + module_problems
    except (CheckError, KeyError, OSError) as e:
        print(f"memory ledger: {e}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps({"result": "fail" if problems else "pass", "pools": budget_rows, "modules": module_rows,
                          "sites": [{"file": r, "line": l, "call": c} for r, l, c in sites], "problems": problems,
                          "warnings": warnings}, indent=2))
    else:
        print_table(budget_rows, module_rows, sites)
    for w in warnings:
        print(f"memory ledger: WARNING: {w}", file=sys.stderr)
    for p in problems:
        print(f"memory ledger: FAIL: {p}", file=sys.stderr)
    if not problems and not args.json:
        print("memory ledger: pass")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
