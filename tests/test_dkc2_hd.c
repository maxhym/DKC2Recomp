/* Synthetic checks of the HD replacement Kong: pack parsing, the motion
 * classifier, OAM ownership, occlusion against higher-priority layers,
 * flipping, forced blank and master brightness. No ROM is used. */
#include "dkc2_hd.h"
#include "snes/ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                             \
  do {                                                                          \
    if (!(cond)) {                                                              \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                               \
    }                                                                           \
  } while (0)

static void Put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void Put32(uint8_t *p, uint32_t v) { Put16(p, v & 0xffff); Put16(p + 2, v >> 16); }

/* Two 8x8 frames: frame 0 opaque red, frame 1 opaque blue with a
 * transparent left half. idle = [0 x4, 1 x4] looping, jump = [1] once. */
static size_t BuildPack(uint8_t *out, uint16_t flags) {
  const unsigned frames = 2, steps = 3, native_only = 1;
  memset(out, 0, 4096);
  memcpy(out, "DKC2HD01", 8);
  Put16(out + 8, flags);
  Put16(out + 10, 4 * 256);      /* 4 HD pixels per native pixel */
  Put16(out + 12, 0);
  Put16(out + 14, 0);
  Put16(out + 16, frames);
  Put16(out + 18, steps);
  Put16(out + 20, native_only);
  Put16(out + 24 + 0 * 8, 0); Put16(out + 26 + 0 * 8, 2); Put16(out + 28 + 0 * 8, 1);
  Put16(out + 24 + 3 * 8, 2); Put16(out + 26 + 3 * 8, 1); Put16(out + 28 + 3 * 8, 0);
  size_t c = 64;
  Put16(out + c, 0); Put16(out + c + 2, 4); c += 4;
  Put16(out + c, 1); Put16(out + c + 2, 4); c += 4;
  Put16(out + c, 1); Put16(out + c + 2, 1); c += 4;
  Put16(out + c, 77); c += 2;                       /* native-only animation */
  const size_t table = c;
  size_t pixels = table + frames * 12;
  for (unsigned f = 0; f < frames; ++f) {
    uint8_t *e = out + table + f * 12;
    Put16(e, 8); Put16(e + 2, 8);
    Put16(e + 4, 4); Put16(e + 6, 8);                /* anchor: bottom centre */
    Put32(e + 8, (uint32_t)pixels);
    for (unsigned i = 0; i < 64; ++i) {
      uint8_t *p = out + pixels + i * 4;
      p[0] = f ? 0 : 255;
      p[1] = 0;
      p[2] = f ? 255 : 0;
      p[3] = (f && (i % 8) < 4) ? 0 : 255;
    }
    pixels += 64 * 4;
  }
  return pixels;
}

static void TestLoading(void) {
  static uint8_t pack[4096];
  size_t size = BuildPack(pack, kDkc2HdFlagReplaceDiddy);
  CHECK(!Dkc2HdLoadBytes(pack, 10));
  pack[0] = 'X';
  CHECK(!Dkc2HdLoadBytes(pack, size));
  pack[0] = 'D';
  CHECK(!Dkc2HdLoadBytes(pack, size - 1)); /* truncated pixels */
  Put16(pack + 26, 0);                     /* idle removed */
  CHECK(!Dkc2HdLoadBytes(pack, size));
  size = BuildPack(pack, kDkc2HdFlagReplaceDiddy);
  CHECK(Dkc2HdLoadBytes(pack, size));
  CHECK(Dkc2HdReady());
  CHECK(Dkc2HdReplaces(0));
  CHECK(!Dkc2HdReplaces(1));
  CHECK(Dkc2HdAllowsState(1, 0x00));
  CHECK(!Dkc2HdAllowsState(77, 0x00));
  CHECK(!Dkc2HdAllowsState(1, 0x24));
  Dkc2HdSetEnabled(false);
  CHECK(!Dkc2HdReplaces(0));
  Dkc2HdSetEnabled(true);
}

