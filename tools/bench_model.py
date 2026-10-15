"""The stage table and the end-to-end job model of tools/bench_stages.py (docs/performance.md).

Measured inputs: the host timings, the Cortex-A9 instruction counts and cycle ranges (vita-vn-ocr's
cost model on the Vita compiler's code under qemu-arm: estimates), the linefind dev regions' strips,
lines, CTC frames and cascade re-reads per job, and the console's own timings from #81's --collect
logs (CONSOLE: aggregates only; the logs are private). Everything else in the job model is a constant
read from the source (polling periods, log lines), and each line of the model says which it is.
"""
import math

# Constants from the source, with where they are (checked by tools/tests/test_bench_model.py).
SOURCE = {
    "take_wait_ms": (100, "kernel/game_ocr.c TAKE_WAIT_US: an idle vjoOcrTake waits this long for a submission, "
                          "which wakes it (#102)"),
    "read_wait_ms": (50, "kernel/game_ocr.c READ_WAIT_US: a pending vjoOcrRead waits this long for the job's "
                         "completion, which wakes it (#102)"),
    "worker_nap_ms": (100, "game_ocr/main.c worker_main: the sleep after a take the kernel answered at once"),
    "log_batch_lines": (24, "shell/shell.h VJO_LOG_BATCH_LINES: the lines one log batch holds (#100)"),
    "control_poll_ms": (50, "shell/worker.c POLL_TIMEOUT_US: the control thread's vjoWaitEvent timeout"),
    "kernel_tick_ms": (16, "kernel/main.c POLL_US: the kernel worker's tick"),
    "signature_every_frames": (8, "kernel/vjo_kernel.h VJO_CHECK_EVERY_FRAMES"),
    "settle_ms": (300, "kernel/scene.h SCENE_STABLE_US"),
}

# The hand-off's polls before #102 (v0.8.0-test.1 and the baseline, main bcb780e), no longer in the source.
POLLS_BEFORE = {
    "worker_take_poll_ms": (100, "game_ocr/main.c worker_main until #102: sceKernelDelayThread(100000) between "
                                 "vjoOcrTake calls"),
    "shell_result_poll_ms": (50, "shell/game_ocr_client.c until #102: delay_us(50000) between vjoOcrRead calls"),
}

# The Shell's log.txt lines of one Auto OCR job that finds lines, with the local dictionary in
# Whole word mode. Every line is its own open/append/close of log.txt, and of status.txt too while
# that holds less than 64 KiB (shell/log_vita.c).
#   start: "job start" (shell/worker.c start_job_kind, before the job is handed over);
#   result: vocr_run_one's "game vocr rc=" and "capture", log_vocr_measurements' "memory" and
#     "speed", one "lines" line per 6 lines, "cascade speed" when the cascade read a line again;
#   steps: one per game-worker step (log_vocr_steps). API 19's resident job has 11: mem, check, two
#     allocations, first pass, signature, find, load, recognize, two frees; main's default job 15:
#     the same and settle, lm, cascade, norm (game_ocr/main.c vjo_game_ocr_run_request);
#   text: main only (#88, log_text_lines): one per recognized line, two where the shown text differs;
#   after: "result arenas" (the Paf allocation), "local dictionary workspace", the first word's
#     "lookup", "job done";
#   subtitle: "subtitle text ready" (the control thread, beside the net thread's lookup).
JOB_LOG = {"start": 1, "result": 4, "steps_api19": 11, "steps": 15, "after": 4, "subtitle": 1}
KERNEL_LOG_LINES_PER_JOB = 3  # settled, ocr job N, first pass in (kernel/main.c, kernel/game_ocr.c)


def job_log_lines(kind, lines, cascade_jobs=0.0, text_extra=0.0):
    """Log lines of one job (JOB_LOG); kind "api19" (v0.8.0-test.1's job), "baseline" (main bcb780e's
    default job) or "default" (main's: the same lines)."""
    n = JOB_LOG["start"] + JOB_LOG["result"] + math.ceil(lines / 6) + JOB_LOG["after"] + JOB_LOG["subtitle"]
    if kind == "api19":
        return n + JOB_LOG["steps_api19"]
    return n + JOB_LOG["steps"] + cascade_jobs + lines + text_extra


