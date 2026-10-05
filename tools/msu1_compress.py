#!/usr/bin/env python3
"""Compress an MSU-1 PCM music pack to Ogg Vorbis for DKC1Recomp.

MSU-1 tracks are raw 44.1 kHz 16-bit stereo PCM behind an 8-byte header
("MSU1" + little-endian loop frame), roughly 10 MiB per minute. This tool
writes each `<name>.pcm` as `<name>.ogg` (Vorbis, quality `--quality`) with
the loop frame stored in the Vorbis comment `MSU1_LOOP`, which
runner/dkc1_msu1.c reads to loop sample-accurately. Sub-folders (optional
alternative tracks) are mirrored.

Every output is verified: it is decoded again and must contain exactly the
same number of frames as the source; the signal-to-noise ratio against the
source is reported. The music packs are third-party content: keep them out
of the repository and distribute only this tool.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

import numpy as np

RATE = 44100
HEADER = 8


def read_header(path: Path) -> tuple[int, int]:
    with path.open("rb") as stream:
        header = stream.read(HEADER)
    if len(header) != HEADER or header[:4] != b"MSU1":
        raise ValueError(f"{path}: not an MSU-1 PCM track")
    frames = (path.stat().st_size - HEADER) // 4
    return struct.unpack("<I", header[4:])[0], frames


def encode(source: Path, target: Path, quality: float) -> dict:
    loop, frames = read_header(source)
    target.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["ffmpeg", "-v", "error", "-y", "-skip_initial_bytes", str(HEADER),
         "-f", "s16le", "-ar", str(RATE), "-ac", "2", "-i", str(source),
         "-c:a", "libvorbis", "-q:a", str(quality),
         "-metadata", f"MSU1_LOOP={loop}", str(target)],
        check=True)
    decoded = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(target), "-f", "s16le", "-ar",
         str(RATE), "-ac", "2", "-"], check=True, capture_output=True).stdout
    got = np.frombuffer(decoded, np.int16).astype(np.float64)
    ref = np.fromfile(source, np.int16, offset=HEADER).astype(np.float64)
    noise = np.sum((got[:ref.size] - ref) ** 2) if got.size >= ref.size else np.inf
    snr = 10 * np.log10(np.sum(ref ** 2) / noise) if noise else float("inf")
    return {
        "track": source.name, "frames": frames, "decoded": got.size // 2,
        "loop": loop, "pcm": source.stat().st_size,
        "ogg": target.stat().st_size, "snr_db": snr,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--in", dest="source", type=Path, required=True,
                        help="MSU-1 pack directory with .pcm tracks")
    parser.add_argument("--out", type=Path, required=True,
                        help="directory for the .ogg pack")
    parser.add_argument("--quality", type=float, default=6.0,
                        help="Vorbis quality (-1..10); 6 is about 192 kbit/s")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--archive", type=Path,
                        help="also write a .msu1 archive (ZIP, stored) of the "
                             "top-level tracks, installable from the Music "
                             "menu on macOS and Windows")
    args = parser.parse_args()

    tracks = sorted(args.source.rglob("*.pcm"))
    if not tracks:
        print(f"no .pcm tracks in {args.source}", file=sys.stderr)
        return 1
    jobs = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for track in tracks:
            target = args.out / track.relative_to(args.source).with_suffix(".ogg")
            jobs.append(pool.submit(encode, track, target, args.quality))
        results = [job.result() for job in jobs]

    failures = 0
    for r in sorted(results, key=lambda r: r["track"]):
        ok = r["decoded"] == r["frames"] and r["loop"] < r["frames"]
        failures += not ok
        print(f"{r['track']:<22} {r['pcm'] / 2**20:7.1f} MiB -> "
              f"{r['ogg'] / 2**20:6.1f} MiB  snr {r['snr_db']:5.1f} dB  "
              f"frames {r['decoded']}/{r['frames']}  "
              f"{'ok' if ok else 'LENGTH MISMATCH'}")
    pcm = sum(r["pcm"] for r in results)
    ogg = sum(r["ogg"] for r in results)
    if args.archive and not failures:
        with zipfile.ZipFile(args.archive, "w", zipfile.ZIP_STORED) as zf:
            for track in sorted(args.out.glob("*.ogg")):
                zf.write(track, track.name)
        print(f"archive {args.archive} "
              f"({args.archive.stat().st_size / 2**20:.1f} MiB)")
    print(f"total {pcm / 2**20:.1f} MiB -> {ogg / 2**20:.1f} MiB "
          f"({100 * ogg / pcm:.1f} %), {len(results)} tracks, "
          f"{failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