static void TestClassifier(void) {
  Dkc2HdMotion m;
  memset(&m, 0, sizeof m);
  int x = 1000;
  for (uint32_t t = 0; t < 6; ++t) CHECK(Dkc2HdClassify(&m, x, 500, true, false, t) == kDkc2HdIdle);
  for (uint32_t t = 6; t < 12; ++t) Dkc2HdClassify(&m, x += 1, 500, true, false, t);
  CHECK(m.animation == kDkc2HdWalk);
  for (uint32_t t = 12; t < 18; ++t) Dkc2HdClassify(&m, x += 3, 500, true, false, t);
  CHECK(m.animation == kDkc2HdRun);
  CHECK(Dkc2HdClassify(&m, x, 490, false, true, 18) == kDkc2HdJump);
  CHECK(m.started == 18);
  CHECK(Dkc2HdClassify(&m, x, 495, false, false, 30) == kDkc2HdFall);
  CHECK(m.started == 30);
  CHECK(Dkc2HdClassify(&m, x + 400, 500, true, false, 31) != kDkc2HdRun); /* teleport ignored */
  /* 2 px per tick starts a walk, but does not end a run already under way. */
  Dkc2HdMotion slow, fast;
  memset(&slow, 0, sizeof slow);
  memset(&fast, 0, sizeof fast);
  int sx = 0, fx = 0;
  for (uint32_t t = 0; t < 8; ++t) Dkc2HdClassify(&slow, sx += 2, 500, true, false, t);
  CHECK(slow.animation == kDkc2HdWalk);
  for (uint32_t t = 0; t < 8; ++t) Dkc2HdClassify(&fast, fx += 3, 500, true, false, t);
  for (uint32_t t = 8; t < 16; ++t) Dkc2HdClassify(&fast, fx += 2, 500, true, false, t);
  CHECK(fast.animation == kDkc2HdRun);
  for (uint32_t t = 16; t < 24; ++t) Dkc2HdClassify(&fast, fx += 1, 500, true, false, t);
  CHECK(fast.animation == kDkc2HdWalk);
}

static Ppu *NewPpu(void) {
  Ppu *ppu = (Ppu *)calloc(1, sizeof *ppu);
  ppu->inidisp = 0x0f;
  return ppu;
}

static Dkc2HdActorInput Actor(uint8_t *slots, uint16_t properties) {
  Dkc2HdActorInput in;
  memset(&in, 0, sizeof in);
  in.screen_x = 100;
  in.screen_y = 101;          /* feet on output row 100 */
  in.world_x = 1000;
  in.world_y = 500;
  in.properties = properties; /* priority bits 12-13 */
  in.grounded = true;
  in.slots = slots;
  return in;
}

static const Dkc2OverlaySprite *Render(Ppu *ppu, const Dkc2HdActorInput *in, uint32_t tick) {
  Dkc2HdBeginFrame(tick);
  Dkc2HdSetActor(0, in);
  for (unsigned line = 1; line <= 224; ++line) Dkc2HdObserveMainLine(ppu, line);
  Dkc2HdFinishFrame(ppu);
  const Dkc2OverlaySprite *sprites = NULL;
  return Dkc2HdSprites(&sprites) == 1 ? sprites : NULL;
}

