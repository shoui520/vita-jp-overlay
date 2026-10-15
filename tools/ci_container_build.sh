#!/usr/bin/env bash
# Builds a commit's Vita targets the way CI's `vita` job builds them (.github/workflows/release.yml):
# in the vitasdk/vitasdk:2026.08 container, pinned here by the digest CI pulled, at CI's checkout path,
# with the job's own commands. The Overlay app's eboot.bin depends on the SDK's libraries, so a build
# with another VitaSDK gives other bytes; this one is CI's (docs/offline-testing.md, "CI's Vita build
# in its container").
#
#   bash tools/ci_container_build.sh [--rev REV] [--out DIR] [--vocr-source DIR] [--compare FILE]
#
#   --rev REV          the commit to build (default HEAD): its committed tree, as actions/checkout
#                      gives it; the working tree's uncommitted changes are not built
#   --out DIR          where the built files and build.log go (default build/ci-container/<commit>)
#   --vocr-source DIR  a clone of bee-san/vita-vn-ocr holding cmake/VocrOCR.cmake's pinned commit:
#                      also builds the three plugins and the RCO, all of CI's `cmake --build`.
#                      Without it only the Overlay app is built, which does not use the OCR runtime.
#   --compare FILE     an eboot.bin, a VPK, or a zip holding JPOverlaySettings.vpk (a release, or
#                      CI's VitaJPOverlay artifact): exit 1 unless its eboot.bin and the VPK's other
#                      files, and a package's plugins and RCO when built, are byte-identical
#
# Needs docker or podman (CONTAINER_ENGINE=docker or podman picks one) and network access from the
# container (apt, GitHub, codeload.github.com), as CI's runner has. The image is linux/amd64, as on
# CI's runner; another host architecture needs the engine's emulation. VJO_CI_IMAGE overrides the
# image, e.g. after CI's tag moves (the job log's "Initialize containers" step prints its digest).
set -euo pipefail

