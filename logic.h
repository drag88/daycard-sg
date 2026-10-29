// Pure helpers shared by the firmware and tools/test_logic.cpp, so they can be checked on a Mac.
#pragma once
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum CatActivity { CAT_SIT, CAT_WALK, CAT_STRETCH, CAT_WASH, CAT_NAP };
struct CatMotion {
  CatActivity activity;
  int dx, direction;
};

// A short outing every 90 seconds, with continuous positions at each activity boundary.
inline CatMotion catMotion(uint32_t now, bool wet = false, bool sleepy = false, bool happy = false, bool concentrating = false) {
  if (wet || sleepy || happy || concentrating) return {sleepy ? CAT_NAP : CAT_SIT, 0, 1};
  uint32_t t = now % 90000;
  if (t < 45000 || t >= 60000) return {CAT_SIT, 0, 1};
  if (t < 49000) return {CAT_WALK, -(int)((t - 45000) * 36 / 4000), -1};
  if (t < 54000) {
    const CatActivity activities[] = {CAT_STRETCH, CAT_WASH, CAT_NAP};
    return {activities[now / 90000 % 3], -36, 1};
  }
  if (t < 59000) return {CAT_WALK, -36 + (int)((t - 54000) * 44 / 5000), 1};
  return {CAT_WALK, 8 - (int)((t - 59000) * 8 / 1000), -1};
}

struct CatCaption { const char *title, *text; };

// voiceHint is the focus prompt with the configured name ("Tap BOOT or say 'Flubber focus'"), or null with voice off.
inline CatCaption catCaption(uint32_t now, CatActivity activity, bool wet, bool sleepy, bool happy, bool evening,
                             const char *voiceHint) {
  unsigned variant = now / 30000 % 3;
  if (sleepy) return {"OFF DUTY", variant % 2 ? "Keeping you company quietly." : "I think we've done enough today."};
  if (wet) return {"UNDER COVER", variant % 2 ? "Keeping my paws dry." : "A good day to stay cosy."};
  if (happy) return {"NICELY DONE", "One small thing, beautifully done."};
  static const char *const titles[] = {"OUT FOR A WANDER", "STRETCH BREAK", "A QUICK TIDY-UP", "CATNAP"};
  static const char *const activityText[][2] = {
    {"Just checking on things.", "A patrol of my tiny kingdom."},
    {"Big stretch. Tiny paws.", "Making room for more sitting."},
    {"Must look presentable.", "One paw at a time."},
    {"Resting my very busy paws.", "Wake me for something lovely."}
  };
  if (activity != CAT_SIT) {
    // Each activity repeats every three outings; vary the line on its next visit.
    unsigned i = activity - CAT_WALK;
    return {titles[i], activityText[i][now / 270000 % 2]};
  }
  if (now % 300000 < 8000)
    return {"A LITTLE FOCUS?", voiceHint ? voiceHint : "Tap BOOT for 25 quiet minutes"};
  static const char *const idleText[][3] = {
    {"Keeping you company.", "No rush. I'm right here.", "A fine spot for a little thinking."},
    {"A little less doing.", "Time to let the day settle.", "Nothing urgent in my kingdom."}
  };
  return {evening ? "TAKING IT EASY" : "IN GOOD COMPANY", idleText[evening][variant]};
}

// Quote authors: the last eight wait their turn, but never every author, or a short list would leave none to pick.
inline bool quoteRecent(const int (&recent)[8], int recentAt, int nAuthors, int author) {
  int avoid = nAuthors > 8 ? 8 : nAuthors - 1;
  for (int j = 0; j < avoid; j++)
    if (recent[((recentAt - 1 - j) % 8 + 8) % 8] == author) return true;
  return false;
}

enum FocusPhase { FOCUS_IDLE, FOCUS_WORK, FOCUS_BREAK, FOCUS_DONE };