static void TestCompositing(void) {
  Ppu *ppu = NewPpu();
  uint8_t slots[128] = {0};
  slots[5] = slots[6] = 1;
  /* OBJ priority 2 sits at level 10; BG1 high-priority tiles are level 12. */
  Dkc2HdActorInput in = Actor(slots, 2u << 12);
  /* A foreground tree covers screen column 100 on every line. */
  for (unsigned i = 0; i < kPpuBufWidth; ++i) ppu->bgBuffers[0].data[i] = 0x7100;
  ppu->bgBuffers[0].data[100 + kPpuExtraLeftRight] = 0xc000;
  const Dkc2OverlaySprite *s = Render(ppu, &in, 100);
  CHECK(s != NULL);
  if (!s) return;
  CHECK(Dkc2HdOwnsSlot(5) && Dkc2HdOwnsSlot(6) && !Dkc2HdOwnsSlot(7));
  CHECK(s->width == 8 && s->height == 8);
  CHECK(s->w == 2.0f && s->h == 2.0f);            /* 8 HD px / scale 4 */
  CHECK(s->x == 99.0f && s->y == 98.0f);          /* anchor (4,8) on (100,100) */
  CHECK(s->brightness == 1.0f);
  /* HD columns 0-3 map to native column 99, 4-7 to column 100 (covered). */
  CHECK(s->rgba[(2 * 8 + 1) * 4 + 3] == 255 && s->rgba[(2 * 8 + 1) * 4 + 0] == 255);
  CHECK(s->rgba[(2 * 8 + 6) * 4 + 3] == 0);
  const uint32_t serial = s->serial;

  /* Another sprite at that column, even with higher priority bits (an
   * animal buddy under its rider), does not cover the Kong. */
  ppu->bgBuffers[0].data[100 + kPpuExtraLeftRight] = 0xe085;
  s = Render(ppu, &in, 101);
  CHECK(s && s->rgba[(2 * 8 + 6) * 4 + 3] == 255);
  ppu->bgBuffers[0].data[100 + kPpuExtraLeftRight] = 0xc000;
  s = Render(ppu, &in, 101);
  CHECK(s && s->rgba[(2 * 8 + 6) * 4 + 3] == 0);

  /* A higher OBJ priority puts the Kong in front of the tree. */
  in.properties = 3u << 12;
  s = Render(ppu, &in, 101);
  CHECK(s && s->rgba[(2 * 8 + 6) * 4 + 3] == 255);
  CHECK(s && s->serial != serial);

  /* Facing left mirrors the frame around the anchor. */
  for (unsigned i = 0; i < kPpuBufWidth; ++i) ppu->bgBuffers[0].data[i] = 0x7100;
  in.properties = (uint16_t)((2u << 12) | 0x4000);
  s = Render(ppu, &in, 102);
  CHECK(s && s->x == 99.0f);

  /* Master brightness dims; forced blank hides the sprite. */
  ppu->inidisp = 0x05;
  s = Render(ppu, &in, 103);
  CHECK(s && s->brightness > 0.33f && s->brightness < 0.34f);
  ppu->inidisp = 0x8f;
  CHECK(Render(ppu, &in, 104) == NULL);

  /* The idle clip loops: 4 ticks of frame 0, then frame 1 (left half clear). */
  ppu->inidisp = 0x0f;
  in.properties = 3u << 12;
  Dkc2HdBeginFrame(198);               /* a frame without the Kong restarts */
  Dkc2HdBeginFrame(199);               /* its clock on the next appearance  */
  for (uint32_t t = 200; t < 205; ++t) s = Render(ppu, &in, t);
  CHECK(s && s->rgba[3] == 0 && s->rgba[2] == 255);
  free(ppu);
}

/* A change of pose cross-fades from the last frame shown for a few ticks. */
static void TestCrossFade(void) {
  static uint8_t pack[4096];
  CHECK(Dkc2HdLoadBytes(pack, BuildPack(pack, kDkc2HdFlagReplaceDiddy)));
  Ppu *ppu = NewPpu();
  for (unsigned i = 0; i < kPpuBufWidth; ++i) ppu->bgBuffers[0].data[i] = 0x7100;
  uint8_t slots[128] = {0};
  Dkc2HdActorInput in = Actor(slots, 3u << 12);
  Dkc2HdBeginFrame(298);
  Dkc2HdBeginFrame(299);
  const Dkc2OverlaySprite *s = Render(ppu, &in, 300);   /* idle: red frame */
  CHECK(s && s->rgba[0] == 255 && s->rgba[3] == 255);
  in.grounded = false;
  in.rising = true;                                     /* jump: blue, left half clear */
  s = Render(ppu, &in, 301);
  CHECK(s && s->width == 8 && s->height == 8);
  /* Left half: only the old red frame, fading out. */
  CHECK(s && s->rgba[0] == 255 && s->rgba[3] > 200 && s->rgba[3] < 255);
  /* Right half: red still, fully opaque; the change starts gently. */
  const uint8_t *right = s ? s->rgba + 6 * 4 : NULL;
  CHECK(right && right[3] == 255 && right[0] > 230 && right[2] > 0);
  for (uint32_t t = 302; t < 306; ++t) s = Render(ppu, &in, t);
  right = s ? s->rgba + 6 * 4 : NULL;                    /* half-way: both poses */
  CHECK(right && right[3] == 255 && right[0] > 90 && right[2] > 90);
  for (uint32_t t = 306; t < 310; ++t) s = Render(ppu, &in, t);
  CHECK(s && s->rgba[3] > 0 && s->rgba[3] < 30);         /* nearly gone */
  s = Render(ppu, &in, 310);
  CHECK(s && s->rgba[3] == 0 && s->rgba[6 * 4 + 2] == 255 && s->rgba[6 * 4 + 0] == 0);
  free(ppu);
}