# vitasdk/vitasdk:2026.08 as CI pulled it for v0.8.0-test.2 (Release run 38689391878, job "vita",
# 2026-10-15T05:55:34Z: "Digest: sha256:fd82d88f..."): the multi-platform index. Its linux/amd64
# image is sha256:5c0c8b36b8b28ff773ae29d1ea4240912834b130b33f187b8338dd7cdd681beb.
IMAGE=${VJO_CI_IMAGE:-docker.io/vitasdk/vitasdk@sha256:fd82d88ff456d118eef16533ebf9a3a6ae0e292684fb8a3a35f15f0707b4692d}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
REV=HEAD
OUT=
VOCR=
COMPARE=
usage() { sed -n '8,18p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
while [ $# -gt 0 ]; do
  case $1 in
    --rev) [ $# -ge 2 ] || usage; REV=$2; shift 2 ;;
    --out) [ $# -ge 2 ] || usage; OUT=$2; shift 2 ;;
    --vocr-source) [ $# -ge 2 ] || usage; VOCR=$2; shift 2 ;;
    --compare) [ $# -ge 2 ] || usage; COMPARE=$2; shift 2 ;;
    *) usage ;;
  esac
done
[ -z "$COMPARE" ] || [ -f "$COMPARE" ] || { echo "ci_container_build: no such file: $COMPARE" >&2; exit 2; }

engine=${CONTAINER_ENGINE:-}
if [ -z "$engine" ]; then
  for e in docker podman; do
    if command -v "$e" >/dev/null 2>&1 && "$e" info >/dev/null 2>&1; then engine=$e; break; fi
  done
elif ! command -v "$engine" >/dev/null 2>&1 || ! "$engine" info >/dev/null 2>&1; then
  echo "ci_container_build: CONTAINER_ENGINE=$engine does not run (\`$engine info\` failed)" >&2
  engine=
fi
if [ -z "$engine" ]; then
  echo "ci_container_build: no working docker or podman (CONTAINER_ENGINE picks one). The steps this" >&2
  echo "runs are in docs/offline-testing.md, \"CI's Vita build in its container\"." >&2
  exit 2
fi

commit=$(git -C "$ROOT" rev-parse --verify "$REV^{commit}")
OUT=${OUT:-$ROOT/build/ci-container/${commit:0:12}}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
rm -f "$OUT/eboot.bin" "$OUT/JPOverlaySettings.vpk" "$OUT"/*.skprx "$OUT"/*.suprx "$OUT/vitajpoverlay.rco"
vocr_commit=
if [ -n "$VOCR" ]; then
  # The runtime at the commit this revision pins (cmake/VocrOCR.cmake checks each file's hash again).
  vocr_commit=$(git -C "$ROOT" show "$commit:cmake/VocrOCR.cmake" | sed -n 's/^set(VJO_VOCR_COMMIT \([0-9a-f]\{40\}\))$/\1/p')
  [ -n "$vocr_commit" ] || { echo "ci_container_build: no VJO_VOCR_COMMIT in $commit's cmake/VocrOCR.cmake" >&2; exit 2; }
  git -C "$VOCR" cat-file -e "$vocr_commit^{commit}" 2>/dev/null \
    || { echo "ci_container_build: $VOCR does not have vita-vn-ocr $vocr_commit (git fetch it)" >&2; exit 2; }
fi

# The job's steps, after actions/checkout: its apt packages, tools/setup_toolchain.sh and its cmake
# commands, in its working directory. The build log goes to stderr; stdout carries only the files.
INNER='set -euo pipefail
exec 3>&1 1>&2
W=/__w/vita-jp-overlay/vita-jp-overlay
mkdir -p "$W" && cd "$W" && tar -x -i
apt-get update && apt-get install -y --no-install-recommends g++ patch python3 openssh-client
WORK=$PWD/.refs/toolchain bash tools/setup_toolchain.sh
export PATH=$PWD/.refs/toolchain/psp2cxml-tool/mbuild:$PATH
if [ -d .refs/vita-vn-ocr/runtime ]; then
  cmake -S vita -B build/vita -DCMAKE_BUILD_TYPE=Release -DVJO_VOCR_SOURCE_DIR=$PWD/.refs/vita-vn-ocr
  cmake --build build/vita -j4
  files="eboot.bin JPOverlaySettings.vpk VitaJPOverlay_Kernel.skprx VitaJPOverlay_Shell.suprx VitaJPOverlay_OCR.suprx vitajpoverlay.rco"
else
  cmake -S vita -B build/vita -DCMAKE_BUILD_TYPE=Release -DVJO_WITH_VOCR=OFF
  cmake --build build/vita -j4 --target vjo_settings_assets
  files="eboot.bin JPOverlaySettings.vpk"
fi
tar -c -C build/vita $files >&3'

echo "ci_container_build: $commit in $IMAGE (linux/amd64) with $engine$( [ -z "$vocr_commit" ] || echo ", vita-vn-ocr $vocr_commit")" >&2
{
  git -C "$ROOT" archive --format=tar "$commit"
  [ -z "$vocr_commit" ] || git -C "$VOCR" archive --format=tar --prefix=.refs/vita-vn-ocr/ "$vocr_commit" runtime
} | "$engine" run --rm -i --platform linux/amd64 "$IMAGE" bash -c "$INNER" 2> >(tee "$OUT/build.log" >&2) \
  | tar -x -C "$OUT"

python3 - "$OUT" "$commit" "$IMAGE" "$COMPARE" <<'PY'
import hashlib, io, sys, zipfile
from pathlib import Path

out, commit, image, compare = Path(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
# Where a package (a release zip, or CI's VitaJPOverlay artifact) holds each built file.
PACKAGE = {"VitaJPOverlay_Kernel.skprx": "ur0/tai/VitaJPOverlay_Kernel.skprx",
           "VitaJPOverlay_Shell.suprx": "ur0/tai/VitaJPOverlay_Shell.suprx",
           "VitaJPOverlay_OCR.suprx": "ur0/tai/VitaJPOverlay_OCR.suprx",
           "vitajpoverlay.rco": "ur0/data/VitaJPOverlay/vitajpoverlay.rco"}


def vpk_files(data):
    """name -> bytes of a VPK's files. Its zip metadata is left out: a VPK records each file's time
    (the build's, the checkout's), so two builds of the same files are never the same zip bytes."""
    z = zipfile.ZipFile(io.BytesIO(data))
    return {n: z.read(n) for n in z.namelist() if not n.endswith("/")}


def sha(data):
    return hashlib.sha256(data).hexdigest()


built = {p.name: p.read_bytes() for p in sorted(out.iterdir()) if p.name != "build.log"}
print(f"built {commit} in {image}:")
for name, data in built.items():
    print(f"  {sha(data)}  {len(data):>9,} B  {name}")
mine = vpk_files(built["JPOverlaySettings.vpk"])
if mine.get("eboot.bin") != built["eboot.bin"]:
    sys.exit("ci_container_build: the VPK's eboot.bin is not the built eboot.bin")
if compare:
    # pairs (what, the reference's bytes, this build's): the VPK's files, and a package's plugins and RCO
    data = Path(compare).read_bytes()
    pairs = []
    if data[:4] != b"PK\x03\x04":
        pairs.append(("eboot.bin", data, built["eboot.bin"]))
    else:
        z = zipfile.ZipFile(io.BytesIO(data))
        names = set(z.namelist())
        if "eboot.bin" in names:
            ref, where = vpk_files(data), ""
        elif "JPOverlaySettings.vpk" in names:
            ref, where = vpk_files(z.read("JPOverlaySettings.vpk")), "JPOverlaySettings.vpk:"
        else:
            sys.exit(f"ci_container_build: {compare} holds neither eboot.bin nor JPOverlaySettings.vpk")
        pairs += [(where + n, ref.get(n), mine.get(n)) for n in sorted(set(ref) | set(mine))]
        pairs += [(path, z.read(path), built[name]) for name, path in PACKAGE.items() if name in built and path in names]
    print(f"{compare}:")
    diff = []
    for what, theirs, ours in pairs:
        if theirs != ours:
            diff.append(what)
        state = ("same" if theirs == ours else "DIFFERENT" if theirs is not None and ours is not None
                 else "only in this build" if theirs is None else "not in this build")
        print(f"  {state:18} {what}" + (f" {sha(theirs)[:16]}" if theirs is not None else ""))
    print(f"DIFFERENT from this build: {', '.join(diff)}" if diff else
          f"identical to this build: {len(pairs)} file(s)" + (" (a VPK's zip records each file's time, which is the "
                                                               "build's own, so its files are compared)"
                                                               if len(pairs) > 1 else ""))
    sys.exit(1 if diff else 0)
PY
