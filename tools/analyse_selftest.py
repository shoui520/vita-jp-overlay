#!/usr/bin/env python3
"""Analyse an on-device self-test report (docs/self-test.md).

    python3 tools/analyse_selftest.py build/device/status/selftest-report.txt [--json]
    python3 tools/analyse_selftest.py --collect build/device/collect/vjo-collect-STAMP [--json]

Prints two tables, then the free memory every memory route read at each step,
the game worker's own memory around each allocation, and what failed, in plain
words:

- the run and its OCR, PASS/FAIL: boot, ok, game, the OCR stages, recognitions,
  done. These rows alone decide the verdict.
- diagnostics, PASS/WARN, never FAIL: the capture probe, the capture block's
  size, the kernel pool, tearing, the kernel memory routes, the game's memory.
  They say how OCR ran and what the console measured; a diagnostic that could
  not read or did not work is a WARN and never changes the verdict. #75's
  first build checked the wrong address range in every kernel memory route:
  its report would have said FAIL with OCR working.

A stage the Vita stopped in (a begin record and no end) is the run's failure
whichever stage it was, the probe included: the verdict is INCOMPLETE.

v2 reports (API 19, `selftest v2`): boot, ok, game, then the stages one OCR job
runs on the first settled screen (fallback, textbox, fullscreen, probe,
textbox2), then done. v1 reports (API 18, `selftest v1`, as #74 sent) get the
same table for their steps (boot, ok, game, ocr, done).

After the diagnostics come each stage's memory headroom (free USER, plus another
model's weights it released, minus the reserve and what the job needed) and the
recognizer tier (vita-vn-ocr #74's Small/Normal/Large proposal) that the game's
measured free memory allows.

--collect DIR reads a folder written by `install_ftp.sh --collect` (tools/collect_vita.py):
the installed files against the package, config.ini (API keys already redacted),
both taiHEN configurations, boot.txt, kernel.txt, log.txt and log.old.txt (every
OCR job, its memory and times, the dictionary, the overlay, Anki), the Anki queue,
crash dumps, the self-test analysis when a report is there, the tier from the lowest
free game memory seen, and what this tool cannot decide, with where the data is.

Exit status follows the verdict: 0 PASS (diagnostic WARNs included), 1 FAIL or
INCOMPLETE, 2 for a file that is not a report. With --collect: the self-test's
verdict when a report is there, else 0. `--json` adds `ocr_result`,
`diagnostics_result` and `warnings`, and each row's `kind` (ocr or diagnostic).
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

KD_TOTAL = 36 * 1024 * 1024          # ScePhyMemPartKD on retail firmware (wiki Memory_budget)
SHELL_TOTAL = 50 * 1024 * 1024       # ScePhyMemPartShell
SHELL_LOW = 512 * 1024               # SceShell free main below this: a firmware thread's next page can fail (#59)
PAF_RESERVE = 1536 * 1024            # shell/memory.cpp RESERVE: below it every PAF request is refused
FULL_FRAME = 960 * 544 * 4           # API 18's kernel capture block at the largest region
CAPTURE_LIMIT = 64 * 1024            # API 19: a region's capture block (one strip and its row checksums)
STEPS_V1 = ("boot", "ok", "game", "ocr", "done")
STAGES = ("fallback", "textbox", "fullscreen", "probe", "textbox2")  # shell/selftest.c's order
ROWS_V2 = ("boot", "ok", "game") + STAGES + ("done",)
# Rows that report how OCR ran, never whether it worked: PASS or WARN only.
DIAGNOSTIC_ROWS = ("probe", "capture<64KiB", "kernel-pool", "tearing", "kernel-memory", "game-memory")
ROUTES = (("user", "user API (sceKernelGetFreeMemorySize)"), ("block kernel", "kernel block route, KD"),
          ("block shell", "kernel block route, SceShell"), ("cas", "address-space route (PSVshell)"),
          ("as", "kernel process route (faps-coredump)"))
# The recognizer tiers vita-vn-ocr #74 proposes (docs/model-tiers.md, its section 1.5: a spec, not
# final): the free game USER a first job of a tier needs is page(weights) + page(max(the line finder's
# arena at 960x544, the tier's W8I arena) + the runtime's structs) + the capture block + the line-block
# ceiling, and the tier keeps its own reserve. Here each tier is priced at its ceiling.
TIER_FINDER_ARENA, TIER_RUNTIME_STRUCTS = 83_323, 3_760     # model-tiers.json "overlay" (main 64b5726)
TIER_CAPTURE, TIER_LINE = 61_440, 516_096                   # VJO_BUDGET_GAME_CAPTURE page, VJO_VOCR_LINE_MAX_BYTES
TIERS = (("Large", 9_000_000, 1_310_720, 16 * 1024 * 1024), ("Normal", 4_000_000, 262_144, 8 * 1024 * 1024),
         ("Small", 999_999, 131_072, 4 * 1024 * 1024))     # (name, weights ceiling, arena ceiling, reserve)
GAME_STEPS_MAX = 20                                         # include/vjo_game_ocr.h VJO_GAME_OCR_STEPS


def page(n):
    return (n + 4095) & ~4095


def tier_need(weights, arena):
    return page(weights) + page(max(TIER_FINDER_ARENA, arena) + TIER_RUNTIME_STRUCTS) + TIER_CAPTURE + TIER_LINE


def tier_table(free_user):
    """Each tier at its ceiling against one measured free USER figure; the recommendation is the largest that fits."""
    rows = []
    for name, weights, arena, reserve in TIERS:
        need = tier_need(weights, arena)
        rows.append({"tier": name, "weights": weights, "arena": arena, "need": need, "reserve": reserve,
                     "margin": free_user - reserve - need, "fits": free_user - reserve >= need})
    best = next((r["tier"] for r in rows if r["fits"]), None)
    return {"free_user": free_user, "rows": rows, "recommendation": best}


def render_tiers(t, where):
    lines = [f"model tiers (vita-vn-ocr #74's proposal, docs/model-tiers.md; priced at each tier's ceiling, an estimate)",
             f"  free game USER {where}: {t['free_user']:,} B",
             f"  {'tier':7} {'weights':>11} {'arena':>10} {'first job needs':>16} {'reserve':>11} {'margin':>13} fits"]
    for r in t["rows"]:
        lines.append(f"  {r['tier']:7} {r['weights']:>11,} {r['arena']:>10,} {r['need']:>16,} {r['reserve']:>11,} "
                     f"{r['margin']:>13,} {'yes' if r['fits'] else 'no'}")
    lines.append(f"  recommendation: {t['recommendation'] or 'none: not even Small fits'} "
                 "(the largest tier whose first job fits beside its reserve; one sample, this game)")
    return lines


# include/vjo_api.h VJO_MEMSTEP_*: where a kernel route stopped
MEMSTEPS = {1: "find", 2: "block/address-space info", 3: "pointer outside the kernel range",
            4: "no kernel memory block", 5: "InfoCore"}

# VJO_E_* of core/conn.h, as the game result reports them (read from the
# source when it is at hand, so the two cannot drift).
RC_NAMES = {0: "ok", -100: "connect", -101: "tls", -102: "http", -103: "out of memory", -104: "parse",
            -105: "too large", -106: "status", -107: "no key", -108: "source", -109: "anki", -110: "not found",
            -111: "anki duplicate", -112: "no host", -113: "local io", -114: "local format", -115: "ocr model",
            -116: "ocr region", -117: "ocr unavailable", -120: "screen changed"}
NATIVE = {0xBFFFFFFF: "no game", 0xBFFFFFFE: "no memory", 0xBFFFFFFD: "busy", 0xBFFFFFFC: "timeout",
          0xBFFFFFFB: "format", 0xBFFFFFFA: "argument", 0xBFFFFFF9: "copy", 0xBFFFFFF7: "cancelled",
          0xBFFFFFF8: "permission", 0x80024302: "NO_FREE_PHYSICAL_PAGE (the partition is out of pages)",
          0x80020001: "SCE_KERNEL_ERROR_ERROR (the call failed)", 0x80010002: "ENOENT", 0x80010013: "ENODEV",
          0x8001000D: "EACCES"}


def load_rc_names():
    """The VJO_E_* values from core/conn.h, if the source is at hand."""
    path = ROOT / "core/conn.h"
    if not path.is_file():
        return
    for m in re.finditer(r"VJO_E_([A-Z_]+)\s*=\s*(-\d+)", path.read_text(errors="replace")):
        RC_NAMES.setdefault(int(m.group(2)), m.group(1).lower().replace("_", " "))


def parse_value(v):
    if re.fullmatch(r"-?\d+", v):
        return int(v)
    if re.fullmatch(r"0x[0-9A-Fa-f]+", v):
        return int(v, 16)
    if re.fullmatch(r"\d+/\d+", v):
        a, b = v.split("/")
        return (int(a), int(b))
    return v


def parse(text):
    """Lines of `kind [words] key=value ...` into records."""
    records = []
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        tokens = line.split()
        kind = tokens[0]
        fields, words = {}, []
        for t in tokens[1:]:
            if "=" in t:
                k, v = t.split("=", 1)
                fields[k] = parse_value(v)
            else:
                words.append(t)
        records.append({"line": n, "kind": kind, "words": words, "fields": fields, "raw": line})
    return records


def u32(v):
    return v & 0xFFFFFFFF if isinstance(v, int) else 0


def s32(v):
    v = u32(v)
    return v - (1 << 32) if v & 0x80000000 else v


def native(v):
    v = u32(v)
    return f"0x{v:08X}" + (f" {NATIVE[v]}" if v in NATIVE else "")


def rc_text(rc):
    rc = s32(rc)
    if rc in RC_NAMES:
        return f"{RC_NAMES[rc]} (rc {rc})"
    return native(rc)


def mib(v):
    return f"{v:,} B"


# ---------------------------------------------------------------- v2 (API 19)

def analyse_v2(records):
    out = {"version": 2, "api": records[0]["fields"].get("api"), "steps": [], "samples": {}, "stages": {},
           "rows": [], "routes": [], "game_memory": [], "findings": [], "verdict": None}
    stages = out["stages"]
    for r in records:
        k, w, f = r["kind"], r["words"], r["fields"]
        if k in ("boot", "config"):
            out[k] = f
        elif k == "step" and w:
            out["steps"].append({"step": w[0], **f})
        elif k in ("mem", "ledger", "kernel") and w:
            out["samples"].setdefault(w[0], {})[k] = f
        elif k == "kmem" and w:
            route = f.get("route", "?") + (f" {f['part']}" if f.get("route") == "block" else "")
            out["samples"].setdefault(w[0], {}).setdefault("kmem", {})[route] = f
        elif k == "stage" and len(w) >= 2:
            s = stages.setdefault(w[0], {"gsteps": [], "lines": []})
            s[w[1]] = f
        elif k == "gstep" and len(w) >= 2:
            stages.setdefault(w[0], {"gsteps": [], "lines": []})["gsteps"].append({"step": w[1], **f})
        elif k in ("result", "memory") and w:
            stages.setdefault(w[0], {"gsteps": [], "lines": []})[k] = f
        elif k == "line" and w:
            stages.setdefault(w[0], {"gsteps": [], "lines": []})["lines"].append(f)
    steps = {s["step"]: s for s in out["steps"]}
    findings = out["findings"]
    rows = out["rows"]

    def row(name, ok, detail, kind="ocr"):
        rows.append({"row": name, "result": ok, "detail": detail, "kind": kind})

    def diag(name, ok, detail):
        """A diagnostic row: PASS, WARN or not reached, never FAIL, so it cannot change the verdict."""
        row(name, None if ok is None else ("PASS" if ok == "PASS" else "WARN"), detail, "diagnostic")

    boot = out.get("boot")
    if "boot" not in steps:
        row("boot", None, "no boot step: the Shell plugin never wrote one (see status.txt and kernel.txt)")
    elif boot and boot.get("mode") == 1:
        row("boot", "FAIL", "SAFE boot: the previous boot did not complete, the plugins stayed inert; reboot once")
    else:
        row("boot", "PASS", f"normal boot (seq {boot.get('seq', '?') if boot else '?'}, "
                            f"{boot.get('failures', 0) if boot else 0} failed boots before)")
    row("ok", "PASS" if "ok" in steps else None,
        f"stable minute at {steps['ok'].get('t', '?')} ms" if "ok" in steps else "not reached")
    g = steps.get("game")
    if not g:
        row("game", None, "not reached: no enabled game was started")
    elif g.get("enabled") == 0:
        row("game", "FAIL", f"{g.get('title')} is disabled in games.ini: no OCR ran")
    else:
        row("game", "PASS", f"{g.get('title')} enabled, region {g.get('region')}"
                            + (f" {g.get('rect')}" if g.get("region") == "user" else " (full screen)"))

    def failing_steps(s):
        bad = []
        for gs in s["gsteps"]:
            rc = s32(gs.get("rc", 0))
            if rc < 0:
                bad.append(f"{gs['step']} {gs.get('kind')}/{gs.get('arg')} {native(rc)}")
        return bad

    recognized = 0
    for name in STAGES:
        s = stages.get(name)
        kind = "diagnostic" if name in DIAGNOSTIC_ROWS else "ocr"
        if not s or "begin" not in s:
            row(name, None, "not reached", kind)
            continue
        end, res, mem = s.get("end"), s.get("result", {}), s.get("memory", {})
        if end is None:
            # A stop is the run's failure in any stage, the probe included: never a WARN.
            row(name, "FAIL", f"began at {s['begin'].get('t', '?')} ms and has no record: the Vita stopped "
                              f"(crash, hang or power) during this stage")
            continue
        rc = s32(end.get("rc", 0))
        bad = failing_steps(s)
        if name == "probe":
            paths = [gs for gs in s["gsteps"] if gs["step"] == "probe"]
            ok_paths = [f"{gs.get('kind')}->{gs.get('arg')}" for gs in paths if s32(gs.get("rc", 0)) > 0]
            failed = [f"{gs.get('kind')}->{gs.get('arg')} {native(gs.get('rc'))}" for gs in paths
                      if s32(gs.get("rc", 0)) <= 0]
            refused = [f"{gs.get('kind')} {native(gs.get('rc'))}" for gs in s["gsteps"]
                       if gs["step"] == "alloc" and s32(gs.get("rc", 0)) < 0]
            primary = "hook->user" in ok_paths
            detail = (f"worked: {', '.join(ok_paths) or 'none'}"
                      + (f"; failed: {', '.join(failed)}" if failed else "")
                      + (f"; memory refused: {', '.join(refused)}" if refused else "")
                      + (f"; the stage ended with {rc_text(rc)}" if rc else ""))
            diag(name, "PASS" if primary else "WARN", detail if primary else
                 "the display hook could not write the game's USER memory: " + detail)
            continue
        if rc != 0:
            row(name, "FAIL", f"{rc_text(rc)}" + (f"; failed steps: {'; '.join(bad)}" if bad else "")
                + (f"; free USER {mem.get('free_user'):,} B" if isinstance(mem.get("free_user"), int) else ""))
            continue
        recognized += end.get("recognized", 0)
        region = end.get("region", "?")
        detail = (f"{region} {end.get('model')}, {end.get('lines', 0)} line(s), "
                  f"capture {res.get('source', '?')} into {res.get('mem', '?')} {res.get('capture', 0):,} B "
                  f"({res.get('strips', 0)} strips of {res.get('strip_rows', 0)} rows, {res.get('tears', 0)} rows "
                  f"changed), weights {res.get('weights', '?')}"
                  + (f" in {res.get('load_ms')} ms" if res.get("weights") == "read" else "")
                  + f", recognize {res.get('recognize_ms', '?')} ms")
        verdict = "PASS"
        if name == "fallback" and not end.get("fallback"):
            verdict, detail = "FAIL", "the fallback model was not used: " + detail
        if name == "fullscreen":
            m = re.match(r"(\d+)x(\d+)", str(region))
            fb = re.match(r"(\d+)x(\d+)", str(end.get("fb", "")))
            if m and fb and m.groups() != fb.groups():
                verdict, detail = "FAIL", "the region was not the whole frame: " + detail
        row(name, verdict, detail)

    # the run's own requirements, one row each: the recognitions decide; the rest are diagnostics
    regions = [(n, stages[n]) for n in STAGES if n != "probe" and n in stages and stages[n].get("end")
               and s32(stages[n]["end"].get("rc", 0)) == 0]
    if regions:
        caps = [(n, s.get("result", {}).get("capture", 0), s.get("result", {}).get("mem", "?")) for n, s in regions]
        largest = max(b for _, b, _ in caps)
        over = [(n, b, m) for n, b, m in caps if b > CAPTURE_LIMIT]
        tall = [(n, gs.get("bytes", 0)) for n, s in regions for gs in s["gsteps"]
                if gs["step"] == "alloc" and gs.get("arg") == "line" and s32(gs.get("rc", 0)) >= 0]
        tall_text = (f"; plus a line taller than a strip, kept whole: {', '.join(f'{b:,} B in {n}' for n, b in tall)}"
                     if tall else "")
        if not over:
            diag("capture<64KiB", "PASS",
                 f"largest capture block {largest:,} B (limit {CAPTURE_LIMIT:,} B)" + tall_text)
        else:
            units = {"cdram": "CDRAM allocates in 256 KiB units", "phycont": "phycont allocates in 1 MiB units"}
            rungs = [units.get(m, m) for m in sorted({m for _, _, m in over if m != "user"})]
            diag("capture<64KiB", "WARN",
                 f"capture block over {CAPTURE_LIMIT:,} B in: {', '.join(f'{n} {b:,} B of {m}' for n, b, m in over)}"
                 + (f" (USER_RW refused it there, and {', '.join(rungs)})" if rungs else "") + tall_text)
        kstrip = [n for n, s in regions if s.get("result", {}).get("source") == "kstrip"]
        diag("kernel-pool", "PASS" if not kstrip else "WARN",
             "every capture was written by the display hook into the game's memory" if not kstrip else
             f"the kernel strip (<= 64 KiB of the kernel pool) was used in: {', '.join(kstrip)}")
        row("recognitions", "PASS" if recognized >= 3 else "FAIL", f"{recognized} line(s) recognized in all")
        tears = sum(s.get("result", {}).get("tears", 0) for _, s in regions)
        diag("tearing", "PASS", f"{tears} row(s) changed between two reads (the job reads again)")

    # memory routes at each step (diagnostics: a route that read nothing is a WARN with its native result)
    for step, sample in out["samples"].items():
        cells = {}
        mem = sample.get("mem")
        if mem is not None:
            rc = u32(mem.get("rc", 0))
            if rc:
                cells["user"] = ("WARN", native(rc))
            elif mem.get("valid") == 0 or (isinstance(mem.get("main"), int) and mem["main"] < 0):
                cells["user"] = ("PASS", f"main {mem.get('main')} (under the margin: 0 B free above it)")
            else:
                cells["user"] = ("PASS", f"main {mem.get('main', 0):,} B")
        for route, f in sample.get("kmem", {}).items():
            rc = u32(f.get("rc", 0))
            if route == "none":
                cells["kernel"] = ("WARN", f"vjoMemInfo {native(rc)}")
            elif rc:
                ks = f.get("step")
                cells[route] = ("WARN", native(rc) + (f" at step {ks} ({MEMSTEPS.get(ks, '?')})" if ks else ""))
            elif route.startswith("block"):
                total = f.get("total", 0)
                cells[route] = ("PASS" if total else "WARN", f"{f.get('free', 0):,} of {total:,} B free")
            elif route == "cas":
                main = f.get("main", (0, 0))
                cells[route] = ("PASS" if isinstance(main, tuple) and main[1] else "WARN",
                                f"main {main[0]:,} of {main[1]:,} B" if isinstance(main, tuple) else str(main))
            elif route == "as":
                parts = [f[k] for k in sorted(f) if re.fullmatch(r"p\d+", k)]
                kd = [p for p in parts if isinstance(p, tuple) and p[1] == KD_TOTAL]
                skipped = f.get("skipped", 0)
                cells[route] = ("PASS" if f.get("parts", 0) else "WARN",
                                f"{f.get('parts', 0)} partitions of {f.get('part', 'kernel')}'s address space"
                                + (f", KD {kd[0][0]:,} B free" if kd else "")
                                + (f"; {skipped} not read ({MEMSTEPS.get(f.get('step'), '?')})" if skipped else ""))
        out["routes"].append({"step": step, "cells": cells})

    # memory queries: the user API and at least one kernel route reading ScePhyMemPartKD, at every step
    if out["routes"]:
        missing, notes = [], set()
        for r in out["routes"]:
            c = r["cells"]
            kd = c.get("block kernel", ("WARN",))[0] == "PASS" or (c.get("as", ("WARN", ""))[0] == "PASS"
                                                                   and "KD" in c["as"][1])
            if c.get("user", ("WARN",))[0] != "PASS" or not kd:
                missing.append(r["step"])
            notes.update(f"{n} {d}" for n, (ok, d) in c.items() if ok == "WARN")
        diag("kernel-memory", "PASS" if not missing else "WARN",
             ("the user API and a KD figure at every step" if not missing else
              f"no KD figure or no user figure at: {', '.join(missing)}")
             + (f"; routes that read nothing: {'; '.join(sorted(notes))}" if notes else ""))
    gmem = [gs for n in STAGES for gs in stages.get(n, {}).get("gsteps", []) if gs["step"] == "mem"]
    if gmem:
        good = [gs for gs in gmem if s32(gs.get("rc", 0)) >= 0 and isinstance(gs.get("main"), int) and gs["main"] >= 0]
        diag("game-memory", "PASS" if good else "WARN",
             f"free USER {good[0]['main']:,} B, CDRAM {good[0].get('cdram', 0):,} B in the game before its first block"
             if good else f"the game's free memory could not be read ({native(gmem[0].get('rc', 0))})")

    row("done", "PASS" if "done" in steps else None,
        f"at {steps['done'].get('t', '?')} ms, the marker removed" if "done" in steps else "not reached")

    # each stage's memory headroom: free USER (+ another model's weights it released) - reserve - need
    out["headroom"] = []
    for name in STAGES:
        mem = stages.get(name, {}).get("memory")
        if not mem or not mem.get("reserve") or not isinstance(mem.get("free_user"), int):
            continue
        need = mem.get("need_workspace", 0) + mem.get("need_weights", 0)
        free = mem["free_user"]
        out["headroom"].append({"stage": name, "model": (stages[name].get("end") or {}).get("model"),
                                "free_user": free, "release": mem.get("release", 0), "reserve": mem["reserve"],
                                "need": need, "headroom": free + mem.get("release", 0) - mem["reserve"] - need
                                if free >= 0 else None})
    # the tier the measured free memory allows (before the worker's first block)
    if gmem:
        good = [gs for gs in gmem if s32(gs.get("rc", 0)) >= 0 and isinstance(gs.get("main"), int) and gs["main"] >= 0]
        if good:
            out["tiers"] = tier_table(good[0]["main"])
            out["tiers"]["where"] = "before the worker's first block"
    full = [n for n in STAGES if len(stages.get(n, {}).get("gsteps", [])) >= GAME_STEPS_MAX]
    if full:
        findings.append(f"The game worker's step log is full ({GAME_STEPS_MAX} steps) in: {', '.join(full)}; that "
                        f"job's later steps were not recorded (its result lines still are).")

    # the game worker's memory around each allocation
    for name in STAGES:
        s = stages.get(name)
        for gs in (s or {}).get("gsteps", []):
            if gs["step"] in ("mem", "alloc", "free"):
                out["game_memory"].append({"stage": name, **gs})

    # findings
    if out.get("config"):
        c = out["config"]
        findings.append(f"Configured {c.get('model')} with fallback {c.get('fallback')}.")
    for step, sample in out["samples"].items():
        paf = sample.get("mem", {}).get("paf_free")
        if isinstance(paf, int) and 0 < paf < PAF_RESERVE:
            findings.append(f"At {step}: the PAF heap had {paf:,} B free, under the {PAF_RESERVE:,} B reserve: every "
                            f"Shell result allocation is refused (dictionary results, Anki).")
        kmem = sample.get("kmem", {})
        if all(u32(f.get("rc", 0)) for f in kmem.values()) and kmem:
            findings.append(f"At {step}: no kernel memory route read a figure.")
    for name in STAGES:
        mem = stages.get(name, {}).get("memory", {})
        if mem.get("cleanup"):
            findings.append(f"{name}: the game worker could not free its memory (cleanup set): it blocks further jobs.")
        io = u32(mem.get("io", 0))
        if io:
            findings.append(f"{name}: model I/O error {native(io)}.")

    begun = [n for n in STAGES if n in stages and "begin" in stages[n] and stages[n].get("end") is None]
    # The run wrote 'done' but an OCR stage left no record: report() drops a line when the file
    # cannot be written (shell/selftest.c), so a missing OCR stage with 'done' present is a failure,
    # not "not reached".
    if "done" in steps:
        for r in rows:
            if r["kind"] == "ocr" and r["result"] is None and r["row"] in STAGES:
                r["result"], r["detail"] = "FAIL", "no record though the run finished: its lines were lost"
    failed = [r["row"] for r in rows if r["kind"] == "ocr" and r["result"] == "FAIL"]
    diagnostics(out)
    warned = out["warnings"]
    if begun:
        out["ocr_result"] = "INCOMPLETE"
        out["verdict"] = (f"INCOMPLETE: stage '{begun[0]}' began and never wrote its record: the Vita stopped during "
                          f"it. The rows before it are what was measured.")
    elif "done" not in steps:
        reached = [r["row"] for r in rows if r["result"] is not None]
        out["ocr_result"] = "INCOMPLETE"
        out["verdict"] = (f"INCOMPLETE: the run reached '{reached[-1] if reached else 'nothing'}' and never wrote "
                          f"'done'. If the Vita crashed or hung, look at the step after it.")
    elif failed:
        out["ocr_result"] = "FAIL"
        out["verdict"] = f"FAIL: {', '.join(failed)} failed; see the rows above."
    else:
        out["ocr_result"] = "PASS"
        out["verdict"] = ("PASS: every stage completed; the figures above are this console's."
                          + (f" The diagnostics that warned ({', '.join(warned)}) do not change this verdict."
                             if warned else ""))
    if out["ocr_result"] == "INCOMPLETE":
        out["diagnostics_result"] = "INCOMPLETE"  # a partial run has no full diagnostic table
    return out


def diagnostics(out):
    """The diagnostics' own result, kept apart from the verdict: WARN when one warned."""
    rows = [r for r in out["rows"] if r.get("kind") == "diagnostic"]
    out["warnings"] = [r["row"] for r in rows if r["result"] == "WARN"]
    unreached = [r["row"] for r in rows if r["result"] is None]
    out["diagnostics_result"] = "WARN" if out["warnings"] else "INCOMPLETE" if unreached else "PASS"