static void TestInactive(void) {
  Dkc2HdBeginFrame(1);
  CHECK(Dkc2HdActiveActors() == 0);
  CHECK(!Dkc2HdOwnsSlot(5));
  Dkc2HdUnload();
  CHECK(!Dkc2HdReady());
  CHECK(!Dkc2HdReplaces(0));
  const Dkc2OverlaySprite *sprites = NULL;
  Dkc2HdFinishFrame(NULL);
  CHECK(Dkc2HdSprites(&sprites) == 0 && sprites == NULL);
}

static void TestToggle(void) {
  static uint8_t pack[4096];
  CHECK(Dkc2HdLoadBytes(pack, BuildPack(pack, kDkc2HdFlagReplaceDiddy)));
  CHECK(!Dkc2HdToggle());
  CHECK(!Dkc2HdReplaces(0));
  CHECK(Dkc2HdToggle());
  CHECK(Dkc2HdReplaces(0));
}

/* v2 pack: idle (frame 0), fidget and fidget2 (frame 1 for 10 ticks, once),
 * roll, carry_idle, hurt (frame 1) and ride (frame 0), and a map from native
 * animation 12 to roll.
 * Native animation 12 is also listed as native-only: the map must win. */
static size_t BuildPackV2(uint8_t *out) {
  const unsigned frames = 2, anims = kDkc2HdFidget2 + 1, steps = 7, native_only = 1, maps = 1;
  memset(out, 0, 4096);
  memcpy(out, "DKC2HD02", 8);
  Put16(out + 8, kDkc2HdFlagReplaceDiddy);
  Put16(out + 10, 4 * 256);
  Put16(out + 16, frames);
  Put16(out + 18, steps);
  Put16(out + 20, native_only);
  Put16(out + 22, anims);
  Put16(out + 24, maps);
  const unsigned slot_step[][2] = {{kDkc2HdIdle, 0}, {kDkc2HdFidget, 1}, {kDkc2HdRoll, 2},
                                   {kDkc2HdCarryIdle, 3}, {kDkc2HdHurt, 4},
                                   {kDkc2HdFidget2, 5}, {kDkc2HdRide, 6}};
  for (unsigned i = 0; i < 7; ++i) {
    uint8_t *r = out + 32 + slot_step[i][0] * 8;
    Put16(r, slot_step[i][1]);
    Put16(r + 2, 1);
    Put16(r + 4, slot_step[i][0] != kDkc2HdFidget && slot_step[i][0] != kDkc2HdFidget2);
  }
  size_t c = 32 + anims * 8;
  for (unsigned i = 0; i < steps; ++i, c += 4) {
    Put16(out + c, i == 0 || i == 6 ? 0 : 1);
    Put16(out + c + 2, i == 1 || i == 5 ? 10 : 4);
  }
  Put16(out + c, 12); c += 2;                        /* native-only */
  Put16(out + c, 12); Put16(out + c + 2, kDkc2HdRoll); c += 4;
  const size_t table = c;
  size_t pixels = table + frames * 12;
  for (unsigned f = 0; f < frames; ++f) {
    uint8_t *e = out + table + f * 12;
    Put16(e, 8); Put16(e + 2, 8); Put16(e + 4, 4); Put16(e + 6, 8);
    Put32(e + 8, (uint32_t)pixels);
    memset(out + pixels, f ? 0x80 : 0xFF, 64 * 4);
    pixels += 64 * 4;
  }
  return pixels;
}

static int Shown(Dkc2HdActorInput *in, uint32_t tick) {
  Dkc2HdBeginFrame(tick);
  Dkc2HdSetActor(0, in);
  return Dkc2HdActorAnimation(0);
}

