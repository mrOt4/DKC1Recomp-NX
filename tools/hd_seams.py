#!/usr/bin/env python3
"""Measure visible 8x8 tile seams in an HD frame (docs/HD_REMASTER.md).

Inputs come from one headless run with DKC1_HD_PPM, DKC1_HD_GBUF and
DKC1_FRAME_PPM. For horizontally adjacent native pixels owned by the same
layer, compare the HD step across the shared edge with the native step.
Pairs that cross a tile boundary are reported separately from pairs inside
a tile: a pack without seams changes both by the same factor, so

  seam_ratio = (hd/native step across tiles) / (hd/native step inside tiles)

is about 1.0 for a seamless pack and grows with visible tile edges.
"""

from __future__ import annotations

import argparse
import json

import numpy as np
from PIL import Image

PIXEL = np.dtype([("main_ref", "<u4"), ("sub_ref", "<u4"), ("under_ref", "<u4"),
                  ("cover_ref", "<u4"), ("main_index", "u1"),
                  ("sub_index", "u1"), ("flags", "u1"), ("main_layer", "u1"),
                  ("under_index", "u1"), ("cover_base", "u1"),
                  ("under_layer", "u1"), ("cover_layer", "u1")])
BUFFER_WIDTH = 448
HEIGHT = 224


def measure(hd_path: str, gbuf_path: str, native_path: str) -> dict:
    native = np.asarray(Image.open(native_path).convert("RGB"), np.float32)
    hd = np.asarray(Image.open(hd_path).convert("RGB"), np.float32)
    width = native.shape[1]
    scale = hd.shape[1] // width
    gbuf = np.fromfile(gbuf_path, PIXEL).reshape(HEIGHT, BUFFER_WIDTH)[:, :width]
    ref = gbuf["main_ref"]
    valid = (ref >> 31) == 1
    char = ref & 0x7ff8
    col = (ref >> 15) & 7
    hflip = (ref >> 18) & 1
    screen_col = np.where(hflip == 1, 7 - col, col)
    left, right = slice(0, width - 1), slice(1, width)
    pair = valid[:, left] & valid[:, right] & \
        (gbuf["main_layer"][:, left] == gbuf["main_layer"][:, right])
    across = pair & (screen_col[:, left] == 7) & (screen_col[:, right] == 0)
    inside = pair & (char[:, left] == char[:, right]) & \
        (screen_col[:, right] == screen_col[:, left] + 1)
    native_step = np.abs(native[:, left] - native[:, right]).sum(axis=2)
    # HD step: last HD column of the left pixel vs first of the right pixel,
    # averaged over the block's rows.
    edge_l = hd[:, scale - 1::scale][:, :width - 1]
    edge_r = hd[:, scale::scale][:, :width - 1]
    hd_step = np.abs(edge_l - edge_r).sum(axis=2)
    hd_step = hd_step.reshape(HEIGHT, scale, width - 1).mean(axis=1)

    def ratio(mask):
        n = native_step[mask].mean() if mask.any() else 0.0
        h = hd_step[mask].mean() if mask.any() else 0.0
        return float(h / n) if n else 0.0, int(mask.sum())

    across_ratio, across_n = ratio(across)
    inside_ratio, inside_n = ratio(inside)
    return {
        "across_tile_hd_vs_native": round(across_ratio, 4),
        "inside_tile_hd_vs_native": round(inside_ratio, 4),
        "seam_ratio": round(across_ratio / inside_ratio, 4) if inside_ratio else None,
        "pairs_across": across_n,
        "pairs_inside": inside_n,
    }


def exact(hd_path: str, native_path: str) -> dict:
    """Blocks of the HD frame that are not a flat copy of the native pixel."""
    native = np.asarray(Image.open(native_path).convert("RGB"))
    hd = np.asarray(Image.open(hd_path).convert("RGB"))
    h, w = native.shape[:2]
    scale = hd.shape[1] // w
    blocks = hd.reshape(h, scale, w, scale, 3)
    same = (blocks == native[:, None, :, None, :]).all(axis=(1, 3, 4))
    return {"blocks": int(h * w), "different": int((~same).sum())}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--hd", required=True, help="DKC1_HD_PPM output")
    parser.add_argument("--gbuf", help="DKC1_HD_GBUF output")
    parser.add_argument("--exact", action="store_true",
                        help="identity-pack check: every HD block must equal "
                             "its native pixel")
    parser.add_argument("--native", required=True, help="DKC1_FRAME_PPM output")
    args = parser.parse_args()
    if args.exact:
        print(json.dumps(exact(args.hd, args.native), indent=2))
        return
    print(json.dumps(measure(args.hd, args.gbuf, args.native), indent=2))


if __name__ == "__main__":
    main()
