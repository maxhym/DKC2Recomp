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
  kMaxMap = 512,
  kFidgetDelay = 300, /* ticks standing still before a fidget */
  kFadeTicks = 9,     /* cross-fade between two poses, about 150 ms */
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
  uint16_t (*map)[2]; /* native animation ID -> slot */
  size_t map_count;
} HdPack;

/* What is shown per Kong slot, beyond the motion classifier. */
typedef struct HdShow {
  int animation;
  uint32_t since, idle_since;
  unsigned native;
  unsigned fidget_turn; /* next fidget slot to try */
  /* Cross-fade: the frame shown last tick, and the one being faded out. */
  const struct HdFrame *last_frame, *fade_from;
  uint32_t fade_start;
  /* Upright to all fours (+1) or back (-1), while its clip plays. */
  int turn;
  uint32_t turn_start;
  bool valid;
} HdShow;
static HdShow s_show[2];

typedef struct HdActor {
  bool active, placed;
  Dkc2HdActorInput input;
  uint8_t slots[128];
  const HdFrame *frame;
  const HdFrame *fade_frame; /* previous pose while cross-fading, or NULL */
  float fade;                /* weight of `frame`, 0..1 */
  bool flip;
  unsigned level;
  int rx, ry, rw, rh;        /* drawn rectangle in HD pixels, origin-relative */
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
static uint8_t *s_tmp;
static size_t s_tmp_capacity;
/* Presentation style and size, chosen by the player (launcher). */
static int s_style = kDkc2HdStyleHd;
static float s_size = 1.0f;

static float EffectiveScale(void) { return s_pack.scale / s_size; }
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
  free(pack->map);
  memset(pack, 0, sizeof *pack);
}