static void TestExtendedSlots(void) {
  static uint8_t pack[4096];
  CHECK(Dkc2HdLoadBytes(pack, BuildPackV2(pack)));
  CHECK(strstr(Dkc2HdStatus(), "7 animations") != NULL);
  CHECK(Dkc2HdHasRide());
  CHECK(Dkc2HdAllowsState(12, 0x00));      /* mapped wins over native-only */
  CHECK(Dkc2HdAllowsState(1, 0x24));       /* hurt pose available */
  CHECK(Dkc2HdHasCarry());
  uint8_t slots[128] = {0};
  Dkc2HdActorInput in = Actor(slots, 2u << 12);
  uint32_t t = 1000;
  Dkc2HdBeginFrame(t++);
  CHECK(Shown(&in, t++) == kDkc2HdIdle);
  in.animation = 12;
  CHECK(Shown(&in, t++) == kDkc2HdRoll);
  in.animation = 0;
  in.state = 0x24;
  CHECK(Shown(&in, t++) == kDkc2HdHurt);
  in.state = 0;
  in.carrying = true;
  /* Diddy holds objects in front; without that clip the overhead one shows. */
  CHECK(Shown(&in, t++) == kDkc2HdCarryFrontIdle);
  in.carrying = false;
  /* Standing still long enough plays a fidget once, then idles again; the
   * fidget slots take turns. */
  int seen[kDkc2HdAnimCount] = {0}, order[3] = {-1, -1, -1}, after = -1;
  unsigned fidgets = 0;
  int last = kDkc2HdIdle;
  for (unsigned i = 0; i < 1000 && fidgets < 3; ++i) {
    const int shown = Shown(&in, t++);
    ++seen[shown];
    if (shown != last && (shown == kDkc2HdFidget || shown == kDkc2HdFidget2))
      order[fidgets++] = shown;
    if (last == kDkc2HdFidget && shown != last && after < 0) after = shown;
    last = shown;
  }
  CHECK(order[0] == kDkc2HdFidget);
  CHECK(order[1] == kDkc2HdFidget2);
  CHECK(order[2] == kDkc2HdFidget);
  CHECK(seen[kDkc2HdFidget2] == 10);
  CHECK(after == kDkc2HdIdle);
  /* The rider on an animal shows the ride pose, ahead of every other slot. */
  in.riding = true;
  in.state = 0x24;
  CHECK(Shown(&in, t++) == kDkc2HdRide);
  in.riding = false;
  in.state = 0;
  /* A truncated or out-of-range map is rejected. */
  const size_t size = BuildPackV2(pack);
  CHECK(!Dkc2HdLoadBytes(pack, 40));
  Put16(pack + 22, 99);
  CHECK(!Dkc2HdLoadBytes(pack, size));
}

/* Three opaque 8x8 frames (red, blue, green): idle = red, walk = blue,
 * stand_to_fours = green for 6 ticks, once; hang and climb_wide_idle = blue,
 * mapped from native animations $39 and $36. */
static size_t BuildPackTurn(uint8_t *out) {
  const unsigned frames = 3, anims = kDkc2HdAnimCount, steps = 3;
  memset(out, 0, 4096);
  memcpy(out, "DKC2HD02", 8);
  Put16(out + 8, kDkc2HdFlagReplaceDiddy);
  Put16(out + 10, 4 * 256);
  Put16(out + 16, frames);
  Put16(out + 18, steps);
  Put16(out + 22, anims);
  Put16(out + 24, 2); /* map entries */
  const unsigned slot_step[][3] = {{kDkc2HdIdle, 0, 1}, {kDkc2HdWalk, 1, 1},
                                   {kDkc2HdStandToFours, 2, 0}, {kDkc2HdHang, 1, 1},
                                   {kDkc2HdClimbWideIdle, 1, 1}};
  for (unsigned i = 0; i < 5; ++i) {
    uint8_t *r = out + 32 + slot_step[i][0] * 8;
    Put16(r, slot_step[i][1]);
    Put16(r + 2, 1);
    Put16(r + 4, slot_step[i][2]);
  }
  size_t c = 32 + anims * 8;
  for (unsigned i = 0; i < steps; ++i, c += 4) {
    Put16(out + c, i);
    Put16(out + c + 2, i == 2 ? 6 : 4);
  }
  Put16(out + c, 0x39); Put16(out + c + 2, kDkc2HdHang); c += 4;
  Put16(out + c, 0x36); Put16(out + c + 2, kDkc2HdClimbWideIdle); c += 4;
  const size_t table = c;
  size_t pixels = table + frames * 12;
  for (unsigned f = 0; f < frames; ++f) {
    uint8_t *e = out + table + f * 12;
    Put16(e, 8); Put16(e + 2, 8); Put16(e + 4, 4); Put16(e + 6, 8);
    Put32(e + 8, (uint32_t)pixels);
    for (unsigned i = 0; i < 64; ++i) {
      uint8_t *px = out + pixels + i * 4;
      px[f == 1 ? 2 : f == 2 ? 1 : 0] = 255;
      px[3] = 255;
    }
    pixels += 64 * 4;
  }
  return pixels;
}

/* Standing to walking goes through the stand-to-fours clip, and back
 * through the same clip reversed. */
