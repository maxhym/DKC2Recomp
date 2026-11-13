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
   clip holds the last jump frame. A run only drops back to a walk well below
   the speed that started it, and walk and run keep their place in the stride
   when one replaces the other. Every change of pose cross-fades from the last
   frame shown over 9 ticks, eased. Between a standing pose and the
   four-legged gaits the `stand_to_fours` clip plays first, forwards or
   backwards.
3. After each main-screen line the PPU hook `Dkc2HdObserveMainLine` marks the
   pixels where a background layer of higher priority than the Kong's OBJ
   priority (`priority * 4 + 2`) is drawn: foreground leaves, pipes, water
   fronts. Other sprites never cover the Kong: between sprites the OAM order
   decides, not the priority bits (an animal buddy carries higher bits than
   its rider and would otherwise hide him).
4. `Dkc2HdFinishFrame` composites the HD frame with those pixels cut out,
   applies master brightness and hides the sprite during forced blank.
5. The shared OpenGL pipeline (`Dkc2GlPipelineRender`, used by the native
   Windows and SDL presenters) draws the result with linear filtering after
   the CRT or flat pass.

The Kong keeps its native sprite whenever the pack does not cover the moment:
riding an animal (unless the pack has a `ride` pose), knocked-out states
without a `hurt` pose, being carried or carrying the partner, team-up and
handoff, and every animation listed in `nativeOnlyAnimations`.

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
| `DKC2_HD_STYLE=hd\|doux\|retro\|crt` | Presentation style (below). |
| `DKC2_HD_SIZE=100` | Size in percent of the pack's (50 to 200). |
| `DKC2_HD_TRACE=1` | Log each animation change with the grounded/rising flags. |

The GDI fallback presenter has no OpenGL and therefore shows the native
sprite.

## Animation slots

Slots 0-4 (idle, walk, run, jump, fall) come from the motion classifier. A
pack may add: `fidget`, `fidget2`, `fidget3` (one plays after 5 s standing
still; they take turns), `roll`, `carry_idle`/`carry_walk` (holding a barrel
or object; the game still draws the barrel), `climb_idle`/`climb_move`/
`climb_down`, `hang` (horizontal rope), `swim`, `hurt`, `ride`, `victory`,
`throw`, `carry_front_idle`/`carry_front_walk`/`throw_front` (an object held
in front of the chest, as Diddy and Dixie do; the overhead carry slots serve
Donkey Kong in DKC1Recomp), `stand_to_fours`, `hang_idle` (hanging still
from a horizontal rope or a hook), `climb_wide_idle`/`climb_wide_move`
(spread between two ropes, seen from behind), `crouch` (ducked low) and
`knockout` (lying knocked out; falls back to `hurt`). Hanging poses are placed on
the top edge of the native sprite's box and two-rope poses on its middle:
on a hook the native Kong hangs below its origin, so the origin's feet row
would draw the pose standing on the hook. For each frame the engine picks, in order: `ride` for the rider on an
animal; the slot the pack's `animationIds` map assigns to the native animation
ID; `hurt` in knock-out states `$24/$25/$59`; the carry slots while the leader
holds an object other than its partner; a fidget; then motion. Missing slots
fall back (carry walk to carry idle, climb down to climb move to climb idle,
hang to climb idle, others to idle), and moments with no slot keep the native
sprite as before. Known rope IDs: single rope `$2F` hang, `$30` up, `$31` down, `$32`
turn; double rope `$34/$35` transitions, `$36` hang, `$37` up, `$38` down
(the two-rope slots); horizontal rope `$39` still, `$3A` moving. Other Diddy-slot IDs Mandro's pack maps: `$09`
roll, `$0A` death and `$29/$2A` hurt (hurt pose), `$0F-$13` and `$15-$18`
carrying in front (pickup, idle, walk `$11`, turn, drop, jump, air, fall,
land), `$14` front throw, `$2B` honey wall (rope hang pose), `$42` hook (horizontal hang),
`$43/$44` and `$A3` swimming, `$45` celebration, `$46` level-end run and `$4E`
skull cart (ride pose). Dixie's IDs are the same plus `$A3`; the engine
subtracts that offset. `DKC2_HD_TRACE=1` prints every native animation ID and
state change, to check or extend the map.

## Styles

Styles run on each composited frame, after the occlusion cut, so the pack
itself never changes:

- `hd`: the frames as drawn.
- `doux`: a 1-2-1 blur of premultiplied colour and 10 % less saturation.
- `retro`: blocks of half the pack scale (2x2 HD pixels at scale 4), 15-bit
  colour like the SNES palette, hard alpha and a one-block dark outline.
- `crt`: softened, with the HD rows that fall in the lower part of each native
  scanline darkened so the character shares the CRT pass's line structure.

DKCountry's settings page sets the style and size at launch.

## Pack format (`.dkc2hd`, little-endian)

`DKC2HD02` (extended slots or a map) shares the first 22 bytes with
`DKC2HD01` below, then: 22 animation count, 24 map count, 32 animation
records (8 bytes each), steps, native-only IDs, map entries (native ID, slot;
4 bytes each), frame table and pixels. Packs with only the five basic slots and
no map are still written as `DKC2HD01`:

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

Extra slots come from `--clip NAME=PATH[@OPTIONS]`, and `--merge` adds them to
an existing `pack.json` without touching the other slots or the tuning:

```
python scripts/extract_hd_kong_frames.py --out mods/my-kong --idle idle.mp4 --merge \
    --clip fidget=chest.mp4 --clip roll=roll.mp4@track \
    --clip swim=swim.mp4@loop,air --clip climb_idle=rope.mp4@loop,air,stand=4
```

Options: `loop` (best loop) or once (default), `track` (centre each frame, for
clips where the character travels), `air` (centre vertically too),
`trim=A-B` (clip frames A to B-1), `stand=N` (rescale a wider shot from the
standing height of its first N frames), `open=N` (remove keyed structures
thinner than N pixels, such as a green pole the character holds), `every=N`
(keep one frame in N, shown N times longer), `main` (drop detached dust or
debris), `stride` (start a loop on its widest frame, so walk and run share a
phase).

The pack builder trims the transparent border of every frame and moves the
anchor with it, so placement does not change and the pack is about a third of
the size of the uncropped frames.

## Limits

- The rider's origin is not its seat, so the ride pose is placed on the
  native rider's own OAM pieces: bottom centre to bottom centre.
- Not yet checked in game: the rider's placement after that change, the rope
  poses' vertical placement,
  and which animation IDs need to stay native (cartwheel and hair-spin are
  expected to need entries in `nativeOnlyAnimations`).
- Palette effects such as water tint, colour math and mosaic do not reach the
  HD sprite; master brightness fades do.
- The CRT tube curvature is not applied to the HD sprite.
- Only Diddy's slot is replaced by default; Dixie and the follower AI pose
  remain native unless `DKC2_HD_REPLACE` says otherwise.
