# Mandro HD frames

Mandro is an original mandrill character made for this project. It is not
Nintendo or Rare content and none of these frames was derived from the game.

* Reference sheet: generated with an image model from a written description
  (a sturdy mandrill with a bright blue-and-red muzzle and a golden mane).
* Motion: short side-view clips generated from that reference on a
  chroma-green background: walk, run and jump, then a lively idle, three
  fidgets (chest beat, scratching, looking around), roll, carrying overhead,
  rope hang, climb up and down, horizontal rope, swim, hit, riding, victory
  and throw. Poles and bars are green so the keying removes them.
* Frames: cut, keyed and aligned by `scripts/extract_hd_kong_frames.py`.
  `pack.json` was written by that script and its durations and native-only
  animation list may be edited by hand.

The build packs this folder into `mods/kong-hd.dkc2hd` beside the hosts. See
[docs/HD_KONGS.md](../../docs/HD_KONGS.md).