static void TestStandToFours(void) {
  static uint8_t pack[4096];
  CHECK(Dkc2HdLoadBytes(pack, BuildPackTurn(pack)));
  Ppu *ppu = NewPpu();
  for (unsigned i = 0; i < kPpuBufWidth; ++i) ppu->bgBuffers[0].data[i] = 0x7100;
  uint8_t slots[128] = {0};
  Dkc2HdActorInput in = Actor(slots, 3u << 12);
  uint32_t t = 500;
  Dkc2HdBeginFrame(t++);
  Dkc2HdBeginFrame(t++);
  const Dkc2OverlaySprite *s = NULL;
  for (unsigned i = 0; i < 6; ++i) s = Render(ppu, &in, t++);
  CHECK(s && s->rgba[0] == 255 && s->rgba[1] == 0 && s->rgba[2] == 0);   /* idle */
  /* Start walking: green shows up before any blue takes over. */
  unsigned green_ticks = 0, first_blue = 0;
  for (unsigned i = 1; i <= 40; ++i) {
    in.world_x += 1;
    s = Render(ppu, &in, t++);
    if (!s) continue;
    if (s->rgba[1] > 128) ++green_ticks;
    if (!first_blue && s->rgba[2] > 128) first_blue = i;
  }
  CHECK(Dkc2HdActorAnimation(0) == kDkc2HdWalk);
  CHECK(green_ticks >= 4);
  CHECK(first_blue > green_ticks);
  CHECK(s && s->rgba[2] == 255 && s->rgba[1] == 0);                       /* walking */
  /* Stop: green again on the way back up, then red. */
  green_ticks = 0;
  for (unsigned i = 0; i < 40; ++i) {
    s = Render(ppu, &in, t++);
    if (s && s->rgba[1] > 128) ++green_ticks;
  }
  CHECK(Dkc2HdActorAnimation(0) == kDkc2HdIdle);
  CHECK(green_ticks >= 4);
  CHECK(s && s->rgba[0] == 255 && s->rgba[1] == 0 && s->rgba[2] == 0);
  free(ppu);
}

/* Hanging poses hang from the top of the native sprite's box and poses
 * between two ropes sit on its middle, wherever the origin is. */
static void TestBoxPlacement(void) {
  static uint8_t pack[4096];
  CHECK(Dkc2HdLoadBytes(pack, BuildPackTurn(pack)));
  Ppu *ppu = NewPpu();
  for (unsigned i = 0; i < kPpuBufWidth; ++i) ppu->bgBuffers[0].data[i] = 0x7100;
  uint8_t slots[128] = {0};
  Dkc2HdActorInput in = Actor(slots, 3u << 12);
  in.has_bounds = true;
  in.bounds_left = 110; in.bounds_right = 130;
  in.bounds_top = 50; in.bounds_bottom = 80;
  Dkc2HdBeginFrame(698);
  Dkc2HdBeginFrame(699);
  /* A ground pose ignores the box. */
  const Dkc2OverlaySprite *s = Render(ppu, &in, 700);
  CHECK(s && s->x == 99.0f && s->y == 98.0f);
  in.animation = 0x39;
  for (uint32_t t = 701; t < 715; ++t) s = Render(ppu, &in, t);
  CHECK(Dkc2HdActorAnimation(0) == kDkc2HdHang);
  CHECK(s && s->x == 119.0f && s->y == 50.0f);     /* top edge, centred */
  in.animation = 0x36;
  for (uint32_t t = 715; t < 730; ++t) s = Render(ppu, &in, t);
  CHECK(Dkc2HdActorAnimation(0) == kDkc2HdClimbWideIdle);
  CHECK(s && s->x == 119.0f && s->y == 64.0f);     /* middle on middle */
  /* Without a box the origin is used, as before. */
  in.has_bounds = false;
  for (uint32_t t = 730; t < 745; ++t) s = Render(ppu, &in, t);
  CHECK(s && s->x == 99.0f && s->y == 98.0f);
  free(ppu);
}