def job_log_appends(kind, lines, cascade_jobs=0.0, text_extra=0.0):
    """Appends of log.txt (and of status.txt while it is under 64 KiB) one job makes. Before #100 every
    line was one; main's (#100) writes the result's lines (rc, capture, memory, speed, cascade speed,
    lines, the steps) and the recognized text's as one batch each (shell/worker.c vocr_run_one,
    log_text_lines; a batch holds log_batch_lines lines), the others one each: job start, the Paf
    allocation, the dictionary workspace, the lookup, job done, subtitle text ready."""
    if kind != "default":
        return job_log_lines(kind, lines, cascade_jobs, text_extra)
    result = JOB_LOG["result"] + math.ceil(lines / 6) + JOB_LOG["steps"] + cascade_jobs
    batches = math.ceil(result / SOURCE["log_batch_lines"][0]) + (1 if lines else 0)
    return JOB_LOG["start"] + batches + JOB_LOG["after"] + JOB_LOG["subtitle"]


# The console: #81's --collect logs of v0.8.0-test.1 (API 19's job; private, aggregates only): the 11
# Auto OCR jobs with VOCR3-H15 in one game's 883x112 region outside the self-test's stages (10 found a
# line, 1 none; 3 read the weights, 8 had them resident), their 30 lookups, and the self-test's 5 stages.
CONSOLE = {
    "h15_ms_per_frame": (5.056, "VOCR3-H15: 664 CTC frames in 10 jobs' recognize= times (the owner: 5.0-5.2 ms); "
                                "vita-vn-ocr's A9 model gives 5.00 ms at its 333 MHz upper bound"),
    "fs_append_ms": (9.4, 47.0, 15.7,
                     "one open/append/close of a file on ux0 with the game running. The self-test's own timestamps "
                     "measure it: each stage's closing report lines (a report, a status.txt and a log.txt append "
                     "each) took 12.1-46.0 ms per append, median 15.7 (5 stages). The 11 jobs agree: what their "
                     "Shell time leaves after the game's job, the lookup and the polls at their mean is 9.4-47.0 ms "
                     "per append they made, median 20.9 (status.txt was still under 64 KiB, so a line was two "
                     "appends, for 9 of them and part of a 10th). Range 9.4-47.0, estimate 15.7 (the direct one)"),
    "job_remainder_per_append_ms": (20.9, 9.4, 47.0,
                                    "the 11 jobs' remainder per append (median, min, max): not independent of the "
                                    "console check, which uses the same jobs"),
    "lookup_fixed_ms": (15.3, "a lookup's open, header and CPU: the intercept of ms against reads, 30 lookups"),
    "lookup_read_ms": (3.82, "one dictionary read (4 KiB at a random offset of the 134 MiB file): the slope"),
    "first_load_ms": (218.0, 231.0, "load= of the 4 jobs that read the weights (the self-test's first stage and 3 "
                                    "of the 11): 1,155,968 B read and SHA-256 checked"),
    # the 11 jobs themselves, means over all 11, for the model's check
    "jobs": {"n": 11, "shell_ms_mean": 1392.7, "shell_ms_median": 1368.0, "game_ms_mean": 488.6,
             "lookup_ms_mean": 80.5, "lookups": 10, "log_lines_mean": 19.1, "log_lines_range": (12, 24),
             "log_appends_mean": 35.9, "status_full_jobs": 1, "strips_mean": 5.45, "find_ms_mean": 104.6,
             "frames_mean": 60.4, "lines_mean": 10 / 11, "lookup_reads_mean": 13.8, "load_ms_mean": 61.0,
             "weights_read_jobs": 3, "region": (883, 112)},
}
FPS = (60, 30)


def ms(a9, name, mhz):
    w = (a9 or {}).get("workloads", {}).get(name)
    if not w or "ms_per_op" not in w:
        return None
    v = w["ms_per_op"].get(str(mhz)) or w["ms_per_op"].get(mhz)
    return tuple(v) if v else None


