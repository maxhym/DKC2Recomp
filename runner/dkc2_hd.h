#ifndef DKC2_HD_H
#define DKC2_HD_H

/* High-resolution replacement Kongs (presentation only).
 *
 * An HD pack (`.dkc2hd`, built by scripts/build_hd_kong_pack.py) holds
 * straight-alpha RGBA frames grouped into idle/walk/run/jump/fall
 * animations. When a pack is loaded, the replaced Kong's native OAM pieces
 * are hidden from the PPU, the PPU records per pixel whether a higher
 * priority layer covers the Kong, and the host draws the HD frame at window
 * resolution with those covered pixels cut out. Native WRAM, VRAM, OAM,
 * collision and timing are never modified. States the pack does not cover
 * (riding, hurt, carried, team-up, barrels...) fall back to the native
 * sprite for that frame. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dkc2_sprite_overlay.h"

typedef struct Ppu Ppu;

#ifdef __cplusplus
extern "C" {
#endif

enum {
  kDkc2HdIdle = 0,
  kDkc2HdWalk = 1,
  kDkc2HdRun = 2,
  kDkc2HdJump = 3,
  kDkc2HdFall = 4,
  kDkc2HdAnimCount = 5,
};

/* Presentation styles, applied to each composited frame at run time. */
enum {
  kDkc2HdStyleHd = 0,    /* the pack's frames as drawn */
  kDkc2HdStyleSoft = 1,  /* softened edges, slightly muted colours */
  kDkc2HdStyleRetro = 2, /* 15-bit colour, coarser pixels, dark outline */
  kDkc2HdStyleCrt = 3,   /* softened and darkened between native scanlines */
  kDkc2HdStyleCount = 4,
};
void Dkc2HdSetStyle(int style);
int Dkc2HdStyle(void);
int Dkc2HdStyleFromName(const char *name);
/* Size relative to the pack (0.5 to 2.0, 1.0 by default). */
void Dkc2HdSetSize(float size);
float Dkc2HdSize(void);

enum {
  kDkc2HdFlagFacesLeft = 1,
  kDkc2HdFlagReplaceDiddy = 2,
  kDkc2HdFlagReplaceDixie = 4,
};

bool Dkc2HdLoad(const char *path);
bool Dkc2HdLoadBytes(const uint8_t *bytes, size_t size);
void Dkc2HdUnload(void);
bool Dkc2HdReady(void);
const char *Dkc2HdStatus(void);
/* Loads DKC2_HD_PACK or mods/kong-hd.dkc2hd; DKC2_HD=0 disables HD.
 * Tuning without rebuilding the pack: DKC2_HD_OFFSET="dx,dy" (native px),
 * DKC2_HD_SCALE (HD px per native px), DKC2_HD_FLIP=1 (mirror the facing),
 * DKC2_HD_REPLACE=diddy|dixie|both, DKC2_HD_TRACE=1 (log animation changes),
 * DKC2_HD_STYLE=hd|doux|retro|crt (presentation style) and DKC2_HD_SIZE=percent
 * (50-200, character size relative to the pack's). */
void Dkc2HdInitialize(void);
void Dkc2HdSetEnabled(bool enabled);
/* Flips HD on/off (bound to F8 in the desktop hosts); returns the new state. */
bool Dkc2HdToggle(void);
bool Dkc2HdEnabled(void);

/* who: 0 = Diddy's slot (type $E4), 1 = Dixie's slot (type $E8). */
bool Dkc2HdReplaces(unsigned who);
/* False for native animation IDs and sprite states the pack leaves native. */
bool Dkc2HdAllowsState(unsigned animation, unsigned state);

/* Motion classifier, exposed for tests. */
typedef struct Dkc2HdMotion {
  int last_x, last_y;
  int speed_q4[4];
  unsigned count;
  int animation;
  uint32_t started;
  bool valid;
} Dkc2HdMotion;
int Dkc2HdClassify(Dkc2HdMotion *motion, int world_x, int world_y,
                   bool grounded, bool rising, uint32_t tick);

typedef struct Dkc2HdActorInput {
  int screen_x, screen_y;   /* compound-sprite origin in authentic screen px */
  int world_x, world_y;
  uint16_t properties;      /* OAM attribute word: flip and priority */
  bool grounded, rising;
  const uint8_t *slots;     /* 128 flags: OAM slots of the native Kong */
} Dkc2HdActorInput;

/* Per-frame protocol, driven by dkc2_kongs.c and the PPU line hook. */
void Dkc2HdBeginFrame(uint32_t tick);
void Dkc2HdSetActor(unsigned who, const Dkc2HdActorInput *input);
bool Dkc2HdOwnsSlot(unsigned slot);
void Dkc2HdObserveMainLine(const Ppu *ppu, unsigned line);
void Dkc2HdFinishFrame(const Ppu *ppu);

/* Sprites for the presenter (matches Dkc2OverlaySpriteSource). */
int Dkc2HdSprites(const Dkc2OverlaySprite **sprites);

/* Test helpers. */
unsigned Dkc2HdActiveActors(void);
int Dkc2HdActorAnimation(unsigned who);

#ifdef __cplusplus
}
#endif

#endif
