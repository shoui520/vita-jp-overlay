#!/usr/bin/env python3
"""The linefind development regions as raw RGBA, for host/vocr_parallel_bench.c.

  vocr_regions_pack.py --manifests DIR --test-games TEST_GAMES.json --reserved RESERVED.json
                       --shots DIR [--shots DIR ...] -o REGIONS.bin

Only vita-vn-ocr's eval/linefind/manifests vita_dev.jsonl and psp_dev.jsonl are read; protected sets
(vita-test, psp-test, fresh native, CLANNAD) are refused as tools/capture_parity.py refuses them, and so is
every VN vita-vn-ocr reserves: its eval/manifests/reserved.json "test" (the CLANNAD family, vita-test,
psp-test and their shared releases) and eval/manifests/test_games.json ("all", "vita-test", "psp-test",
their families). Masks are
filled as build_gt.load_shot fills them. The pack holds crops of copyrighted screenshots: keep it outside
the repository. Output: u32 count, then per region u32 width, u32 height, width x height RGBA bytes.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_parity as CP  # noqa: E402


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--manifests", required=True)
    ap.add_argument("--shots", action="append", required=True)
    ap.add_argument("--test-games", required=True, help="vita-vn-ocr eval/manifests/test_games.json")
    ap.add_argument("--reserved", required=True, help="vita-vn-ocr eval/manifests/reserved.json")
    ap.add_argument("-o", "--out", required=True)
    args = ap.parse_args(argv)
    t = json.loads(Path(args.test_games).read_text(encoding="utf-8"))
    reserved = set(t["all"]) | set(t["vita-test"]) | set(t["psp-test"]) | {"v4"}
    reserved |= set(json.loads(Path(args.reserved).read_text(encoding="utf-8"))["test"])
    for k, members in t.get("family", {}).items():
        if k in reserved or reserved.intersection(members):
            reserved |= {k, *members}
    regions, refused = [], 0
    for name in CP.DEV_MANIFESTS:
        for line in (Path(args.manifests) / name).read_text(encoding="utf-8").splitlines():
            row = json.loads(line)
            if CP.protected(row) or row.get("vn") in reserved:
                refused += 1
                continue
            shot = CP.find_shot(row["shot"], args.shots)
            if shot is None or hashlib.sha256(shot.read_bytes()).hexdigest() != row["sha256"]:
                raise SystemExit(f"{row['shot']}: missing or not the manifest's file")
            (w, h), rgba = CP.load_frame(shot, row.get("mask"))
            x0, y0, x1, y1 = row["region"]
            rows = [rgba[(y * w + x0) * 4:(y * w + x1) * 4] for y in range(y0, y1)]
            regions.append((x1 - x0, y1 - y0, b"".join(rows)))
    with open(args.out, "wb") as f:
        f.write(struct.pack("<I", len(regions)))
        for w, h, px in regions:
            f.write(struct.pack("<II", w, h))
            f.write(px)
    print(json.dumps({"regions": len(regions), "refused_protected_rows": refused}))


if __name__ == "__main__":
    main()
