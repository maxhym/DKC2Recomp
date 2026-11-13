"""Unit tests for scripts/build_hd_kong_pack.py (standard library only)."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import build_hd_kong_pack as pack  # noqa: E402


def write_png(path, w, h, pixel, filter_type=0, channels=4):
    rows = b""
    for _ in range(h):
        rows += bytes([filter_type]) + (bytes(pixel[:channels]) * w if filter_type == 0
                                        else bytes(pixel[:channels]) + b"\x00" * channels * (w - 1))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    color = 6 if channels == 4 else 2
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" +
                           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, color, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


class BuildHdKongPackTests(unittest.TestCase):
    def test_png_decoding_handles_filters_and_rgb(self):
        with tempfile.TemporaryDirectory() as tmp:
            write_png(Path(tmp) / "a.png", 3, 2, (10, 20, 30, 40))
            self.assertEqual(pack.read_png_rgba(Path(tmp) / "a.png"),
                             (3, 2, bytes((10, 20, 30, 40)) * 6))
            # Sub filter: each byte adds the pixel to its left.
            write_png(Path(tmp) / "b.png", 3, 1, (5, 6, 7, 8), filter_type=1)
            self.assertEqual(pack.read_png_rgba(Path(tmp) / "b.png")[2], bytes((5, 6, 7, 8)) * 3)
            write_png(Path(tmp) / "c.png", 2, 1, (1, 2, 3), channels=3)
            self.assertEqual(pack.read_png_rgba(Path(tmp) / "c.png")[2], bytes((1, 2, 3, 255)) * 2)

    def test_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "idle").mkdir()
            write_png(root / "idle/0.png", 4, 4, (255, 0, 0, 255))
            write_png(root / "idle/1.png", 4, 4, (0, 0, 255, 255))
            (root / "pack.json").write_text(json.dumps({
                "scale": 2.5, "offset": [1, -2], "facesLeft": True, "replace": ["diddy", "dixie"],
                "anchor": [2, 3], "nativeOnlyAnimations": [10, 11],
                "animations": {
                    "idle": {"frames": [["idle/0.png", 3], ["idle/1.png", 3], ["idle/0.png", 2]]},
                    "jump": {"loop": False, "frames": [["idle/1.png", 1]]},
                }}))
            data, frames, steps = pack.build(root)
            self.assertEqual((frames, steps), (2, 4))
            self.assertEqual(data[:8], b"DKC2HD01")
            flags, scale, ox, oy, nframes, nsteps, nonly = struct.unpack_from("<HHhhHHH", data, 8)
            self.assertEqual(flags, 1 | 2 | 4)
            self.assertEqual((scale, ox, oy), (640, 16, -32))
            self.assertEqual((nframes, nsteps, nonly), (2, 4, 2))
            idle = struct.unpack_from("<HHHH", data, 24)
            jump = struct.unpack_from("<HHHH", data, 24 + 3 * 8)
            self.assertEqual(idle[:3], (0, 3, 1))
            self.assertEqual(jump[:3], (3, 1, 0))
            steps_at = 64
            self.assertEqual(struct.unpack_from("<HH", data, steps_at + 8), (0, 2))
            table = steps_at + 4 * 4 + 2 * 2
            w, h, ax, ay, offset = struct.unpack_from("<HHhhI", data, table + 12)
            self.assertEqual((w, h, ax, ay), (4, 4, 2, 3))
            self.assertEqual(data[offset:offset + 4], bytes((0, 0, 255, 255)))
            self.assertEqual(len(data), offset + 64)

    def test_transparent_borders_are_cropped_and_anchor_follows(self):
        # 5x4 frame, one opaque pixel at (3, 1) and one at (1, 2).
        rgba = bytearray(5 * 4 * 4)
        for x, y in ((3, 1), (1, 2)):
            rgba[(y * 5 + x) * 4:(y * 5 + x) * 4 + 4] = bytes((9, 8, 7, 255))
        x0, y0, w, h, out = pack.crop_transparent(5, 4, bytes(rgba))
        self.assertEqual((x0, y0, w, h), (1, 1, 3, 2))
        self.assertEqual(out[(0 * 3 + 2) * 4:(0 * 3 + 2) * 4 + 4], bytes((9, 8, 7, 255)))
        self.assertEqual(out[(1 * 3 + 0) * 4:(1 * 3 + 0) * 4 + 4], bytes((9, 8, 7, 255)))
        self.assertEqual(pack.crop_transparent(2, 2, bytes(16))[2:4], (1, 1))
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "idle").mkdir()
            path = root / "idle/0.png"
            rows = b"".join(b"\x00" + bytes(rgba[y * 20:(y + 1) * 20]) for y in range(4))

            def chunk(kind, body):
                return struct.pack(">I", len(body)) + kind + body + \
                    struct.pack(">I", zlib.crc32(kind + body))
            path.write_bytes(b"\x89PNG\r\n\x1a\n" +
                             chunk(b"IHDR", struct.pack(">IIBBBBB", 5, 4, 8, 6, 0, 0, 0)) +
                             chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
            (root / "pack.json").write_text(json.dumps({
                "anchor": [2, 3], "animations": {"idle": {"frames": [["idle/0.png", 1]]}}}))
            data, _, _ = pack.build(root)
            table = 64 + 4
            self.assertEqual(struct.unpack_from("<HHhh", data, table), (3, 2, 1, 2))

    def test_extended_slots_and_map_use_v2(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_png(root / "a.png", 2, 2, (9, 9, 9, 255))
            (root / "pack.json").write_text(json.dumps({
                "animationIds": {"climb_idle": [47, 54], "roll": [12]},
                "animations": {
                    "idle": {"frames": [["a.png", 4]]},
                    "roll": {"frames": [["a.png", 2], ["a.png", 2]]},
                    "climb_idle": {"frames": [["a.png", 6]]},
                }}))
            data, frames, steps = pack.build(root)
            self.assertEqual(data[:8], b"DKC2HD02")
            anims, maps = struct.unpack_from("<HH", data, 22)
            self.assertEqual(anims, pack.ANIMATIONS.index("climb_idle") + 1)
            self.assertEqual(maps, 3)
            roll = struct.unpack_from("<HHHH", data, 32 + pack.ANIMATIONS.index("roll") * 8)
            climb = struct.unpack_from("<HHHH", data, 32 + pack.ANIMATIONS.index("climb_idle") * 8)
            self.assertEqual((roll[1], roll[2]), (2, 0))
            self.assertEqual((climb[1], climb[2]), (1, 1))
            map_at = 32 + anims * 8 + steps * 4
            self.assertEqual(struct.unpack_from("<HH", data, map_at), (47, pack.ANIMATIONS.index("climb_idle")))
            (root / "pack.json").write_text(json.dumps({
                "animationIds": {"dance": [1]}, "animations": {"idle": {"frames": [["a.png", 1]]}}}))
            with self.assertRaises(ValueError):
                pack.build(root)

    def test_rejects_bad_specs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_png(root / "a.png", 2, 2, (0, 0, 0, 255))
            (root / "pack.json").write_text(json.dumps({"animations": {"walk": {"frames": [["a.png", 2]]}}}))
            with self.assertRaises(ValueError):
                pack.build(root)
            (root / "pack.json").write_text(json.dumps({"animations": {"idle": {"frames": [["a.png", 0]]}}}))
            with self.assertRaises(ValueError):
                pack.build(root)

    def test_mandro_pack_builds(self):
        source = ROOT / "mods/mandro-hd"
        if not (source / "pack.json").exists():
            self.skipTest("Mandro frames are not present")
        data, frames, steps = pack.build(source)
        self.assertGreater(frames, 50)
        for name in pack.ANIMATIONS[:5]:
            index = pack.ANIMATIONS.index(name)
            self.assertGreater(struct.unpack_from("<HHHH", data, 24 + index * 8)[1], 0, name)


if __name__ == "__main__":
    unittest.main()
