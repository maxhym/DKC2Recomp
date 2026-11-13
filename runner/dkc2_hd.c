#include "dkc2_hd.h"
#include "snes/ppu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  kHeaderSize = 64,
  kMaxFrames = 1024,
  kMaxSteps = 4096,
  kMaxNativeOnly = 512,
  kMaxFrameSide = 1024,
  kMaskRows = 240,
};

typedef struct HdFrame {
  uint16_t width, height;
  int16_t anchor_x, anchor_y;
  const uint8_t *rgba;
} HdFrame;
typedef struct HdStep { uint16_t frame, duration; } HdStep;
typedef struct HdAnimation { uint16_t first, count, loop; } HdAnimation;

typedef struct HdPack {
  uint8_t *data;
  size_t size;
  uint16_t flags;
  float scale, offset_x, offset_y;
  HdFrame *frames;
  size_t frame_count;
  HdStep *steps;
  size_t step_count;
  HdAnimation animations[kDkc2HdAnimCount];
  uint16_t *native_only;
  size_t native_only_count;
} HdPack;

typedef struct HdActor {
  bool active, placed;
  Dkc2HdActorInput input;
  uint8_t slots[128];
  const HdFrame *frame;
  bool flip;
  unsigned level;
  float left, top, width, height;
} HdActor;

static HdPack s_pack;
static bool s_enabled = true;
static char s_status[256] = "No HD character pack loaded.";
static HdActor s_actors[2];
static Dkc2HdMotion s_motion[2];
static uint8_t s_owned[128];
static uint8_t s_mask[kMaskRows][kPpuBufWidth];
static uint8_t *s_out[2];
static size_t s_out_capacity[2];
static Dkc2OverlaySprite s_sprites[2];
static int s_sprite_count;
static uint32_t s_serial;
static uint32_t s_tick;
static int s_out_width = kPpuXPixels;

static uint16_t Read16(const uint8_t *p) { return (uint16_t)(p[0] | (unsigned)p[1] << 8); }
static uint32_t Read32(const uint8_t *p) { return Read16(p) | (uint32_t)Read16(p + 2) << 16; }

static void FreePack(HdPack *pack) {
  free(pack->data);
  free(pack->frames);
  free(pack->steps);
  free(pack->native_only);
  memset(pack, 0, sizeof *pack);
}

void Dkc2HdUnload(void) {
  FreePack(&s_pack);
  memset(s_actors, 0, sizeof s_actors);
  memset(s_motion, 0, sizeof s_motion);
  memset(s_owned, 0, sizeof s_owned);
  s_sprite_count = 0;
  (void)snprintf(s_status, sizeof s_status, "No HD character pack loaded.");
}

bool Dkc2HdReady(void) { return s_pack.frame_count != 0; }
const char *Dkc2HdStatus(void) { return s_status; }
void Dkc2HdSetEnabled(bool enabled) { s_enabled = enabled; }
bool Dkc2HdToggle(void) {
  s_enabled = !s_enabled;
  return Dkc2HdEnabled();
}
bool Dkc2HdEnabled(void) { return s_enabled && Dkc2HdReady(); }

static bool Invalid(const char *why) {
  (void)snprintf(s_status, sizeof s_status, "HD pack rejected: %s", why);
  return false;
}

