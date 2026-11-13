#ifndef DKC2_SPRITE_OVERLAY_H
#define DKC2_SPRITE_OVERLAY_H

/* Host-resolution sprites drawn by the GL pipeline after the game image.
 * The type is shared by the producer (dkc2_hd.c) and the presenter
 * (desktop_present_sdl.c) without either including the other. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Dkc2OverlaySprite {
  const uint8_t *rgba;  /* straight-alpha RGBA8, width * height * 4 bytes */
  int width, height;
  /* Destination rectangle in source-frame pixels (the same coordinate
   * system as the presented game image, origin top-left). */
  float x, y, w, h;
  float brightness;     /* SNES master brightness, 0..1 */
  uint32_t serial;      /* changes whenever the pixels change */
} Dkc2OverlaySprite;

/* Returns the sprites for the frame being presented; NULL source = none. */
typedef int (*Dkc2OverlaySpriteSource)(const Dkc2OverlaySprite **sprites);

#ifdef __cplusplus
}
#endif

#endif
