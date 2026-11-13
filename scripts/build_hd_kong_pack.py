#!/usr/bin/env python3
"""Build a `.dkc2hd` high-resolution Kong pack from PNG frames.

Uses only the Python standard library so it can run as part of a normal
build. Input is a directory holding `pack.json` and RGBA PNG frames:

    {
      "name": "Mandro",
      "scale": 4.0,                 # HD pixels per native SNES pixel
      "offset": [0, 0],             # native-pixel nudge of the anchor
      "facesLeft": false,           # true if the art faces left
      "replace": ["diddy"],         # "diddy" and/or "dixie"
      "anchor": [128, 248],         # HD pixel placed on the Kong's origin
      "nativeOnlyAnimations": [10, 11],
      "animations": {
        "idle": {"loop": true, "frames": [["idle/000.png", 3], ...]},
        "walk": ..., "run": ..., "jump": ..., "fall": ...
      }
    }

Only "idle" is required. Frame durations are in 60 Hz game frames.
The binary layout is documented in docs/HD_KONGS.md.
"""
import argparse
import json
from pathlib import Path
import struct
import sys
import zlib

ANIMATIONS = ("idle", "walk", "run", "jump", "fall")
FLAG_FACES_LEFT, FLAG_DIDDY, FLAG_DIXIE = 1, 2, 4
HEADER_SIZE = 64


def read_png_rgba(path):
    """Decode an 8-bit RGBA or RGB, non-interlaced PNG."""
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, width = 8, [], None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or color not in (2, 6) or interlace:
                raise ValueError(f"{path}: need 8-bit RGB/RGBA, non-interlaced")
            channels = 4 if color == 6 else 3
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if width is None:
        raise ValueError(f"{path}: missing IHDR")
    raw = zlib.decompress(b"".join(idat))
    stride = width * channels
    out = bytearray(width * height * 4)
    prev = bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 0 and channels == 4:  # fast path used by our own frames
            out[y * stride:(y + 1) * stride] = line
            prev = line
            continue
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            elif f != 0:
                raise ValueError(f"{path}: bad PNG filter {f}")
        for x in range(width):
            o = (y * width + x) * 4
            s = x * channels
            out[o:o + 3] = line[s:s + 3]
            out[o + 3] = line[s + 3] if channels == 4 else 255
        prev = line
    return width, height, bytes(out)


def build(directory):
    directory = Path(directory)
    spec = json.loads((directory / "pack.json").read_text(encoding="utf-8"))
    scale = float(spec.get("scale", 4.0))
    if not 0.25 <= scale <= 16:
        raise ValueError("scale must be between 0.25 and 16")
    offset_x, offset_y = (float(v) for v in spec.get("offset", [0, 0]))
    anchor = spec.get("anchor")
    flags = FLAG_FACES_LEFT if spec.get("facesLeft") else 0
    for who in spec.get("replace", ["diddy"]):
        flags |= {"diddy": FLAG_DIDDY, "dixie": FLAG_DIXIE}[who]
    animations = spec["animations"]
    if "idle" not in animations:
        raise ValueError("the idle animation is required")
    unknown = set(animations) - set(ANIMATIONS)
    if unknown:
        raise ValueError(f"unknown animations: {sorted(unknown)}")

    frame_index, frames, steps, table = {}, [], [], []
    for name in ANIMATIONS:
        anim = animations.get(name)
        if not anim:
            table.append((0, 0, 0))
            continue
        first = len(steps)
        for entry in anim["frames"]:
            file, duration = entry[0], int(entry[1])
            if not 1 <= duration <= 600:
                raise ValueError(f"{name}: duration out of range")
            if file not in frame_index:
                w, h, rgba = read_png_rgba(directory / file)
                ax, ay = entry[2:4] if len(entry) >= 4 else (anchor or [w // 2, h - 1])
                frame_index[file] = len(frames)
                frames.append((w, h, int(ax), int(ay), rgba))
            steps.append((frame_index[file], duration))
        table.append((first, len(steps) - first, 1 if anim.get("loop", name in ("idle", "walk", "run")) else 0))
    native_only = [int(v) for v in spec.get("nativeOnlyAnimations", [])]

    header = bytearray(HEADER_SIZE)
    header[0:8] = b"DKC2HD01"
    struct.pack_into("<HHhhHHH", header, 8, flags, round(scale * 256),
                     round(offset_x * 16), round(offset_y * 16),
                     len(frames), len(steps), len(native_only))
    for i, (first, count, loop) in enumerate(table):
        struct.pack_into("<HHHH", header, 24 + i * 8, first, count, loop, 0)
    body = bytearray()
    for frame, duration in steps:
        body += struct.pack("<HH", frame, duration)
    for value in native_only:
        body += struct.pack("<H", value)
    pixels_at = HEADER_SIZE + len(body) + 12 * len(frames)
    frame_table, pixels = bytearray(), bytearray()
    for w, h, ax, ay, rgba in frames:
        frame_table += struct.pack("<HHhhI", w, h, ax, ay, pixels_at + len(pixels))
        pixels += rgba
    return bytes(header + body + frame_table + pixels), len(frames), len(steps)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("pack_dir", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    data, frames, steps = build(args.pack_dir)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(f"wrote {args.output} ({len(data)} bytes, {frames} frames, {steps} steps)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
