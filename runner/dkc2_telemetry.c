#include "dkc2_telemetry.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  kSlotA = 0x0DE2,
  kSlotB = 0x0E40,
  /* Larger single-frame jumps are file loads or resets, not pickups. A
   * banana bunch is the largest ordinary award. */
  kMaxBananaStep = 10,
  kMaxCoinStep = 10,
  /* Larger horizontal jumps are room changes or catch-up teleports. */
  kMaxMoveStep = 48,
};

static uint16_t Read16(const uint8_t *wram, uint32_t address) {
  return (uint16_t)(wram[address] | ((uint16_t)wram[address + 1] << 8));
}

void Dkc2TelemetryReadSnapshot(const uint8_t *wram, Dkc2TelemetrySnapshot *out) {
  memset(out, 0, sizeof *out);
  out->level = Read16(wram, 0x00D3);
  out->demo = Read16(wram, 0x05FB);
  out->bananas_bcd = Read16(wram, 0x096D);
  out->kremkoins = Read16(wram, 0x08CC);
  out->banana_coins = Read16(wram, 0x08CA);
  out->animal = Read16(wram, 0x006E);
  out->kong_flags = Read16(wram, 0x08C2);
  out->leader = Read16(wram, 0x0597);
  out->kong_state[0] = Read16(wram, kSlotA + 0x2E);
  out->kong_state[1] = Read16(wram, kSlotB + 0x2E);
  out->kong_x[0] = Read16(wram, kSlotA + 0x06);
  out->kong_x[1] = Read16(wram, kSlotB + 0x06);
}

int Dkc2TelemetryBcdToInt(uint16_t bcd) {
  int value = 0;
  int scale = 1;
  for (int digit = 0; digit < 4; ++digit) {
    int nibble = (bcd >> (digit * 4)) & 15;
    if (nibble > 9) return -1;
    value += nibble * scale;
    scale *= 10;
  }
  return value;
}

bool Dkc2TelemetryIsKnockoutState(uint16_t state) {
  return state == 0x05 || state == 0x24 || state == 0x25 || state == 0x59;
}

static void Emit(Dkc2Telemetry *t, const char *line) {
  if (t->sink) t->sink(t->context, line);
}

void Dkc2TelemetryInit(Dkc2Telemetry *t, Dkc2TelemetrySink sink, void *context) {
  memset(t, 0, sizeof *t);
  t->sink = sink;
  t->context = context;
}

void Dkc2TelemetryHello(Dkc2Telemetry *t, const char *build, uint64_t frame) {
  char line[160];
  char safe[48];
  size_t n = 0;
  for (const char *p = build ? build : ""; *p && n + 1 < sizeof safe; ++p)
    if (*p != '"' && *p != '\\' && (unsigned char)*p >= 0x20) safe[n++] = *p;
  safe[n] = 0;
  snprintf(line, sizeof line,
           "{\"e\":\"hello\",\"proto\":%d,\"game\":\"dkc2\",\"build\":\"%s\",\"f\":%" PRIu64 "}",
           kDkc2TelemetryProtocol, safe, frame);
  Emit(t, line);
  t->said_hello = true;
}

static int BananaDelta(uint16_t before_bcd, uint16_t after_bcd) {
  int before = Dkc2TelemetryBcdToInt(before_bcd);
  int after = Dkc2TelemetryBcdToInt(after_bcd);
  if (before < 0 || after < 0 || before > 99 || after > 99 || before == after) return 0;
  int delta = after > before ? after - before : after + 100 - before;
  return delta <= kMaxBananaStep ? delta : 0;
}

static int CoinDelta(uint16_t before, uint16_t after) {
  if (after <= before) return 0;
  int delta = after - before;
  return delta <= kMaxCoinStep ? delta : 0;
}

