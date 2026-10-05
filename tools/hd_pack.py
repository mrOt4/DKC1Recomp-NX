#!/usr/bin/env python3
"""Build a DKC1 HD texture pack from a private tile dump (docs/HD_REMASTER.md).

Pipeline:
  1. Read <dump>/dump.bin, written by the host with DKC1_HD_DUMP=<dump>. Each
     record is one unique character plus its best on-screen occurrence: a
     40x40 RGBA crop of the character's own layer (the BG tilemap or the
     OBJ layer rebuilt from VRAM/OAM, so other layers never leak in): the
     8x8 tile and a 16-pixel ring of its real neighbours, plus its palette
     and raw planar data.
  2. Lay the crops out on sheets with edge-replicated padding and upscale
     the sheets with Real-ESRGAN (ncnn-vulkan build).
  3. Cut each tile's centre back out, undo its on-screen flips, and
     requantize every HD pixel to the nearest colour (OKLab) among the
     palette entries that character itself uses. Transparency is the
     layer's alpha resampled smoothly, so silhouettes lose their 8x8
     staircase; the HD tile stays indexable: the runtime still applies the
     live CGRAM, fades and colour math.
  4. Write <out>/tiles.bin (the format runner/dkc1_hd.c reads) and
     <out>/pack.json.

The dump and the pack contain graphics derived from the user's own ROM.
Keep both outside the repository and do not redistribute them; distribute
this tool and its recipe instead.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image

SUPPORTED_ROM_SHA256 = (
    "fa8cacf5bbfc39ee6bbaa557adf89133d60d42f6cf9e1db30d5a36a469f74d15")
DUMP_MAGIC = b"DKC1HDD1"
PACK_MAGIC = b"DKC1HDP1"
RING = 16
CROP = 8 + 2 * RING
PAD = 8
CELL = CROP + 2 * PAD
DEPTH_2BPP = 0
RECORD = struct.Struct("<Q4BfI16H48B")
CROP_BYTES = CROP * CROP * 4


@dataclass
class Record:
    key: int
    depth: int
    hflip: bool
    vflip: bool
    score: float
    count: int
    raw: np.ndarray       # 16 uint16 words
    palette: np.ndarray   # 16x3 uint8
    crop: np.ndarray      # 40x40x3 uint8, transparent areas filled
    alpha: np.ndarray     # 40x40 uint8, the layer's own transparency
    layer_context: bool   # False: composed-frame fallback, alpha unusable


def read_dump(path: Path) -> list[Record]:
    return parse_records(path.read_bytes(), str(path))


def parse_records(data: bytes, path: str = "dump") -> list[Record]:
    if data[:8] != DUMP_MAGIC:
        raise SystemExit(f"{path}: not a DKC1 HD dump")
    version, count = struct.unpack_from("<II", data, 8)
    if version != 3:
        raise SystemExit(f"{path}: unsupported dump version {version}")
    records, offset = [], 16
    for _ in range(count):
        fields = RECORD.unpack_from(data, offset)
        offset += RECORD.size
        crop = np.frombuffer(data, np.uint8, CROP_BYTES, offset)
        offset += CROP_BYTES
        key, depth, hflip, vflip, has_crop, score, seen = fields[:7]
        if not has_crop:
            continue
        records.append(Record(
            key=key, depth=depth, hflip=bool(hflip), vflip=bool(vflip),
            score=score, count=seen,
            raw=np.array(fields[7:23], np.uint16),
            palette=np.array(fields[23:71], np.uint8).reshape(16, 3),
            crop=fill_transparent(crop.reshape(CROP, CROP, 4)),
            alpha=crop.reshape(CROP, CROP, 4)[..., 3].copy(),
            layer_context=has_crop == 1))
    return records


def fill_transparent(rgba: np.ndarray) -> np.ndarray:
    """RGB with transparent pixels grown in from opaque neighbours, so the
    upscaler never blends a tile's edge toward an unrelated colour."""
    rgb = rgba[..., :3].astype(np.float32)
    known = rgba[..., 3] > 0
    if not known.any():
        return rgba[..., :3].copy()
    while not known.all():
        acc = np.zeros_like(rgb)
        weight = np.zeros(known.shape, np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                k = np.roll(np.roll(known, dy, 0), dx, 1)
                acc += np.roll(np.roll(rgb, dy, 0), dx, 1) * k[..., None]
                weight += k
        grow = ~known & (weight > 0)
        rgb[grow] = acc[grow] / weight[grow][:, None]
        known = known | grow
    return np.clip(rgb + 0.5, 0, 255).astype(np.uint8)


def merge_dumps(directories: list[Path]) -> list[Record]:
    """Best-scoring occurrence of every character across several dumps
    (e.g. one per level); sightings are summed. Records are compared on
    their fixed header first and only the winners are decoded."""
    size = RECORD.size + CROP_BYTES
    best: dict[int, tuple[tuple, bytes]] = {}
    seen: dict[int, int] = {}
    for directory in directories:
        path = directory / "dump.bin"
        if not path.exists():
            continue
        data = path.read_bytes()
        if data[:8] != DUMP_MAGIC:
            raise SystemExit(f"{path}: not a DKC1 HD dump")
        version, count = struct.unpack_from("<II", data, 8)
        if version != 3:
            raise SystemExit(f"{path}: unsupported dump version {version}")
        for i in range(count):
            offset = 16 + i * size
            key, _, _, _, has_crop, score, sightings = struct.unpack_from(
                "<Q4BfI", data, offset)
            seen[key] = seen.get(key, 0) + sightings
            if not has_crop:
                continue
            rank = (score, has_crop == 1)
            kept = best.get(key)
            if kept is None or rank > kept[0]:
                best[key] = (rank, data[offset:offset + size])
    blob = b"".join(raw for _, raw in best.values())
    header = DUMP_MAGIC + struct.pack("<II", 3, len(best))
    records = parse_records(header + blob)
    for record in records:
        record.count = seen[record.key]
    return records


def decode_char(raw: np.ndarray, depth: int) -> np.ndarray:
    """8x8 palette-relative indices of a planar SNES character."""
    out = np.zeros((8, 8), np.uint8)
    for row in range(8):
        p01 = int(raw[row])
        p23 = int(raw[row + 8]) if depth != DEPTH_2BPP else 0
        for col in range(8):
            bit = 7 - col
            value = ((p01 >> bit) & 1) | (((p01 >> (bit + 8)) & 1) << 1)
            value |= (((p23 >> bit) & 1) << 2) | (((p23 >> (bit + 8)) & 1) << 3)
            out[row, col] = value
    return out


def srgb_to_oklab(rgb: np.ndarray) -> np.ndarray:
    c = rgb.astype(np.float32) / 255.0
    c = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    lms = c @ np.array([[0.4122214708, 0.2119034982, 0.0883024619],
                        [0.5363325363, 0.6806995451, 0.2817188376],
                        [0.0514459929, 0.1073969566, 0.6299787005]],
                       np.float32)
    lms = np.cbrt(lms)
    return lms @ np.array([[0.2104542553, 1.9779984951, 0.0259040371],
                           [0.7936177850, -2.4285922050, 0.7827717662],
                           [-0.0040720468, 0.4505937099, -0.8086757660]],
                          np.float32)


def verify_rom(path: Path) -> str:
    data = path.read_bytes()
    if len(data) % 0x8000 == 512:
        data = data[512:]
    digest = hashlib.sha256(data).hexdigest()
    if digest != SUPPORTED_ROM_SHA256:
        raise SystemExit(f"{path}: unsupported ROM (sha256={digest})")
    return digest


def upscale_sheets(records: list[Record], upscaler: Path, model: str,
                   work: Path, per_row: int) -> list[np.ndarray]:
    """Returns each record's 32x32 HD crop centre (screen orientation)."""
    per_sheet = per_row * per_row
    centres: list[np.ndarray] = []
    for first in range(0, len(records), per_sheet):
        batch = records[first:first + per_sheet]
        rows = (len(batch) + per_row - 1) // per_row
        sheet = np.zeros((rows * CELL, per_row * CELL, 3), np.uint8)
        for i, record in enumerate(batch):
            y, x = divmod(i, per_row)
            padded = np.pad(record.crop, ((PAD, PAD), (PAD, PAD), (0, 0)),
                            mode="edge")
            sheet[y * CELL:(y + 1) * CELL, x * CELL:(x + 1) * CELL] = padded
        index = first // per_sheet
        source = work / f"sheet{index:03d}.png"
        target = work / f"sheet{index:03d}_x4.png"
        Image.fromarray(sheet).save(source)
        print(f"  sheet {index}: {len(batch)} tiles", flush=True)
        subprocess.run([str(upscaler), "-i", str(source), "-o", str(target),
                        "-n", model, "-s", "4", "-f", "png"],
                       check=True, cwd=upscaler.parent,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        big = np.asarray(Image.open(target).convert("RGB"))
        for i in range(len(batch)):
            y, x = divmod(i, per_row)
            top = (y * CELL + PAD + RING) * 4
            left = (x * CELL + PAD + RING) * 4
            centres.append(big[top:top + 32, left:left + 32].copy())
    return centres


BAYER4 = (np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9],
                    [15, 7, 13, 5]], np.float32) + 0.5) / 16.0