struct FocusSession {
  FocusPhase phase = FOCUS_IDLE;
  uint32_t started = 0;
  static constexpr uint32_t WORK_MS = 25 * 60000, BREAK_MS = 5 * 60000;
  bool active() const { return phase == FOCUS_WORK || phase == FOCUS_BREAK; }
  void start(uint32_t now) { phase = FOCUS_WORK, started = now; }
  void stop() { phase = FOCUS_IDLE; }
  uint32_t duration() const { return phase == FOCUS_WORK ? WORK_MS : BREAK_MS; }
  bool update(uint32_t now) {
    bool changed = false;
    while (active() && now - started >= duration()) {
      started += duration();
      phase = phase == FOCUS_WORK ? FOCUS_BREAK : FOCUS_DONE;
      changed = true;
    }
    return changed;
  }
  unsigned remaining(uint32_t now) const {
    uint32_t elapsed = now - started;
    return !active() || elapsed >= duration() ? 0 : (duration() - elapsed + 999) / 1000;
  }
  int growth(uint32_t now) const {
    if (phase == FOCUS_IDLE) return 0;
    if (phase != FOCUS_WORK) return 4;
    uint32_t elapsed = now - started;
    return elapsed >= WORK_MS ? 4 : (int)((uint64_t)elapsed * 4 / WORK_MS);
  }
};

enum Routine { ROUTINE_MORNING, ROUTINE_WORK, ROUTINE_EVENING };
inline Routine routineFor(int hour, int morning, int work, int evening) {
  return hour < morning || hour >= evening ? ROUTINE_EVENING : hour < work ? ROUTINE_MORNING : ROUTINE_WORK;
}

inline int64_t sgtDay(time_t utc) { return ((int64_t)utc + 8 * 3600) / 86400; }

// The display fonts cover ASCII. Reject unsupported text rather than storing an unreadable daily note.
inline bool cleanIntention(const char *in, char *out, size_t n) {
  while (*in == ' ') in++;
  size_t len = strlen(in);
  while (len && in[len - 1] == ' ') len--;
  if (len >= n) return false;
  for (size_t i = 0; i < len; i++)
    if ((unsigned char)in[i] < 32 || (unsigned char)in[i] > 126) return false;
  memcpy(out, in, len);
  out[len] = 0;
  return true;
}

enum Priority { PRIORITY_NONE, PRIORITY_OFFLINE, PRIORITY_WEATHER_STALE, PRIORITY_BUS_STALE, PRIORITY_AIR, PRIORITY_RAIN, PRIORITY_COUNT };
constexpr uint8_t priorityFlag(Priority p) { return (uint8_t)(1u << p); }

inline uint8_t priorityMask(bool wifi, bool weatherFresh, bool busFresh, bool morning, int psi,
                            bool psiFresh, bool severeWeather) {
  uint8_t mask = 0;
  if (!wifi) mask |= priorityFlag(PRIORITY_OFFLINE);
  else {
    if (!weatherFresh) mask |= priorityFlag(PRIORITY_WEATHER_STALE);
    if (morning && !busFresh) mask |= priorityFlag(PRIORITY_BUS_STALE);
  }
  if (psiFresh && psi >= 101) mask |= priorityFlag(PRIORITY_AIR);
  if (weatherFresh && severeWeather) mask |= priorityFlag(PRIORITY_RAIN);
  return mask;
}

struct MiddlePanel {
  static constexpr uint32_t NOTICE_MS = 12000, REPEAT_MS = 120000;
  Priority showing = PRIORITY_NONE;
  int last = 0;
  uint32_t shownAt = 0, lastAt = 0;
  bool seen = false;