bool Dkc2HdLoadBytes(const uint8_t *bytes, size_t size) {
  if (!bytes || size < kHeaderSize || size > 256u * 1024u * 1024u ||
      memcmp(bytes, "DKC2HD01", 8) != 0)
    return Invalid("not a DKC2HD01 file");
  HdPack pack;
  memset(&pack, 0, sizeof pack);
  pack.flags = Read16(bytes + 8);
  const unsigned scale_q8 = Read16(bytes + 10);
  pack.offset_x = (float)(int16_t)Read16(bytes + 12) / 16.0f;
  pack.offset_y = (float)(int16_t)Read16(bytes + 14) / 16.0f;
  pack.frame_count = Read16(bytes + 16);
  pack.step_count = Read16(bytes + 18);
  pack.native_only_count = Read16(bytes + 20);
  if (scale_q8 < 64 || scale_q8 > 16 * 256) return Invalid("scale out of range");
  pack.scale = (float)scale_q8 / 256.0f;
  if (!pack.frame_count || pack.frame_count > kMaxFrames || !pack.step_count ||
      pack.step_count > kMaxSteps || pack.native_only_count > kMaxNativeOnly)
    return Invalid("bad table sizes");
  for (unsigned a = 0; a < kDkc2HdAnimCount; ++a) {
    const uint8_t *p = bytes + 24 + a * 8;
    pack.animations[a].first = Read16(p);
    pack.animations[a].count = Read16(p + 2);
    pack.animations[a].loop = Read16(p + 4);
    if (pack.animations[a].count &&
        (size_t)pack.animations[a].first + pack.animations[a].count > pack.step_count)
      return Invalid("animation outside the step table");
  }
  if (!pack.animations[kDkc2HdIdle].count) return Invalid("the idle animation is required");
  size_t cursor = kHeaderSize;
  const size_t tables = pack.step_count * 4 + pack.native_only_count * 2 + pack.frame_count * 12;
  if (tables > size - cursor) return Invalid("truncated tables");
  pack.steps = (HdStep *)calloc(pack.step_count, sizeof *pack.steps);
  pack.native_only = (uint16_t *)calloc(pack.native_only_count ? pack.native_only_count : 1,
                                        sizeof *pack.native_only);
  pack.frames = (HdFrame *)calloc(pack.frame_count, sizeof *pack.frames);
  pack.data = (uint8_t *)malloc(size);
  if (!pack.steps || !pack.native_only || !pack.frames || !pack.data) {
    FreePack(&pack);
    return Invalid("out of memory");
  }
  memcpy(pack.data, bytes, size);
  pack.size = size;
  for (size_t i = 0; i < pack.step_count; ++i, cursor += 4) {
    pack.steps[i].frame = Read16(bytes + cursor);
    pack.steps[i].duration = Read16(bytes + cursor + 2);
    if (pack.steps[i].frame >= pack.frame_count || !pack.steps[i].duration) {
      FreePack(&pack);
      return Invalid("bad animation step");
    }
  }
  for (size_t i = 0; i < pack.native_only_count; ++i, cursor += 2)
    pack.native_only[i] = Read16(bytes + cursor);
  for (size_t i = 0; i < pack.frame_count; ++i, cursor += 12) {
    HdFrame *f = &pack.frames[i];
    f->width = Read16(bytes + cursor);
    f->height = Read16(bytes + cursor + 2);
    f->anchor_x = (int16_t)Read16(bytes + cursor + 4);
    f->anchor_y = (int16_t)Read16(bytes + cursor + 6);
    const uint32_t offset = Read32(bytes + cursor + 8);
    const size_t bytes_needed = (size_t)f->width * f->height * 4;
    if (!f->width || !f->height || f->width > kMaxFrameSide || f->height > kMaxFrameSide ||
        offset < cursor || offset > size || bytes_needed > size - offset) {
      FreePack(&pack);
      return Invalid("frame pixels outside the file");
    }
    f->rgba = pack.data + offset;
  }
  FreePack(&s_pack);
  s_pack = pack;
  memset(s_motion, 0, sizeof s_motion);
  (void)snprintf(s_status, sizeof s_status, "HD pack loaded: %zu frames, scale %.2f",
                 pack.frame_count, pack.scale);
  return true;
}

bool Dkc2HdLoad(const char *path) {
  FILE *f = path ? fopen(path, "rb") : NULL;
  if (!f) {
    (void)snprintf(s_status, sizeof s_status, "HD pack not found: %s", path ? path : "(null)");
    return false;
  }
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return Invalid("unreadable file"); }
  const long length = ftell(f);
  if (length <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return Invalid("unreadable file"); }
  uint8_t *bytes = (uint8_t *)malloc((size_t)length);
  const bool read = bytes && fread(bytes, 1, (size_t)length, f) == (size_t)length;
  fclose(f);
  const bool ok = read && Dkc2HdLoadBytes(bytes, (size_t)length);
  free(bytes);
  if (!read) return Invalid("unreadable file");
  return ok;
}