def per_frame(a9, name, mhz):
    """ms per CTC frame (low, high) of a recognize_* workload."""
    w = a9["workloads"][name]
    lo, hi = ms(a9, name, mhz)
    k = w["ops"] / max(1, w["counters"]["frames"])
    return lo * k, hi * k


def lm_per_frame(a9, mhz):
    """The beam + LM decoder's ms per CTC frame: job_h15_lm minus job_h15_greedy, per frame."""
    a, b = ms(a9, "job_h15_lm", mhz), ms(a9, "job_h15_greedy", mhz)
    f = max(1, a9["workloads"]["job_h15_lm"]["counters"]["frames"])
    return max(0.0, (a[0] - b[0]) / f), max(0.0, (a[1] - b[1]) / f)


def fmt_range(v, digits=1):
    if v is None:
        return "–"
    lo, hi = v
    if hi < 0.1:
        return f"{lo * 1000:.0f}-{hi * 1000:.0f} µs"
    return f"{lo:.{digits}f}-{hi:.{digits}f} ms"


def memory_note(name, c, ops):
    """What the stage touches, from its counters (bytes per op)."""
    if not c:
        return ""
    ops = max(1, ops or 1)
    if name.startswith("capture"):
        # per op = per strip; the region's last strip is shorter (896x156 in 64-row strips: 64 + 64 + 28)
        mean_rows = c["rows"] / max(1, c["strips"])
        return (f"reads {c['fb_bytes_read'] // ops:,} B of the frame, writes {c['grey_bytes_written'] // ops:,} B of "
                f"grey per strip (mean {mean_rows:.1f} rows of {c['strip_rows']}-row strips); kernel scratch "
                f"2 x 3,840 B (static)")
    if name == "scene_sig":
        return f"sampled rows of the {c['sig_w']}x{c['sig_h']} region (A8B8G8R8), {c['grid_cols']}-column grid"
    if name == "find":
        return f"{c['arena_bytes']:,} B arena; {c['row_reads']} row reads (two passes) of the region"
    if name.startswith("recognize"):
        return f"{c['arena_bytes']:,} B arena + resident weights; {c['frames'] // ops} CTC frames per line"
    if name.startswith("job"):
        extra = ""
        if c.get("lm_lines"):
            extra = f"; {c['lm_lines']} lines decoded with the LM"
        if c.get("settle_passes"):
            extra += f", {c['settle_passes']} settle pass"
        return (f"{c['workspace_bytes']:,} B workspace + {c['capture_bytes']:,} B capture block; {c['strips']} strips, "
                f"{c['rows_read']} rows read for a {c['strip_rows']}-row strip{extra}")
    if name.startswith("sha256"):
        return f"{c['bytes']:,} B of weights"
    if name.startswith("lookup"):
        return (f"{c['reads'] / ops:.1f} reads, {c['read_bytes'] / ops / 1024:.1f} KiB and {c['opens'] / ops:.0f} open "
                f"per lookup; arena peak {c['arena_peak']:,} B of the Shell's 64 KiB")
    if name == "config":
        return f"{c['bytes']:,} B of config.ini into a {c.get('struct_bytes', 0):,} B VjoConfig"
    if name == "log":
        return f"{c['bytes'] / ops:.0f} B per line"
    if name == "anki":
        return f"{c['json_bytes'] / ops:.0f} B queue record per card"
    if name == "postproc":
        return f"{c['bytes_in'] / ops:.0f} B of text"
    return ""