void Dkc2TelemetryStep(Dkc2Telemetry *t, const Dkc2TelemetrySnapshot *now, uint64_t frame) {
  char line[192];
  const Dkc2TelemetrySnapshot *was = &t->previous;
  const bool demo = now->demo != 0;

  if (t->have_previous && !demo && was->demo == 0) {
    if (now->level != was->level) {
      snprintf(line, sizeof line, "{\"e\":\"level\",\"f\":%" PRIu64 ",\"level\":%u}", frame, (unsigned)now->level);
      Emit(t, line);
    }
    int bananas = BananaDelta(was->bananas_bcd, now->bananas_bcd);
    if (bananas > 0) {
      snprintf(line, sizeof line, "{\"e\":\"bananas\",\"f\":%" PRIu64 ",\"n\":%d,\"level\":%u}",
               frame, bananas, (unsigned)now->level);
      Emit(t, line);
    }
    int kremkoins = CoinDelta(was->kremkoins, now->kremkoins);
    if (kremkoins > 0) {
      snprintf(line, sizeof line, "{\"e\":\"coins\",\"f\":%" PRIu64 ",\"kind\":\"kremkoin\",\"n\":%d}", frame, kremkoins);
      Emit(t, line);
    }
    int coins = CoinDelta(was->banana_coins, now->banana_coins);
    if (coins > 0) {
      snprintf(line, sizeof line, "{\"e\":\"coins\",\"f\":%" PRIu64 ",\"kind\":\"banana_coin\",\"n\":%d}", frame, coins);
      Emit(t, line);
    }
    if (now->animal != was->animal) {
      snprintf(line, sizeof line, "{\"e\":\"animal\",\"f\":%" PRIu64 ",\"animal\":%u}", frame, (unsigned)now->animal);
      Emit(t, line);
    }
    for (int k = 0; k < 2; ++k) {
      if (Dkc2TelemetryIsKnockoutState(now->kong_state[k]) &&
          !Dkc2TelemetryIsKnockoutState(was->kong_state[k])) {
        /* With the partner still present the player keeps a Kong; otherwise
         * this knockout ends the attempt. */
        const bool partner = (was->kong_flags & 0x4000) != 0;
        snprintf(line, sizeof line,
                 partner ? "{\"e\":\"hit\",\"f\":%" PRIu64 ",\"level\":%u,\"x\":%u}"
                         : "{\"e\":\"death\",\"f\":%" PRIu64 ",\"level\":%u,\"x\":%u,\"cause\":\"knockout\"}",
                 frame, (unsigned)now->level, (unsigned)now->kong_x[k]);
        Emit(t, line);
      }
    }
    int slot = now->leader == kSlotA ? 0 : now->leader == kSlotB ? 1 : -1;
    if (slot >= 0 && now->leader == was->leader && now->level == was->level) {
      int dx = (int)now->kong_x[slot] - (int)was->kong_x[slot];
      if (dx < 0) dx = -dx;
      if (dx <= kMaxMoveStep) t->move_px += (uint32_t)dx;
    }
  }

  if (++t->frames_since_tick >= kDkc2TelemetryFramesPerTick) {
    t->frames_since_tick = 0;
    snprintf(line, sizeof line, "{\"e\":\"tick\",\"f\":%" PRIu64 ",\"demo\":%s,\"level\":%u}",
             frame, demo ? "true" : "false", (unsigned)now->level);
    Emit(t, line);
    if (t->move_px) {
      snprintf(line, sizeof line, "{\"e\":\"move\",\"f\":%" PRIu64 ",\"dx\":%u}", frame, (unsigned)t->move_px);
      Emit(t, line);
      t->move_px = 0;
    }
    if (t->pending_stomps) {
      snprintf(line, sizeof line, "{\"e\":\"stomp\",\"f\":%" PRIu64 ",\"n\":%u}", frame, (unsigned)t->pending_stomps);
      Emit(t, line);
      t->pending_stomps = 0;
    }
  }
  t->previous = *now;
  t->have_previous = true;
}

void Dkc2TelemetryAddStomps(Dkc2Telemetry *t, unsigned count) {
  t->pending_stomps += count;
}

