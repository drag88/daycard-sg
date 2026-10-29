// c++ -std=c++17 tools/test_logic.cpp -o /tmp/test_logic && /tmp/test_logic
#include <assert.h>
#include <string.h>
#include <initializer_list>
#include "../logic.h"
#include "../quote.h"

int main() {
  {
    assert(catMotion(44999).activity == CAT_SIT);
    auto out = catMotion(45000), away = catMotion(49000), back = catMotion(54000), home = catMotion(59000);
    assert(out.activity == CAT_WALK && out.dx == 0 && out.direction == -1);
    assert(away.activity == CAT_STRETCH && away.dx == -36);
    assert(back.activity == CAT_WALK && back.dx == -36 && back.direction == 1);
    assert(home.activity == CAT_WALK && home.dx == 8 && home.direction == -1);
    assert(catMotion(60000).activity == CAT_SIT && catMotion(60000).dx == 0);
    assert(catMotion(90000 + 49000).activity == CAT_WASH);
    assert(catMotion(180000 + 49000).activity == CAT_NAP);
    int previous = 0;
    for (uint32_t t = 0; t < 270000; t += 40) {
      auto m = catMotion(t);
      assert(m.dx >= -36 && m.dx <= 8);
      assert(m.dx - previous >= -1 && m.dx - previous <= 1);
      previous = m.dx;
    }
  }
  {
    assert(catMotion(47000, true, false, false, false).activity == CAT_SIT);
    assert(catMotion(47000, false, true, false, false).activity == CAT_NAP);
    assert(catMotion(47000, false, false, true, false).activity == CAT_SIT);
    assert(catMotion(47000, false, false, false, true).activity == CAT_SIT);
    auto caption = [](uint32_t t, bool wet = false, bool sleepy = false, bool happy = false,
                      bool evening = false, bool voice = true) {
      return catCaption(t, catMotion(t, wet, sleepy, happy).activity, wet, sleepy, happy, evening, voice);
    };
    assert(!strcmp(caption(47000).title, "OUT FOR A WANDER"));
    assert(!strcmp(caption(51000).title, "STRETCH BREAK"));
    assert(!strcmp(caption(141000).title, "A QUICK TIDY-UP"));
    assert(!strcmp(caption(231000).title, "CATNAP"));
    assert(strcmp(caption(51000).text, caption(321000).text));
    assert(!strcmp(caption(47000, true).title, "UNDER COVER"));
    assert(!strcmp(caption(47000, true, true, true).title, "OFF DUTY"));
    assert(!strcmp(caption(47000, false, false, true).title, "NICELY DONE"));
    assert(strstr(caption(0).text, "Flubber focus"));
    assert(strstr(caption(7999).text, "Flubber focus"));
    assert(!strstr(caption(8000).text, "Flubber focus"));
    assert(!strstr(caption(299999).text, "Flubber focus"));
    assert(strstr(caption(300000).text, "Flubber focus"));
    assert(!strstr(caption(0, false, false, false, false, false).text, "Flubber"));
    assert(strcmp(caption(10000).text, caption(40000).text));
    assert(strcmp(caption(10000).text, caption(10000, false, false, false, true).text));
  }
  {
    FocusSession focus;
    focus.start(1000);
    assert(focus.active() && focus.phase == FOCUS_WORK);
    assert(focus.remaining(1000) == 25 * 60);
    assert(!focus.update(1000 + 25 * 60000 - 1));
    assert(focus.update(1000 + 25 * 60000) && focus.phase == FOCUS_BREAK);
    assert(focus.remaining(1000 + 25 * 60000) == 5 * 60);
    assert(focus.update(1000 + 30 * 60000) && focus.phase == FOCUS_DONE);
    assert(!focus.active() && !focus.update(1000 + 31 * 60000));
    focus.start(UINT32_MAX - 10000);
    assert(focus.remaining(20000) == 24 * 60 + 30);
    assert(focus.update((uint32_t)(UINT32_MAX - 10000 + 30 * 60000u)) && focus.phase == FOCUS_DONE);
    focus.stop();
    assert(focus.phase == FOCUS_IDLE);
    assert(routineFor(6, 7, 10, 18) == ROUTINE_EVENING);
    assert(routineFor(7, 7, 10, 18) == ROUTINE_MORNING);
    assert(routineFor(10, 7, 10, 18) == ROUTINE_WORK);
    assert(routineFor(18, 7, 10, 18) == ROUTINE_EVENING);
    assert(sgtDay(1790605812) == sgtDay(1790605812 + 60));
    const time_t midnight = daysFromCivil(2026, 10, 22) * 86400 - 8 * 3600;
    assert(sgtDay(midnight) != sgtDay(midnight - 1));
    char note[57];
    assert(cleanIntention("  Finish one thing today  ", note, sizeof note));
    assert(!strcmp(note, "Finish one thing today"));
    assert(cleanIntention("", note, sizeof note) && !*note);
    assert(!cleanIntention("a\nb", note, sizeof note));
    assert(!cleanIntention("\xe4\xb8\xad", note, sizeof note));
    assert(!cleanIntention("012345678901234567890123456789012345678901234567890123456789", note, sizeof note));
  }
  {
    auto mask = [](int psi, bool fresh = true, bool storm = false) {
      return priorityMask(true, true, true, false, psi, fresh, storm);
    };
    assert(mask(100) == 0 && mask(101) == priorityFlag(PRIORITY_AIR));
    assert(mask(200) == priorityFlag(PRIORITY_AIR) && mask(201) == priorityFlag(PRIORITY_AIR));
    assert(mask(300, false) == 0);
    assert(priorityMask(false, false, false, true, 40, true, false) == priorityFlag(PRIORITY_OFFLINE));
    assert(priorityMask(true, false, false, true, 40, true, false) ==
           (priorityFlag(PRIORITY_WEATHER_STALE) | priorityFlag(PRIORITY_BUS_STALE)));
    MiddlePanel panel;
    assert(panel.choose(1000, mask(148), 148) == PRIORITY_AIR);
    assert(panel.choose(12999, mask(148), 148) == PRIORITY_AIR);
    assert(panel.choose(13000, mask(148), 148) == PRIORITY_NONE);
    assert(panel.choose(120999, mask(148), 148) == PRIORITY_NONE);
    assert(panel.choose(121000, mask(148), 148) == PRIORITY_AIR);
    assert(panel.choose(121001, mask(100), 100) == PRIORITY_NONE);
    assert(panel.choose(121002, mask(201), 201) == PRIORITY_AIR);
    assert(panel.choose(900000, mask(201), 201) == PRIORITY_AIR);
    assert(panel.choose(912001, mask(200), 200) == PRIORITY_NONE);
    assert(panel.choose(912002, mask(300, false), 300) == PRIORITY_NONE);
    MiddlePanel multiple;
    assert(multiple.choose(0, mask(148, true, true), 148) == PRIORITY_AIR);
    assert(multiple.choose(12000, mask(148, true, true), 148) == PRIORITY_NONE);
    assert(multiple.choose(120000, mask(148, true, true), 148) == PRIORITY_RAIN);
    assert(multiple.choose(240000, mask(148, true, true), 148) == PRIORITY_AIR);
    MiddlePanel wrap;
    const uint32_t start = UINT32_MAX - 5000;
    assert(wrap.choose(start, mask(148), 148) == PRIORITY_AIR);
    assert(wrap.choose(start + 12000u, mask(148), 148) == PRIORITY_NONE);
    assert(wrap.choose(start + 120000u, mask(148), 148) == PRIORITY_AIR);
  }
  // 2026-09-28 22:30:12 SGT is 14:30:12 UTC.
  assert(parseIso8601("2026-09-28T22:30:12+08:00") == 1790605812);
  assert(parseIso8601("2026-09-28T14:30:12Z") == 1790605812);
  assert(parseIso8601("") == 0 && parseIso8601(nullptr) == 0);
  assert(daysFromCivil(2024, 2, 29) + 1 == daysFromCivil(2024, 3, 1));

  time_t now = 1790605812;
  char s[8];
  busLabel(minutesUntil(now + 185, now), s, sizeof s), assert(!strcmp(s, "3 min"));
  busLabel(minutesUntil(now + 59, now), s, sizeof s), assert(!strcmp(s, "Arr"));
  busLabel(minutesUntil(now - 70, now), s, sizeof s), assert(!strcmp(s, "Arr"));
  busLabel(minutesUntil(now - 200, now), s, sizeof s), assert(!strcmp(s, "--"));
  busLabel(minutesUntil(0, now), s, sizeof s), assert(!strcmp(s, "--"));

  assert(sceneFor("Heavy Thundery Showers with Gusty Winds", 14) == THUNDER);
  assert(sceneFor("Heavy Showers", 14) == HEAVY_RAIN);
  assert(sceneFor("Light Rain", 3) == RAIN);
  assert(sceneFor("Partly Cloudy (Night)", 14) == PARTLY_MOON);
  assert(sceneFor("Partly Cloudy (Day)", 22) == PARTLY_SUN);
  assert(sceneFor("Fair", 12) == SUN && sceneFor("Fair", 21) == MOON);
  assert(sceneFor("Cloudy", 12) == CLOUDY && sceneFor("Hazy", 12) == HAZE && sceneFor("Windy", 12) == WIND);

  {
    assert(!strcmp(compactWeather("Heavy Thundery Showers with Gusty Winds", THUNDER), "Thundery, gusty"));
    assert(!strcmp(compactWeather("Heavy Showers", HEAVY_RAIN), "Heavy rain"));
    assert(!strcmp(compactWeather("Cloudy", CLOUDY), "Cloudy"));
    auto width = [](const char *s) { int n = 0; for (; *s; s++) n += ((unsigned char)*s & 0xc0) != 0x80; return n; };
    char text[80] = "Heavy Thundery Showers with Gusty Winds";
    fitText(text, sizeof text, 20, width);
    assert(width(text) <= 20 && strstr(text, "..."));
    strcpy(text, "Showers");fitText(text, sizeof text, 7, width);assert(!strcmp(text, "Showers"));
    strcpy(text, "A\xc2\xb7" "BCDEFG");fitText(text, sizeof text, 5, width);assert(!strcmp(text, "A\xc2\xb7..."));
    strcpy(text, "Weather");fitText(text, sizeof text, 2, width);assert(!*text);
    char tight[8] = "abcdefg";fitText(tight, sizeof tight, 6, width);assert(!strcmp(tight, "abc..."));
  }

  assert(!strcmp(psiBand(50), "Good") && !strcmp(psiBand(104), "Unhealthy") && !strcmp(psiBand(-1), ""));

  const uint8_t beef[] = {0xBE, 0xEF};  // SHTC3 datasheet example
  assert(crc8(beef, 2) == 0x92);

  assert(busX(0, 400, 104, 148) == 400);
  assert(busX(BUS_IN_MS, 400, 104, 148) == 148);
  assert(busX(BUS_ANIM_MS, 400, 104, 148) == -104);
  for (int count : {49, 51, 34}) {  // every quote once per cycle, whatever the list length
    bool seen[64] = {};
    for (int s = 0; s < count; s++) seen[quoteAt(now + s * 1800, 30, count)] = true;
    for (int i = 0; i < count; i++) assert(seen[i]);
  }
  assert(quoteAt(now, 30, 49) == quoteAt(now + 60, 30, 49));  // steady within a slot
  {
    uint32_t ring[4];
    int count = 0;
    for (uint32_t v : {10u, 11u, 12u, 13u, 14u, 11u}) ring[count++ % 4] = v;  // 10 and the first 11 overwritten
    assert(ringAge(ring, count, 4, 11) == 0 && ringAge(ring, count, 4, 14) == 1 && ringAge(ring, count, 4, 12) == 3);
    assert(ringAge(ring, count, 4, 10) == -1 && ringAge(ring, 0, 4, 11) == -1);
    const uint32_t twice[4] = {7, 8, 7};
    assert(ringAge(twice, 3, 4, 7) == 0);  // shown twice: the latest showing counts
  }
  assert(hour12(0) == 12 && hour12(9) == 9 && hour12(12) == 12 && hour12(13) == 1 && hour12(23) == 11);
  assert(batteryPercent(4.25f) == 100 && batteryPercent(3.1f) == 0);
  assert(batteryPercent(3.80f) == 50 && batteryPercent(3.83f) == 55 && batteryPercent(4.05f) == 85);
  assert(fabsf(rhAt(50, 25, 25) - 50) < 0.01f);
  assert(fabsf(rhAt(43, 41.2f, 28) - 89.5f) < 1.0f);  // 13 degrees of self-heating hides a humid room
  assert(rhAt(60, 40, 20) == 100);
  assert(fabsf(feelsLike(30, 70, 2) - 34.4f) < 0.1f);   // BoM apparent temperature, worked by hand
  assert(feelsLike(30, 70, 5) < feelsLike(30, 70, 0));  // a breeze cools
  assert(!strcmp(loadLabel("SDA"), "standing") && !strcmp(loadLabel(""), ""));
  {
    int y, m, d;
    civilFromDays(daysFromCivil(2026, 9, 28), y, m, d);
    assert(y == 2026 && m == 9 && d == 28);
    assert(weekdayOf(daysFromCivil(2026, 10, 1)) == 4);  // a Thursday
  }
  auto utc = [](int y, int m, int d, int h, int mi) { return (time_t)(daysFromCivil(y, m, d) * 86400 + h * 3600 + mi * 60); };
  const WorldClock india{"IND", 330, NO_DST}, newYork{"NYC", -300, US_DST}, frankfurt{"FRA", 60, EU_DST},
      sydney{"SYD", 600, AU_DST}, auckland{"AKL", 720, NZ_DST};
  assert(zoneOffsetMin(india, utc(2026, 1, 1, 0, 0)) == 330);
  // 2026: US DST 8 Mar 07:00 UTC to 1 Nov 06:00 UTC; EU DST 29 Mar to 25 Oct, 01:00 UTC.
  assert(zoneOffsetMin(newYork, utc(2026, 3, 8, 6, 59)) == -300 && zoneOffsetMin(newYork, utc(2026, 3, 8, 7, 0)) == -240);
  assert(zoneOffsetMin(newYork, utc(2026, 11, 1, 5, 59)) == -240 && zoneOffsetMin(newYork, utc(2026, 11, 1, 6, 0)) == -300);
  assert(zoneOffsetMin(frankfurt, utc(2026, 3, 29, 0, 59)) == 60 && zoneOffsetMin(frankfurt, utc(2026, 3, 29, 1, 0)) == 120);
  assert(zoneOffsetMin(frankfurt, utc(2026, 10, 25, 0, 59)) == 120 && zoneOffsetMin(frankfurt, utc(2026, 10, 25, 1, 0)) == 60);
  assert(zoneOffsetMin(newYork, utc(2027, 3, 14, 7, 0)) == -240 && zoneOffsetMin(frankfurt, utc(2027, 3, 28, 1, 0)) == 120);
  // Southern summers span the new year. 2026: Sydney 5 Apr / 4 Oct, Auckland 5 Apr / 27 Sep (checked against tzdata).
  assert(zoneOffsetMin(sydney, utc(2026, 4, 4, 15, 59)) == 660 && zoneOffsetMin(sydney, utc(2026, 4, 4, 16, 0)) == 600);
  assert(zoneOffsetMin(sydney, utc(2026, 10, 3, 15, 59)) == 600 && zoneOffsetMin(sydney, utc(2026, 10, 3, 16, 0)) == 660);
  assert(zoneOffsetMin(sydney, utc(2026, 12, 31, 14, 0)) == 660 && zoneOffsetMin(sydney, utc(2027, 1, 1, 0, 0)) == 660);
  assert(zoneOffsetMin(auckland, utc(2026, 4, 4, 13, 59)) == 780 && zoneOffsetMin(auckland, utc(2026, 4, 4, 14, 0)) == 720);
  assert(zoneOffsetMin(auckland, utc(2026, 9, 26, 13, 59)) == 720 && zoneOffsetMin(auckland, utc(2026, 9, 26, 14, 0)) == 780);

  // Wikiquote lines, shaped like the ones on the pages in QUOTE_AUTHORS (October 2026).
  {
    char q[400];
    assert(wikiquoteText("* Principles<br>You can't say A is made of B<br>or vice versa.<br>All mass is interaction.", q, sizeof q));
    assert(!strcmp(q, "Principles You can't say A is made of B or vice versa. All mass is interaction."));
    assert(wikiquoteText("* We [[scientists]] are clever, too [[Machine|clever]] for the world.<ref name=\"x\">Letter</ref>", q, sizeof q));
    assert(!strcmp(q, "We scientists are clever, too clever for the world."));
    assert(wikiquoteText("* Each of us lives only now, this brief instant. (Hays translation)", q, sizeof q));
    assert(!strcmp(q, "Each of us lives only now, this brief instant."));
    assert(wikiquoteText("* “The world is nothing but change. Our life is only perception.”", q, sizeof q));
    assert(!strcmp(q, "The world is nothing but change. Our life is only perception."));
    const char *passage = "* We have a habit. '''A very great deal more truth can become known than can be proven.''' So.";
    assert(wikiquoteText(passage, q, sizeof q, true) && !strcmp(q, "A very great deal more truth can become known than can be proven."));
    assert(!wikiquoteText("* Quaeris Alcidae parem? Nemo est nisi ipse.", q, sizeof q));          // Latin original
    assert(!wikiquoteText("** chapter 3, p. 75, the source line under a quote.", q, sizeof q));    // citation
    assert(!wikiquoteText("* {{cquote|Some templated text that is long enough.}}", q, sizeof q));  // template
    assert(!wikiquoteText("* '''life is not guaranteed,''' and the rest of it.", q, sizeof q, true));  // fragment
    assert(!wikiquoteText("* See Also: ''Operating Manual for Spaceship Earth'' (1969)", q, sizeof q));
    assert(wikiquoteStop("== Disputed and/or attributed ==") && wikiquoteStop("== Quotations about Feynman =="));
    assert(!wikiquoteStop("== Quotes ==") && !wikiquoteStop("=== Quotes about the Machine (1950) ==="));
    const char *u = "“éa";
    assert(nextCodepoint(u) == 0x201C && nextCodepoint(u) == 0xE9 && nextCodepoint(u) == 'a');
    static uint8_t buf[QUOTE_BUF];
    Bitmap b = {};
    assert(setQuote("Beware of bugs in the above code; I have only proved it correct, not tried it.", "Donald Knuth", buf, &b));
    assert(b.w == QUOTE_W && b.h > 30 && b.h <= QUOTE_MAX_H && b.bits >= buf && b.bits + b.h * QUOTE_STRIDE <= buf + QUOTE_BUF);
    assert(!setQuote("I have only proved it correct, not tried it. I have only proved it correct, not tried it. "
                     "I have only proved it correct, not tried it.", "Donald Knuth", nullptr, nullptr));  // three lines
    assert(!setQuote("Thinking in 中文 is not in the font.", "Someone", nullptr, nullptr));
  }
  puts("logic ok");
}