def stage_rows(host, a9, workloads):
    rows = []
    for w in workloads:
        h = (host or {}).get("workloads", {}).get(w["name"])
        a = (a9 or {}).get("workloads", {}).get(w["name"])
        if not h and not a:
            continue
        a_ok = a if a and "error" not in a else None
        src = a_ok or h  # memory: the Vita compiler's 32-bit build first, the x86-64 host's otherwise
        rows.append({
            "workload": w["name"], "unit": w["unit"], "what": w["what"],
            "ops_per_iter": src.get("ops"),
            "host_ms": h["ms_per_op"] if h else None,
            "a9_insns": a_ok.get("insns_per_op") if a_ok else None,
            "a9_444": ms(a9, w["name"], 444), "a9_333": ms(a9, w["name"], 333),
            "l1d_misses": a_ok.get("l1d_misses_per_op") if a_ok else None,
            "libc_share": (a_ok.get("share", {}).get("libc", 0) + a_ok.get("share", {}).get("printf", 0))
            if a_ok else None,
            "parity": a.get("parity") if a else None,
            "counters": src.get("counters"),
            "counters_from": "a9" if a_ok else "host",
            "memory": memory_note(w["name"], src.get("counters"), src.get("ops")),
        })
    return rows


def dev_default(dev, fps):
    return (dev or {}).get("summary", {}).get(f"default_{fps}fps", {})