void Dkc2TelemetryStateLoaded(Dkc2Telemetry *t, uint64_t frame) {
  char line[64];
  snprintf(line, sizeof line, "{\"e\":\"state_load\",\"f\":%" PRIu64 "}", frame);
  Emit(t, line);
  /* The loaded state is a new baseline: nothing that differs from the
   * pre-load frame was earned by playing. */
  t->have_previous = false;
}

void Dkc2TelemetryBye(Dkc2Telemetry *t, uint64_t frame) {
  char line[64];
  if (t->move_px) {
    snprintf(line, sizeof line, "{\"e\":\"move\",\"f\":%" PRIu64 ",\"dx\":%u}", frame, (unsigned)t->move_px);
    Emit(t, line);
    t->move_px = 0;
  }
  snprintf(line, sizeof line, "{\"e\":\"bye\",\"f\":%" PRIu64 "}", frame);
  Emit(t, line);
}

/* ---- File-backed host instance ---- */

#ifndef DKC2_TELEMETRY_BUILD
#define DKC2_TELEMETRY_BUILD "0.0.8"
#endif

static Dkc2Telemetry s_host;
static FILE *s_file;
static bool s_checked;
static uint64_t s_last_frame;
static unsigned s_lines_since_flush;

/* ------------------------------------------------- boot sound capture --- */

enum { kBootAudioMinSeconds = 10 };

static void BootAudioPut32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static bool BootAudioHeader(Dkc2BootAudio *b, uint32_t data_bytes) {
  uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                   16, 0, 0, 0, 1, 0};
  const uint32_t align = (uint32_t)b->channels * 2u;
  BootAudioPut32(h + 4, data_bytes ? data_bytes + 36u : 0u);
  h[22] = (uint8_t)b->channels;
  BootAudioPut32(h + 24, (uint32_t)b->rate);
  BootAudioPut32(h + 28, (uint32_t)b->rate * align);
  h[32] = (uint8_t)align;
  h[34] = 16;
  memcpy(h + 36, "data", 4);
  BootAudioPut32(h + 40, data_bytes);
  return fseek(b->file, 0, SEEK_SET) == 0 && fwrite(h, 1, sizeof h, b->file) == sizeof h;
}

bool Dkc2BootAudioOpen(Dkc2BootAudio *b, const char *path, int channels,
                       int rate, double max_seconds) {
  memset(b, 0, sizeof *b);
  if (!path || !path[0] || channels < 1 || channels > 2 || rate < 8000 || rate > 96000 ||
      !(max_seconds >= 1.0 && max_seconds <= 600.0))
    return false;
  b->file = fopen(path, "wb");
  if (!b->file) return false;
  b->channels = channels;
  b->rate = rate;
  b->max_frames = (uint32_t)(max_seconds * rate);
  if (!BootAudioHeader(b, 0)) {
    fclose(b->file);
    b->file = NULL;
    return false;
  }
  return true;
}

static void BootAudioFinish(Dkc2BootAudio *b) {
  if (!b->file) return;
  (void)BootAudioHeader(b, b->frames * (uint32_t)b->channels * 2u);
  fclose(b->file);
  b->file = NULL;
  b->done = true;
}

void Dkc2BootAudioWrite(Dkc2BootAudio *b, const int16_t *samples, int frames) {
  if (!b->file || !samples || frames <= 0) return;
  if (!b->heard) {
    /* Skip the silence before the first sound. */
    int first = 0;
    for (; first < frames * b->channels; ++first)
      if (samples[first] > 16 || samples[first] < -16) break;
    first /= b->channels;
    if (first >= frames) return;
    b->heard = true;
    samples += first * b->channels;
    frames -= first;
  }
  if ((uint32_t)frames > b->max_frames - b->frames) frames = (int)(b->max_frames - b->frames);
  for (int i = 0; i < frames * b->channels; ++i) {
    const uint16_t v = (uint16_t)samples[i];
    uint8_t le[2];
    le[0] = (uint8_t)v;
    le[1] = (uint8_t)(v >> 8);
    if (fwrite(le, 1, 2, b->file) != 2) {
      BootAudioFinish(b);
      return;
    }
  }
  b->frames += (uint32_t)frames;
  if (b->frames >= b->max_frames ||
      (b->stop && b->frames >= (uint32_t)kBootAudioMinSeconds * (uint32_t)b->rate))
    BootAudioFinish(b);
}