# ---------------------------------------------------------------- v1 (API 18)

def analyse_v1(records):
    out = {"version": 1, "api": records[0]["fields"].get("api"), "steps": [], "samples": {}, "game": None,
           "ocr": None, "fails": [], "findings": [], "rows": [], "routes": [], "verdict": None}
    for r in records:
        k, w, f = r["kind"], r["words"], r["fields"]
        if k == "boot":
            out["boot"] = f
        elif k == "config":
            out["config"] = f
        elif k == "step":
            step = w[0] if w else "?"
            for word in w[1:]:  # the ocr step's bare "WxH"
                m = re.fullmatch(r"(\d+)x(\d+)", word)
                if m:
                    f["width"], f["height"] = int(m.group(1)), int(m.group(2))
            out["steps"].append({"step": step, **f})
            if step == "game":
                out["game"] = f
            elif step == "ocr":
                out["ocr"] = dict(f)
        elif k in ("mem", "ledger", "kernel") and w:
            out["samples"].setdefault(w[0], {})[k] = {**f, "words": w[1:]}
        elif k == "game":
            out["ocr"] = {**(out["ocr"] or {}), **f}
        elif k == "fail":
            out["fails"].append({"what": w[0] if w else "?", **f})
    steps = [s["step"] for s in out["steps"]]
    progress = [s for s in steps if s in STEPS_V1]
    out["reached"] = progress[-1] if progress else None
    findings, rows = out["findings"], out["rows"]

    def row(name, ok, detail):
        rows.append({"row": name, "result": ok, "detail": detail, "kind": "ocr"})

    boot = out.get("boot")
    if boot:
        if boot.get("mode") == 1:
            findings.append("This was a SAFE boot: the previous boot did not complete, the plugins stayed inert. "
                            "The report has only the boot sample; reboot once more to test normally.")
        if boot.get("failures", 0):
            findings.append(f"{boot['failures']} normal boot(s) in a row had failed before this one.")
        if boot.get("write", 0) & 0x80000000:
            findings.append(f"boot.txt was not writable (0x{boot['write']:08X}): the boot guard cannot remember this boot.")
    if "boot" in steps:
        row("boot", "FAIL" if boot and boot.get("mode") == 1 else "PASS",
            "SAFE boot" if boot and boot.get("mode") == 1 else f"normal boot (seq {boot.get('seq') if boot else '?'})")
    else:
        row("boot", None, "no boot step")
    row("ok", "PASS" if "ok" in steps else None, "stable minute" if "ok" in steps else "not reached")
    g = out["game"]
    if g is None:
        row("game", None, "not reached")
    elif g.get("enabled") == 0:
        row("game", "FAIL", f"{g.get('title')} is disabled in games.ini: no OCR ran")
        findings.append(f"The game {g.get('title')} is disabled in games.ini: no OCR ran. Enable it in the Overlay app "
                        f"(JP Overlay Settings in older builds) and run again.")
    else:
        row("game", "PASS", f"{g.get('title')} enabled, region {g.get('region')}")

    captures = [fl for fl in out["fails"] if fl["what"] == "capture"]
    counts = {}
    for fl in out["fails"]:
        key = (fl["what"], u32(fl.get("rc", 0)))
        counts[key] = counts.get(key, 0) + 1
    for (what, rc), n in counts.items():
        findings.append(f"{what} failed{f' {n} times' if n > 1 else ''}: {NATIVE.get(rc, hex(rc))}"
                        + (" — the kernel could not allocate the capture block in ScePhyMemPartKD." if what == "capture" and rc == 0xBFFFFFFE else ""))
    if captures and all(u32(c.get("rc", 0)) == 0xBFFFFFFE for c in captures):
        findings.append(f"The capture failed {len(captures)} time(s) with no memory: API 18 asks the kernel for a "
                        f"{FULL_FRAME:,} B capture buffer at the full screen, and its pool refused it (#74 measured "
                        f"0x80024302, NO_FREE_PHYSICAL_PAGE, each time). API 19 takes no kernel buffer for OCR: the game "
                        f"reads its own screen in strips.")
        if "done" not in steps:
            findings.append("API 18's self-test never finished after a failed capture: it stayed armed and kept forcing "
                            "the full-screen region, so a smaller region could not be tried in this run (fixed in API 19).")
    o = out["ocr"]
    if o is not None:
        rc = o.get("rc", 0)
        name = RC_NAMES.get(rc, str(rc))
        if rc == 0:
            findings.append(f"OCR ran in the game at {o.get('width', '?')}x{o.get('height', '?')} with {o.get('model')}"
                            + (" (the fallback model)" if o.get("fallback") else "")
                            + f": {o.get('recognized', 0)} line(s) recognized, {o.get('recognize_ms', '?')} ms recognizing.")
        else:
            findings.append(f"The OCR job failed: {name} (rc {rc}).")
        fu = o.get("free_user")
        if isinstance(fu, int) and o.get("reserve") is not None:
            need = o.get("need_workspace", 0) + o.get("need_weights", 0) + o.get("reserve", 0) - o.get("release", 0)
            verdict = "passed" if fu >= need else "refused"
            findings.append(f"The game had {fu:,} B of USER free; the job needed {need:,} B "
                            f"(workspace {o.get('need_workspace', 0):,} + weights {o.get('need_weights', 0):,} + reserve "
                            f"{o.get('reserve', 0):,}): check {verdict}.")
            if fu < need and rc == -103:
                findings.append("Out of memory in the GAME, not in SceShell: a smaller model or a tighter region fits; "
                                "this title cannot run the configured one.")
        if o.get("io", 0):
            io = o["io"] & 0xFFFFFFFF
            findings.append(f"Model I/O error 0x{io:08X}" + (" (ENODEV: the game cannot see ux0; the kernel broker should have been used)" if io == 0x80010013 else "")
                            + (" (ENOENT: the model file is missing)" if io == 0x80010002 else ""))
        if o.get("cleanup", 0):
            findings.append("The game worker could not free its memory after the job (cleanup_status set): it blocks further jobs.")
        if o.get("capture_rc", 0):
            findings.append(f"The capture reported 0x{o['capture_rc'] & 0xFFFFFFFF:08X}: {NATIVE.get(o['capture_rc'] & 0xFFFFFFFF, '?')}.")
        row("ocr", "PASS" if rc == 0 else "FAIL", f"{RC_NAMES.get(rc, rc)} at {o.get('width', '?')}x{o.get('height', '?')}")
    elif captures:
        row("ocr", "FAIL", f"never reached: the capture failed {len(captures)} time(s) "
                           f"({NATIVE.get(u32(captures[0].get('rc', 0)), hex(u32(captures[0].get('rc', 0))))})")
    else:
        row("ocr", None, "not reached")
    row("done", "PASS" if "done" in steps else None, "reached" if "done" in steps else "not reached")

    for step in dict.fromkeys(s for s in steps):
        s = out["samples"].get(step)
        if not s:
            continue
        mem, kern = s.get("mem", {}), s.get("kernel", {})
        cells = {}
        main = mem.get("shell_main")
        if mem:
            if u32(mem.get("rc", 0)):
                cells["user"] = ("WARN", native(mem.get("rc")))
                findings.append(f"At {step}: sceKernelGetFreeMemorySize failed ({native(mem.get('rc'))}).")
            elif isinstance(main, int) and main < 0:
                cells["user"] = ("PASS", f"main {main} (under the margin: 0 B free above it)")
            else:
                cells["user"] = ("PASS", f"main {main:,} B")
                if isinstance(main, int) and main < SHELL_LOW:
                    findings.append(f"At {step}: SceShell's partition had only {main:,} B free (user view). This is the "
                                    f"headroom a firmware thread needs for its next page; #59's crash happened in this state.")
        paf = mem.get("paf_free")
        if isinstance(paf, int) and 0 < paf < PAF_RESERVE:
            findings.append(f"At {step}: the PAF heap had {paf:,} B free, under the {PAF_RESERVE:,} B reserve: every "
                            f"Shell result allocation is refused (dictionary results, Anki).")
        if kern and kern.get("words") and kern["words"][0] == "unavailable":
            cells["kernel"] = ("WARN", f"sample unavailable {native(kern.get('rc', 0))}")
            findings.append(f"At {step}: the kernel memory sample was unavailable (rc 0x{u32(kern.get('rc', 0)):08X}).")
        elif kern:
            if u32(kern.get("kd_rc", 0)) == 0:
                cells["block kernel"] = ("PASS", f"{kern.get('kd_free', 0):,} of {kern.get('kd_total', 0):,} B free")
                if kern.get("kd_total") != KD_TOTAL:
                    findings.append(f"At {step}: the kernel module's partition reads {kern.get('kd_total', 0):,} B total, "
                                    f"not the 36 MiB of ScePhyMemPartKD: treat kd_free as unconfirmed.")
            else:
                cells["block kernel"] = ("WARN", native(kern.get("kd_rc")))
            if u32(kern.get("shell_rc", 0)) == 0:
                cells["block shell"] = ("PASS", f"{kern.get('shell_free', 0):,} of {kern.get('shell_total', 0):,} B free")
                if kern.get("shell_total") not in (None, SHELL_TOTAL):
                    findings.append(f"At {step}: the Shell's partition reads {kern['shell_total']:,} B total, not 50 MiB.")
            else:
                cells["block shell"] = ("WARN", native(kern.get("shell_rc")))
        out["routes"].append({"step": step, "cells": cells})
    bad_routes = {name for r in out["routes"] for name, (ok, _) in r["cells"].items() if ok == "WARN"}
    for name in sorted(bad_routes):
        rcs = sorted({c[1] for r in out["routes"] for n, c in r["cells"].items() if n == name and c[0] == "WARN"})
        findings.append(f"The {name} memory route failed at every step it was tried ({', '.join(rcs)}): its zero "
                        f"figures are not measurements.")

    # v1 has no diagnostic rows: its memory routes are its diagnostics
    out["warnings"] = [f"{name} route" for name in sorted(bad_routes)]
    out["diagnostics_result"] = "WARN" if bad_routes else "PASS"
    failed = [r["row"] for r in rows if r["result"] == "FAIL"]
    out["ocr_result"] = "PASS" if "done" in steps and not failed else "FAIL" if failed or out["reached"] is None \
        else "INCOMPLETE"
    if "done" in steps and not failed:
        out["verdict"] = "PASS: boot, game start and an OCR job completed; see the figures above."
    elif failed:
        out["verdict"] = f"FAIL: {', '.join(failed)} failed; see the rows above."
    elif out["reached"] is None:
        out["verdict"] = "FAIL: the Shell plugin never wrote a step: it did not start (see status.txt and kernel.txt)."
    else:
        nxt = {"boot": "the stable minute (ok)", "ok": "a game start", "game": "an OCR result (the first settled text "
               "screen)", "ocr": "done"}.get(out["reached"], "done")
        out["verdict"] = (f"INCOMPLETE: the run reached '{out['reached']}' and never wrote {nxt}. "
                          + ("If the Vita crashed or hung there, the step after it is where to look." if out["reached"] != "boot"
                             else "Either the console was off within the first minute, or it hung or crashed before it."))
    return out