def hd_mask(record: Record, native: np.ndarray, scale: int) -> np.ndarray:
    """Opaque HD pixels, in the stored character's orientation.

    With layer-isolated context the silhouette is resampled smoothly from the
    layer's alpha around the tile, so edges and diagonals lose their 8x8
    staircase and may grow into the tile's transparent texels; the runtime
    fills shrunk pixels from the layer beneath and draws grown ones over it.
    Composed-frame fallbacks keep the native silhouette."""
    if not record.layer_context:
        return np.kron(native != 0, np.ones((scale, scale), bool))
    # Inside its own cell only the character's silhouette counts: the OBJ
    # layer can stack other sprites there. The ring keeps edges continuous.
    alpha = record.alpha.copy()
    own = (native != 0).astype(np.uint8) * 255
    if record.hflip:
        own = own[:, ::-1]
    if record.vflip:
        own = own[::-1, :]
    alpha[RING:RING + 8, RING:RING + 8] = own
    big = Image.fromarray(alpha).resize((CROP * scale, CROP * scale),
                                        Image.LANCZOS)
    lo = RING * scale
    mask = np.asarray(big)[lo:lo + 8 * scale, lo:lo + 8 * scale] >= 128
    if record.hflip:
        mask = mask[:, ::-1]
    if record.vflip:
        mask = mask[::-1, :]
    return mask