void Dkc2BootAudioGameplay(Dkc2BootAudio *b) {
  if (b->file) b->stop = true;
}

bool Dkc2BootAudioDone(const Dkc2BootAudio *b) { return b->done; }

static Dkc2BootAudio s_boot_audio;
static bool s_boot_audio_checked;

static void BootAudioClose(void) { BootAudioFinish(&s_boot_audio); }

void Dkc2TelemetryHostAudio(const int16_t *samples, int frames, int channels,
                            int rate) {
  if (!s_boot_audio_checked) {
    s_boot_audio_checked = true;
    const char *path = getenv("DKCOUNTRY_BOOT_AUDIO");
    if (path && path[0]) {
      const char *limit = getenv("DKCOUNTRY_BOOT_AUDIO_SECONDS");
      const double seconds = limit && limit[0] ? atof(limit) : 70.0;
      if (Dkc2BootAudioOpen(&s_boot_audio, path, channels, rate, seconds))
        atexit(BootAudioClose);
      else
        fprintf(stderr, "boot audio: cannot write %s; continuing without it\n", path);
    }
  }
  Dkc2BootAudioWrite(&s_boot_audio, samples, frames);
}

static void FileSink(void *context, const char *line) {
  FILE *file = (FILE *)context;
  fputs(line, file);
  fputc('\n', file);
  if (++s_lines_since_flush >= 16) {
    fflush(file);
    s_lines_since_flush = 0;
  }
}

static void HostClose(void) {
  if (!s_file) return;
  Dkc2TelemetryBye(&s_host, s_last_frame);
  fclose(s_file);
  s_file = NULL;
}

static bool HostReady(void) {
  if (s_checked) return s_file != NULL;
  s_checked = true;
  const char *path = getenv("DKCOUNTRY_TELEMETRY");
  if (!path || !path[0]) return false;
  s_file = fopen(path, "ab");
  if (!s_file) {
    fprintf(stderr, "telemetry: cannot open %s; continuing without it\n", path);
    return false;
  }
  Dkc2TelemetryInit(&s_host, FileSink, s_file);
  atexit(HostClose);
  return true;
}

void Dkc2TelemetryHostFrame(const uint8_t *wram, uint64_t frame) {
  if (!wram) return;
  if (s_boot_audio.file) {
    /* With no input the game goes from its title screen to the attract
     * demo: the boot sound ends there. */
    Dkc2TelemetrySnapshot now;
    Dkc2TelemetryReadSnapshot(wram, &now);
    if (now.demo != 0) Dkc2BootAudioGameplay(&s_boot_audio);
  }
  if (!HostReady()) return;
  if (!s_host.said_hello) Dkc2TelemetryHello(&s_host, DKC2_TELEMETRY_BUILD, frame);
  Dkc2TelemetrySnapshot snapshot;
  Dkc2TelemetryReadSnapshot(wram, &snapshot);
  Dkc2TelemetryStep(&s_host, &snapshot, frame);
  s_last_frame = frame;
  if (s_host.frames_since_tick == 0) {
    fflush(s_file);
    s_lines_since_flush = 0;
  }
}

void Dkc2TelemetryHostStomps(unsigned count) {
  if (count && s_file) Dkc2TelemetryAddStomps(&s_host, count);
}

void Dkc2TelemetryHostStateLoaded(void) {
  if (s_file) Dkc2TelemetryStateLoaded(&s_host, s_last_frame);
}
