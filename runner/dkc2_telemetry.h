#ifndef DKC2_TELEMETRY_H
#define DKC2_TELEMETRY_H

/* Optional, read-only session telemetry for the DKCountry launcher.
 *
 * When DKCOUNTRY_TELEMETRY names a file, the host appends one JSON object per
 * line describing what happens in the game (protocol version 1, specified in
 * the DKCountry repository's docs/TELEMETRY_PROTOCOL.md). Without that
 * variable nothing is opened and nothing is written. The module only reads
 * WRAM after a completed frame; it never writes emulated state, so enabling
 * it cannot change game behaviour, timing or save data. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  kDkc2TelemetryProtocol = 1,
  kDkc2TelemetryFramesPerTick = 60,
};

/* WRAM fields the bridge observes. Addresses are documented in
 * docs/HARDWARE_NOTES.md and runner/dkc2_coop.c. */
typedef struct Dkc2TelemetrySnapshot {
  uint16_t level;         /* $00D3 current level identifier */
  uint16_t demo;          /* $05FB nonzero during attract demos */
  uint16_t bananas_bcd;   /* $096D shared BCD banana counter (0-99) */
  uint16_t kremkoins;     /* $08CC */
  uint16_t banana_coins;  /* $08CA */
  uint16_t animal;        /* $006E ridden animal type, 0 on foot */
  uint16_t kong_flags;    /* $08C2, bit $4000 set while both Kongs are present */
  uint16_t leader;        /* $0597 sprite slot of the controlled Kong */
  uint16_t kong_state[2]; /* sprite state word ($2E) of slots $0DE2/$0E40 */
  uint16_t kong_x[2];     /* sprite X ($06) of the same slots */
} Dkc2TelemetrySnapshot;

typedef void (*Dkc2TelemetrySink)(void *context, const char *line);

typedef struct Dkc2Telemetry {
  Dkc2TelemetrySink sink;
  void *context;
  Dkc2TelemetrySnapshot previous;
  bool have_previous;
  bool said_hello;
  uint32_t frames_since_tick;
  uint32_t move_px;
  uint32_t pending_stomps;
} Dkc2Telemetry;

void Dkc2TelemetryReadSnapshot(const uint8_t *wram, Dkc2TelemetrySnapshot *out);
int Dkc2TelemetryBcdToInt(uint16_t bcd);
/* True for the Kong sprite states the cartridge uses when a Kong is knocked
 * out (the same set the co-op layer treats as losing that Kong). */
bool Dkc2TelemetryIsKnockoutState(uint16_t state);

void Dkc2TelemetryInit(Dkc2Telemetry *t, Dkc2TelemetrySink sink, void *context);
void Dkc2TelemetryHello(Dkc2Telemetry *t, const char *build, uint64_t frame);
void Dkc2TelemetryStep(Dkc2Telemetry *t, const Dkc2TelemetrySnapshot *now, uint64_t frame);
void Dkc2TelemetryAddStomps(Dkc2Telemetry *t, unsigned count);
void Dkc2TelemetryStateLoaded(Dkc2Telemetry *t, uint64_t frame);
void Dkc2TelemetryBye(Dkc2Telemetry *t, uint64_t frame);

/* Process-wide file-backed instance used by the hosts. All calls are no-ops
 * unless DKCOUNTRY_TELEMETRY was set when the first frame ran. */
void Dkc2TelemetryHostFrame(const uint8_t *wram, uint64_t frame);
void Dkc2TelemetryHostStomps(unsigned count);
void Dkc2TelemetryHostStateLoaded(void);

/* Boot sound capture for the DKCountry launcher. When DKCOUNTRY_BOOT_AUDIO
 * names a file, the sound the host renders from power-on (logos, title
 * screen) is written there as a 16-bit PCM WAV, without its leading
 * silence, until gameplay starts or DKCOUNTRY_BOOT_AUDIO_SECONDS (default
 * 70) have been written. The sizes in the header stay zero until the file
 * is complete. Nothing from the ROM is written besides that sound, and the
 * file stays on the player's machine. */
typedef struct Dkc2BootAudio {
  FILE *file;
  uint32_t frames;      /* written so far */
  uint32_t max_frames;
  int channels, rate;
  bool heard;           /* past the leading silence */
  bool stop;            /* gameplay started */
  bool done;
} Dkc2BootAudio;

bool Dkc2BootAudioOpen(Dkc2BootAudio *b, const char *path, int channels,
                       int rate, double max_seconds);
void Dkc2BootAudioWrite(Dkc2BootAudio *b, const int16_t *samples, int frames);
/* Gameplay started: the capture ends, unless it has barely begun. */
void Dkc2BootAudioGameplay(Dkc2BootAudio *b);
bool Dkc2BootAudioDone(const Dkc2BootAudio *b);

/* Host hook: call with each block of game audio, before any music pack is
 * mixed in. Inert unless the launcher asked for a capture; during a capture
 * run the block is silenced after it is recorded, because that run is
 * hidden. */
void Dkc2TelemetryHostAudio(int16_t *samples, int frames, int channels,
                            int rate);

#ifdef __cplusplus
}
#endif

#endif