/* Styles work on the composited frame; the pack is never modified. */
static void TestStyles(void) {
  static uint8_t pack[4096];
  CHECK(Dkc2HdLoadBytes(pack, BuildPack(pack, kDkc2HdFlagReplaceDiddy)));
  CHECK(Dkc2HdStyleFromName("retro") == kDkc2HdStyleRetro);
  CHECK(Dkc2HdStyleFromName("crt") == kDkc2HdStyleCrt);
  CHECK(Dkc2HdStyleFromName("nonsense") == kDkc2HdStyleHd);
  Ppu *ppu = NewPpu();
  uint8_t slots[128] = {0};
  slots[5] = 1;
  Dkc2HdActorInput in = Actor(slots, 3u << 12);

  Dkc2HdSetStyle(kDkc2HdStyleHd);
  Dkc2HdBeginFrame(500);
  Dkc2HdBeginFrame(501);
  const Dkc2OverlaySprite *s = Render(ppu, &in, 502);
  CHECK(s && s->w == 2.0f);
  const float hd_width = s ? s->w : 0;

  /* Size: half size draws the same frame on half the native area. */
  Dkc2HdSetSize(0.5f);
  s = Render(ppu, &in, 503);
  CHECK(s && s->w == hd_width * 0.5f && s->h == 1.0f);
  Dkc2HdSetSize(9.0f); /* out of range: back to 1.0 */
  CHECK(Dkc2HdSize() == 1.0f);

  /* Retro: hard alpha, 15-bit colours. */
  Dkc2HdSetStyle(kDkc2HdStyleRetro);
  s = Render(ppu, &in, 504);
  bool hard = s != NULL, five_bit = s != NULL;
  for (int i = 0; s && i < s->width * s->height; ++i) {
    const uint8_t *p = s->rgba + i * 4;
    if (p[3] != 0 && p[3] != 255) hard = false;
    if (p[3] && ((p[0] & 7) != (p[0] >> 5))) five_bit = false;
  }
  CHECK(hard && five_bit);

  /* CRT: rows between native scanlines are darker than the others. */
  Dkc2HdSetStyle(kDkc2HdStyleCrt);
  s = Render(ppu, &in, 505);
  unsigned bright = 0, dark = 0;
  for (int y = 0; s && y < s->height; ++y) {
    const uint8_t *p = s->rgba + ((size_t)y * s->width + s->width / 2) * 4;
    if (!p[3]) continue;
    if (p[0] > 200) ++bright; else if (p[0] < 190) ++dark;
  }
  CHECK(bright > 0 && dark > 0);

  /* Soft: alpha gains intermediate values on the edges. */
  Dkc2HdSetStyle(kDkc2HdStyleSoft);
  s = Render(ppu, &in, 506);
  bool partial = false;
  for (int i = 0; s && i < s->width * s->height; ++i)
    if (s->rgba[i * 4 + 3] > 0 && s->rgba[i * 4 + 3] < 255) partial = true;
  CHECK(partial);
  Dkc2HdSetStyle(kDkc2HdStyleHd);
  free(ppu);
}

static void TestRealPack(const char *path) {
  if (!path) return;
  CHECK(Dkc2HdLoad(path));
  CHECK(Dkc2HdReplaces(0));
  fprintf(stdout, "%s\n", Dkc2HdStatus());
  /* The shipped pack ducks, lies knocked out, and holds the partner
   * overhead in a team-up instead of leaving those to the native sprite. */
  uint8_t slots[128] = {0};
  Dkc2HdActorInput in = Actor(slots, 2u << 12);
  uint32_t t = 5000;
  Dkc2HdBeginFrame(t++);
  static const struct { unsigned native; int slot; } kPoses[] = {
      {0x0C, kDkc2HdCrouch}, {0x0D, kDkc2HdCrouch}, {0x0E, kDkc2HdCrouch},
      {0x0A, kDkc2HdKnockout}, {0x28, kDkc2HdKnockout}, {0x29, kDkc2HdHurt},
      {0x1D, kDkc2HdCarryIdle}, {0x1F, kDkc2HdCarryWalk}, {0x26, kDkc2HdThrow}};
  for (size_t i = 0; i < sizeof kPoses / sizeof kPoses[0]; ++i) {
    in.animation = kPoses[i].native;
    CHECK(Dkc2HdAllowsState(in.animation, 0x00));
    CHECK(Shown(&in, t++) == kPoses[i].slot);
  }
}

int main(int argc, char **argv) {
  TestLoading();
  TestClassifier();
  TestCompositing();
  TestToggle();
  TestStyles();
  TestCrossFade();
  TestStandToFours();
  TestBoxPlacement();
  TestExtendedSlots();
  TestInactive();
  TestRealPack(argc > 1 ? argv[1] : NULL);
  if (failures) {
    fprintf(stderr, "%d HD Kong check(s) failed\n", failures);
    return 1;
  }
  puts("dkc2 hd tests passed");
  return 0;
}