def analyse(text):
    load_rc_names()
    records = parse(text)
    if not records or records[0]["kind"] != "selftest":
        raise ValueError("not a self-test report (first line must start with 'selftest')")
    if records[0]["words"][:1] == ["v1"]:
        return analyse_v1(records)
    return analyse_v2(records)


def render(result):
    lines = [f"self-test report v{result['version']}, API {result['api']}"]
    sections = (("ocr", "the run and its OCR (PASS/FAIL: these rows decide the verdict)"),
                ("diagnostic", "diagnostics (PASS/WARN: a WARN never changes the verdict)"))
    for kind, title in sections:
        rows = [r for r in result["rows"] if r.get("kind", "ocr") == kind]
        if rows:
            lines += ["", title, f"  {'stage':16} {'result':7} detail"]
            lines += [f"  {r['row']:16} {r['result'] or '-':7} {r['detail']}" for r in rows]
    route_names = [n for n, _ in ROUTES] + ["kernel"]
    used = [n for n in route_names if any(n in r["cells"] for r in result["routes"])]
    if used:
        lines += ["", "memory routes (diagnostics; PASS: the route read a figure; WARN: it did not, its native result)"]
        for r in result["routes"]:
            lines.append(f"  {r['step']}:")
            for n in used:
                if n in r["cells"]:
                    ok, detail = r["cells"][n]
                    label = dict(ROUTES).get(n, n)
                    lines.append(f"    {label:40} {ok:5} {detail}")
    if result.get("game_memory"):
        lines += ["", "game worker memory (free USER/CDRAM in the game after each step)"]
        for g in result["game_memory"]:
            rc = s32(g.get("rc", 0))
            lines.append(f"  {g['stage']:10} {g['step']:5} {str(g.get('arg')):9} {str(g.get('kind')):7} "
                         f"{g.get('bytes', 0):>9,} B rc={'ok' if rc >= 0 else native(rc):12} "
                         f"main={g.get('main', 0):,} cdram={g.get('cdram', 0):,}")
    if result.get("headroom"):
        lines += ["", "memory headroom per stage (free USER + released weights - reserve - workspace - weights)"]
        for h in result["headroom"]:
            lines.append(f"  {h['stage']:10} {str(h['model']):20} free {h['free_user']:>12,} reserve {h['reserve']:>10,} "
                         f"need {h['need']:>10,} headroom "
                         + (f"{h['headroom']:>12,} B" + (" (refused)" if h["headroom"] < 0 else "")
                            if h["headroom"] is not None else "unknown (free USER not read)"))
    if result.get("tiers"):
        lines += [""] + render_tiers(result["tiers"], result["tiers"].get("where", ""))
    warned = result.get("warnings", [])
    diag = {"WARN": f"WARN ({', '.join(warned)}); diagnostics never change the verdict",
            "INCOMPLETE": "not all reached"}.get(result.get("diagnostics_result"), result.get("diagnostics_result"))
    lines += ["", "findings:"] + [f"  - {f}" for f in result["findings"]]
    lines += ["", f"diagnostics: {diag}", result["verdict"]]
    return "\n".join(lines)