  Priority choose(uint32_t now, uint8_t eligible, int psi) {
    if (!eligible) {
      showing = PRIORITY_NONE, seen = false;
      return showing;
    }
    if (psi >= 201 && (eligible & priorityFlag(PRIORITY_AIR))) {
      showing = PRIORITY_AIR, last = PRIORITY_AIR;
      shownAt = lastAt = now, seen = true;
      return showing;
    }
    if (showing != PRIORITY_NONE && (eligible & priorityFlag(showing)) && now - shownAt < NOTICE_MS)
      return showing;
    showing = PRIORITY_NONE;
    if (seen && now - lastAt < REPEAT_MS) return showing;
    // Persistent advisories take turns, leaving most of the time for the normal card.
    for (int i = 1; i < PRIORITY_COUNT; i++) {
      int p = (last + i - 1) % (PRIORITY_COUNT - 1) + 1;
      if (eligible & priorityFlag((Priority)p)) {
        showing = (Priority)p, last = p;
        shownAt = lastAt = now, seen = true;
        break;
      }
    }
    return showing;
  }
};

// Days since 1970-01-01 for a civil date (Howard Hinnant's days_from_civil).
inline int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

// "2026-09-28T22:30:12+08:00" -> unix seconds. LTA sends "" when no bus is coming; that and junk give 0.
inline time_t parseIso8601(const char *s) {
  int Y, M, D, h, m, sec, oh = 0, om = 0;
  char sign = 'Z';
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d%c%d:%d", &Y, &M, &D, &h, &m, &sec, &sign, &oh, &om) < 6) return 0;
  int64_t off = sign == 'Z' ? 0 : (oh * 3600 + om * 60) * (sign == '-' ? -1 : 1);
  return (time_t)(daysFromCivil(Y, M, D) * 86400 + h * 3600 + m * 60 + sec - off);
}

constexpr int NO_BUS = INT_MIN;

// Whole minutes to arrival, truncated like LTA's own app (59 s away is "Arr").
inline int minutesUntil(time_t eta, time_t now) { return eta ? (int)((eta - now) / 60) : NO_BUS; }

// Label under each bus number. An ETA more than a minute in the past means the data went stale.
inline void busLabel(int mins, char *out, size_t n) {
  if (mins == NO_BUS || mins < -1) snprintf(out, n, "--");
  else if (mins <= 0) snprintf(out, n, "Arr");
  else snprintf(out, n, "%d min", mins);
}

enum Scene { SUN, MOON, PARTLY_SUN, PARTLY_MOON, CLOUDY, RAIN, HEAVY_RAIN, THUNDER, HAZE, WIND, SCENE_COUNT };

inline bool has(const char *hay, const char *needle) {
  for (; *hay; hay++) {
    size_t i = 0;
    while (needle[i] && tolower((unsigned char)hay[i]) == needle[i]) i++;
    if (!needle[i]) return true;
  }
  return false;
}

// NEA's forecast text ("Heavy Thundery Showers with Gusty Winds", "Fair (Night)", ...) -> animation.
inline Scene sceneFor(const char *forecast, int hour) {
  bool night = has(forecast, "night") || (!has(forecast, "(day)") && (hour < 7 || hour >= 19));
  if (has(forecast, "thunder")) return THUNDER;
  bool wet = has(forecast, "rain") || has(forecast, "shower") || has(forecast, "drizzle");
  if (wet) return has(forecast, "heavy") ? HEAVY_RAIN : RAIN;
  if (has(forecast, "partly")) return night ? PARTLY_MOON : PARTLY_SUN;
  if (has(forecast, "cloud") || has(forecast, "overcast")) return CLOUDY;
  if (has(forecast, "haz") || has(forecast, "mist") || has(forecast, "fog")) return HAZE;
  if (has(forecast, "wind")) return WIND;
  if (has(forecast, "fair") || has(forecast, "sunny") || has(forecast, "warm")) return night ? MOON : SUN;
  return CLOUDY;
}

inline const char *compactWeather(const char *forecast, Scene sc) {
  if (sc == THUNDER) return has(forecast, "gust") ? "Thundery, gusty" : "Thundery showers";
  if (sc == HEAVY_RAIN) return has(forecast, "gust") ? "Heavy rain, gusty" : "Heavy rain";
  return forecast;
}