def job_model(a9, dev, mhz, fps, kind="default", first_job=False, given=None):
    """The latency of one Auto OCR job, settled screen to the first word ready, as a (low, high) range and a
    point estimate in ms, with its parts. kind "default": main's default job at the dev regions' means, with
    #100's log batches and #102's hand-off; "baseline": the same job on main bcb780e (line-by-line logging,
    the polls); "api19": v0.8.0-test.1's job (greedy, no wait, no cascade, the polls). given: the job's own strips, frames, lines,
    log lines, lookup reads and region scale, in place of the dev means (the console check)."""
    s = (dev or {}).get("summary", {})
    d = dev_default(dev, fps)
    g = given or {}
    frame_ms = 1000.0 / fps
    if kind == "api19":
        strips = g.get("strips", s.get("strips", {}).get("mean", 8))
        frames = g.get("frames", s.get("frames", {}).get("mean", 86))
        lines = g.get("lines", s.get("lines", {}).get("mean", 2.4))
        display = (strips * frame_ms, strips * frame_ms)
        casc_frames, casc_jobs = 0.0, 0.0
    else:
        strips = d.get("strips", {}).get("mean", 9)
        frames = d.get("frames", {}).get("mean", 93)
        lines = d.get("lines", {}).get("mean", 2.4)
        # the dev lane's simulated display time: a frame per strip, and the wait's sleeps
        dms = d.get("display_ms", {}).get("mean", strips * frame_ms)
        display = (dms, dms)
        casc_frames = d.get("cascade_frames", {}).get("mean", 0.0)
        casc_jobs = d.get("jobs_with_cascade", 0) / max(1, s.get("regions", 1))
    log_lines = g.get("log_lines", job_log_lines(kind, lines, casc_jobs))
    # Each append is of log.txt, and of status.txt too while that is under 64 KiB (est. the first 10-20 jobs
    # of a boot): the job's own appends assume status.txt full, and status_extra is what a job adds while it
    # is not. given log_appends (the console check): the appends the logs counted.
    job_appends = job_log_appends(kind, lines, casc_jobs)
    appends = g.get("log_appends")
    scale = g.get("scale", 1.0)  # the finder's work scales with the region's pixels
    find = tuple(v * scale for v in ms(a9, "find", mhz))
    h15 = per_frame(a9, "recognize_h15", mhz)
    lmf = lm_per_frame(a9, mhz) if kind != "api19" else (0.0, 0.0)
    w20 = per_frame(a9, "recognize_w20", mhz)
    hook = ms(a9, "capture_abgr", mhz)
    lookup_cpu = ms(a9, "lookup_first", mhz)
    post = ms(a9, "postproc", mhz)
    fmt_line = ms(a9, "log", mhz)
    lookup = a9["workloads"]["lookup_first"]["counters"]
    reads_per = g.get("lookup_reads", lookup["reads"] / max(1, lookup["lookups"]))
    polled = kind != "default"  # #102: the take and the read wait in the kernel for the submission / completion
    take = POLLS_BEFORE["worker_take_poll_ms"][0] if polled else 0
    poll = POLLS_BEFORE["shell_result_poll_ms"][0] if polled else 0
    fs_lo, fs_hi, fs_est, _ = CONSOLE["fs_append_ms"]
    lk_fixed, lk_read = CONSOLE["lookup_fixed_ms"][0], CONSOLE["lookup_read_ms"][0]
    # The console's own rate where it was measured: H15 at 5.06 ms per frame is the A9 model's upper
    # bound at 333 MHz (5.00); the estimate column takes every compute part at its upper bound, scaled
    # by that ratio.
    k_console = CONSOLE["h15_ms_per_frame"][0] / per_frame(a9, "recognize_h15", 333)[1]
    parts = {}

    def part(name, lo, hi, est, note):
        parts[name] = (lo, hi, est, note)

    if polled:
        part("take (game worker poll)", 0.0, float(take), take / 2,
             f"0-{take} ms, mean {take / 2:.0f}: {POLLS_BEFORE['worker_take_poll_ms'][1]}")
    else:
        part("take (the submission wakes the worker)", 0.0, 0.0, 0.0,
             f"{SOURCE['take_wait_ms'][1]}; the woken thread's scheduling (the lowest user priority) is not modelled")
    wait = "" if kind == "api19" else ", the stable-text wait's pass and sleep included"
    part("capture + finder" + ("" if kind == "api19" else " + wait"), max(display[0], find[0]), display[1] + find[1],
         display[1] + find[1],
         f"{strips:.1f} strips, one display frame each at {fps} fps ({display[0]:.0f} ms{wait}); the finder's "
         f"{fmt_range(find)} (A9) inside them (low) or after them (high); the hook's CPU ({fmt_range(hook, 2)} per "
         f"strip) is the game's, in its display call")
    part("recognition (VOCR3-H15" + (")" if kind == "api19" else ", beam + LM)"),
         frames * (h15[0] + lmf[0]), frames * (h15[1] + lmf[1]), frames * (h15[1] + lmf[1]) * k_console,
         f"A9 estimate, {frames:.0f} CTC frames ({lines:.1f} lines): {fmt_range(h15, 2)} a frame"
         + ("" if kind == "api19" else f" + the LM's {fmt_range(lmf, 2)}") +
         f"; console {CONSOLE['h15_ms_per_frame'][0]} ms a frame")
    if kind != "api19":
        part("cascade (VOCR4-W20 re-reads)", casc_frames * (w20[0] + lmf[0]), casc_frames * (w20[1] + lmf[1]),
             casc_frames * (w20[1] + lmf[1]) * k_console,
             f"{casc_frames:.0f} CTC frames a job read again (dev mean: {d.get('cascade_share', 0) * 100:.0f}% of "
             f"the lines), {fmt_range(w20, 2)} a frame (A9) + the LM")
    if polled:
        part("result (Shell poll)", 0.0, float(poll), poll / 2,
             f"0-{poll} ms, mean {poll / 2:.0f}: {POLLS_BEFORE['shell_result_poll_ms'][1]}")
    else:
        part("result (the completion wakes the Shell)", 0.0, 0.0, 0.0, SOURCE["read_wait_ms"][1])
    if appends is None:
        how = ("one append each" if kind != "default" else
               f"{job_appends:.0f} appends: the result's and the text's lines as one batch each (#100)")
        part("log.txt", job_appends * fs_lo, job_appends * fs_hi, job_appends * fs_est,
             f"{log_lines:.0f} lines, {how}, of log.txt (status.txt full); console {fs_lo:.1f}-{fs_hi:.1f} ms an "
             f"append (estimate {fs_est}); formatting {fmt_range(fmt_line, 3)} a line")
    else:
        part("log.txt (+ status.txt)", appends * fs_lo, appends * fs_hi, appends * fs_est,
             f"{log_lines:.1f} lines, {appends:.1f} appends (status.txt under 64 KiB for most jobs); console "
             f"{fs_lo:.1f}-{fs_hi:.1f} ms an append (estimate {fs_est})")
    part("text post-processing", post[0], post[1], post[1], "A9 estimate")
    lk_jobs = g.get("lookup_share", 1.0)  # the share of jobs that look a word up (a job without a line does not)
    part("first-word lookup", lk_jobs * (lookup_cpu[0] + lk_fixed) + reads_per * lk_read * 0.8,
         lk_jobs * lk_fixed + reads_per * lk_read * 1.2, lk_jobs * lk_fixed + reads_per * lk_read,
         f"console: {lk_fixed} ms + {lk_read} ms a read, {reads_per:.1f} reads (CPU {fmt_range(lookup_cpu, 2)}, A9, "
         f"inside the console's {lk_fixed} ms); ±20% on the reads")
    if g.get("load_ms"):
        part("weights read (some jobs)", g["load_ms"], g["load_ms"], g["load_ms"],
             "console: the jobs' mean load= (3 of the 11 read the weights, 218-222 ms each)")
    if first_job:
        sha = ms(a9, "sha256_h15", mhz)
        lo_ms, hi_ms = CONSOLE["first_load_ms"][:2]
        lm_bytes = 987600 if kind != "api19" else 0
        k_lm = lm_bytes / 1155968
        part("weights" + ("" if kind == "api19" else " + LM") + ": read + SHA-256 (first job)", lo_ms * (1 + k_lm),
             hi_ms * (1 + k_lm), (lo_ms + hi_ms) / 2 * (1 + k_lm),
             f"console: {lo_ms:.0f}-{hi_ms:.0f} ms for the 1,155,968 B weights (SHA-256 alone {fmt_range(sha)}, A9)"
             + ("" if kind == "api19" else "; the 987,600 B LM scaled by its bytes"))
    # math.fsum: correctly rounded, so the totals are the same bits on every Python (3.12's sum()
    # compensates, older ones do not)
    lo = math.fsum(p[0] for p in parts.values())
    hi = math.fsum(p[1] for p in parts.values())
    est = math.fsum(p[2] for p in parts.values())
    return {"mhz": mhz, "fps": fps, "kind": kind, "first_job": first_job, "strips": strips, "lines": lines,
            "frames": frames, "log_lines": log_lines, "cascade_frames": casc_frames,
            "log_appends": job_appends if appends is None else appends,
            "status_extra": [job_appends * fs_lo, job_appends * fs_hi, job_appends * fs_est],
            "parts": {k: {"low": v[0], "high": v[1], "estimate": v[2], "note": v[3]} for k, v in parts.items()},
            "total": [lo, hi], "estimate": est, "mean_polling_ms": take / 2 + poll / 2}


