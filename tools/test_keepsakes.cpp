// c++ -std=c++17 tools/test_keepsakes.cpp -o /tmp/test_keepsakes && /tmp/test_keepsakes
#include <assert.h>
#include <initializer_list>
#include "../keepsakes.h"

int main() {
  auto sgt = [](int y, int m, int d, int h = 0) { return (time_t)(daysFromCivil(y, m, d) * 86400 + (h - 8) * 3600); };
  GardenState garden;
  assert(garden.earn(sgt(2026, 10, 31, 23)) && garden.total == 1 && garden.monthly == 1);
  garden.earn(sgt(2026, 10, 31, 23));
  assert(garden.thisMonth(sgt(2026, 10, 31)) == 2);
  assert(garden.thisMonth(sgt(2026, 11, 1)) == 0);
  garden.earn(sgt(2026, 11, 1));
  assert(garden.total == 3 && garden.month == 202611 && garden.monthly == 1);
  char record[80];
  garden.record(record, sizeof record);
  GardenState restored;
  assert(parseGardenRecord(record, restored) && restored.total == 3 && restored.monthly == 1);
  assert(!parseGardenRecord("v1,3,202611,1,00000000", restored));
  record[4] = '9';
  assert(!parseGardenRecord(record, restored) && restored.total == 3);
  garden.record(record, sizeof record);
  record[strlen(record) - 1] = 0;
  assert(!parseGardenRecord(record, restored));
  garden.record(record, sizeof record);
  strcat(record, "junk");
  assert(!parseGardenRecord(record, restored));
  garden.earn(0);
  assert(garden.total == 4 && garden.month == 202611 && garden.monthly == 1);
  GardenState recovered;
  GardenLogReader reader;
  auto feed = [&](const char *text) { for (; *text; text++) reader.push(*text, recovered); };
  garden.record(record, sizeof record);
  feed(record);
  assert(recovered.total == 0);  // a write ending before its newline is not committed
  feed("\r\n");
  assert(recovered.total == 4);
  feed("v1,5,202611");  // torn append; the next writer always starts with a newline
  feed("\n");
  assert(recovered.total == 4);
  garden.earn(sgt(2026, 11, 2));
  garden.record(record, sizeof record);
  feed(record); feed("\n");
  assert(recovered.total == 5 && recovered.monthly == 2);
  restored.record(record, sizeof record);  // older valid snapshots cannot roll progress back
  feed(record); feed("\n");
  assert(recovered.total == 5);
  for (int i = 0; i < 100; i++) feed("x");
  feed("\n");
  garden.earn(sgt(2026, 11, 3)); garden.record(record, sizeof record);
  feed(record); feed("\n");
  assert(recovered.total == 6);
  garden = {UINT32_MAX, 202610, 1};
  assert(!garden.earn(sgt(2026, 10, 24)) && garden.total == UINT32_MAX);

  MemoryCaption memory;
  assert(cleanMemory("2024-09", "  Beach trip  ", memory) && !strcmp(memory.caption, "Beach trip"));
  assert(cleanMemory("2024-02-29", "Leap day", memory));
  for (const char *bad : {"2023-02-29", "2026-04-31", "2024-00", "2024-13", "2024-09-00", "2024-09-32",
                          "24-09", "2024-9", "2024-09junk", "2024-09-1x", "2024/09", "1899-12"})
    assert(!cleanMemory(bad, "A trip", memory));
  assert(!cleanMemory("2024-09", "", memory));
  assert(!cleanMemory("2024-09", "a\nb", memory));
  assert(!cleanMemory("2024-09", "\xe4\xb8\xad", memory));
  assert(memorySeason("2024-09", sgt(2026, 9, 24)) == 1);
  assert(memorySeason("2024-09-24", sgt(2026, 9, 24)) == 2);
  assert(memorySeason("2024-09-24", sgt(2026, 9, 24) - 1) == 1);
  assert(memorySeason("2024-09", sgt(2026, 10, 1)) == 0);
  assert(memorySeason("2027-09", sgt(2026, 9, 24)) == 0);
  assert(memorySeason("2026-09-25", sgt(2026, 9, 24)) == 0);
  assert(memorySeason("2024-09", 0) == 0);
  memoryDateLabel("2024-09", record, sizeof record);
  assert(!strcmp(record, "SEP 2024"));
  memoryDateLabel("2024-09-24", record, sizeof record);
  assert(!strcmp(record, "24 SEP 2024"));
  for (const char *name : {"memory-123.jpg", "memory-123.jpg.json", "a.jpeg", "a.jpeg.json"}) assert(memoryFilename(name));
  for (const char *name : {"", ".jpg", "../a.jpg", "a/../b.jpg", "a\\b.jpg", "a..jpg", "x.json", "a.jpg.jsonjunk",
                           "a.jpg\nx", "a.JPG", "a .jpg"}) assert(!memoryFilename(name));
  // One award even if the display resumes after both work and break have elapsed; cancelled work earns none.
  FocusSession focus;
  focus.start(UINT32_MAX - 1000);
  FocusPhase before = focus.phase;
  assert(focus.update((uint32_t)(UINT32_MAX - 1000 + 31 * 60000u)));
  assert(before == FOCUS_WORK && focus.phase == FOCUS_DONE);
  assert(!focus.update((uint32_t)(UINT32_MAX - 1000 + 32 * 60000u)));
  focus.start(0); focus.stop();
  assert(!focus.update(31 * 60000u));
  puts("keepsakes ok");
}
