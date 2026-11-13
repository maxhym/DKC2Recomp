/* Synthetic checks of the DKCountry telemetry bridge. No ROM is used: WRAM
 * images and snapshots are built by hand. */
#include "dkc2_telemetry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

typedef struct {
  char text[16384];
  int lines;
} Capture;

static void Sink(void *context, const char *line) {
  Capture *c = (Capture *)context;
  strncat(c->text, line, sizeof c->text - strlen(c->text) - 2);
  strncat(c->text, "\n", sizeof c->text - strlen(c->text) - 1);
  c->lines++;
}

static int Count(const Capture *c, const char *needle) {
  int n = 0;
  for (const char *p = c->text; (p = strstr(p, needle)) != NULL; ++p) ++n;
  return n;
}

static Dkc2TelemetrySnapshot Base(void) {
  Dkc2TelemetrySnapshot s;
  memset(&s, 0, sizeof s);
  s.level = 7;
  s.leader = 0x0DE2;
  s.kong_flags = 0x4000;
  s.kong_x[0] = 1000;
  s.kong_x[1] = 980;
  s.kong_state[0] = s.kong_state[1] = 0x13;
  return s;
}

static void TestSnapshotReadsDocumentedAddresses(void) {
  static uint8_t wram[0x20000];
  memset(wram, 0, sizeof wram);
  wram[0x00D3] = 0x2A;
  wram[0x05FB] = 1;
  wram[0x096D] = 0x47;
  wram[0x08CC] = 3;
  wram[0x08CA] = 0x10;
  wram[0x006E] = 0x9C; wram[0x006F] = 0x01;
  wram[0x08C2 + 1] = 0x40;
  wram[0x0597] = 0x40; wram[0x0598] = 0x0E;
  wram[0x0DE2 + 0x2E] = 0x24;
  wram[0x0E40 + 0x06] = 0x34; wram[0x0E40 + 0x07] = 0x12;
  Dkc2TelemetrySnapshot s;
  Dkc2TelemetryReadSnapshot(wram, &s);
  CHECK(s.level == 0x2A);
  CHECK(s.demo == 1);
  CHECK(Dkc2TelemetryBcdToInt(s.bananas_bcd) == 47);
  CHECK(s.kremkoins == 3 && s.banana_coins == 0x10);
  CHECK(s.animal == 0x19C);
  CHECK(s.kong_flags == 0x4000);
  CHECK(s.leader == 0x0E40);
  CHECK(s.kong_state[0] == 0x24);
  CHECK(s.kong_x[1] == 0x1234);
}

static void TestBcd(void) {
  CHECK(Dkc2TelemetryBcdToInt(0x0099) == 99);
  CHECK(Dkc2TelemetryBcdToInt(0x0000) == 0);
  CHECK(Dkc2TelemetryBcdToInt(0x001A) == -1);
}

