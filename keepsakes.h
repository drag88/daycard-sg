// Persistent garden snapshots and postcard metadata, also checked by the host tests.
#pragma once
#include "logic.h"

inline uint32_t gardenMonth(time_t now) {
  if (now < 1700000000) return 0;
  int y, m, d;
  civilFromDays(sgtDay(now), y, m, d);
  return y <= 9999 ? y * 100 + m : 0;
}

struct GardenState {
  uint32_t total = 0, month = 0, monthly = 0;
  bool earn(time_t now) {
    if (total == UINT32_MAX) return false;
    total++;
    uint32_t key = gardenMonth(now);
    if (!key) return true;  // an unset clock earns a flower without erasing dated progress
    if (month != key) month = key, monthly = 0;
    if (key) monthly++;
    return true;
  }
  uint32_t thisMonth(time_t now) const { return month == gardenMonth(now) ? monthly : 0; }
  uint32_t checksum() const { return 0x67617264u ^ total ^ month * 2654435761u ^ monthly * 2246822519u; }
  void record(char *out, size_t n) const {
    snprintf(out, n, "v1,%u,%u,%u,%08x", (unsigned)total, (unsigned)month, (unsigned)monthly, (unsigned)checksum());
  }
};

inline bool parseGardenRecord(const char *line, GardenState &out) {
  unsigned total, month, monthly, check;
  int end = 0;
  if (sscanf(line, "v1,%u,%u,%u,%x%n", &total, &month, &monthly, &check, &end) != 4 || line[end]) return false;
  if (monthly > total || (!month && monthly) || (month && (month / 100 < 2023 || month / 100 > 9999 ||
      month % 100 < 1 || month % 100 > 12))) return false;
  GardenState got = {total, month, monthly};
  if (got.checksum() != check) return false;
  out = got;
  return true;
}

struct GardenLogReader {
  char line[80] = {};
  size_t n = 0;
  bool overflow = false;
  void push(char ch, GardenState &state) {
    if (ch == '\n') {
      if (n && line[n - 1] == '\r') n--;
      line[n] = 0;
      GardenState got;
      if (!overflow && parseGardenRecord(line, got) && got.total >= state.total) state = got;
      n = 0, overflow = false;
    } else if (n < sizeof line - 1) line[n++] = ch;
    else overflow = true;
  }
};

struct MemoryCaption { char date[11] = {}, caption[57] = {}; };

inline bool memoryDate(const char *text, int &y, int &m, int &d) {
  size_t n = strlen(text);
  if ((n != 7 && n != 10) || text[4] != '-' || (n == 10 && text[7] != '-')) return false;
  for (size_t i = 0; i < n; i++)
    if (i != 4 && i != 7 && !isdigit((unsigned char)text[i])) return false;
  y = (text[0] - '0') * 1000 + (text[1] - '0') * 100 + (text[2] - '0') * 10 + text[3] - '0';
  m = (text[5] - '0') * 10 + text[6] - '0';
  d = n == 10 ? (text[8] - '0') * 10 + text[9] - '0' : 0;
  if (y < 1900 || m < 1 || m > 12) return false;
  int days = (int)((m == 12 ? daysFromCivil(y + 1, 1, 1) : daysFromCivil(y, m + 1, 1)) - daysFromCivil(y, m, 1));
  return n == 7 || (d >= 1 && d <= days);
}

inline bool cleanMemory(const char *date, const char *caption, MemoryCaption &out) {
  int y, m, d;
  MemoryCaption got;
  if (!memoryDate(date, y, m, d) || !cleanIntention(caption, got.caption, sizeof got.caption) || !*got.caption) return false;
  memcpy(got.date, date, strlen(date) + 1);
  out = got;
  return true;
}

inline int memorySeason(const char *date, time_t now) {
  int y, m, d, cy, cm, cd;
  if (now < 1700000000 || !memoryDate(date, y, m, d)) return 0;
  civilFromDays(sgtDay(now), cy, cm, cd);
  if (m != cm || y > cy || (y == cy && d > cd)) return 0;
  return d && d == cd ? 2 : 1;
}

inline void memoryDateLabel(const char *date, char *out, size_t n) {
  static const char *const months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  int y, m, d;
  if (!memoryDate(date, y, m, d)) { if (n) *out = 0; return; }
  if (d) snprintf(out, n, "%d %s %d", d, months[m - 1], y);
  else snprintf(out, n, "%s %d", months[m - 1], y);
}

// USB uploads stay within /memories, with bounded ASCII filenames and JPEG/sidecar extensions.
inline bool memoryFilename(const char *name) {
  size_t n = strlen(name);
  if (!n || n > 80 || name[0] == '.') return false;
  for (size_t i = 0; i < n; i++)
    if (!isalnum((unsigned char)name[i]) && name[i] != '-' && name[i] != '_' && name[i] != '.') return false;
  if (strstr(name, "..")) return false;
  if (n > 5 && !strcmp(name + n - 5, ".json")) n -= 5;
  return (n > 4 && !strncmp(name + n - 4, ".jpg", 4)) || (n > 5 && !strncmp(name + n - 5, ".jpeg", 5));
}
