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
      "animationIds": {"climb_idle": [47, 54], "roll": [12]},
      "animations": {
        "idle": {"loop": true, "frames": [["idle/000.png", 3], ...]},
        "walk": ..., "run": ..., "jump": ..., "fall": ...,
        "fidget": ..., "roll": ..., "carry_idle": ..., "carry_walk": ...,
        "climb_idle": ..., "climb_move": ..., "swim": ..., "hurt": ...,
        "ride": ..., "victory": ..., "throw": ..., "fidget2": ..., "fidget3": ...,
        "hang": ..., "climb_down": ..., "carry_front_idle": ...,
        "carry_front_walk": ..., "throw_front": ..., "stand_to_fours": ...,
        "hang_idle": ..., "climb_wide_idle": ..., "climb_wide_move": ...
      }
    }

Only "idle" is required. Frame durations are in 60 Hz game frames.
"animationIds" maps native animation IDs to a slot; a pack using only the
first five slots and no map is written in the original DKC2HD01 layout,
anything else as DKC2HD02.
The binary layout is documented in docs/HD_KONGS.md.
"""
import argparse
import json
from pathlib import Path
import struct
import sys
import zlib

ANIMATIONS = ("idle", "walk", "run", "jump", "fall", "fidget", "roll", "carry_idle",
              "carry_walk", "climb_idle", "climb_move", "swim", "hurt", "ride", "victory",
              "throw", "fidget2", "fidget3", "hang", "climb_down", "carry_front_idle",
              "carry_front_walk", "throw_front", "stand_to_fours", "hang_idle",
              "climb_wide_idle", "climb_wide_move", "crouch", "knockout")
BASIC = 5
LOOPING = ("idle", "walk", "run", "carry_idle", "carry_walk", "climb_idle", "climb_move",
           "swim", "ride", "hang", "climb_down", "carry_front_idle", "carry_front_walk",
           "hang_idle", "climb_wide_idle", "climb_wide_move", "crouch", "knockout")
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


def crop_transparent(width, height, rgba):
    """Trim fully transparent borders; returns (x0, y0, width, height, rgba).
    The anchor moves with the crop, so placement is unchanged."""
    rows = [y for y in range(height)
            if any(rgba[(y * width + x) * 4 + 3] for x in range(width))]
    if not rows:
        return 0, 0, 1, 1, bytes(4)
    y0, y1 = rows[0], rows[-1] + 1
    x0, x1 = width, 0
    for y in range(y0, y1):
        alpha = rgba[(y * width) * 4 + 3:((y + 1) * width) * 4:4]
        xs = [x for x, a in enumerate(alpha) if a]
        if xs:
            x0, x1 = min(x0, xs[0]), max(x1, xs[-1] + 1)
    out = bytearray()
    for y in range(y0, y1):
        out += rgba[(y * width + x0) * 4:(y * width + x1) * 4]
    return x0, y0, x1 - x0, y1 - y0, bytes(out)


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
                x0, y0, w, h, rgba = crop_transparent(w, h, rgba)
                frame_index[file] = len(frames)
                frames.append((w, h, int(ax) - x0, int(ay) - y0, rgba))
            steps.append((frame_index[file], duration))
        table.append((first, len(steps) - first, 1 if anim.get("loop", name in LOOPING) else 0))
    native_only = [int(v) for v in spec.get("nativeOnlyAnimations", [])]
    mapping = []
    for name, ids in spec.get("animationIds", {}).items():
        if name not in ANIMATIONS:
            raise ValueError(f"animationIds names an unknown animation: {name}")
        mapping += [(int(i), ANIMATIONS.index(name)) for i in ids]
    used = max([i for i, (_, count, _) in enumerate(table) if count] + [0]) + 1
    v2 = used > BASIC or bool(mapping)
    count = used if v2 else BASIC

    header = bytearray(32 + count * 8 if v2 else HEADER_SIZE)
    header[0:8] = b"DKC2HD02" if v2 else b"DKC2HD01"
    struct.pack_into("<HHhhHHH", header, 8, flags, round(scale * 256),
                     round(offset_x * 16), round(offset_y * 16),
                     len(frames), len(steps), len(native_only))
    if v2:
        struct.pack_into("<HH", header, 22, count, len(mapping))
    records = 32 if v2 else 24
    for i, (first, n, loop) in enumerate(table[:count]):
        struct.pack_into("<HHHH", header, records + i * 8, first, n, loop, 0)
    body = bytearray()
    for frame, duration in steps:
        body += struct.pack("<HH", frame, duration)
    for value in native_only:
        body += struct.pack("<H", value)
    for native, slot in mapping:
        body += struct.pack("<HH", native, slot)
    pixels_at = len(header) + len(body) + 12 * len(frames)
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