static void TestGameplayEvents(void) {
  Capture c;
  memset(&c, 0, sizeof c);
  Dkc2Telemetry t;
  Dkc2TelemetryInit(&t, Sink, &c);
  Dkc2TelemetryHello(&t, "0.0.9\"x", 0);
  CHECK(strstr(c.text, "\"game\":\"dkc2\"") != NULL);
  CHECK(strstr(c.text, "\"build\":\"0.0.9x\"") != NULL);

  Dkc2TelemetrySnapshot s = Base();
  s.bananas_bcd = 0x95;
  uint64_t f = 1;
  Dkc2TelemetryStep(&t, &s, f++);              /* baseline: nothing emitted */
  CHECK(Count(&c, "\"e\":\"bananas\"") == 0);

  s.bananas_bcd = 0x96;                        /* +1 */
  s.kong_x[0] = 1010;
  Dkc2TelemetryStep(&t, &s, f++);
  s.bananas_bcd = 0x06;                        /* bunch across the wrap: +10 */
  s.kong_x[0] = 1030;
  Dkc2TelemetryStep(&t, &s, f++);
  s.bananas_bcd = 0x45;                        /* file load jump: ignored */
  Dkc2TelemetryStep(&t, &s, f++);
  CHECK(Count(&c, "\"e\":\"bananas\"") == 2);
  CHECK(strstr(c.text, "\"n\":1,\"level\":7") != NULL);
  CHECK(strstr(c.text, "\"n\":10,\"level\":7") != NULL);

  s.kremkoins = 1;
  s.banana_coins = 2;
  s.animal = 0x19C;
  Dkc2TelemetryStep(&t, &s, f++);
  CHECK(Count(&c, "\"kind\":\"kremkoin\",\"n\":1") == 1);
  CHECK(Count(&c, "\"kind\":\"banana_coin\",\"n\":2") == 1);
  CHECK(Count(&c, "\"animal\":412") == 1);
  s.banana_coins = 0;                          /* spending is not collecting */
  s.animal = 0;
  Dkc2TelemetryStep(&t, &s, f++);
  CHECK(Count(&c, "\"e\":\"coins\"") == 2);
  CHECK(Count(&c, "\"animal\":0}") == 1);

  s.kong_state[0] = 0x24;                      /* leader hit, partner present */
  Dkc2TelemetryStep(&t, &s, f++);
  Dkc2TelemetryStep(&t, &s, f++);              /* staying hurt is not a new hit */
  CHECK(Count(&c, "\"e\":\"hit\"") == 1);
  s.kong_flags = 0;                            /* partner gone */
  s.kong_state[0] = 0x13;
  Dkc2TelemetryStep(&t, &s, f++);
  s.kong_state[0] = 0x59;
  s.kong_x[0] = 1500;                          /* teleport-sized jump: not walked */
  Dkc2TelemetryStep(&t, &s, f++);
  CHECK(Count(&c, "\"e\":\"death\"") == 1);
  CHECK(strstr(c.text, "\"x\":1500,\"cause\":\"knockout\"") != NULL);

  s.level = 8;
  Dkc2TelemetryStep(&t, &s, f++);
  CHECK(Count(&c, "\"e\":\"level\",") == 1);

  Dkc2TelemetryAddStomps(&t, 3);
  while (f < 61) Dkc2TelemetryStep(&t, &s, f++);
  CHECK(Count(&c, "\"e\":\"tick\"") == 1);
  CHECK(strstr(c.text, "\"demo\":false,\"level\":8") != NULL);
  CHECK(strstr(c.text, "\"e\":\"move\",\"f\":60,\"dx\":30}") != NULL);
  CHECK(strstr(c.text, "\"e\":\"stomp\",\"f\":60,\"n\":3}") != NULL);

  Dkc2TelemetryStateLoaded(&t, f);
  s.bananas_bcd = 0x07;                        /* differs after a load: baseline */
  Dkc2TelemetryStep(&t, &s, f++);
  CHECK(Count(&c, "\"e\":\"state_load\"") == 1);
  CHECK(Count(&c, "\"e\":\"bananas\"") == 2);

  Dkc2TelemetryBye(&t, f);
  CHECK(strstr(c.text, "\"e\":\"bye\"") != NULL);
}

static void TestDemoIsSilent(void) {
  Capture c;
  memset(&c, 0, sizeof c);
  Dkc2Telemetry t;
  Dkc2TelemetryInit(&t, Sink, &c);
  Dkc2TelemetrySnapshot s = Base();
  s.demo = 2;
  for (uint64_t f = 1; f <= 120; ++f) {
    s.bananas_bcd = (uint16_t)((f / 10) % 10);
    s.level = (uint16_t)(f % 3);
    s.kong_state[0] = (f % 7) ? 0x13 : 0x24;
    Dkc2TelemetryStep(&t, &s, f);
  }
  CHECK(Count(&c, "\"e\":\"bananas\"") == 0);
  CHECK(Count(&c, "\"e\":\"level\"") == 0);
  CHECK(Count(&c, "\"e\":\"hit\"") == 0);
  CHECK(Count(&c, "\"e\":\"death\"") == 0);
  CHECK(Count(&c, "\"demo\":true") == 2);
}

