# HD replacement Kong

An optional, presentation-only layer that draws an original high-resolution
character over Diddy's slot (or Dixie's) at window resolution instead of the
SNES sprite. The repository ships one such character, **Mandro**
(`mods/mandro-hd`, original art; see its README).

Game logic is untouched: WRAM, VRAM, OAM, collision, hitboxes and timing stay
native. The layer only hides the Kong's OAM pieces from the PPU and draws a
textured quad in their place.

## How it works

1. `dkc2_kongs.c` finds the Kong actor (type `$E4` Diddy slot, `$E8` Dixie
   slot) and its displayed origin (`MatchLayout`), as Project Kongs already
   does. If the pack replaces that slot and the current animation/state is
   not native-only, it hands `Dkc2HdSetActor` the screen origin, world
   position, OAM attribute word, grounded (`+$1E & $0101`) and rising
   (`+$24 & $8000`) flags and the OAM slots it owns. Those slots are then
   hidden from the PPU.
2. A classifier picks idle, walk, run, jump or fall from the motion averaged
   over four frames. Run falls back to walk at 8/5 speed; fall without its own
   clip holds the last jump frame.
3. After each main-screen line the PPU hook `Dkc2HdObserveMainLine` marks the
   pixels where a layer of higher priority than the Kong's OBJ priority
   (`priority * 4 + 2`) is drawn: foreground leaves, pipes, water fronts.
4. `Dkc2HdFinishFrame` composites the HD frame with those pixels cut out,
   applies master brightness and hides the sprite during forced blank.
5. The shared OpenGL pipeline (`Dkc2GlPipelineRender`, used by the native
   Windows and SDL presenters) draws the result with linear filtering after
   the CRT or flat pass.

The Kong keeps its native sprite whenever the pack does not cover the moment:
riding an animal, hurt and knocked-out states, being carried or carrying the
partner, team-up and handoff, and every animation listed in
`nativeOnlyAnimations`.

## Testing Mandro

1. Generate the sources with your own ROM and build a desktop host as usual
   (`generate_snesrecomp.ps1`, then CMake with `-DDKC2_BUILD_SNESRECOMP=ON`).
   The post-build step writes `mods/kong-hd.dkc2hd` beside the executable;
   the hosts load it at start-up and print `HD pack loaded: ...` to stderr.
   `scripts\create_windows_version.ps1` and `scripts\make_release.ps1` copy
   that file into the `mods` folder of the packaged version.
2. Play with Diddy in front. **F8** switches between Mandro and the original
   sprite, so you can compare the placement.
3. Adjust without rebuilding through environment variables:

| Variable | Effect |
| --- | --- |
| `DKC2_HD=0` | Do not load the pack. |
| `DKC2_HD_PACK=path` | Load another `.dkc2hd` file. |
| `DKC2_HD_OFFSET=dx,dy` | Move the character, in SNES pixels (e.g. `0,3`). |
| `DKC2_HD_SCALE=4` | HD pixels per SNES pixel; larger draws smaller. |
| `DKC2_HD_FLIP=1` | Invert the facing if Mandro looks backwards. |
| `DKC2_HD_REPLACE=diddy\|dixie\|both` | Which slot(s) Mandro replaces. |
| `DKC2_HD_TRACE=1` | Log each animation change with the grounded/rising flags. |

The GDI fallback presenter has no OpenGL and therefore shows the native
sprite.

## Pack format (`.dkc2hd`, little-endian)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | `DKC2HD01` |
| 8 | 2 | flags: 1 faces left, 2 replace Diddy, 4 replace Dixie |
| 10 | 2 | scale, 8.8 fixed point (HD pixels per SNES pixel) |
| 12 | 2+2 | anchor offset x/y, signed 12.4 SNES pixels |
| 16 | 2 | frame count |
| 18 | 2 | step count |
| 20 | 2 | native-only animation count |
| 24 | 5 x 8 | idle, walk, run, jump, fall: first step, step count, loop, 0 |
| 64 | 4 x steps | frame index, duration in 60 Hz frames |
| ... | 2 x n | native-only animation IDs |
| ... | 12 x frames | width, height, anchor x, anchor y (s16), RGBA offset (u32) |
| ... | | straight-alpha RGBA pixels |

`scripts/build_hd_kong_pack.py <folder> <output>` builds it from `pack.json`
and PNG frames using only the Python standard library. Its docstring describes
`pack.json`.

## Making another character

`scripts/extract_hd_kong_frames.py` (needs ffmpeg, numpy and Pillow) turns
side-view clips on a chroma-green background into aligned frames:

```
python scripts/extract_hd_kong_frames.py --out mods/my-kong \
    --idle idle.mp4 --walk walk.mp4 --run run.mp4 --jump jump.mp4
```

It keys the green, finds the best loop in the idle, walk and run clips, splits
the jump at its apex into jump and fall, and scales everything so the idle
body is 192 HD pixels tall on a 256 canvas with the feet at (128, 248). Point
`DKC2_HD_KONG_PACK_DIR` at the new folder when configuring CMake.

## Limits

- Not yet checked in game: anchor offset, facing, the grounded/rising bits and
  which animation IDs need to stay native (swimming, climbing, cartwheel and
  hair-spin are expected to need entries in `nativeOnlyAnimations`).
- Palette effects such as water tint, colour math and mosaic do not reach the
  HD sprite; master brightness fades do.
- The CRT tube curvature is not applied to the HD sprite.
- Only Diddy's slot is replaced by default; Dixie and the follower AI pose
  remain native unless `DKC2_HD_REPLACE` says otherwise.