def console_check(a9):
    """API 19's job at the console jobs' own means, against what those jobs took (333 MHz, 60 fps). Not an
    independent validation: recognition's rate and the lookup's fit come from the same logs; the log
    lines are costed with the self-test's appends, so the remainder (unexplained) is what the model
    does not account for."""
    c = CONSOLE["jobs"]
    w, h = c["region"]
    scale = (w * h) / (896 * 156)
    # the Shell's job time (job N done ... in T ms) runs from the job's hand-over to its last lookup: no
    # "job start" or "job done" line inside it; its lines made the appends counted in the logs (status.txt
    # aligned with log.txt), the control thread's "subtitle text ready" (in every window) aside
    m = job_model(a9, None, 333, 60, kind="api19",
                  given={"strips": c["strips_mean"], "frames": c["frames_mean"], "lines": c["lines_mean"],
                         "log_lines": c["log_lines_mean"], "log_appends": c["log_appends_mean"],
                         "lookup_reads": c["lookup_reads_mean"], "lookup_share": c["lookups"] / c["n"],
                         "load_ms": c["load_ms_mean"], "scale": scale})
    unexplained = c["shell_ms_mean"] - m["estimate"]
    return {"model": m, "measured": c, "unexplained_ms": unexplained,
            "unexplained_per_append_ms": unexplained / c["log_appends_mean"]}