# ---------------------------------------------------------------- --collect (install_ftp.sh --collect)

KERNEL_TXT_CAP = 256 * 1024          # kernel/main.c KFILE_CAP: kernel.txt per boot
LOG_TXT_CAP = 1024 * 1024            # shell/log_vita.c LOG_FILE_CAP: log.txt before it rotates
LOG = {
    "job": re.compile(r"^game vocr rc=(-?\d+) (\S+?)( \(fallback(, refused too)?\))?: (\d+)x(\d+) at \d+,\d+ of \d+x\d+ "
                      r"fmt 0x([0-9A-Fa-f]+), lines=(\d+) USER workspace=(\d+) weights=(\w+) resident=(\d+), find=(\d+) ms "
                      r"load=(\d+) ms recognize=(\d+) ms frames=(\d+) io=0x([0-9A-Fa-f]+) stat=0x([0-9A-Fa-f]+) cleanup=(-?\d+)"),
    "capture": re.compile(r"^game vocr capture: (.+?) into (\S+), (\d+) B block, (\d+)-row strips, (\d+) strips in (\d+) ms, "
                          r"(\d+) rows changed"),
    "memory": re.compile(r"^game vocr memory: free USER=(-?\d+)(?: \+ own weights (\d+))?, needs workspace (\d+) \+ weights "
                         r"(\d+) \+ reserve (\d+): check (passed|refused)(?:, headroom (-?\d+))?"),
    "memory_unknown": re.compile(r"^game vocr memory: free USER unknown \(error 0x([0-9A-Fa-f]+)\)"),
    "speed": re.compile(r"^game vocr speed: (\d+) frames in (\d+) us, (\d+) us per frame"),
    "unusable": re.compile(r"^game vocr: (\S+) is unusable here \(io=0x([0-9A-Fa-f]+)\), using (\S+)"),
    "line": re.compile(r"(\d+)x(\d+) (\d+)f (\d+)\.(\d)ms"),
    # shell/worker.c: the dictionary's display name (core/config.c), "Local dictionaries" by default
    "game": re.compile(r"^game (\S+) started: dictionary (.+?) \(key (\S+)\), trigger (\S+), subtitles (\S+), ocr_mode (\S+)"),
    "disabled": re.compile(r"^(\S+): JP Overlay disabled"),
    "done": re.compile(r"^job (\d+) done rc=(-?\d+) in (\d+) ms, arena peak (\d+)"),
    "overlay": re.compile(r"^overlay closed: (\d+) frames, (\d+) word moves, stick (\d+), slowest frame (\d+) ms"),
    "anki_save": re.compile(r"^anki: save (.+) rc=(-?\d+), (\d+)-byte screenshot, (\d+) ms"),
    "anki_queue": re.compile(r"^anki: queue (-?\d+) card\(s\)"),
    "anki_sent": re.compile(r"^anki: sent (\d+), (\d+) duplicates, (\d+) unreadable, (\d+) too large, (-?\d+) pending"),
    "gstep": re.compile(r"^gstep (job|probe) (\w+) kind=(\S+) arg=(\S+) rc=0x([0-9A-Fa-f]+) bytes=(\d+) .*main=(-?\d+) "
                        r"cdram=(-?\d+)"),
    # API 20, include/vjo_game_ocr.h: the stable-text wait (rc 1 held, 2 limit reached) and a skipped blank line
    "settle": re.compile(r"^gstep job settle kind=\S+ arg=(\d+) rc=0x([0-9A-Fa-f]+) bytes=(\d+) sum=0x([0-9A-Fa-f]+) .*us=(\d+)"),
    "blank": re.compile(r"^gstep job blank kind=\S+ arg=(\d+) rc=0x([0-9A-Fa-f]+) bytes=(\d+) sum=0x([0-9A-Fa-f]+) .*us=(\d+)"),
    # API 21 (docs/ocr-speed.md): parallel lines and the line cache, the ARM clock, the game's frame pacing
    "par": re.compile(r"^gstep job par kind=\S+ arg=(\d+) rc=0x([0-9A-Fa-f]+) bytes=(\d+) sum=0x([0-9A-Fa-f]+) .*us=(\d+)"),
    "clock": re.compile(r"^gstep job clock kind=\S+ arg=(\d+) rc=0x([0-9A-Fa-f]+) bytes=(\d+) sum=0x([0-9A-Fa-f]+)"),
    "pace": re.compile(r"^gstep job pace kind=\S+ arg=(\d+) rc=0x([0-9A-Fa-f]+) bytes=(\d+) sum=0x([0-9A-Fa-f]+) "
                       r"main=(-?\d+) cdram=(-?\d+) phycont=(-?\d+) us=(\d+)"),
}
TROUBLE = re.compile(r"fail|refused|error|unusable|timed out|cannot|not loaded|SAFE MODE|not starting|missing at|"
                     r"too large|not opened|unreadable|not writable|unavailable|success=0|reserve guard|not started|"
                     r"damaged|mismatch|out of memory|not enough memory|not a directory", re.I)
