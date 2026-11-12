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

int main(void) {
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