def build_report(host, a9, dev, workloads):
    rows = stage_rows(host, a9, workloads)
    report = {"stages": rows, "source": {k: v[0] for k, v in SOURCE.items()},
              "polls_before": {k: v[0] for k, v in POLLS_BEFORE.items()},
              "console": {k: (list(v) if isinstance(v, tuple) else v) for k, v in CONSOLE.items()},
              "job_log": JOB_LOG, "host_cpu": (host or {}).get("cpu"), "dev": (dev or {}).get("summary")}
    md = ["| workload | per | host (x86-64) | A9 insns | A9 at 444 MHz | A9 at 333 MHz | libc share | parity |",
          "|---|---|---:|---:|---:|---:|---:|:-:|"]
    for r in rows:
        if r["host_ms"] is None:
            host_cell = "–"
        elif r["host_ms"] < 1:
            host_cell = f"{r['host_ms'] * 1000:.1f} µs"
        else:
            host_cell = f"{r['host_ms']:.2f} ms"
        if r["a9_insns"] is None:
            a9_cells = "– | – | – | – | –"
        else:
            parity = "yes" if r["parity"] else "NO" if r["parity"] is False else "–"
            a9_cells = (f"{r['a9_insns']:,.0f} | {fmt_range(r['a9_444'], 2)} | {fmt_range(r['a9_333'], 2)} | "
                        f"{r['libc_share'] * 100:.0f}% | {parity}")
        md.append(f"| `{r['workload']}` | {r['unit']} | {host_cell} | {a9_cells} |")
    md += ["", "| workload | what it is | memory it touches (per op) |", "|---|---|---|"]
    md += [f"| `{r['workload']}` | {r['what']} | {r['memory']} |" for r in rows]
    if a9 and "recognize_h15" in a9.get("workloads", {}):
        report["jobs"] = []
        for kind, title in (("default", "main's default job (#100's log batches, #102's hand-off)"),
                            ("baseline", "the baseline: the same job on main bcb780e"),
                            ("api19", "API 19's job (v0.8.0-test.1)")):
            models = [job_model(a9, dev, mhz, fps, kind=kind) for mhz in (444, 333) for fps in FPS]
            report["jobs"] += models
            md += ["", f"| {title} | 444 MHz, 60 fps | 444 MHz, 30 fps | 333 MHz, 60 fps | 333 MHz, 30 fps | "
                       "estimate (333 MHz, 60 fps) |", "|---|---:|---:|---:|---:|---:|"]
            for part in models[0]["parts"]:
                md.append(f"| {part} | " + " | ".join(
                    f"{m['parts'][part]['low']:.0f}-{m['parts'][part]['high']:.0f} ms" for m in models) +
                    f" | {models[2]['parts'][part]['estimate']:.0f} ms |")
            md.append("| **total** | " + " | ".join(f"**{m['total'][0]:.0f}-{m['total'][1]:.0f} ms**" for m in models)
                      + f" | **{models[2]['estimate']:.0f} ms** |")
            md.append("| + while status.txt is under 64 KiB (a boot's first jobs, est. 10-20: each append twice) | " +
                      " | ".join(f"+{m['status_extra'][0]:.0f}-{m['status_extra'][1]:.0f} ms" for m in models) +
                      f" | +{models[2]['status_extra'][2]:.0f} ms |")
            first = [job_model(a9, dev, mhz, fps, kind=kind, first_job=True) for mhz in (444, 333) for fps in FPS]
            report["jobs"] += first
            md.append("| total, a game's first job | " + " | ".join(
                f"{m['total'][0]:.0f}-{m['total'][1]:.0f} ms" for m in first) + f" | {first[2]['estimate']:.0f} ms |")
        chk = console_check(a9)
        report["console_check"] = chk
        m, c = chk["model"], chk["measured"]
        md += ["", "| console check (API 19's job, the 11 console jobs' means) | model, 333 MHz, 60 fps | console |",
               "|---|---:|---:|",
               f"| the Shell's job time | {m['total'][0]:.0f}-{m['total'][1]:.0f} ms (estimate {m['estimate']:.0f}) | "
               f"{c['shell_ms_mean']:.0f} ms mean, {c['shell_ms_median']:.0f} median |",
               f"| not explained by the model | | {chk['unexplained_ms']:.0f} ms "
               f"({chk['unexplained_per_append_ms']:.1f} ms per append) |"]
    return report, "\n".join(md) + "\n"