# shell/memory.cpp: a PAF request the reserve guard or Paf refused; shell/ledger.c: a refused thread
SHELL_REFUSAL = re.compile(r"Paf reserve guard|Paf allocation: .*success=0|^thread \S+ refused|^anki: \w+ not started")


def _read(folder, rel):
    p = folder / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else None


def _span(values):
    return f"{min(values):,}-{max(values):,}" if values else "-"


def summarise_log(text):
    """log.txt (and log.old.txt): boot, games, every OCR job, memory, speed, the dictionary, the overlay, Anki."""
    s = {"lines": 0, "boot": [], "games": {}, "disabled": set(), "jobs": [], "captures": {}, "memory": [],
         "memory_unknown": 0, "speed_us": [], "unusable": [], "line_ms": [], "dictionary_ms": [], "overlay": [],
         "anki": {"saves": 0, "save_fail": 0, "queue": None, "sent": 0, "picture_off": 0}, "trouble": {},
         "gstep_fail": {}, "config_warnings": [],
         "settle": {"jobs": 0, "held": 0, "timeout": 0, "none": 0, "ms": [], "passes": [], "changes": 0, "toggling_jobs": 0},
         "blank": {"lines": 0, "sizes": [], "edges": []},
         "par": {"jobs": 0, "helpers": [], "on_helpers": 0, "cached": 0, "not_started": 0, "share_min": [], "share_mean": []},
         "clock": {"jobs": 0, "raised": 0, "during": [], "after": [], "before": []},
         "pace": {"jobs": 0, "max_ms": [], "slow": 0, "frames": 0, "before_max_ms": [], "before_slow": 0, "before_frames": 0}}
    in_blank = False  # the job's BLANK steps follow each other; each carries the job's total in rc
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        s["lines"] += 1
        blank_step, in_blank = in_blank, False
        if line.startswith(("bootstrap:", "Vita JP Overlay shell started", "BOOT GUARD", "boot guard:", "worker start",
                            "kernel plugin not loaded", "kernel API version", "RCO missing")):
            if len(s["boot"]) < 40:
                s["boot"].append(line)
        if line.startswith("config warning:"):
            s["config_warnings"].append(line[16:])
        m = LOG["settle"].match(line)
        if m:
            st, rc = s["settle"], int(m.group(2), 16)
            st["jobs"] += 1
            st[{1: "held", 2: "timeout"}.get(rc, "none")] += 1
            if rc in (1, 2):
                st["ms"].append(int(m.group(5)) // 1000)
                st["passes"].append(int(m.group(1)))
            st["changes"] += int(m.group(3))
            st["toggling_jobs"] += (int(m.group(4), 16) >> 16) > 0
            continue
        m = LOG["blank"].match(line)
        if m:
            size = int(m.group(3))
            if not blank_step:  # the job's first: its total (at most VJO_VOCR_BLANK_LOGGED lines are logged)
                s["blank"]["lines"] += int(m.group(2), 16)
            in_blank = True
            s["blank"]["sizes"].append(f"{size >> 16}x{size & 0xFFFF}")
            s["blank"]["edges"].append(int(m.group(4), 16) & 0xFFFF)
            continue
        m = LOG["par"].match(line)
        if m:
            pa, packed = s["par"], int(m.group(4), 16)
            pa["jobs"] += 1
            pa["helpers"].append(int(m.group(1)))
            pa["on_helpers"] += int(m.group(2), 16)
            pa["not_started"] += packed >> 24
            pa["cached"] += (packed >> 16) & 0xFF
            if packed & 0xFF:
                pa["share_min"].append((packed >> 8) & 0xFF)
                pa["share_mean"].append(packed & 0xFF)
            continue
        m = LOG["clock"].match(line)
        if m:
            ck = s["clock"]
            ck["jobs"] += 1
            ck["raised"] += int(m.group(2), 16) == 0 and int(m.group(3)) > int(m.group(1))
            ck["before"].append(int(m.group(1)))
            ck["during"].append(int(m.group(3)))
            ck["after"].append(int(m.group(4), 16))
            continue
        m = LOG["pace"].match(line)
        if m:
            pc = s["pace"]
            pc["jobs"] += 1
            pc["frames"] += int(m.group(1))
            pc["max_ms"].append(int(m.group(2), 16) // 1000)
            pc["slow"] += int(m.group(4), 16)
            pc["before_frames"] += int(m.group(8))
            pc["before_max_ms"].append(int(m.group(6)) // 1000)
            pc["before_slow"] += int(m.group(7))
            continue
        m = LOG["job"].match(line)
        if m:
            s["jobs"].append({"rc": int(m.group(1)), "model": m.group(2), "fallback": bool(m.group(3)),
                              "refused_too": bool(m.group(4)), "region": f"{m.group(5)}x{m.group(6)}",
                              "lines": int(m.group(8)), "weights": m.group(10), "find": int(m.group(12)),
                              "load": int(m.group(13)), "recognize": int(m.group(14)), "frames": int(m.group(15)),
                              "io": int(m.group(16), 16), "cleanup": int(m.group(18))})
            continue
        for key in ("capture", "memory", "memory_unknown", "speed", "unusable", "game", "disabled", "done", "overlay",
                    "anki_save", "anki_queue", "anki_sent", "gstep"):
            m = LOG[key].match(line)
            if not m:
                continue
            if key == "capture":
                k = f"{m.group(1)} into {m.group(2)}"
                s["captures"][k] = s["captures"].get(k, 0) + 1
            elif key == "memory":
                s["memory"].append({"free": int(m.group(1)), "release": int(m.group(2) or 0), "need":
                                    int(m.group(3)) + int(m.group(4)), "reserve": int(m.group(5)),
                                    "passed": m.group(6) == "passed",
                                    "headroom": int(m.group(7)) if m.group(7) is not None else
                                    int(m.group(1)) + int(m.group(2) or 0) - int(m.group(5)) - int(m.group(3)) - int(m.group(4))})
            elif key == "memory_unknown":
                s["memory_unknown"] += 1
            elif key == "speed":
                s["speed_us"].append(int(m.group(3)))
            elif key == "unusable":
                s["unusable"].append(f"{m.group(1)} io=0x{m.group(2)} -> {m.group(3)}")
            elif key == "game":
                g = s["games"].setdefault(m.group(1), {"starts": 0, "ocr_mode": m.group(6), "dictionary": m.group(2)})
                g["starts"] += 1
            elif key == "disabled":
                s["disabled"].add(m.group(1))
            elif key == "done":
                s["dictionary_ms"].append(int(m.group(3)))
                s.setdefault("job_rc", {})
                s["job_rc"][int(m.group(2))] = s["job_rc"].get(int(m.group(2)), 0) + 1
            elif key == "overlay":
                s["overlay"].append(int(m.group(4)))
            elif key == "anki_save":
                s["anki"]["saves"] += 1
                s["anki"]["save_fail"] += int(m.group(2)) < 0
            elif key == "anki_queue":
                s["anki"]["queue"] = int(m.group(1))
            elif key == "anki_sent":
                s["anki"]["sent"] += int(m.group(1))
            elif key == "gstep" and s32(int(m.group(5), 16)) < 0:
                k = f"{m.group(2)} {m.group(3)}/{m.group(4)} {native(int(m.group(5), 16))}"
                s["gstep_fail"][k] = s["gstep_fail"].get(k, 0) + 1
            break
        if line.startswith("game vocr lines"):
            s["line_ms"] += [int(m.group(4)) for m in LOG["line"].finditer(line)]
        if line.startswith("anki: Picture field off"):
            s["anki"]["picture_off"] += 1
        if SHELL_REFUSAL.search(line):
            s.setdefault("shell_refusals", []).append(line[:160])
        sent = LOG["anki_sent"].match(line)
        if sent and not int(sent.group(3)) and not int(sent.group(4)):
            continue  # "0 unreadable, 0 too large": a summary, not a problem
        if TROUBLE.search(line) and not line.startswith("gstep"):
            key = re.sub(r"\d+", "N", line)[:120]
            s["trouble"][key] = s["trouble"].get(key, 0) + 1
    s["disabled"] = sorted(s["disabled"])
    return s


def summarise_kernel(text, size):
    s = {"bytes": size, "capped": size >= KERNEL_TXT_CAP - 512, "started": [], "hooks": [], "registered": 0,
         "jobs": 0, "rows_fail": {}, "alloc_fail": [], "other": []}
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("Vita JP Overlay kernel"):
            s["started"].append(line)
        elif line.startswith("hooks:"):
            s["hooks"].append(line)
        elif line.startswith("ocr worker registered"):
            s["registered"] += 1
        elif re.match(r"^ocr job \d+: \d+x\d+", line):
            s["jobs"] += 1
        elif line.startswith("ocr rows ") and "->" in line:
            k = line.split("->")[-1].strip()
            s["rows_fail"][k] = s["rows_fail"].get(k, 0) + 1
        elif re.search(r"alloc .*failed|hook .*failed|timed out|refused|kept", line):
            if len(s["alloc_fail"]) < 20:
                s["alloc_fail"].append(line)
    return s


def summarise_tai(text):
    """Which section each of our plugins is registered under (comments skipped)."""
    out = {"kernel": [], "shell": [], "worker": [], "worker_all": False, "commented": 0}
    section = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#"):
            out["commented"] += "VitaJPOverlay" in line
            continue
        if line.startswith("*"):
            section = line[1:]
            continue
        name = line.rsplit("/", 1)[-1]
        if name == "VitaJPOverlay_Kernel.skprx":
            out["kernel"].append(section)
        elif name == "VitaJPOverlay_Shell.suprx":
            out["shell"].append(section)
        elif name == "VitaJPOverlay_OCR.suprx":
            out["worker"].append(section)
            out["worker_all"] |= section == "ALL"
    return out


def summarise_collect(folder):
    folder = Path(folder)
    data = "ux0/data/VitaJPOverlay/"
    record = json.loads(_read(folder, "COLLECT.json") or "{}")
    out = {"folder": folder.name, "record": {k: record.get(k) for k in ("collected_at", "host", "missing", "errors",
                                                                          "redacted_keys", "package")},
           "undecided": []}
    # the installed files against the package
    checks = record.get("package_check") or {}
    out["package_check"] = {"checked": len(checks), "match": sorted(k for k, v in checks.items() if v.get("match")),
                            "mismatch": sorted(k for k, v in checks.items() if v.get("match") is False),
                            "unknown": sorted(k for k, v in checks.items() if v.get("match") is None)}
    # configuration
    cfg = {}
    for line in (_read(folder, data + "config.ini") or "").splitlines():
        m = re.match(r"^\s*([a-z0-9_]+)\s*=\s*(.*?)\s*$", line)
        if m:
            cfg[m.group(1)] = m.group(2)
    out["config"] = {k: cfg.get(k) for k in ("ocr_backend", "vocr_model", "vocr_fallback_model", "ocr_mode", "dictionary",
                                             "local_dictionaries", "log_file", "anki_mining", "anki_field_picture",
                                             "japanese_only_selection", "highlight_mode")}
    out["config"]["anki_host"] = "set" if cfg.get("anki_host") else "empty"
    out["tai"] = {}
    for dev in ("ur0", "ux0"):
        t = _read(folder, f"{dev}/tai/config.txt")
        if t is not None:
            out["tai"][f"{dev}:tai/config.txt"] = summarise_tai(t)
    boot = _read(folder, "ur0/data/VitaJPOverlay/boot.txt")
    out["boot_record"] = None if boot is None else boot.strip() or \
        "(empty: the installer reset it and no boot has been recorded since)"
    # logs
    logs = "".join(t for t in (_read(folder, data + "log.old.txt"), _read(folder, data + "log.txt")) if t)
    out["log"] = summarise_log(logs) if logs else None
    out["log_rotated"] = (folder / (data + "log.old.txt")).is_file()
    kt = _read(folder, data + "kernel.txt")
    out["kernel"] = summarise_kernel(kt, (folder / (data + "kernel.txt")).stat().st_size) if kt is not None else None
    st = _read(folder, data + "status.txt")
    out["status_head"] = [l for l in (st or "").splitlines() if l.startswith(("bootstrap:", "BOOT GUARD", "Vita JP Overlay",
                                                                                "worker start", "boot guard"))][:12]
    sp = _read(folder, data + "status.prev.txt")
    out["status_prev_head"] = [l for l in (sp or "").splitlines() if l.startswith(("BOOT GUARD", "boot guard"))][:6]
    # the Anki queue on the card
    q = folder / (data + "anki_queue")
    names = [p.name for p in q.iterdir()] if q.is_dir() else []
    out["anki_queue"] = {"cards": sum(n.endswith(".json") and not n.endswith(".duplicate.json") and ".tmp" not in n
                                      for n in names),
                         "duplicates": sum(n.endswith(".duplicate.json") for n in names),
                         "pictures": sum(n.endswith(".jpg") for n in names), "temp": sum(n.endswith(".tmp") for n in names),
                         "anki_host_txt": (_read(folder, data + "anki_host.txt") or "").strip() or None}
    out["dumps"] = sorted((p.name, p.stat().st_size) for p in (folder / "ux0/data").glob("*.psp2dmp")) \
        if (folder / "ux0/data").is_dir() else []
    # the self-test
    report = _read(folder, data + "selftest-report.txt")
    armed = (folder / (data + "selftest.txt")).is_file()
    out["selftest"] = None
    if report is not None and not report.strip():
        out["selftest_error"] = "the report is empty (the Shell began the test and wrote no line)"
    elif report:
        try:
            out["selftest"] = analyse(report)
        except ValueError as e:
            out["selftest_error"] = str(e)
    out["selftest_armed"] = armed
    # the tier each measured free USER figure allows; the game's is the most conservative of them
    frees = [m["free"] + m["release"] for m in (out["log"] or {}).get("memory", []) if m["free"] >= 0]
    st_free = (out["selftest"] or {}).get("tiers", {}).get("free_user") if out["selftest"] else None
    samples = ([("before the self-test's first block (selftest-report.txt)", st_free)] if st_free else []) + \
              ([("the lowest any job in log.txt saw (refused jobs included; weights may be resident then)", min(frees))]
               if frees else [])
    out["tiers_all"] = [dict(tier_table(v), where=w) for w, v in samples]
    if out["tiers_all"]:
        order = [t[0] for t in TIERS] + [None]
        out["tiers"] = max(out["tiers_all"], key=lambda t: order.index(t["recommendation"]))
    # what the analyser cannot decide, and where in the zip the data to decide it is
    und = out["undecided"]
    for e in record.get("errors") or []:
        und.append(f"a file could not be collected ({e}); its directory listing is in listings/")
    if out.get("selftest_error"):
        und.append(f"the self-test report could not be analysed ({out['selftest_error']}): "
                   f"{data}selftest-report.txt and status.txt are in the zip")
    if out["status_prev_head"] or (folder / (data + "kernel.prev.txt")).is_file():
        und.append(f"the previous boot's records ({data}status.prev.txt, kernel.prev.txt) are in the zip, not summarised"
                   + (f"; it had: {out['status_prev_head'][0]}" if out["status_prev_head"] else ""))
    if (out["log"] or {}).get("shell_refusals"):
        und.append(f"SceShell refused memory {len(out['log']['shell_refusals'])} time(s) (threads/PAF; listed above with the other "
                   f"problem lines): whether that is SceShell's own use or ours needs the status.txt ledger lines")
    if out["selftest"]:
        for w in out["selftest"].get("warnings", []):
            src = {"probe": "the probe stage's gstep lines", "kernel-memory": "the kmem lines",
                   "capture<64KiB": "the result/gstep lines", "kernel-pool": "the result lines"}.get(w, "its rows")
            und.append(f"diagnostic {w} warned: {src} in {data}selftest-report.txt")
    if out["dumps"]:
        und.append("a crash dump is on the card: the analyser cannot decode it; ux0/data/*.psp2dmp is in the zip "
                   "(vita-parse-core reads it with the matching ELFs)")
    if out["kernel"] and out["kernel"]["capped"]:
        und.append(f"kernel.txt reached its {KERNEL_TXT_CAP:,} B cap this boot: later kernel lines were dropped")
    if out["log_rotated"]:
        und.append(f"log.txt rotated at {LOG_TXT_CAP:,} B: the oldest lines are in log.old.txt (both are in the zip)")
    und.append("recognition accuracy: the logs hold sizes and times, not the text; compare with your photos")
    return out


def render_collect(c):
    L = [f"collect {c['folder']} ({c['record'].get('collected_at')}, {c['record'].get('host')})"]
    pkg = c["record"].get("package") or {}
    if pkg:
        L.append(f"  package {pkg.get('version')} source {str(pkg.get('source_revision'))[:12]}; "
                 f"install settings {pkg.get('install_settings') or '-'}")
    pc = c["package_check"]
    if pc["checked"]:
        L.append(f"  installed files: {len(pc['match'])} of {pc['checked']} match the package"
                 + (f"; DIFFER or absent: {', '.join(pc['mismatch'])}" if pc["mismatch"] else ""))
    if c["record"].get("missing"):
        L.append(f"  not on the Vita: {', '.join(c['record']['missing'])}")
    for e in c["record"].get("errors") or []:
        L.append(f"  collect error: {e}")
    cfg = c["config"]
    L.append("  config.ini: " + ", ".join(f"{k}={v}" for k, v in cfg.items() if v is not None)
             + f"; API keys redacted: {', '.join(c['record'].get('redacted_keys') or []) or 'none set'}")
    for name, t in c["tai"].items():
        L.append(f"  {name}: kernel under {t['kernel'] or 'NOTHING'}, shell under {t['shell'] or 'NOTHING'}, "
                 f"OCR worker under {t['worker'] or 'no game'}" + (" and *ALL (WRONG: SceShell loads it)" if t["worker_all"]
                                                                    else "") + f"; {t['commented']} commented line(s)")
    L.append(f"  boot.txt: {c['boot_record'] or '(none)'}")
    if c.get("status_prev_head"):
        L.append("  previous boot (status.prev.txt): " + "; ".join(c["status_prev_head"][:3]))
    k = c["kernel"]
    if k:
        L.append(f"  kernel.txt: {k['bytes']:,} B{' (cap reached)' if k['capped'] else ''}; "
                 + "; ".join(k["started"][-1:] + k["hooks"][-1:])
                 + f"; worker registrations {k['registered']}, OCR jobs submitted {k['jobs']}"
                 + (f"; rows failures {k['rows_fail']}" if k["rows_fail"] else ""))
        for a in k["alloc_fail"][:8]:
            L.append(f"    kernel: {a}")
    lg = c["log"]
    if lg:
        L.append(f"  log.txt: {lg['lines']:,} lines" + (" (rotated: log.old.txt included)" if c["log_rotated"] else ""))
        for b in lg["boot"][:8]:
            L.append(f"    {b}")
        if lg["games"]:
            L.append("  games: " + ", ".join(f"{t} x{g['starts']} (ocr_mode {g['ocr_mode']}, {g['dictionary']})"
                                            for t, g in lg["games"].items())
                     + (f"; disabled: {', '.join(lg['disabled'])}" if lg["disabled"] else ""))
        jobs = lg["jobs"]
        if jobs:
            rcs = {}
            models = {}
            for j in jobs:
                rcs[j["rc"]] = rcs.get(j["rc"], 0) + 1
                key = j["model"] + (" (fallback)" if j["fallback"] else "")
                models[key] = models.get(key, 0) + 1
            ok = [j for j in jobs if j["rc"] == 0 and j["lines"]]
            L.append(f"  OCR jobs: {len(jobs)}; " + ", ".join(f"rc {rc_text(r)} x{n}" for r, n in sorted(rcs.items()))
                     + "; models " + ", ".join(f"{m} x{n}" for m, n in models.items()))
            if ok:
                loads = [j["load"] for j in ok if j["weights"] == "read"]
                L.append(f"  times (ms): find {_span([j['find'] for j in ok])}, load {_span(loads)} ({len(loads)} read), "
                         f"recognize {_span([j['recognize'] for j in ok])}; per line {_span(lg['line_ms'])}; "
                         f"per CTC frame {_span(lg['speed_us'])} us")
            if lg["captures"]:
                L.append("  capture: " + ", ".join(f"{k} x{n}" for k, n in lg["captures"].items()))
        st = lg.get("settle") or {}
        if st.get("jobs"):
            L.append(f"  stable-text wait: {st['jobs']} job(s): rows held still in {st['held']}, the limit reached in "
                     f"{st['timeout']}, no line to wait for in {st['none']}; waited {_span(st['ms'])} ms "
                     f"({_span(st['passes'])} passes); {st['changes']} row change(s) seen while waiting (text typing "
                     f"out); rows toggling (an icon) in {st['toggling_jobs']} job(s)")
        bl = lg.get("blank") or {}
        if bl.get("lines"):
            L.append(f"  blank lines skipped: {bl['lines']} ({', '.join(bl['sizes'][:6])}"
                     f"{', ...' if len(bl['sizes']) > 6 else ''}; edges {_span(bl['edges'])} per 10,000)")
        pa = lg.get("par") or {}
        if pa.get("jobs"):
            share = (f"; the recognizing threads' share of a core: lowest {_span(pa['share_min'])}%, mean "
                     f"{_span(pa['share_mean'])}%" if pa["share_mean"] else "")
            L.append(f"  parallel lines: {pa['jobs']} job(s) with helpers {_span(pa['helpers'])}; {pa['on_helpers']} "
                     f"line(s) on a helper, {pa['not_started']} helper(s) did not start, {pa['cached']} line(s) from "
                     f"the cache{share}")
        ck = lg.get("clock") or {}
        if ck.get("jobs"):
            L.append(f"  ARM clock (ocr_boost_clock): {ck['jobs']} job(s), raised in {ck['raised']}; MHz before "
                     f"{_span(ck['before'])}, during {_span(ck['during'])}, after {_span(ck['after'])}")
        pc = lg.get("pace") or {}
        if pc.get("jobs"):
            L.append(f"  game frames during OCR jobs: {pc['frames']} in {pc['jobs']} job(s), longest gap "
                     f"{_span(pc['max_ms'])} ms, {pc['slow']} over 25 ms; between jobs {pc['before_frames']} frames, "
                     f"longest gap {_span(pc['before_max_ms'])} ms, {pc['before_slow']} over 25 ms")
        if lg["memory"]:
            frees = [m["free"] for m in lg["memory"]]
            heads = [m["headroom"] for m in lg["memory"]]
            refused = sum(not m["passed"] for m in lg["memory"])
            L.append(f"  game memory: free USER {_span(frees)} B; headroom {_span(heads)} B; refused {refused} of "
                     f"{len(lg['memory'])}" + (f"; free USER unreadable {lg['memory_unknown']}x" if lg["memory_unknown"] else ""))
        if lg["gstep_fail"]:
            L.append("  game worker steps that failed: " + "; ".join(f"{k} x{n}" for k, n in lg["gstep_fail"].items()))
        if lg["unusable"]:
            L.append("  model files unusable: " + "; ".join(lg["unusable"]))
        if lg["dictionary_ms"]:
            rcs = ", ".join(f"rc {rc_text(r)} x{n}" for r, n in sorted(lg.get("job_rc", {}).items()))
            L.append(f"  Shell jobs (OCR + dictionary): {len(lg['dictionary_ms'])} ({rcs}), {_span(lg['dictionary_ms'])} ms")
        if lg["overlay"]:
            L.append(f"  overlay opened {len(lg['overlay'])}x, slowest frame {max(lg['overlay'])} ms")
        a = lg["anki"]
        L.append(f"  Anki: {a['saves']} save(s) ({a['save_fail']} failed), queue {a['queue'] if a['queue'] is not None else '-'}"
                 f", {a['sent']} sent" + (", Picture off in this build" if a["picture_off"] else ""))
        if lg["config_warnings"]:
            L.append("  config warnings: " + "; ".join(lg["config_warnings"][:5]))
        if lg["trouble"]:
            L.append("  lines reporting a problem (digits as N; count):")
            for t, n in sorted(lg["trouble"].items(), key=lambda kv: -kv[1])[:20]:
                L.append(f"    {n:>4} x {t}")
    else:
        L.append("  log.txt: (none: log_file off, or nothing logged)")
    q = c["anki_queue"]
    L.append(f"  anki_queue on the card: {q['cards']} card(s), {q['duplicates']} duplicate(s), {q['pictures']} picture(s), "
             f"{q['temp']} temp file(s); anki_host.txt {q['anki_host_txt'] or '(none)'}")
    L.append("  crash dumps: " + (", ".join(f"{n} ({b:,} B)" for n, b in c["dumps"]) if c["dumps"] else "none"))
    for t in c.get("tiers_all") or []:
        L += [""] + render_tiers(t, t["where"])
    if c.get("tiers"):
        L.append(f"  recommendation for this game: {c['tiers']['recommendation'] or 'none (not even Small fits)'} "
                 "(the most conservative of the samples above)")
    L.append("")
    if c["selftest"]:
        L += ["self-test", render(c["selftest"])]
    elif c.get("selftest_error"):
        L.append(f"self-test: the report is not readable: {c['selftest_error']}")
    else:
        L.append("self-test: no report" + (" (selftest.txt is there: armed, and it has not run yet)" if c["selftest_armed"]
                                          else " (it was not armed: bash tools/install_ftp.sh --self-test VITA_IP)"))
    L += ["", "not decided here (the data to decide it is in this zip):"] + [f"  - {u}" for u in c["undecided"]]
    return "\n".join(L)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("report", type=Path, nargs="?")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--collect", type=Path, metavar="DIR",
                        help="a folder written by install_ftp.sh --collect: every log, record and the self-test")
    args = parser.parse_args()
    if args.collect:
        if not (args.collect / "COLLECT.json").is_file():
            print(f"analyse_selftest: {args.collect} is not a collect folder (no COLLECT.json)", file=sys.stderr)
            return 2
        load_rc_names()
        c = summarise_collect(args.collect)
        print(json.dumps(c, indent=2, default=list) if args.json else render_collect(c))
        if c.get("selftest_error"):
            return 1  # a report is there and cannot be read: the run stopped somewhere
        return 0 if not c["selftest"] or c["selftest"]["ocr_result"] == "PASS" else 1
    if not args.report:
        parser.error("give a report, or --collect DIR")
    try:
        result = analyse(args.report.read_text(encoding="utf-8", errors="replace"))
    except (OSError, ValueError) as e:
        print(f"analyse_selftest: {e}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(result, indent=2, default=list))
    else:
        print(render(result))
    # the OCR verdict alone: a diagnostic WARN never makes this 1
    return 0 if result["ocr_result"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