template<class Measure>
inline void fitText(char *text, size_t capacity, int width, Measure measure) {
  if (!capacity || measure(text) <= width) return;
  size_t n = strlen(text);
  while (n) {
    do { n--; } while (n && ((unsigned char)text[n] & 0xc0) == 0x80);
    text[n] = 0;
    if (n + 4 <= capacity && measure(text) + measure("...") <= width) {
      memcpy(text + n, "...", 4);
      return;
    }
  }
}

// NEA's 24-hour PSI bands.
inline const char *psiBand(int psi) {
  if (psi < 0) return "";
  if (psi <= 50) return "Good";
  if (psi <= 100) return "Moderate";
  if (psi <= 200) return "Unhealthy";
  if (psi <= 300) return "Very unhealthy";
  return "Hazardous";
}

// Sensirion CRC-8 (poly 0x31, init 0xFF) used by the SHTC3.
inline uint8_t crc8(const uint8_t *d, int n) {
  uint8_t c = 0xFF;
  while (n--) {
    c ^= *d++;
    for (int b = 0; b < 8; b++) c = c & 0x80 ? (uint8_t)((c << 1) ^ 0x31) : (uint8_t)(c << 1);
  }
  return c;
}

// Quote for the slot containing `now`. Stepping by a stride coprime to the count visits every quote once per
// cycle but out of list order, so an author's lines don't arrive back to back.
inline int quoteAt(time_t now, int minutesPer, int count) {
  auto gcd = [](int a, int b) { while (b) { int t = a % b; a = b, b = t; } return a; };
  int stride = 17;
  while (gcd(stride, count) != 1) stride++;
  return (int)((int64_t)(now / (minutesPer * 60)) * stride % count);
}

// How many pushes ago `h` went into a ring of `cap` entries that has had `count` pushes (0 = the newest), or -1 if
// it isn't there: never pushed, or overwritten since. A value pushed twice counts from its latest push.
inline int ringAge(const uint32_t *ring, int count, int cap, uint32_t h) {
  int best = -1;
  for (int i = 0; i < (count < cap ? count : cap); i++)
    if (ring[i] == h && (best < 0 || (count - 1 - i) % cap < best)) best = (count - 1 - i) % cap;
  return best;
}

// Bus drive-by: glide in from the right, wait at the stop, pull away to the left (Singapore drives on the left).
constexpr uint32_t BUS_IN_MS = 1500, BUS_HOLD_MS = 1800, BUS_OUT_MS = 1400;
constexpr uint32_t BUS_ANIM_MS = BUS_IN_MS + BUS_HOLD_MS + BUS_OUT_MS;

inline int busX(uint32_t t, int screenW, int busW, int stopX) {
  if (t < BUS_IN_MS) {
    float p = 1 - t / (float)BUS_IN_MS;
    return stopX + (int)((screenW - stopX) * p * p * p);  // ease out
  }
  if (t < BUS_IN_MS + BUS_HOLD_MS) return stopX;
  float p = (t - BUS_IN_MS - BUS_HOLD_MS) / (float)BUS_OUT_MS;
  if (p > 1) p = 1;
  return stopX - (int)((stopX + busW) * p * p * p);  // ease in
}

inline int hour12(int h24) { return h24 % 12 ? h24 % 12 : 12; }

