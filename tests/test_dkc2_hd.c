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

static void TestRealPack(const char *path) {
  if (!path) return;
  CHECK(Dkc2HdLoad(path));
  CHECK(Dkc2HdReplaces(0));
  fprintf(stdout, "%s\n", Dkc2HdStatus());
}

int main(int argc, char **argv) {
  TestLoading();
  TestClassifier();
  TestCompositing();
  TestToggle();
  TestInactive();
  TestRealPack(argc > 1 ? argv[1] : NULL);
  if (failures) {
    fprintf(stderr, "%d HD Kong check(s) failed\n", failures);
    return 1;
  }
  puts("dkc2 hd tests passed");
  return 0;
}