void Dkc2HdInitialize(void) {
  const char *toggle = getenv("DKC2_HD");
  if (toggle && toggle[0] == '0') {
    s_enabled = false;
    return;
  }
  const char *path = getenv("DKC2_HD_PACK");
  if (!path || !*path) path = "mods/kong-hd.dkc2hd";
  FILE *f = fopen(path, "rb");
  if (!f) return;
  fclose(f);
  if (!Dkc2HdLoad(path)) {
    fprintf(stderr, "%s\n", s_status);
    return;
  }
  const char *offset = getenv("DKC2_HD_OFFSET");
  float dx, dy;
  if (offset && sscanf(offset, "%f,%f", &dx, &dy) == 2) {
    s_pack.offset_x += dx;
    s_pack.offset_y += dy;
  }
  const char *scale = getenv("DKC2_HD_SCALE");
  const float value = scale ? (float)atof(scale) : 0.0f;
  if (value >= 0.25f && value <= 16.0f) s_pack.scale = value;
  const char *flip = getenv("DKC2_HD_FLIP");
  if (flip && flip[0] == '1') s_pack.flags ^= kDkc2HdFlagFacesLeft;
  const char *replace = getenv("DKC2_HD_REPLACE");
  if (replace && *replace) {
    s_pack.flags &= (uint16_t)~(kDkc2HdFlagReplaceDiddy | kDkc2HdFlagReplaceDixie);
    if (strstr(replace, "diddy") || strstr(replace, "both"))
      s_pack.flags |= kDkc2HdFlagReplaceDiddy;
    if (strstr(replace, "dixie") || strstr(replace, "both"))
      s_pack.flags |= kDkc2HdFlagReplaceDixie;
  }
  fprintf(stderr, "%s\n", s_status);
}

bool Dkc2HdReplaces(unsigned who) {
  if (!Dkc2HdEnabled() || who > 1) return false;
  return (s_pack.flags & (who ? kDkc2HdFlagReplaceDixie : kDkc2HdFlagReplaceDiddy)) != 0;
}

bool Dkc2HdAllowsState(unsigned animation, unsigned state) {
  /* Knocked out, barrel, carried, thrown and rope-transition states keep
   * the native art; the pack has no matching poses. */
  switch (state) {
    case 0x05: case 0x18: case 0x21: case 0x24: case 0x25: case 0x36: case 0x3E: case 0x59:
      return false;
    default:
      break;
  }
  for (size_t i = 0; i < s_pack.native_only_count; ++i)
    if (s_pack.native_only[i] == animation) return false;
  return true;
}

int Dkc2HdClassify(Dkc2HdMotion *m, int world_x, int world_y,
                   bool grounded, bool rising, uint32_t tick) {
  int dx = 0;
  if (m->valid) {
    dx = world_x - m->last_x;
    if (dx < 0) dx = -dx;
    if (dx > 48) dx = 0; /* room change or teleport */
  } else {
    memset(m->speed_q4, 0, sizeof m->speed_q4);
    m->count = 0;
    m->animation = -1;
  }
  m->speed_q4[m->count % 4] = dx * 16;
  m->count++;
  const unsigned n = m->count < 4 ? m->count : 4;
  int sum = 0;
  for (unsigned i = 0; i < n; ++i) sum += m->speed_q4[i];
  const int speed = n ? sum / (int)n : 0;
  int animation;
  if (!grounded) animation = rising ? kDkc2HdJump : kDkc2HdFall;
  else if (speed < 6) animation = kDkc2HdIdle;
  else if (speed < 40) animation = kDkc2HdWalk;
  else animation = kDkc2HdRun;
  if (animation != m->animation) m->started = tick;
  m->animation = animation;
  m->last_x = world_x;
  m->last_y = world_y;
  m->valid = true;
  return animation;
}

static int Resolve(int animation) {
  if (s_pack.animations[animation].count) return animation;
  switch (animation) {
    case kDkc2HdRun: return Resolve(kDkc2HdWalk);
    case kDkc2HdFall: return s_pack.animations[kDkc2HdJump].count ? kDkc2HdJump : kDkc2HdIdle;
    default: return kDkc2HdIdle;
  }
}