def quantize_tile(record: Record, centre: np.ndarray, scale: int,
                  dither: float, detail: float, palette: str) -> np.ndarray:
    """Palette-relative HD tile in the stored character's orientation."""
    if detail > 0:
        # Mix back some of the original CGI grain: a smooth upscale hides
        # the texture that, natively, masks the edges between 8x8 tiles.
        lo = record.crop[RING:RING + 8, RING:RING + 8].astype(np.float32)
        grain = np.kron(lo, np.ones((4, 4, 1), np.float32))
        centre = np.clip(centre.astype(np.float32) * (1 - detail) +
                         grain * detail + 0.5, 0, 255).astype(np.uint8)
    if scale != 4:
        centre = np.asarray(Image.fromarray(centre).resize(
            (8 * scale, 8 * scale), Image.LANCZOS))
    if record.hflip:
        centre = centre[:, ::-1]
    if record.vflip:
        centre = centre[::-1, :]
    native = decode_char(record.raw, record.depth)
    if palette == "row":
        # Every opaque entry of the palette row the tile was seen with.
        colors = 4 if record.depth == DEPTH_2BPP else 16
        used, seen = [], set()
        for index in range(1, colors):
            rgb = tuple(record.palette[index])
            if rgb not in seen:
                seen.add(rgb)
                used.append(index)
        used = np.array(used, np.uint8)
    else:
        # Only indices the character itself uses: the same character can be
        # shown with other palette rows, whose other entries are unrelated.
        used = np.unique(native[native != 0])
    mask = hd_mask(record, native, scale)
    tile = np.zeros((8 * scale, 8 * scale), np.uint8)
    if used.size == 0:
        return tile
    choices = srgb_to_oklab(record.palette[used])
    pixels = srgb_to_oklab(centre.reshape(-1, 3))
    if dither and used.size > 1:
        # Ordered dithering keeps the average tone of DKC's own dithered
        # shading. The threshold depends only on the HD pixel position, so
        # neighbouring tiles dither coherently across their shared edge.
        gaps = np.sqrt(((choices[:, None] - choices[None]) ** 2).sum(axis=2))
        gaps[gaps == 0] = np.inf
        spread = float(np.median(gaps.min(axis=1)))
        size = 8 * scale
        threshold = np.tile(BAYER4, (size // 4, size // 4)).reshape(-1)
        pixels = pixels.copy()
        pixels[:, 0] += (threshold - 0.5) * spread * dither
    nearest = np.argmin(((pixels[:, None, :] - choices[None, :, :]) ** 2)
                        .sum(axis=2), axis=1)
    tile[:] = used[nearest].reshape(8 * scale, 8 * scale)
    tile[~mask] = 0
    return tile


def write_pack(out: Path, tiles: dict[int, np.ndarray], scale: int,
               digest: str, meta: dict) -> None:
    out.mkdir(parents=True, exist_ok=True)
    keys = sorted(tiles)
    tile_bytes = (8 * scale) ** 2
    data_start = 64 + 16 * len(keys)
    header = (PACK_MAGIC + struct.pack("<IIII", 1, scale, len(keys), 0) +
              bytes.fromhex(digest) + bytes(8))
    with (out / "tiles.bin").open("wb") as stream:
        stream.write(header)
        for i, key in enumerate(keys):
            stream.write(struct.pack("<QII", key, data_start + i * tile_bytes, 0))
        for key in keys:
            stream.write(tiles[key].tobytes())
    meta = dict(meta, schema="dkc1.hd-pack.v1", rom_sha256=digest,
                scale=scale, tiles=len(keys))
    (out / "pack.json").write_text(json.dumps(meta, indent=2) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--dump", type=Path, required=True, nargs="+",
                        help="one or more directories holding dump.bin "
                             "(DKC1_HD_DUMP); the best occurrence of each "
                             "character across all of them is used")
    parser.add_argument("--rom", type=Path, required=True,
                        help="the same DKC1 USA v1.0 ROM (digest is checked)")
    parser.add_argument("--out", type=Path, required=True,
                        help="pack directory to write (outside the repo)")
    parser.add_argument("--upscaler", type=Path,
                        help="realesrgan-ncnn-vulkan executable")
    parser.add_argument("--model", default="realesr-animevideov3",
                        help="model name in the upscaler's models/ folder")
    parser.add_argument("--scale", type=int, default=4, choices=(2, 3, 4))
    parser.add_argument("--name", default="DKC1 HD Remaster")
    parser.add_argument("--palette", choices=("used", "row"), default="used",
                        help="quantize to the indices the tile uses, or to "
                             "its whole palette row")
    parser.add_argument("--detail", type=float, default=0.35,
                        help="share of the original pixel grain mixed back")
    parser.add_argument("--dither", type=float, default=0.7,
                        help="ordered dithering strength (0 = nearest colour)")
    parser.add_argument("--sheet", type=int, default=36,
                        help="tiles per sheet row (sheet = N x N tiles)")
    parser.add_argument("--limit", type=int, default=0,
                        help="only the N most frequently seen characters")
    parser.add_argument("--cache", type=Path,
                        help="reuse/save the model output (.npz) so that "
                             "quantization settings can be compared quickly")
    parser.add_argument("--identity", action="store_true",
                        help="regression pack: nearest copies of the native "
                             "tiles (no upscaler needed)")
    parser.add_argument("--keep-work", action="store_true")
    args = parser.parse_args()

    digest = verify_rom(args.rom)
    records = merge_dumps(args.dump)
    if args.identity:
        # Regression pack: every HD texel is its native texel. The runtime
        # must then reproduce the native frame exactly (tools/hd_seams.py
        # --exact checks it).
        tiles = {r.key: np.kron(decode_char(r.raw, r.depth),
                                np.ones((args.scale, args.scale), np.uint8))
                 for r in records}
        write_pack(args.out, tiles, args.scale, digest,
                   {"name": "identity (regression)"})
        print(f"wrote {len(tiles)} identity tiles to {args.out}")
        return 0
    if args.limit:
        records = sorted(records, key=lambda r: -r.count)[:args.limit]
    print(f"{len(records)} characters with context from "
          f"{len(args.dump)} dump(s)")
    work = Path(tempfile.mkdtemp(prefix="dkc1-hd-"))
    try:
        keys = np.array([r.key for r in records], np.uint64)
        cached = None
        if args.cache and args.cache.exists():
            with np.load(args.cache) as data:
                if (str(data["model"]) == args.model and
                        np.array_equal(data["keys"], keys)):
                    cached = list(data["centres"])
        if cached is not None:
            centres = cached
            print(f"reusing upscaled centres from {args.cache}")
        else:
            centres = upscale_sheets(records, args.upscaler.resolve(),
                                     args.model, work, args.sheet)
            if args.cache:
                np.savez(args.cache, keys=keys, model=args.model,
                         centres=np.stack(centres))
        tiles = {r.key: quantize_tile(r, c, args.scale, args.dither,
                                      args.detail, args.palette)
                 for r, c in zip(records, centres)}
    finally:
        if args.keep_work:
            print(f"work files kept in {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)
    write_pack(args.out, tiles, args.scale, digest, {
        "name": args.name,
        "generator": {"tool": "tools/hd_pack.py", "upscaler": "Real-ESRGAN "
                      "ncnn-vulkan", "model": args.model,
                      "quantize": "OKLab nearest, used indices, Bayer 4x4 "
                                  f"dither {args.dither}",
                      "detail": args.detail,
                      "alpha": "Lanczos-resampled layer alpha (native "
                               "silhouette for frame-context tiles)"},
    })
    print(f"wrote {len(tiles)} tiles at {args.scale}x to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
