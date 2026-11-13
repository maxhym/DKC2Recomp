#!/usr/bin/env python3
"""Turn side-view character clips shot on a chroma-green background into the
aligned RGBA frames and pack.json consumed by build_hd_kong_pack.py.

Authoring tool (needs ffmpeg, numpy and Pillow); the build itself does not.

    python scripts/extract_hd_kong_frames.py --out mods/mandro-hd \
        --idle idle.mp4 --walk walk.mp4 --run run.mp4 --jump jump.mp4

* Keying: pixels whose green clearly dominates red and blue become
  transparent; edges are despilled.
* Idle, walk and run: the best-matching loop inside each clip is found
  automatically. Horizontal drift is removed; the feet stay on one line.
* Jump: the airborne part is split at its highest point into "jump"
  (rising) and "fall" (descending); take-off and landing crouches are
  dropped because the game supplies its own timing.
* Every animation shares one scale, set so the idle body is --body-height
  HD pixels tall, on a --canvas square with the anchor at the feet.
* Extra slots (fidget, roll, carry_idle, climb_idle, swim, hurt...) come from
  repeated --clip NAME=PATH[@OPTIONS] arguments. OPTIONS is a comma list:
  "loop" (find the best loop, as for walk) or "once" (default; all frames),
  "track" (centre each frame on its own centre of mass, for clips where the
  character travels), "air" (also centre vertically, for swimming or
  hanging), "trim=A-B" (keep clip frames A..B-1), "stand=N" (the first N
  frames show the character standing; their height rescales a clip framed
  wider than the idle one), "open=N" (remove keyed structures thinner than N
  pixels, such as a pole or bar the character holds), "every=N" (keep one
  clip frame in N, each shown N times longer; halves a long clip's size),
  "main" (keep only the body's connected shape, dropping detached dust or
  debris), "stride" (start a loop on its widest frame, so that walk and run cycles
  begin at the same point of the stride).
* --merge keeps the animations, animationIds and tuning of an existing
  pack.json in --out and only replaces the slots given on this run.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import zlib

import numpy as np
from PIL import Image, ImageDraw, ImageFilter


def read_video(path):
    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height", "-of", "csv=p=0", str(path)],
        check=True, capture_output=True, text=True).stdout.strip().split(",")
    w, h = int(probe[0]), int(probe[1])
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-f", "rawvideo",
                          "-pix_fmt", "rgb24", "-"], check=True, capture_output=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(-1, h, w, 3)


def key(frame):
    rgb = frame.astype(np.float32)
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    dominance = g - np.maximum(r, b)
    alpha = 1.0 - np.clip((dominance - 18.0) / 50.0, 0.0, 1.0)
    # Despill: no remaining pixel may be greener than its strongest other channel.
    g2 = np.minimum(g, np.maximum(r, b) + 6.0)
    out = np.stack([r, g2, b, alpha * 255.0], axis=-1)
    out[alpha < 0.04] = 0
    return out.astype(np.uint8)


def remove_thin(rgba, size):
    """Drop opaque structures thinner than `size` pixels (a morphological
    opening of the alpha mask); soft edges of the body are kept."""
    size = max(3, size | 1)
    alpha = Image.fromarray(rgba[..., 3])
    opened = alpha.filter(ImageFilter.MinFilter(size)).filter(ImageFilter.MaxFilter(size))
    out = rgba.copy()
    out[..., 3] = np.minimum(rgba[..., 3], np.asarray(opened))
    out[out[..., 3] == 0] = 0
    return out


def keep_main(rgba):
    """Keep the connected shape that contains the body's core; detached
    blobs (dust, debris) become transparent."""
    # A copy: an image sharing the array's memory is read-only for floodfill.
    mask = Image.fromarray(((rgba[..., 3] > 40) * 255).astype(np.uint8)).copy()
    core = np.asarray(mask.filter(ImageFilter.MinFilter(31)))
    ys, xs = np.nonzero(core)
    if not len(xs):
        return rgba
    seed = (int(xs[len(xs) // 2]), int(ys[len(ys) // 2]))
    ImageDraw.floodfill(mask, seed, 128)
    out = rgba.copy()
    out[np.asarray(mask) != 128] = 0
    return out


def measure(rgba):
    mask = rgba[..., 3] > 128
    ys, xs = np.nonzero(mask)
    if len(xs) < 50:
        return None
    return {"top": ys.min(), "bottom": ys.max(), "left": xs.min(), "right": xs.max(),
            "cx": float(xs.mean()), "cy": float(ys.mean()), "height": ys.max() - ys.min() + 1}


def best_loop(frames, lo, hi):
    small = [np.asarray(Image.fromarray(f).resize((64, 64), Image.BILINEAR), np.float32) for f in frames]
    best = None
    for i in range(len(small)):
        for j in range(i + lo, min(len(small), i + hi + 1)):
            a, b = small[i], small[j]
            diff = np.abs(a[..., :3] * a[..., 3:4] - b[..., :3] * b[..., 3:4]).mean() / 255.0
            if best is None or diff < best[0]:
                best = (diff, i, j)
    return best[1], best[2]


def resize_rgba(rgba, scale):
    h, w = rgba.shape[:2]
    pre = rgba.astype(np.float32)
    pre[..., :3] *= pre[..., 3:4] / 255.0
    size = (max(1, round(w * scale)), max(1, round(h * scale)))
    channels = [np.asarray(Image.fromarray(pre[..., c]).resize(size, Image.LANCZOS), np.float32)
                for c in range(4)]
    out = np.stack(channels, axis=-1)
    a = np.clip(out[..., 3:4], 0, 255)
    rgb = np.where(a > 0.5, out[..., :3] * 255.0 / np.maximum(a, 1e-3), 0)
    return np.clip(np.concatenate([rgb, a], axis=-1), 0, 255).astype(np.uint8)


def write_png(path, rgba):
    h, w = rgba.shape[:2]
    rows = b"".join(b"\x00" + rgba[y].tobytes() for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))
    Path(path).write_bytes(data)


def place(rgba, m, scale, canvas, anchor, feet_y, com_x, airborne_offset=None):
    """Scale one keyed frame and paste it so that com_x lands on the anchor
    column and feet_y (source pixels) on the anchor row."""
    small = resize_rgba(rgba, scale)
    out = np.zeros((canvas, canvas, 4), np.uint8)
    if airborne_offset is None:
        dy = anchor[1] - round(feet_y * scale)
    else:
        dy = anchor[1] - round(m["cy"] * scale) - airborne_offset
    dx = anchor[0] - round(com_x * scale)
    h, w = small.shape[:2]
    y0, x0 = max(0, dy), max(0, dx)
    y1, x1 = min(canvas, dy + h), min(canvas, dx + w)
    if y1 > y0 and x1 > x0:
        out[y0:y1, x0:x1] = small[y0 - dy:y1 - dy, x0 - dx:x1 - dx]
    return out


def durations(count, fps=24.0):
    """Game-frame (60 Hz) durations for `count` clip frames at `fps`."""
    out, t = [], 0.0
    for _ in range(count):
        start = round(t * 60.0 / fps)
        t += 1
        out.append(max(1, round(t * 60.0 / fps) - start))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--idle", type=Path, required=True)
    parser.add_argument("--walk", type=Path)
    parser.add_argument("--run", type=Path)
    parser.add_argument("--jump", type=Path)
    parser.add_argument("--jump-calibration", type=Path,
                        help="a jump clip framed like the idle clip; its take-off crouch "
                             "sets the scale of a wider-framed --jump clip")
    parser.add_argument("--name", default="Mandro")
    parser.add_argument("--body-height", type=int, default=192)
    parser.add_argument("--canvas", type=int, default=256)
    parser.add_argument("--native-scale", type=float, default=4.0)
    parser.add_argument("--clip", action="append", default=[], metavar="NAME=PATH[@OPTS]",
                        help="an extra slot; see the module docstring for OPTS")
    parser.add_argument("--merge", action="store_true",
                        help="keep the other slots and settings of an existing pack.json")
    args = parser.parse_args()
    anchor = (args.canvas // 2, args.canvas - 8)
    args.out.mkdir(parents=True, exist_ok=True)

    clips = {}
    for name in ("idle", "walk", "run", "jump"):
        path = getattr(args, name)
        if path:
            frames = [key(f) for f in read_video(path)]
            clips[name] = (frames, [measure(f) for f in frames])

    idle_frames, idle_m = clips["idle"]
    valid = [m for m in idle_m if m]
    scale = args.body_height / float(np.median([m["height"] for m in valid]))
    idle_feet_to_com = float(np.median([m["bottom"] - m["cy"] for m in valid]))
    animations, report = {}, {}

    def emit(name, frames, ms, loop, airborne=False, ground=None, clip_scale=None,
             track=False, every=1):
        clip_scale = clip_scale or scale
        (args.out / name).mkdir(exist_ok=True)
        for stale in (args.out / name).glob("*.png"):
            stale.unlink()
        entries = []
        if ground is None:
            ground = max(m["bottom"] for m in ms)
        com_x = float(np.median([m["cx"] for m in ms]))
        frames, ms = frames[::every], ms[::every]
        for k, (f, m, d) in enumerate(zip(frames, ms, durations(len(frames), 24.0 / every))):
            # Airborne frames are centred one by one (the game moves the Kong);
            # ground cycles keep their natural sway around the clip's median.
            cx = m["cx"] if airborne or track else com_x
            img = place(f, m, clip_scale, args.canvas, anchor, ground, cx,
                        round(idle_feet_to_com * scale) if airborne else None)
            rel = f"{name}/{k:03d}.png"
            write_png(args.out / rel, img)
            entries.append([rel, d])
        animations[name] = {"loop": loop, "frames": entries}
        report[name] = len(entries)

    i, j = best_loop(idle_frames, 30, min(84, len(idle_frames) - 1))
    emit("idle", idle_frames[i:j], idle_m[i:j], True)
    for name, (lo, hi) in (("walk", (10, 30)), ("run", (8, 24))):
        if name in clips:
            frames, ms = clips[name]
            i, j = best_loop(frames, lo, hi)
            emit(name, frames[i:j], ms[i:j], True)
    if "jump" in clips:
        frames, ms = clips["jump"]
        ok = [k for k, m in enumerate(ms) if m]
        jump_scale = scale
        if args.jump_calibration:
            cal = [measure(key(f)) for f in read_video(args.jump_calibration)[:4]]
            cal_h = float(np.median([m["height"] for m in cal if m]))
            own_h = float(np.median([ms[k]["height"] for k in ok[:4]]))
            jump_scale = scale * cal_h / own_h
        ground = max(ms[k]["bottom"] for k in ok[:6])
        air = [k for k in ok if ms[k]["bottom"] < ground - 0.04 * ms[k]["height"]]
        if air:
            start, end = air[0], air[-1]
            apex = min(range(start, end + 1), key=lambda k: ms[k]["bottom"] if ms[k] else 1e9)
            rise = [k for k in range(start, apex + 1) if ms[k]]
            fall = [k for k in range(apex + 1, end + 1) if ms[k]]
            emit("jump", [frames[k] for k in rise], [ms[k] for k in rise], False, True,
                 clip_scale=jump_scale)
            if fall:
                emit("fall", [frames[k] for k in fall], [ms[k] for k in fall], False, True,
                     clip_scale=jump_scale)

    for item in args.clip:
        name, _, rest = item.partition("=")
        path, _, opts = rest.partition("@")
        if not name or not path:
            parser.error(f"--clip expects NAME=PATH[@OPTS], got {item!r}")
        opts = {o.split("=")[0]: (o.split("=", 1)[1] if "=" in o else True)
                for o in opts.split(",") if o}
        raw = read_video(Path(path))
        if "trim" in opts:
            a, b = (int(v) for v in opts["trim"].split("-"))
            raw = raw[a:b]
        frames = [key(f) for f in raw]
        if "open" in opts:
            frames = [remove_thin(f, int(opts["open"])) for f in frames]
        if "main" in opts:
            frames = [keep_main(f) for f in frames]
        ms = [measure(f) for f in frames]
        keep = [k for k, m in enumerate(ms) if m]
        frames, ms = [frames[k] for k in keep], [ms[k] for k in keep]
        if not frames:
            parser.error(f"--clip {name}: no keyed character found")
        clip_scale = scale
        if "stand" in opts:
            n = max(1, int(opts["stand"]))
            clip_scale = scale * float(np.median([m["height"] for m in ms[:n]])) ** -1 * \
                float(np.median([m["height"] for m in valid]))
        loop = "loop" in opts
        if loop:
            n = len(frames)
            i, j = best_loop(frames, max(6, n // 4), max(7, n - 1))
            frames, ms = frames[i:j], ms[i:j]
            if "stride" in opts:
                k = max(range(len(ms)), key=lambda q: ms[q]["right"] - ms[q]["left"])
                frames, ms = frames[k:] + frames[:k], ms[k:] + ms[:k]
        emit(name, frames, ms, loop, airborne="air" in opts, clip_scale=clip_scale,
             track="track" in opts, every=max(1, int(opts.get("every", 1))))

    previous = {}
    pack_path = args.out / "pack.json"
    if args.merge and pack_path.exists():
        previous = json.loads(pack_path.read_text(encoding="utf-8"))
        merged = dict(previous.get("animations", {}))
        merged.update(animations)
        animations = merged

    spec = {
        "name": args.name,
        "scale": args.native_scale,
        "offset": [0, 0],
        "facesLeft": False,
        "replace": ["diddy"],
        "anchor": list(anchor),
        "nativeOnlyAnimations": [10, 11] + list(range(29, 43)) + [73] + list(range(94, 159)),
        "animations": animations,
    }
    for field in ("scale", "offset", "facesLeft", "replace", "nativeOnlyAnimations",
                  "animationIds"):
        if field in previous:
            spec[field] = previous[field]
    (args.out / "pack.json").write_text(json.dumps(spec, indent=1) + "\n", encoding="utf-8")
    print(json.dumps({"scale": scale, "frames": report}))


if __name__ == "__main__":
    main()