static const HdFrame *FrameAt(int animation, uint32_t elapsed) {
  const HdAnimation *a = &s_pack.animations[animation];
  uint32_t total = 0;
  for (unsigned i = 0; i < a->count; ++i) total += s_pack.steps[a->first + i].duration;
  if (!total) return &s_pack.frames[s_pack.steps[a->first].frame];
  if (a->loop) elapsed %= total;
  else if (elapsed >= total) return &s_pack.frames[s_pack.steps[a->first + a->count - 1].frame];
  for (unsigned i = 0; i < a->count; ++i) {
    const HdStep *step = &s_pack.steps[a->first + i];
    if (elapsed < step->duration) return &s_pack.frames[step->frame];
    elapsed -= step->duration;
  }
  return &s_pack.frames[s_pack.steps[a->first + a->count - 1].frame];
}

void Dkc2HdBeginFrame(uint32_t tick) {
  for (unsigned a = 0; a < 2; ++a) {
    if (!s_actors[a].active) s_motion[a].valid = false;
    s_actors[a].active = s_actors[a].placed = false;
  }
  memset(s_owned, 0, sizeof s_owned);
  memset(s_mask, 0, sizeof s_mask);
  s_tick = tick;
}

static bool s_trace_checked, s_trace;

void Dkc2HdSetActor(unsigned who, const Dkc2HdActorInput *input) {
  if (who > 1 || !input || !Dkc2HdEnabled()) return;
  HdActor *actor = &s_actors[who];
  actor->active = true;
  actor->placed = false;
  actor->input = *input;
  if (input->slots) memcpy(actor->slots, input->slots, sizeof actor->slots);
  else memset(actor->slots, 0, sizeof actor->slots);
  for (unsigned i = 0; i < 128; ++i)
    if (actor->slots[i]) s_owned[i] = 1;
  const int previous = s_motion[who].animation;
  const int animation = Dkc2HdClassify(&s_motion[who], input->world_x, input->world_y,
                                       input->grounded, input->rising, s_tick);
  const int resolved = Resolve(animation);
  uint32_t elapsed = s_tick - s_motion[who].started;
  if (resolved == kDkc2HdWalk && animation == kDkc2HdRun)
    elapsed = elapsed * 8 / 5; /* no run clip: a faster walk */
  if (resolved == kDkc2HdJump && animation == kDkc2HdFall)
    elapsed = UINT32_MAX / 2; /* no fall clip: hold the jump's last pose */
  actor->frame = FrameAt(resolved, elapsed);
  const unsigned priority = SPRITE_PRIO_TO_PRIO((input->properties >> 12) & 3,
                                                (input->properties & 0x800) == 0);
  actor->level = (priority << 8) >> 12;
  if (!s_trace_checked) {
    s_trace_checked = true;
    s_trace = getenv("DKC2_HD_TRACE") != NULL;
  }
  if (s_trace && previous != s_motion[who].animation)
    fprintf(stderr, "hd who=%u class=%d grounded=%d rising=%d x=%d y=%d\n", who,
            s_motion[who].animation, input->grounded, input->rising, input->world_x, input->world_y);
}

bool Dkc2HdOwnsSlot(unsigned slot) {
  return slot < 128 && s_owned[slot] != 0;
}

static void Place(HdActor *actor, const Ppu *ppu) {
  if (actor->placed || !actor->frame) return;
  const HdFrame *f = actor->frame;
  const float scale = s_pack.scale;
  const bool art_left = (s_pack.flags & kDkc2HdFlagFacesLeft) != 0;
  const bool native_flip = (actor->input.properties & 0x4000) != 0;
  actor->flip = native_flip != art_left;
  const float origin_x = (float)(actor->input.screen_x - ppu->wsPresentationXBias +
                                 (int)ppu->extraLeftCur) + s_pack.offset_x;
  const float origin_y = (float)(actor->input.screen_y - 1) + s_pack.offset_y;
  actor->width = (float)f->width / scale;
  actor->height = (float)f->height / scale;
  actor->left = actor->flip ? origin_x - (float)(f->width - f->anchor_x) / scale
                            : origin_x - (float)f->anchor_x / scale;
  actor->top = origin_y - (float)f->anchor_y / scale;
  s_out_width = kPpuXPixels + (int)ppu->extraLeftCur + (int)ppu->extraRightCur;
  if (s_out_width > kPpuBufWidth) s_out_width = kPpuBufWidth;
  actor->placed = true;
}

