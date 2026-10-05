#!/usr/bin/env bash
# Dump HD tile candidates for every entrance ID (recipes/hd_dump_warp.dks.in).
#
#   tools/hd_dump_all.sh <headless-exe> <rom> <out-dir> [first last jobs]
#
# Each entrance gets <out-dir>/<id>/dump.bin; pass them all to
# tools/hd_pack.py --dump. Runs widescreen (HD capture needs it). The dumps
# hold graphics decoded from your ROM: keep them out of the repository.
set -euo pipefail
exe=$1 rom=$2 out=$3 first=${4:-0} last=${5:-255} jobs=${6:-$(nproc)}
here=$(cd "$(dirname "$0")/.." && pwd)
template="$here/recipes/hd_dump_warp.dks.in"
mkdir -p "$out"
# 50 run/roll/jump cycles to the right, then 20 back to the left.
cycle=$(for i in $(seq 50); do printf '80 * 12\n82 * 4\n80 * 8\n81 * 18\n80 * 18\n'; done
        for i in $(seq 20); do printf '40 * 12\n42 * 4\n40 * 8\n41 * 18\n40 * 18\n'; done)
export exe rom out template cycle
dump_one() {
  local id dir
  printf -v id '%04x' "$1"
  dir="$out/$id"
  mkdir -p "$dir"
  python3 - "$template" "$dir/route.dks" "$id" <<'PY'
import os, sys
text = open(sys.argv[1]).read()
text = text.replace("@ENTRANCE@", sys.argv[3]).replace("@CYCLE@", os.environ["cycle"])
open(sys.argv[2], "w").write(text)
PY
  if DKC1_SCRIPT="$dir/route.dks" DKC1_WIDESCREEN=1 DKC1_HD_DUMP="$dir" \
       "$exe" "$rom" 20000 > "$dir/run.log" 2>&1; then
    echo "$id $(grep -o 'dumped [0-9]* characters' "$dir/run.log" || echo 'no dump')"
  else
    echo "$id failed (see $dir/run.log)"
  fi
}
export -f dump_one
seq "$first" "$last" | xargs -P "$jobs" -I{} bash -c 'dump_one {}'