// Li-ion resting voltage -> charge, from a typical 18650 discharge curve (a straight line overstates the middle).
inline int batteryPercent(float v) {
  static const float V[] = {3.30f, 3.45f, 3.60f, 3.68f, 3.72f, 3.76f, 3.80f, 3.86f, 3.92f, 4.00f, 4.10f, 4.20f};
  static const int P[] = {0, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
  if (v <= V[0]) return 0;
  for (int i = 1; i < 12; i++)
    if (v < V[i]) return P[i - 1] + (int)((v - V[i - 1]) / (V[i] - V[i - 1]) * (P[i] - P[i - 1]) + 0.5f);
  return 100;
}

// Humidity the room really has, given a reading taken in air warmed from tRoom to tSensor. Warming doesn't change
// the moisture, only the saturation pressure, so RH scales by the ratio of the two (Magnus formula).
inline float rhAt(float rhSensor, float tSensor, float tRoom) {
  auto es = [](float t) { return 6.112f * expf(17.62f * t / (243.12f + t)); };
  float rh = rhSensor * es(tSensor) / es(tRoom);
  return rh > 100 ? 100 : rh < 0 ? 0 : rh;
}

// "Feels like": the Bureau of Meteorology's apparent temperature (Steadman 1994, shade, no sun), the one most weather
// sites quote. Humidity adds heat through vapour pressure (hPa), wind takes it away.
inline float feelsLike(float t, float rh, float windMs) {
  float e = rh / 100 * 6.105f * expf(17.27f * t / (237.7f + t));
  return t + 0.33f * e - 0.70f * windMs - 4.00f;
}

// LTA's crowding codes, in words.
inline const char *loadLabel(const char *code) {
  if (!strcmp(code, "SEA")) return "seats";
  if (!strcmp(code, "SDA")) return "standing";
  if (!strcmp(code, "LSD")) return "crowded";
  return "";
}

// ---- World clocks: UTC offsets with each zone's daylight-saving rule, no global TZ switching ----

inline int weekdayOf(int64_t days) { return (int)((days % 7 + 11) % 7); }  // 0 = Sunday; day 0 was a Thursday

inline void civilFromDays(int64_t z, int &y, int &m, int &d) {  // Hinnant's civil_from_days
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = (int)(doy - (153 * mp + 2) / 5 + 1);
  m = (int)(mp < 10 ? mp + 3 : mp - 9);
  y = (int)(yoe + era * 400) + (m <= 2);
}

inline int64_t nthSunday(int y, int m, int n) {  // as days since 1970
  int64_t first = daysFromCivil(y, m, 1);
  return first + (7 - weekdayOf(first)) % 7 + 7 * (n - 1);
}

inline int64_t lastSunday(int y, int m) {
  int64_t last = (m == 12 ? daysFromCivil(y + 1, 1, 1) : daysFromCivil(y, m + 1, 1)) - 1;
  return last - weekdayOf(last);
}

// US/Canada: 2nd Sun Mar to 1st Sun Nov, 02:00 local. EU/UK: last Sun Mar to last Sun Oct, 01:00 UTC.
// AU (NSW, Victoria, ACT, Tasmania): 1st Sun Oct to 1st Sun Apr. NZ: last Sun Sep to 1st Sun Apr. Southern rules
// switch at 02:00 local standard time.
enum DstRule { NO_DST, US_DST, EU_DST, AU_DST, NZ_DST };

struct WorldClock {
  const char *label;
  int stdOffsetMin;  // standard time, minutes east of UTC
  DstRule dst;
};

inline int zoneOffsetMin(const WorldClock &c, time_t utc) {
  int y, m, d;
  civilFromDays((int64_t)utc / 86400, y, m, d);
  const time_t twoAm = 2 * 3600 - c.stdOffsetMin * 60;  // 02:00 local standard time, in seconds after UTC midnight
  bool summer = false;
  switch (c.dst) {
    case NO_DST: break;
    case US_DST:  // ends at 02:00 daylight time, which is 01:00 standard
      summer = utc >= nthSunday(y, 3, 2) * 86400 + twoAm && utc < nthSunday(y, 11, 1) * 86400 + twoAm - 3600;
      break;
    case EU_DST: summer = utc >= lastSunday(y, 3) * 86400 + 3600 && utc < lastSunday(y, 10) * 86400 + 3600; break;
    case AU_DST: summer = utc >= nthSunday(y, 10, 1) * 86400 + twoAm || utc < nthSunday(y, 4, 1) * 86400 + twoAm; break;
    case NZ_DST: summer = utc >= lastSunday(y, 9) * 86400 + twoAm || utc < nthSunday(y, 4, 1) * 86400 + twoAm; break;
  }
  return c.stdOffsetMin + (summer ? 60 : 0);
}