static void TestHostIsInertWithoutVariable(void) {
  static uint8_t wram[0x20000];
  Dkc2TelemetryHostFrame(wram, 1);
  Dkc2TelemetryHostStomps(4);
  Dkc2TelemetryHostStateLoaded();
  CHECK(1);
}

static uint32_t Le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static size_t ReadAll(const char *path, uint8_t *out, size_t capacity) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  const size_t n = fread(out, 1, capacity, f);
  fclose(f);
  return n;
}

static void TestBootAudio(void) {
  static uint8_t bytes[1 << 20];
  static int16_t block[2 * 8000];
  const char *path = "dkc2_boot_audio_test.wav";
  enum { kRate = 8000, kHalf = kRate / 2 };
  Dkc2BootAudio b;
  CHECK(!Dkc2BootAudioOpen(&b, "", 2, kRate, 5));
  CHECK(!Dkc2BootAudioOpen(&b, path, 3, kRate, 5));
  Dkc2BootAudioWrite(&b, NULL, 4);         /* closed: harmless */

  /* 20 s limit. A second of silence, then sound: the silence is dropped. */
  CHECK(Dkc2BootAudioOpen(&b, path, 2, kRate, 20));
  Dkc2BootAudioWrite(&b, block, kRate);
  CHECK(b.frames == 0 && !b.heard);
  for (int i = 0; i < kRate; ++i)
    block[2 * i] = block[2 * i + 1] = (int16_t)(i < kHalf ? 0 : 1000 + i);
  Dkc2BootAudioWrite(&b, block, kRate);
  CHECK(b.heard && b.frames == kHalf);
  /* Until the capture ends the header says "no data yet". */
  fflush(b.file);
  size_t n = ReadAll(path, bytes, sizeof bytes);
  CHECK(n == 44 + kHalf * 4 && memcmp(bytes, "RIFF", 4) == 0 && Le32(bytes + 40) == 0);
  CHECK((int16_t)(bytes[44] | bytes[45] << 8) == 1000 + kHalf);

  /* Gameplay before ten seconds of sound does not cut the capture short;
   * past ten seconds it ends it. */
  Dkc2BootAudioGameplay(&b);
  for (int i = 0; i < 9; ++i) Dkc2BootAudioWrite(&b, block, kRate);
  CHECK(!Dkc2BootAudioDone(&b) && b.frames == 9u * kRate + kHalf);
  Dkc2BootAudioWrite(&b, block, kRate);
  CHECK(Dkc2BootAudioDone(&b) && b.file == NULL);
  const uint32_t data = (10u * kRate + kHalf) * 4u;
  n = ReadAll(path, bytes, sizeof bytes);
  CHECK(n == 44 + data);
  CHECK(Le32(bytes + 40) == data && Le32(bytes + 4) == data + 36);
  CHECK(Le32(bytes + 24) == kRate && bytes[22] == 2 && bytes[34] == 16);
  Dkc2BootAudioWrite(&b, block, kRate);    /* finished: ignored */

  /* Without gameplay the limit ends it, exactly. */
  CHECK(Dkc2BootAudioOpen(&b, path, 1, kRate, 1.5));
  for (int i = 0; i < kRate; ++i) block[i] = 2000;
  Dkc2BootAudioWrite(&b, block, kRate);
  Dkc2BootAudioWrite(&b, block, kRate);
  CHECK(Dkc2BootAudioDone(&b));
  n = ReadAll(path, bytes, sizeof bytes);
  CHECK(n == 44 + 3u * kHalf * 2 && Le32(bytes + 40) == 3u * kHalf * 2 && bytes[22] == 1);
  remove(path);
}

int main(void) {
  TestBootAudio();
  TestSnapshotReadsDocumentedAddresses();
  TestBcd();
  TestGameplayEvents();
  TestDemoIsSilent();
  if (getenv("DKCOUNTRY_TELEMETRY") == NULL) TestHostIsInertWithoutVariable();
  if (failures) {
    fprintf(stderr, "%d telemetry check(s) failed\n", failures);
    return 1;
  }
  puts("dkc2 telemetry tests passed");
  return 0;
}