void Dkc2HdObserveMainLine(const Ppu *ppu, unsigned line) {
  if (!ppu || line < 1 || line > kMaskRows) return;
  const int row = (int)line - 1;
  for (unsigned a = 0; a < 2; ++a) {
    HdActor *actor = &s_actors[a];
    if (!actor->active) continue;
    Place(actor, ppu);
    if (row < (int)floorf(actor->top) || row > (int)ceilf(actor->top + actor->height)) continue;
    int x0 = (int)floorf(actor->left), x1 = (int)ceilf(actor->left + actor->width);
    if (x0 < 0) x0 = 0;
    if (x1 > s_out_width) x1 = s_out_width;
    for (int x = x0; x < x1; ++x) {
      const int index = x - (int)ppu->extraLeftCur + kPpuExtraLeftRight;
      if (index < 0 || index >= kPpuBufWidth) continue;
      const unsigned level = ppu->bgBuffers[0].data[index] >> 12;
      if (level > actor->level) s_mask[row][x] |= (uint8_t)(1u << a);
    }
  }
}

void Dkc2HdFinishFrame(const Ppu *ppu) {
  s_sprite_count = 0;
  if (!ppu || (ppu->inidisp & 0x80)) return;
  const float brightness = (float)(ppu->inidisp & 15) / 15.0f;
  ++s_serial;
  for (unsigned a = 0; a < 2; ++a) {
    HdActor *actor = &s_actors[a];
    if (!actor->active || !actor->frame) continue;
    Place(actor, ppu);
    const HdFrame *f = actor->frame;
    const size_t needed = (size_t)f->width * f->height * 4;
    if (s_out_capacity[a] < needed) {
      uint8_t *grown = (uint8_t *)realloc(s_out[a], needed);
      if (!grown) continue;
      s_out[a] = grown;
      s_out_capacity[a] = needed;
    }
    const float inv = 1.0f / s_pack.scale;
    for (unsigned v = 0; v < f->height; ++v) {
      const int ny = (int)floorf(actor->top + ((float)v + 0.5f) * inv);
      const bool row_ok = ny >= 0 && ny < 224;
      uint8_t *dst = s_out[a] + (size_t)v * f->width * 4;
      for (unsigned u = 0; u < f->width; ++u, dst += 4) {
        const unsigned su = actor->flip ? f->width - 1 - u : u;
        const uint8_t *src = f->rgba + ((size_t)v * f->width + su) * 4;
        const int nx = (int)floorf(actor->left + ((float)u + 0.5f) * inv);
        const bool visible = src[3] && row_ok && nx >= 0 && nx < s_out_width &&
                             !(s_mask[ny][nx] & (1u << a));
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = visible ? src[3] : 0;
      }
    }
    Dkc2OverlaySprite *sprite = &s_sprites[s_sprite_count++];
    sprite->rgba = s_out[a];
    sprite->width = f->width;
    sprite->height = f->height;
    sprite->x = actor->left;
    sprite->y = actor->top;
    sprite->w = actor->width;
    sprite->h = actor->height;
    sprite->brightness = brightness;
    sprite->serial = s_serial * 2 + a;
  }
}

int Dkc2HdSprites(const Dkc2OverlaySprite **sprites) {
  if (sprites) *sprites = s_sprite_count ? s_sprites : NULL;
  return s_sprite_count;
}

unsigned Dkc2HdActiveActors(void) {
  return (unsigned)s_actors[0].active + (unsigned)s_actors[1].active;
}

int Dkc2HdActorAnimation(unsigned who) {
  return who < 2 && s_actors[who].active ? s_motion[who].animation : -1;
}