void Dkc2HdUnload(void) {
  FreePack(&s_pack);
  memset(s_actors, 0, sizeof s_actors);
  memset(s_show, 0, sizeof s_show);
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
  const bool v2 = bytes && size >= 32 && memcmp(bytes, "DKC2HD02", 8) == 0;
  if (!bytes || size < 32 || size > 256u * 1024u * 1024u ||
      (!v2 && (size < kHeaderSize || memcmp(bytes, "DKC2HD01", 8) != 0)))
    return Invalid("not a DKC2HD01/02 file");
  /* v1: five animation records at 24, tables at 64. v2: animation and map
   * counts at 22/24, records at 32, then steps, native-only list, map. */
  const unsigned anim_count = v2 ? Read16(bytes + 22) : kDkc2HdBasicAnimCount;
  const size_t map_count = v2 ? Read16(bytes + 24) : 0;
  const size_t records = v2 ? 32 : 24;
  if (!anim_count || anim_count > kDkc2HdAnimCount || map_count > kMaxMap ||
      records + (size_t)anim_count * 8 > size)
    return Invalid("bad animation table");
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
  for (unsigned a = 0; a < anim_count; ++a) {
    const uint8_t *p = bytes + records + a * 8;
    pack.animations[a].first = Read16(p);
    pack.animations[a].count = Read16(p + 2);
    pack.animations[a].loop = Read16(p + 4);
    if (pack.animations[a].count &&
        (size_t)pack.animations[a].first + pack.animations[a].count > pack.step_count)
      return Invalid("animation outside the step table");
  }
  if (!pack.animations[kDkc2HdIdle].count) return Invalid("the idle animation is required");
  size_t cursor = v2 ? records + (size_t)anim_count * 8 : kHeaderSize;
  pack.map_count = map_count;
  const size_t tables = pack.step_count * 4 + pack.native_only_count * 2 + map_count * 4 +
                        pack.frame_count * 12;
  if (tables > size - cursor) return Invalid("truncated tables");
  pack.steps = (HdStep *)calloc(pack.step_count, sizeof *pack.steps);
  pack.native_only = (uint16_t *)calloc(pack.native_only_count ? pack.native_only_count : 1,
                                        sizeof *pack.native_only);
  pack.frames = (HdFrame *)calloc(pack.frame_count, sizeof *pack.frames);
  pack.map = (uint16_t (*)[2])calloc(map_count ? map_count : 1, sizeof *pack.map);
  pack.data = (uint8_t *)malloc(size);
  if (!pack.steps || !pack.native_only || !pack.frames || !pack.map || !pack.data) {
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
  for (size_t i = 0; i < map_count; ++i, cursor += 4) {
    pack.map[i][0] = Read16(bytes + cursor);
    pack.map[i][1] = Read16(bytes + cursor + 2);
    if (pack.map[i][1] >= kDkc2HdAnimCount) {
      FreePack(&pack);
      return Invalid("map names an unknown animation slot");
    }
  }
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
  memset(s_show, 0, sizeof s_show);
  unsigned slots = 0;
  for (unsigned a = 0; a < kDkc2HdAnimCount; ++a) slots += s_pack.animations[a].count != 0;
  (void)snprintf(s_status, sizeof s_status,
                 "HD pack loaded: %zu frames, %u animations, scale %.2f",
                 pack.frame_count, slots, pack.scale);
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
  const char *style = getenv("DKC2_HD_STYLE");
  if (style && *style) Dkc2HdSetStyle(Dkc2HdStyleFromName(style));
  const char *size = getenv("DKC2_HD_SIZE");
  if (size && *size) Dkc2HdSetSize((float)atof(size) / 100.0f);
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

static int MappedSlot(unsigned animation) {
  for (size_t i = 0; i < s_pack.map_count; ++i)
    if (s_pack.map[i][0] == animation && s_pack.animations[s_pack.map[i][1]].count)
      return s_pack.map[i][1];
  return -1;
}

static bool IsHurtState(unsigned state) {
  return state == 0x24 || state == 0x25 || state == 0x59;
}

bool Dkc2HdHasRide(void) {
  return Dkc2HdEnabled() && s_pack.animations[kDkc2HdRide].count != 0;
}

bool Dkc2HdHasCarry(void) {
  return Dkc2HdEnabled() && (s_pack.animations[kDkc2HdCarryIdle].count ||
                             s_pack.animations[kDkc2HdCarryWalk].count ||
                             s_pack.animations[kDkc2HdCarryFrontIdle].count);
}

bool Dkc2HdAllowsState(unsigned animation, unsigned state) {
  /* A pose the pack maps or a hurt pose it carries wins over the defaults. */
  if (MappedSlot(animation) >= 0) return true;
  if (IsHurtState(state) && s_pack.animations[kDkc2HdHurt].count) return true;
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
  /* A run only drops back to a walk well below the speed that started it,
   * so a speed hovering at the threshold does not flicker between both. */
  else if (speed < (m->animation == kDkc2HdRun ? 30 : 40)) animation = kDkc2HdWalk;
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
    case kDkc2HdCarryWalk: return Resolve(kDkc2HdCarryIdle);
    case kDkc2HdClimbMove: return Resolve(kDkc2HdClimbIdle);
    case kDkc2HdClimbDown: return Resolve(kDkc2HdClimbMove);
    case kDkc2HdHang: return Resolve(kDkc2HdClimbIdle);
    case kDkc2HdCarryFrontWalk: return Resolve(kDkc2HdCarryFrontIdle);
    case kDkc2HdCarryFrontIdle: return Resolve(kDkc2HdCarryIdle);
    case kDkc2HdThrowFront: return Resolve(kDkc2HdThrow);
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

static uint32_t Duration(int animation) {
  const HdAnimation *a = &s_pack.animations[animation];
  uint32_t total = 0;
  for (unsigned i = 0; i < a->count; ++i) total += s_pack.steps[a->first + i].duration;
  return total;
}

static bool IsFidget(int animation) {
  return animation == kDkc2HdFidget || animation == kDkc2HdFidget2 ||
         animation == kDkc2HdFidget3;
}

static bool IsUpright(int animation) {
  return animation == kDkc2HdIdle || IsFidget(animation);
}

static bool IsOnFours(int animation) {
  return animation == kDkc2HdWalk || animation == kDkc2HdRun;
}

/* The fidget slots the pack carries take turns. */
static int NextFidget(HdShow *show) {
  static const int kFidgets[3] = {kDkc2HdFidget, kDkc2HdFidget2, kDkc2HdFidget3};
  for (unsigned i = 0; i < 3; ++i) {
    const unsigned turn = (show->fidget_turn + i) % 3;
    if (s_pack.animations[kFidgets[turn]].count) {
      show->fidget_turn = (turn + 1) % 3;
      return kFidgets[turn];
    }
  }
  return -1;
}

void Dkc2HdBeginFrame(uint32_t tick) {
  for (unsigned a = 0; a < 2; ++a) {
    if (!s_actors[a].active) s_motion[a].valid = s_show[a].valid = false;
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
  HdShow *show = &s_show[who];
  const int previous = show->valid ? show->animation : -1;
  const unsigned previous_native = show->native;
  const int moving = Dkc2HdClassify(&s_motion[who], input->world_x, input->world_y,
                                    input->grounded, input->rising, s_tick);
  if (!show->valid) {
    show->valid = true;
    show->animation = -1;
    show->idle_since = s_tick;
    show->last_frame = show->fade_from = NULL;
  }
  /* A finished fidget restarts the wait before the next one. */
  if (IsFidget(show->animation) && s_tick - show->since >= Duration(show->animation))
    show->idle_since = s_tick;
  /* Priority: riding, the pack's map for this native animation, a hurt
   * pose, the carry poses, a fidget after standing still, then motion. */
  int animation = input->riding && Dkc2HdHasRide() ? kDkc2HdRide : -1;
  if (animation < 0) animation = MappedSlot(input->animation);
  if (animation < 0 && IsHurtState(input->state) && s_pack.animations[kDkc2HdHurt].count)
    animation = kDkc2HdHurt;
  if (animation < 0 && input->carrying && Dkc2HdHasCarry())
    /* Diddy and Dixie hold objects in front of them. */
    animation = moving == kDkc2HdWalk || moving == kDkc2HdRun ? kDkc2HdCarryFrontWalk
                                                              : kDkc2HdCarryFrontIdle;
  if (animation < 0 && moving == kDkc2HdIdle) {
    if (IsFidget(show->animation) && s_tick - show->since < Duration(show->animation))
      animation = show->animation;
    else if (s_tick - show->idle_since >= kFidgetDelay)
      animation = NextFidget(show);
  }
  if (animation < 0) animation = moving;
  if (animation != kDkc2HdIdle && !IsFidget(animation)) show->idle_since = s_tick;
  if (animation != show->animation) {
    /* Motion classes keep the classifier's clock; other slots start now. */
    const bool both_motion = animation < kDkc2HdBasicAnimCount &&
                             show->animation >= 0 && show->animation < kDkc2HdBasicAnimCount;
    const bool strides = (animation == kDkc2HdWalk || animation == kDkc2HdRun) &&
                         (show->animation == kDkc2HdWalk || show->animation == kDkc2HdRun);
    const uint32_t from = strides ? Duration(Resolve(show->animation)) : 0;
    const uint32_t to = strides ? Duration(Resolve(animation)) : 0;
    if (from && to && Resolve(show->animation) != Resolve(animation)) {
      /* Walk and run keep their place in the stride across the change. */
      const uint32_t phase = (s_tick - show->since) % from;
      show->since = s_tick - (uint32_t)((uint64_t)phase * to / from);
    } else {
      show->since = both_motion ? s_motion[who].started : s_tick;
    }
    if (previous >= 0 && show->last_frame) {
      show->fade_from = show->last_frame;
      show->fade_start = s_tick;
    }
    /* Between a standing pose and the four-legged gaits, the body goes
     * down or up through its own clip instead of a plain cross-fade. A
     * change of mind part-way continues from the same point, backwards. */
    const int turn = IsUpright(show->animation) && IsOnFours(animation) ? 1 :
                     IsOnFours(show->animation) && IsUpright(animation) ? -1 : 0;
    const uint32_t turn_length = Duration(kDkc2HdStandToFours);
    if (turn && turn_length && previous >= 0) {
      const uint32_t done = show->turn ? s_tick - show->turn_start : turn_length;
      show->turn_start = done < turn_length ? s_tick - (turn_length - 1 - done) : s_tick;
      show->turn = turn;
    } else if (!IsFidget(animation) || !IsUpright(show->animation)) {
      show->turn = 0;
    }
    show->animation = animation;
  }
  show->native = input->animation;
  const int resolved = Resolve(animation);
  uint32_t elapsed = s_tick - show->since;
  if (resolved == kDkc2HdWalk && animation == kDkc2HdRun)
    elapsed = elapsed * 8 / 5; /* no run clip: a faster walk */
  if (resolved == kDkc2HdJump && animation == kDkc2HdFall)
    elapsed = UINT32_MAX / 2; /* no fall clip: hold the jump's last pose */
  actor->frame = FrameAt(resolved, elapsed);
  if (show->turn) {
    const uint32_t length = Duration(kDkc2HdStandToFours);
    const uint32_t done = s_tick - show->turn_start;
    if (done < length) {
      actor->frame = FrameAt(kDkc2HdStandToFours, show->turn > 0 ? done : length - 1 - done);
      show->since = s_tick; /* the gait or the rest starts when it ends */
    } else {
      show->turn = 0;
      if (show->last_frame) {
        show->fade_from = show->last_frame;
        show->fade_start = s_tick;
      }
    }
  }
  actor->fade_frame = NULL;
  actor->fade = 1.0f;
  if (show->fade_from) {
    const uint32_t age = s_tick - show->fade_start;
    if (age < kFadeTicks && show->fade_from != actor->frame) {
      actor->fade_frame = show->fade_from;
      /* Eased, so the change starts and ends gently. */
      const float t = (float)(age + 1) / (float)(kFadeTicks + 1);
      actor->fade = t * t * (3.0f - 2.0f * t);
    } else {
      show->fade_from = NULL;
    }
  }
  show->last_frame = actor->frame;
  const unsigned priority = SPRITE_PRIO_TO_PRIO((input->properties >> 12) & 3,
                                                (input->properties & 0x800) == 0);
  actor->level = (priority << 8) >> 12;
  if (!s_trace_checked) {
    s_trace_checked = true;
    s_trace = getenv("DKC2_HD_TRACE") != NULL;
  }
  /* The trace also lists native animation IDs, to grow the pack's map. */
  if (s_trace && (previous != show->animation || previous_native != input->animation))
    fprintf(stderr, "hd who=%u slot=%d native=0x%02X state=0x%02X carrying=%d grounded=%d rising=%d x=%d y=%d\n",
            who, show->animation, input->animation, input->state, input->carrying,
            input->grounded, input->rising, input->world_x, input->world_y);
}

bool Dkc2HdOwnsSlot(unsigned slot) {
  return slot < 128 && s_owned[slot] != 0;
}

/* A frame's extent in HD pixels relative to the actor's origin. */
static void FrameRect(const HdFrame *f, bool flip, int *x0, int *y0, int *x1, int *y1) {
  *x0 = flip ? -((int)f->width - f->anchor_x) : -f->anchor_x;
  *x1 = *x0 + (int)f->width;
  *y0 = -f->anchor_y;
  *y1 = *y0 + (int)f->height;
}

/* The frame's pixel at an origin-relative HD position, or NULL outside it. */
static const uint8_t *FramePixel(const HdFrame *f, bool flip, int x, int y) {
  const int su = flip ? f->anchor_x - 1 - x : x + f->anchor_x;
  const int sv = y + f->anchor_y;
  if (su < 0 || sv < 0 || su >= (int)f->width || sv >= (int)f->height) return NULL;
  return f->rgba + ((size_t)sv * f->width + (size_t)su) * 4;
}

static void Place(HdActor *actor, const Ppu *ppu) {
  if (actor->placed || !actor->frame) return;
  const HdFrame *f = actor->frame;
  const float scale = EffectiveScale();
  const bool art_left = (s_pack.flags & kDkc2HdFlagFacesLeft) != 0;
  const bool native_flip = (actor->input.properties & 0x4000) != 0;
  actor->flip = native_flip != art_left;
  const float origin_x = (float)(actor->input.screen_x - ppu->wsPresentationXBias +
                                 (int)ppu->extraLeftCur) + s_pack.offset_x;
  const float origin_y = (float)(actor->input.screen_y - 1) + s_pack.offset_y;
  /* The drawn rectangle covers the pose and, while cross-fading, the one
   * it replaces; both are positioned by their own anchor. */
  int x0, y0, x1, y1;
  FrameRect(f, actor->flip, &x0, &y0, &x1, &y1);
  if (actor->fade_frame) {
    int px0, py0, px1, py1;
    FrameRect(actor->fade_frame, actor->flip, &px0, &py0, &px1, &py1);
    if (px0 < x0) x0 = px0;
    if (py0 < y0) y0 = py0;
    if (px1 > x1) x1 = px1;
    if (py1 > y1) y1 = py1;
  }
  actor->rx = x0;
  actor->ry = y0;
  actor->rw = x1 - x0;
  actor->rh = y1 - y0;
  actor->width = (float)actor->rw / scale;
  actor->height = (float)actor->rh / scale;
  actor->left = origin_x + (float)x0 / scale;
  actor->top = origin_y + (float)y0 / scale;
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
      /* Only backgrounds cover the Kong: between sprites the OAM order
       * decides, not the priority bits, and he is drawn over them (an
       * animal buddy carries higher bits than its rider). Sprite pixels
       * use the upper half of the palette. */
      const PpuZbufType pixel = ppu->bgBuffers[0].data[index];
      if ((unsigned)(pixel >> 12) > actor->level && (pixel & 0x80) == 0)
        s_mask[row][x] |= (uint8_t)(1u << a);
    }
  }
}

/* ---- Presentation styles: run on the composited frame, never on the pack. */

static bool Scratch(size_t bytes) {
  if (s_tmp_capacity >= bytes) return true;
  uint8_t *grown = (uint8_t *)realloc(s_tmp, bytes);
  if (!grown) return false;
  s_tmp = grown;
  s_tmp_capacity = bytes;
  return true;
}

/* 1-2-1 blur of premultiplied colour, horizontally then vertically. */
static void Soften(uint8_t *px, unsigned w, unsigned h) {
  if (!Scratch((size_t)w * h * 4)) return;
  for (int pass = 0; pass < 2; ++pass) {
    const uint8_t *src = px;
    uint8_t *dst = s_tmp;
    for (unsigned y = 0; y < h; ++y) {
      for (unsigned x = 0; x < w; ++x) {
        unsigned acc[4] = {0, 0, 0, 0}, total = 0;
        for (int d = -1; d <= 1; ++d) {
          const int sx = pass == 0 ? (int)x + d : (int)x;
          const int sy = pass == 0 ? (int)y : (int)y + d;
          if (sx < 0 || sy < 0 || sx >= (int)w || sy >= (int)h) continue;
          const uint8_t *p = src + ((size_t)sy * w + (size_t)sx) * 4;
          const unsigned weight = d == 0 ? 2 : 1;
          for (int c = 0; c < 3; ++c) acc[c] += (unsigned)p[c] * p[3] / 255 * weight;
          acc[3] += (unsigned)p[3] * weight;
          total += weight;
        }
        uint8_t *o = dst + ((size_t)y * w + x) * 4;
        const unsigned alpha = acc[3] / total;
        for (int c = 0; c < 3; ++c) {
          const unsigned premul = acc[c] / total;
          o[c] = (uint8_t)(alpha ? (premul * 255 / alpha > 255 ? 255 : premul * 255 / alpha) : 0);
        }
        o[3] = (uint8_t)alpha;
      }
    }
    memcpy(px, s_tmp, (size_t)w * h * 4);
  }
}

static void Saturate(uint8_t *px, size_t count, float amount) {
  for (size_t i = 0; i < count; ++i, px += 4) {
    const float grey = 0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2];
    for (int c = 0; c < 3; ++c) {
      float v = grey + (px[c] - grey) * amount;
      px[c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
  }
}

/* SNES look: blocks of `block` HD pixels, 15-bit colour, hard alpha and a
 * one-block dark outline. */
static void Retro(uint8_t *px, unsigned w, unsigned h, unsigned block) {
  if (block < 1) block = 1;
  const unsigned bw = (w + block - 1) / block, bh = (h + block - 1) / block;
  if (!Scratch((size_t)bw * bh * 4)) return;
  uint8_t *cells = s_tmp;
  for (unsigned by = 0; by < bh; ++by) {
    for (unsigned bx = 0; bx < bw; ++bx) {
      unsigned acc[4] = {0, 0, 0, 0}, n = 0;
      for (unsigned y = by * block; y < (by + 1) * block && y < h; ++y)
        for (unsigned x = bx * block; x < (bx + 1) * block && x < w; ++x) {
          const uint8_t *p = px + ((size_t)y * w + x) * 4;
          for (int c = 0; c < 3; ++c) acc[c] += (unsigned)p[c] * p[3];
          acc[3] += p[3];
          ++n;
        }
      uint8_t *cell = cells + ((size_t)by * bw + bx) * 4;
      const bool solid = n && acc[3] / n >= 128;
      for (int c = 0; c < 3; ++c) {
        const unsigned v = acc[3] ? acc[c] / acc[3] : 0;
        cell[c] = (uint8_t)((v & 0xF8) | (v >> 5)); /* 5 bits per channel */
      }
      cell[3] = solid ? 255 : 0;
    }
  }
  /* Outline: an empty cell touching a solid one becomes dark brown. */
  for (unsigned by = 0; by < bh; ++by)
    for (unsigned bx = 0; bx < bw; ++bx) {
      uint8_t *cell = cells + ((size_t)by * bw + bx) * 4;
      if (cell[3] == 255) continue;
      static const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (int k = 0; k < 4; ++k) {
        const int nx = (int)bx + d[k][0], ny = (int)by + d[k][1];
        if (nx < 0 || ny < 0 || nx >= (int)bw || ny >= (int)bh) continue;
        const uint8_t *nb = cells + ((size_t)ny * bw + (size_t)nx) * 4;
        if (nb[3] == 255) {
          cell[0] = 24; cell[1] = 16; cell[2] = 8; cell[3] = 254; /* not a neighbour source */
          break;
        }
      }
    }
  for (unsigned y = 0; y < h; ++y)
    for (unsigned x = 0; x < w; ++x) {
      uint8_t *p = px + ((size_t)y * w + x) * 4;
      const uint8_t *cell = cells + ((size_t)(y / block) * bw + x / block) * 4;
      memcpy(p, cell, 4);
      if (p[3] == 254) p[3] = 255;
    }
}

/* Darkens the HD rows that fall between the native scanlines, so the
 * character shares the CRT pass's line structure. */
static void Scanlines(uint8_t *px, unsigned w, unsigned h, float top, float scale) {
  const float inv = 1.0f / scale;
  for (unsigned y = 0; y < h; ++y) {
    const float ny = top + ((float)y + 0.5f) * inv;
    const float frac = ny - floorf(ny);
    if (frac < 0.55f) continue;
    uint8_t *p = px + (size_t)y * w * 4;
    for (unsigned x = 0; x < w; ++x, p += 4)
      for (int c = 0; c < 3; ++c) p[c] = (uint8_t)(p[c] * 0.68f);
  }
}

static void ApplyStyle(uint8_t *px, unsigned w, unsigned h, float top, float scale) {
  switch (s_style) {
    case kDkc2HdStyleSoft:
      Soften(px, w, h);
      Saturate(px, (size_t)w * h, 0.9f);
      break;
    case kDkc2HdStyleRetro:
      Retro(px, w, h, (unsigned)(scale / 2.0f + 0.5f));
      break;
    case kDkc2HdStyleCrt:
      Soften(px, w, h);
      Scanlines(px, w, h, top, scale);
      break;
    default:
      break;
  }
}

void Dkc2HdSetStyle(int style) {
  s_style = style >= kDkc2HdStyleHd && style < kDkc2HdStyleCount ? style : kDkc2HdStyleHd;
}
int Dkc2HdStyle(void) { return s_style; }
void Dkc2HdSetSize(float size) { s_size = size >= 0.5f && size <= 2.0f ? size : 1.0f; }
float Dkc2HdSize(void) { return s_size; }

int Dkc2HdStyleFromName(const char *name) {
  static const char *const names[kDkc2HdStyleCount] = {"hd", "doux", "retro", "crt"};
  for (int i = 0; name && i < kDkc2HdStyleCount; ++i)
    if (strcmp(name, names[i]) == 0) return i;
  return kDkc2HdStyleHd;
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
    const HdFrame *g = actor->fade_frame;
    const unsigned w = (unsigned)actor->rw, h = (unsigned)actor->rh;
    const size_t needed = (size_t)w * h * 4;
    if (s_out_capacity[a] < needed) {
      uint8_t *grown = (uint8_t *)realloc(s_out[a], needed);
      if (!grown) continue;
      s_out[a] = grown;
      s_out_capacity[a] = needed;
    }
    const float inv = 1.0f / EffectiveScale();
    const float keep = actor->fade, drop = 1.0f - actor->fade;
    for (unsigned v = 0; v < h; ++v) {
      const int ny = (int)floorf(actor->top + ((float)v + 0.5f) * inv);
      const bool row_ok = ny >= 0 && ny < 224;
      uint8_t *dst = s_out[a] + (size_t)v * w * 4;
      for (unsigned u = 0; u < w; ++u, dst += 4) {
        const int x = actor->rx + (int)u, y = actor->ry + (int)v;
        const uint8_t *src = FramePixel(f, actor->flip, x, y);
        const int nx = (int)floorf(actor->left + ((float)u + 0.5f) * inv);
        const bool visible = row_ok && nx >= 0 && nx < s_out_width &&
                             !(s_mask[ny][nx] & (1u << a));
        if (!g) {
          dst[0] = src ? src[0] : 0;
          dst[1] = src ? src[1] : 0;
          dst[2] = src ? src[2] : 0;
          dst[3] = src && visible ? src[3] : 0;
          continue;
        }
        /* Cross-fade in premultiplied space so edges do not darken. */
        const uint8_t *old = FramePixel(g, actor->flip, x, y);
        const float a_new = src ? (float)src[3] * keep : 0.0f;
        const float a_old = old ? (float)old[3] * drop : 0.0f;
        const float alpha = a_new + a_old;
        if (alpha < 0.5f || !visible) {
          dst[0] = dst[1] = dst[2] = dst[3] = 0;
          continue;
        }
        for (unsigned c = 0; c < 3; ++c)
          dst[c] = (uint8_t)(((src ? (float)src[c] * a_new : 0.0f) +
                              (old ? (float)old[c] * a_old : 0.0f)) / alpha + 0.5f);
        dst[3] = (uint8_t)(alpha + 0.5f);
      }
    }
    ApplyStyle(s_out[a], w, h, actor->top, EffectiveScale());
    Dkc2OverlaySprite *sprite = &s_sprites[s_sprite_count++];
    sprite->rgba = s_out[a];
    sprite->width = w;
    sprite->height = h;
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
  return who < 2 && s_actors[who].active ? s_show[who].animation : -1;
}
