// Daycard: a Singapore desk card for the Waveshare ESP32-S3-RLCD-4.2.
// Clock, NEA rain nowcast with a live weather animation, outside and indoor climate, UV and PSI,
// a sourced quote from Wikiquote every half hour, and live LTA bus arrivals with a drive-by when a bus is a minute out.
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_sntp.h>
#include <esp_task_wdt.h>
#include <Wire.h>
#include <math.h>
#include <mutex>
#include "ST7305_U8g2.h"
#include "assets.h"
#include "quote.h"
#include "audio.h"
#if __has_include("config.local.h")
#include "config.local.h"  // your own settings, kept out of git
#else
#include "config.h"
#endif
#include "voice.h"
#include "esp_mn_speech_commands.h"
#include "photos.h"
#include "logic.h"

constexpr int W = 400, H = 300, MARGIN = 12;
constexpr int PIN_KEY = 18, PIN_BOOT = 0, PIN_SDA = 13, PIN_SCL = 14, PIN_BATTERY = 4;
constexpr int PIN_SD_CLK = 38, PIN_SD_CMD = 21, PIN_SD_D0 = 39;  // microSD, 1-bit SDMMC
constexpr int NBUS = sizeof(BUS_SERVICES) / sizeof(*BUS_SERVICES);
static_assert(NBUS >= 1 && NBUS <= 5, "BUS_SERVICES in config.h: list 1 to 5 services, the bus board has five rows");
static_assert(sizeof WORLD_CLOCKS / sizeof *WORLD_CLOCKS == 3, "WORLD_CLOCKS in config.h: list exactly three clocks");
constexpr uint32_t FRAME_MS = 40;  // 25 fps; the panel manages 80

static const char *DAYS[] = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"};
static const char *MONTHS[] = {"JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY",
                               "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"};

static ST7305_U8g2 lcd(11, 12, 5, 40, 41);
static U8G2 *g;
static Preferences prefs;
static WiFiManager wm;
static WiFiManagerParameter keyParam("lta", "LTA DataMall AccountKey (leave blank to keep the saved one)", "", 64);
static char apPass[9];
static bool portalActive = false, portalForced = false;  // forced = opened with KEY, possibly while online
static uint32_t closePortalAt = 0;                       // set after the key is saved
static String ltaKey;

struct Bus {
  time_t eta[3] = {};    // next three arrivals, 0 = none
  char type[3][3] = {};  // SD single deck, DD double deck, BD bendy
  char load[3][4] = {};  // SEA seats, SDA standing, LSD crowded
  bool armed = false;    // set while the next bus is 2+ minutes out, so each arrival animates once
};

// Shared by the network task and renderer; always under M.
static struct {
  String nowcast, nowcastPeriod;
  int uv = -1, psi = -1, pm25 = -1;
  float inT = NAN, inH = NAN, batV = NAN;
  float airT = NAN, airH = NAN, windMs = NAN;  // outside, from AIR_STATIONS
  float rawT = NAN, rawH = NAN, firstT = NAN, firstH = NAN;  // sensor as read; first = right after boot
  String periodLabel[3], periodText[3];
  String outDay[4], outText[4];
  int outLo[4] = {}, outHi[4] = {};
  Bus bus[NBUS];
  bool busLive = false;
  uint32_t busAt = 0, weatherAt = 0, psiAt = 0;
} S;
static std::mutex M;

// KEY steps through views; bus, room and forecast return after a minute.
enum View { V_CARD, V_BUSES, V_ROOM, V_FORECAST, V_PHOTOS, V_FOCUS, V_GARDEN, V_COUNT };
static int view = V_CARD;
static uint32_t viewUntil = 0;

static FocusSession focus;
static GardenState garden;
static bool gardenSaved = true;
static uint32_t toastUntil = 0, celebrateAt = 0;
static char toastText[64], intention[57] = {};
static int64_t intentionDay = -1;
static Routine routine = ROUTINE_WORK;

// Room history: one sample a minute, raw sensor values so a later calibration re-colours the whole chart.
struct Sample {
  uint32_t t;
  float rawT, rawH, batV;
};
constexpr int HIST = 1440;  // 24 h
static Sample *hist;        // ring buffer in PSRAM, guarded by M
static int histHead = 0, histCount = 0;
static bool sdOk = false, audioOk = false, voiceOk = false;
static const char *ROOM_LOG = "/daycard/room.csv";  // fixed-width lines, so the last day is one seek away
static const char *GARDEN_LOG = "/daycard/garden.log";
constexpr int LOG_LINE = 28;

static bool saveGarden() {  // one atomic NVS value, plus a recoverable journal on the card
  char record[80];
  garden.record(record, sizeof record);
  bool saved = prefs.putString("garden-v1", record) == strlen(record);
  if (sdOk) {
    File f = SD_MMC.open(GARDEN_LOG, FILE_APPEND);
    if (f) {
      // A newline separates a new snapshot from a write interrupted by power loss.
      bool separated = f.print('\n') == 1;
      size_t written = f.println(record);
      f.flush();
      saved |= separated && written == strlen(record) + 2;
    }
  }
  return saved;
}

static void loadGarden() {
  GardenState got;
  if (parseGardenRecord(prefs.getString("garden-v1", "").c_str(), got)) garden = got;
  uint32_t onboard = garden.total;
  if (sdOk) {
    File f = SD_MMC.open(GARDEN_LOG);
    if (f) {
      if (f.size() > 4096) { f.seek(f.size() - 4096); f.readStringUntil('\n'); }
      GardenLogReader reader;
      while (f.available()) reader.push(f.read(), garden);
    }
  }
  if (garden.total > onboard) {
    char record[80];
    garden.record(record, sizeof record);
    prefs.putString("garden-v1", record);
  }
}

static void remember(const Sample &s) {  // caller holds M
  hist[histHead] = s;
  histHead = (histHead + 1) % HIST;
  if (histCount < HIST) histCount++;
}

static const Sample &sampleAt(int i) { return hist[(histHead - histCount + i + HIST) % HIST]; }  // 0 = oldest

static void loadHistory() {
  File f = SD_MMC.open(ROOM_LOG);
  if (!f) return;
  size_t back = (size_t)LOG_LINE * HIST;
  f.seek(f.size() > back ? f.size() - back - 1 : 0);
  if (f.position()) f.readStringUntil('\n');  // land on a line start even if a write was cut short
  while (f.available()) {
    String line = f.readStringUntil('\n');
    unsigned long t;
    Sample s;
    if (sscanf(line.c_str(), "%lu,%f,%f,%f", &t, &s.rawT, &s.rawH, &s.batV) == 4 && t > 1700000000)
      s.t = t, remember(s);
  }
}

// ---------- network ----------

static bool getJson(const String &url, JsonDocument &doc, const char *accountKey = nullptr) {
  NetworkClientSecure tls;
  tls.useBuiltinCACertBundle();
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(tls, url)) return false;
  if (accountKey) http.addHeader("AccountKey", accountKey);
  int code = http.GET();
  bool ok = code == 200 && !deserializeJson(doc, http.getString());
  if (!ok) Serial.printf("GET %s -> %d\n", url.c_str(), code);  // never log the key
  http.end();
  return ok;
}

static const String NEA = "https://api-open.data.gov.sg/v2/real-time/api/";

static bool fetchBuses() {
  String key;
  {
    std::lock_guard<std::mutex> l(M);  // setup can replace the key from the other core
    key = ltaKey;
  }
  if (key.isEmpty()) return true;
  JsonDocument doc;
  if (!getJson(String("https://datamall2.mytransport.sg/ltaodataservice/v3/BusArrival?BusStopCode=") + BUS_STOP, doc,
               key.c_str()))
    return false;
  Bus got[NBUS];
  static const char *next[3] = {"NextBus", "NextBus2", "NextBus3"};
  Serial.print("buses at stop:");
  for (JsonObject s : doc["Services"].as<JsonArray>()) {
    Serial.printf(" %s", (const char *)(s["ServiceNo"] | "?"));
    for (int i = 0; i < NBUS; i++)
      if (s["ServiceNo"] == BUS_SERVICES[i])
        for (int k = 0; k < 3; k++) {
          JsonObject b = s[next[k]];
          got[i].eta[k] = parseIso8601(b["EstimatedArrival"] | "");
          strlcpy(got[i].type[k], b["Type"] | "SD", sizeof got[i].type[k]);
          strlcpy(got[i].load[k], b["Load"] | "", sizeof got[i].load[k]);
        }
  }
  Serial.println();
  std::lock_guard<std::mutex> l(M);
  for (int i = 0; i < NBUS; i++) {
    bool armed = S.bus[i].armed;
    S.bus[i] = got[i];
    S.bus[i].armed = armed;
  }
  S.busLive = true, S.busAt = millis();
  return true;
}

static bool fetchNowcast() {
  JsonDocument doc;
  if (!getJson(NEA + "two-hr-forecast", doc)) return false;
  JsonObject item = doc["data"]["items"][0];
  for (JsonObject f : item["forecasts"].as<JsonArray>())
    if (f["area"] == NEA_AREA) {
      std::lock_guard<std::mutex> l(M);
      S.nowcast = f["forecast"].as<String>();
      S.nowcastPeriod = item["valid_period"]["text"].as<String>();
      if (S.nowcast.isEmpty()) return false;
      S.weatherAt = millis();
      return true;
    }
  return false;
}

static bool fetchUv() {
  JsonDocument doc;
  if (!getJson(NEA + "uv", doc)) return false;
  JsonVariant v = doc["data"]["records"][0]["index"][0]["value"];  // newest hour first
  std::lock_guard<std::mutex> l(M);
  S.uv = v.isNull() ? -1 : v.as<int>();
  return true;
}

static bool fetchPsi() {
  JsonDocument doc;
  if (!getJson(NEA + "psi", doc)) return false;
  JsonObject r = doc["data"]["items"][0]["readings"];
  std::lock_guard<std::mutex> l(M);
  S.psi = r["psi_twenty_four_hourly"][REGION] | -1;
  S.pm25 = r["pm25_twenty_four_hourly"][REGION] | -1;
  if (S.psi < 0) return false;
  S.psiAt = millis();
  return true;
}

// The newest one-minute reading from the first of AIR_STATIONS that reported one.
static float stationValue(JsonDocument &doc) {
  JsonArray data = doc["data"]["readings"][0]["data"];
  for (const char *id : AIR_STATIONS)
    for (JsonObject r : data)
      if (r["stationId"] == id) return r["value"].as<float>();
  return NAN;
}

static bool fetchOutside() {
  static const char *const feeds[3] = {"air-temperature", "relative-humidity", "wind-speed"};
  float v[3];
  for (int i = 0; i < 3; i++) {
    JsonDocument doc;
    if (!getJson(NEA + feeds[i], doc)) return false;
    v[i] = stationValue(doc);
  }
  std::lock_guard<std::mutex> l(M);
  S.airT = v[0], S.airH = v[1], S.windMs = v[2] * 0.5144f;  // NEA reports wind in knots
  return true;
}

static bool fetchOutlook() {
  JsonDocument day, four;
  if (!getJson(NEA + "twenty-four-hr-forecast", day) || !getJson(NEA + "four-day-outlook", four)) return false;
  std::lock_guard<std::mutex> l(M);
  int i = 0;
  for (JsonObject p : day["data"]["records"][0]["periods"].as<JsonArray>()) {
    if (i == 3) break;
    S.periodLabel[i] = p["timePeriod"]["text"].as<String>();
    S.periodText[i++] = p["regions"][REGION]["text"].as<String>();
  }
  i = 0;
  for (JsonObject f : four["data"]["records"][0]["forecasts"].as<JsonArray>()) {
    if (i == 4) break;
    S.outDay[i] = f["day"].as<String>();
    S.outText[i] = f["forecast"]["summary"].as<String>();
    S.outText[i].trim();
    S.outLo[i] = f["temperature"]["low"] | 0;
    S.outHi[i++] = f["temperature"]["high"] | 0;
  }
  return true;
}

static bool shtc(uint16_t cmd) {
  Wire.beginTransmission(0x70);
  Wire.write(cmd >> 8);
  Wire.write(cmd & 0xFF);
  return Wire.endTransmission() == 0;
}

static bool readIndoor() {
  uint8_t b[6];
  if (!shtc(0x3517)) return false;  // wake
  delay(1);
  shtc(0x7866);  // measure, temperature first, no clock stretching
  delay(20);
  bool got = Wire.requestFrom(0x70, 6) == 6;
  for (auto &x : b) x = Wire.read();
  shtc(0xB098);  // sleep
  if (!got || crc8(b, 2) != b[2] || crc8(b + 3, 2) != b[5]) return false;
  std::lock_guard<std::mutex> l(M);
  S.rawT = 175.0f * ((b[0] << 8) | b[1]) / 65536.0f - 45.0f;
  S.rawH = 100.0f * ((b[3] << 8) | b[4]) / 65536.0f;
  if (isnan(S.firstT)) S.firstT = S.rawT, S.firstH = S.rawH;
  S.inT = S.rawT + TEMP_OFFSET_C;
  S.inH = rhAt(S.rawH, S.rawT, S.inT) + RH_OFFSET;
  return true;
}

static bool readBattery() {
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(PIN_BATTERY);
  float v = mv / 16.0f * BATTERY_DIVIDER / 1000;
  Serial.printf("battery %.2f V\n", v);
  std::lock_guard<std::mutex> l(M);
  S.batV = v < 2.5f ? NAN : isnan(S.batV) ? v : S.batV * 0.7f + v * 0.3f;  // below 2.5 V: no cell fitted
  return true;
}

static bool logRoom() {
  time_t now = time(nullptr);
  Sample s;
  {
    std::lock_guard<std::mutex> l(M);
    if (now < 1700000000 || isnan(S.rawT)) return true;
    s = {(uint32_t)now, S.rawT, S.rawH, isnan(S.batV) ? 0 : S.batV};
    remember(s);
  }
  if (!sdOk) return true;
  File f = SD_MMC.open(ROOM_LOG, FILE_APPEND);
  if (!f) return true;
  if (!f.size()) f.print("epoch     ,rawT ,rawRH,batV\n");
  f.printf("%10lu,%5.1f,%5.1f,%4.2f\n", (unsigned long)s.t, s.rawT, s.rawH, s.batV);
  f.close();
  return true;
}

// Photos decode on the network core while the photo view is up; the renderer only ever sees a finished one.
static uint8_t *photo, *photoSpare;  // 1-bit frames in PSRAM; photo is guarded by M
static bool photoReady = false;
static String photoPath, photoCredit;
static MemoryCaption photoMemory;
static uint32_t photoDueAt = 0;
static char photoNote[64] = "Loading photo...";

static bool photoJob() {
  if (photoReady && (view != V_PHOTOS || (int32_t)(millis() - photoDueAt) < 0)) return true;  // first one decodes early
  photoDueAt = millis() + PHOTO_MINUTES * 60000;
  if (!sdOk) return strlcpy(photoNote, "No memory card", sizeof photoNote), true;
  static bool preferMemory = true, seasonal = true;
  String avoid = photoPath;
  for (int tries = 0; tries < 5; tries++) {  // skip anything the decoder can't read (progressive JPEGs, renamed HEICs)
    bool personal = tries % 2 ? !preferMemory : preferMemory;
    MemoryCaption caption;
    String path = personal ? randomMemory(avoid, time(nullptr), seasonal, caption) : randomPhoto(avoid);
    if (path.isEmpty()) {
      personal = !personal;
      path = personal ? randomMemory(avoid, time(nullptr), seasonal, caption) : randomPhoto(avoid);
    }
    if (path.isEmpty()) break;
    if (loadPhoto(path.c_str(), photoSpare, personal ? 230 : PH)) {
      std::lock_guard<std::mutex> l(M);
      std::swap(photo, photoSpare);
      photoReady = true, photoPath = path, photoCredit = creditFor(path);
      photoMemory = personal ? caption : MemoryCaption{};
      preferMemory = !personal;
      if (personal) seasonal = !seasonal;
      Serial.printf("photo %s\n", path.c_str());
      return true;
    }
    Serial.printf("photo %s: can't decode\n", path.c_str());
    avoid = path;
  }
  strlcpy(photoNote, "Add JPEGs to photos, or use tools/memory.py", sizeof photoNote);
  return true;
}

// Live quotes. Set on the network core into a ring of three buffers, so the one being set is never the one on
// screen or the one fading out; until the first arrives the card shows the built-in quotes.
static uint8_t *quoteBufs[3];
static Bitmap liveQuote = {};              // guarded by M; no bits until the first fetch
static volatile bool quoteWanted = false;  // "Flubber, next quote" or serial 'q'
static uint32_t quoteSeen[512];            // recent quotes, as hashes, so none comes back for about ten days
static int quoteSeenCount = 0;
static const char *QUOTE_LOG = "/daycard/quotes.log";  // every quote shown: epoch, hash, text | author

static uint32_t fnv1a(const char *s) {
  uint32_t h = 2166136261u;
  while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
  return h;
}

static void loadSeenQuotes() {  // the nightly restart would otherwise forget what's been shown
  File f = SD_MMC.open(QUOTE_LOG);
  if (!f) return;
  if (f.size() > 128 * 1024) f.seek(f.size() - 128 * 1024), f.readStringUntil('\n');
  while (f.available()) {
    unsigned long t, h;
    if (sscanf(f.readStringUntil('\n').c_str(), "%lu %lx", &t, &h) == 2) quoteSeen[quoteSeenCount++ % 512] = h;
  }
}

// One quote from an author's page, read line by line as it streams in (pages run to 450 KB). Every sourced line
// that fits the card and hasn't shown lately is a candidate; reservoir sampling keeps one without storing the rest.
// An author with only a few short lines runs out well inside the ten days; then the line of theirs shown longest
// ago comes back, rather than the author sitting out (and costing a minute's retry) until their lines age off.
static bool fetchQuote(int author, char *text, size_t n) {
  NetworkClientSecure tls;
  tls.useBuiltinCACertBundle();
  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);  // no chunked encoding, so the stream is the page itself
  http.setUserAgent("daycard/1.0 (ESP32 desk clock)");  // Wikimedia asks for a descriptive agent
  const char *page = QUOTE_AUTHORS[author][0], *name = QUOTE_AUTHORS[author][1];
  if (!http.begin(tls, String("https://en.wikiquote.org/w/index.php?action=raw&title=") + page)) return false;
  int code = http.GET();
  int found = 0, staleAge = -1;
  if (code == 200) {
    NetworkClient *s = http.getStreamPtr();
    static char line[1024], q[400];
    bool cut = false;  // the previous read filled the buffer, so this one is the rest of an overlong line
    while (s->connected() || s->available()) {
      size_t len = s->readBytesUntil('\n', line, sizeof line - 1);
      line[len] = 0;
      bool whole = !cut;
      cut = len == sizeof line - 1;
      if (!whole || cut) continue;
      if (wikiquoteStop(line)) break;
      bool fits = wikiquoteText(line, q, sizeof q) && setQuote(q, name, nullptr, nullptr);
      if (!fits) fits = wikiquoteText(line, q, sizeof q, true) && setQuote(q, name, nullptr, nullptr);
      if (!fits) continue;
      int age = ringAge(quoteSeen, quoteSeenCount, 512, fnv1a(q));
      if (age < 0 && esp_random() % ++found == 0) strlcpy(text, q, n);
      else if (age >= 0 && !found && age > staleAge) staleAge = age, strlcpy(text, q, n);
    }
  }
  http.end();
  Serial.printf("quote: %s -> %d, %d new lines that fit", page, code, found);
  if (!found && staleAge >= 0) Serial.printf("; all shown lately, reusing the one from %d quotes ago", staleAge);
  Serial.println();
  return found > 0 || staleAge >= 0;
}

static bool quoteJob() {  // runs every second and keeps its own time, so a failure retries within a minute
  constexpr int NAUTHORS = sizeof QUOTE_AUTHORS / sizeof *QUOTE_AUTHORS;
  static long slot = -1;
  static uint32_t retryAt = 0;
  static int recent[8] = {-1, -1, -1, -1, -1, -1, -1, -1}, recentAt = 0;  // the same voice doesn't come round too soon
  time_t now = time(nullptr);
  long due = now / (QUOTE_MINUTES * 60);
  if (WiFi.status() != WL_CONNECTED || now < 1700000000 || !quoteBufs[2]) return true;
  if ((int32_t)(millis() - retryAt) < 0 || (!quoteWanted && due == slot)) return true;
  retryAt = millis() + 60000;  // a failed fetch waits a minute; a good one clears this below
  int author;
  bool again;
  do {
    author = esp_random() % NAUTHORS, again = false;
    for (int r : recent) again |= r == author;
  } while (again);
  static char text[400];
  if (!fetchQuote(author, text, sizeof text)) return true;
  static int ring = 0;
  ring = (ring + 1) % 3;
  Bitmap b;
  if (!setQuote(text, QUOTE_AUTHORS[author][1], quoteBufs[ring], &b)) return true;  // fetchQuote checked it fits
  {
    std::lock_guard<std::mutex> l(M);
    liveQuote = b;
  }
  slot = due, quoteWanted = false, retryAt = millis();
  recent[recentAt++ % 8] = author;
  uint32_t h = fnv1a(text);
  quoteSeen[quoteSeenCount++ % 512] = h;
  Serial.printf("quote: %s | %s\n", text, QUOTE_AUTHORS[author][1]);
  if (sdOk)
    if (File f = SD_MMC.open(QUOTE_LOG, FILE_APPEND))
      f.printf("%lu %08lx %s | %s\n", (unsigned long)now, (unsigned long)h, text, QUOTE_AUTHORS[author][1]), f.close();
  return true;
}

// ---------- self-healing ----------

// The PCF85063 RTC at 0x51 keeps UTC across restarts, so the clock survives one while Wi-Fi is down.
// (Across a full power-off only if a backup cell is fitted to the RTC BAT header.)
static uint8_t bcd(int v) { return (uint8_t)((v / 10) << 4 | v % 10); }
static int unbcd(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }

static bool rtcWrite(time_t utc) {
  int y, m, d;
  civilFromDays(utc / 86400, y, m, d);
  int s = utc % 86400;
  Wire.beginTransmission(0x51);
  Wire.write(0x04);  // seconds first; writing it also clears the oscillator-stop flag
  for (int v : {s % 60, s / 60 % 60, s / 3600, d}) Wire.write(bcd(v));
  Wire.write(weekdayOf(utc / 86400)), Wire.write(bcd(m)), Wire.write(bcd(y - 2000));
  return Wire.endTransmission() == 0;
}

static time_t rtcRead() {  // 0 if the RTC lost power or reads nonsense
  Wire.beginTransmission(0x51);
  Wire.write(0x04);
  if (Wire.endTransmission(false) || Wire.requestFrom(0x51, 7) != 7) return 0;
  uint8_t r[7];
  for (auto &b : r) b = Wire.read();
  int y = 2000 + unbcd(r[6]), mo = unbcd(r[5] & 0x1F), d = unbcd(r[3] & 0x3F);
  if (r[0] & 0x80 || y < 2025 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  return (time_t)(daysFromCivil(y, mo, d) * 86400 + unbcd(r[2] & 0x3F) * 3600 + unbcd(r[1] & 0x7F) * 60 + unbcd(r[0] & 0x7F));
}

static volatile bool ntpSynced = false;
static void onNtp(struct timeval *) { ntpSynced = true; }

// Restarts are logged to the card with their reason, so a self-heal leaves a trail.
static void noteRestart(const char *why) {
  Serial.printf("restart: %s\n", why);
  time_t now = time(nullptr);
  if (sdOk)
    if (File f = SD_MMC.open("/daycard/restarts.log", FILE_APPEND)) f.printf("%lu %s\n", (unsigned long)now, why), f.close();
}

static void restartNow(const char *why) {
  noteRestart(why);
  delay(200);
  ESP.restart();
}

static uint32_t lastNetOk = 0;  // last success of any internet feed; set by the scheduler

// Checked every 30 s. The ESP32's own auto-reconnect once gave up for 6 hours, so: rejoin Wi-Fi ourselves, rebuild
// the connection if feeds go quiet, restart as the last resort or when memory runs low, and once a night anyway.
static bool health() {
  static bool wasUp = false, rebuilt = false;  // boot starts offline, so the first log line is "back"
  static uint32_t downSince = 0;
  bool up = WiFi.status() == WL_CONNECTED;
  if (up != wasUp) Serial.printf(up ? "wifi: back, %d dBm\n" : "wifi: lost (status %d)\n", up ? WiFi.RSSI() : WiFi.status());
  wasUp = up;
  if (up || portalActive) downSince = 0;
  else if (!downSince) downSince = millis();  // give a connect in progress (boot, a blip) its own chance first
  else if (millis() - downSince > 20000) WiFi.disconnect(false), WiFi.begin();  // then retry the saved network
  uint32_t quiet = millis() - lastNetOk;
  if (quiet < 20 * 60000UL) rebuilt = false;
  else if (!rebuilt && !portalActive) rebuilt = true, Serial.println("health: no data for 20 min, rebuilding Wi-Fi"),
                                       WiFi.disconnect(true), delay(500), WiFi.begin();
  if (quiet > 45 * 60000UL && !portalActive) restartNow("no data for 45 min");
  if (ESP.getMaxAllocHeap() < 16 * 1024) restartNow("low memory");
  if (ntpSynced) ntpSynced = false, Serial.println(rtcWrite(time(nullptr)) ? "rtc: set from ntp" : "rtc: write failed");
  time_t now = time(nullptr);
  tm lt;
  localtime_r(&now, &lt);
  if (lt.tm_year + 1900 >= 2025 && lt.tm_hour == 4 && lt.tm_min < 2 && millis() > 3600000UL) restartNow("nightly");
  return true;
}

struct Job {
  const char *name;
  bool (*run)();
  uint32_t every;
  bool needsWifi;
  uint32_t next, lastOk;
};

static Job jobs[] = {{"health", health, 30000, false},       {"indoor", readIndoor, 30000, false},
                     {"battery", readBattery, 30000, false}, {"room log", logRoom, 60000, false},
                     {"photo", photoJob, 1000, false},       {"quote", quoteJob, 1000, false},
                     {"buses", fetchBuses, 30000, true},
                     {"nowcast", fetchNowcast, 600000, true}, {"outside", fetchOutside, 300000, true},
                     {"uv", fetchUv, 1800000, true},
                     {"psi", fetchPsi, 1800000, true},       {"outlook", fetchOutlook, 3600000, true}};

static void netTask(void *) {
  esp_task_wdt_add(nullptr);  // a hung fetch restarts the board instead of freezing the data
  for (;;) {
    for (auto &j : jobs) {
      if ((int32_t)(millis() - j.next) < 0 || (j.needsWifi && WiFi.status() != WL_CONNECTED)) continue;
      bool ok = j.run();
      if (ok) j.lastOk = millis();
      if (ok && j.needsWifi) lastNetOk = millis();
      esp_task_wdt_reset();
      j.next = millis() + (ok ? j.every : min(j.every, (uint32_t)60000));  // retry failures within a minute
    }
    delay(500);
  }
}

// ---------- drawing helpers ----------

static int spacedWidth(const char *s, int gap) {
  int w = 0;
  char c[2] = {};
  for (; *s; s++) c[0] = *s, w += g->getStrWidth(c) + gap;
  return w - gap;
}

// Letter-spaced caps for labels.
static void spaced(int x, int y, const char *s, int gap) {
  char c[2] = {};
  for (; *s; s++) c[0] = *s, g->drawStr(x, y, c), x += g->getStrWidth(c) + gap;
}

static void centered(int cx, int y, const char *s) { g->drawUTF8(cx - g->getUTF8Width(s) / 2, y, s); }
static void rightAligned(int rx, int y, const char *s) { g->drawUTF8(rx - g->getUTF8Width(s), y, s); }
static void blit(int x, int y, const Bitmap &b) { g->drawXBMP(x, y, b.w, b.h, b.bits); }

// ---------- weather tile ----------

constexpr int TILE_X = 284, TILE_Y = 24, TILE_W = 104, TILE_H = 110, TILE_CX = TILE_X + TILE_W / 2;
static uint32_t frame = 0;
static int forcedScene = -1;  // serial 'w' cycles scenes for testing

// Cloud = three discs on a flat base. Filled in ink, then refilled in paper 2px in, it becomes an outline.
static void cloudShape(int cx, int base, int s, int inset) {
  g->drawDisc(cx - s * 55 / 100, base - s * 35 / 100, max(s * 35 / 100 - inset, 1));
  g->drawDisc(cx - s * 5 / 100, base - s * 55 / 100, max(s / 2 - inset, 1));
  g->drawDisc(cx + s * 50 / 100, base - s * 30 / 100, max(s * 30 / 100 - inset, 1));
  g->drawBox(cx - s * 55 / 100, base - s * 35 / 100, s * 105 / 100, s * 35 / 100 - inset);
}

static bool inCloud(int x, int y, int cx, int base, int s, int inset) {
  auto disc = [&](int dx, int dy, int r) {
    int ex = x - (cx + dx), ey = y - (base + dy);
    r -= inset;
    return r > 0 && ex * ex + ey * ey <= r * r;
  };
  return disc(-s * 55 / 100, -s * 35 / 100, s * 35 / 100) || disc(-s * 5 / 100, -s * 55 / 100, s / 2) ||
         disc(s * 50 / 100, -s * 30 / 100, s * 30 / 100) ||
         (x >= cx - s * 55 / 100 && x < cx + s / 2 && y >= base - s * 35 / 100 && y < base - inset);
}

// shade: 0 = white cloud, 1 = 25% grey (heavy rain), 2 = 50% grey (storm).
static void cloud(int cx, int base, int s, int shade) {
  g->setDrawColor(1);
  cloudShape(cx, base, s, 0);
  g->setDrawColor(0);
  cloudShape(cx, base, s, 2);
  g->setDrawColor(1);
  if (!shade) return;
  for (int y = base - s * 105 / 100; y < base; y++)
    for (int x = cx - s * 9 / 10; x < cx + s * 8 / 10; x++) {
      bool on = shade == 2 ? ((x + y) & 1) == 0 : (x & 1) == 0 && (y & 1) == 0;
      if (on && inCloud(x, y, cx, base, s, 3)) g->drawPixel(x, y);
    }
}

static void sun(int cx, int cy, int r) {
  g->drawDisc(cx, cy, r);
  float a0 = frame * 0.012f;  // one slow turn every ~20 s
  for (int i = 0; i < 12; i++) {
    float a = a0 + i * (float)M_PI / 6;
    float c = cosf(a), s = sinf(a);
    int x0 = cx + (int)((r + 5) * c), y0 = cy + (int)((r + 5) * s);
    int x1 = cx + (int)((r + 12) * c), y1 = cy + (int)((r + 12) * s);
    g->drawLine(x0, y0, x1, y1);
    g->drawLine(x0 + 1, y0, x1 + 1, y1);
  }
}

static void moon(int cx, int cy, int r) {
  static const int8_t stars[][2] = {{8, 14}, {24, 72}, {84, 10}, {96, 60}, {72, 92}, {12, 96}, {50, 4}};
  for (int i = 0; i < 7; i++) {
    int x = TILE_X + stars[i][0], y = TILE_Y + stars[i][1];
    bool twinkle = (frame + i * 37) % 90 < 6;
    g->drawPixel(x, y);
    if (twinkle) g->drawHLine(x - 2, y, 5), g->drawVLine(x, y - 2, 5);
  }
  g->drawDisc(cx, cy, r);
  g->setDrawColor(0);
  g->drawDisc(cx + r / 2, cy - r / 4, r - 2);
  g->setDrawColor(1);
}

// Raindrops falling from under the cloud (top = cloud base, relative to the tile).
static void rain(int count, int speed, int len, int top) {
  static int16_t dx[40], dy[40];
  static bool seeded = false;
  if (!seeded) {
    for (int i = 0; i < 40; i++) dx[i] = random(TILE_W - 24), dy[i] = random(TILE_H);
    seeded = true;
  }
  for (int i = 0; i < count; i++) {
    dy[i] += speed;
    if (top + dy[i] > TILE_H) dy[i] = random(8), dx[i] = random(TILE_W - 24);
    int x = TILE_X + 12 + dx[i], y = TILE_Y + top + dy[i];
    g->drawLine(x, y, x - len / 3, y + len);
  }
}

static void bolt(int x, int y) {
  static const int8_t p[][2] = {{0, 0}, {-9, 16}, {-1, 16}, {-8, 34}, {8, 11}, {0, 11}, {6, 0}};
  for (int i = 0; i < 6; i++) {
    g->drawLine(x + p[i][0], y + p[i][1], x + p[i + 1][0], y + p[i + 1][1]);
    g->drawLine(x + p[i][0] + 1, y + p[i][1], x + p[i + 1][0] + 1, y + p[i + 1][1]);
  }
}

static void drawWeather(Scene sc) {
  float t = frame;
  int drift = (int)(6 * sinf(t * 0.02f));
  switch (sc) {
    case SUN: sun(TILE_CX, TILE_Y + 54, 20); break;
    case MOON: moon(TILE_CX - 4, TILE_Y + 52, 22); break;
    case PARTLY_SUN:
      sun(TILE_CX - 16, TILE_Y + 38, 14);
      cloud(TILE_CX + 10 + drift, TILE_Y + 96, 46, 0);
      break;
    case PARTLY_MOON:
      moon(TILE_CX - 14, TILE_Y + 38, 17);
      cloud(TILE_CX + 10 + drift, TILE_Y + 96, 46, 0);
      break;
    case CLOUDY:
      cloud(TILE_CX - 12 + (int)(8 * sinf(t * 0.013f)), TILE_Y + 60, 32, 0);
      cloud(TILE_CX + 10 + (int)(6 * sinf(t * 0.017f + 1)), TILE_Y + 96, 46, 0);
      break;
    case RAIN:
      rain(14, 3, 6, 56);
      cloud(TILE_CX + 4, TILE_Y + 58, 50, 0);
      break;
    case HEAVY_RAIN:
      rain(32, 5, 9, 56);
      cloud(TILE_CX + 4, TILE_Y + 58, 50, 1);
      break;
    case THUNDER: {
      rain(22, 4, 8, 56);
      uint32_t cycle = frame % 170;  // a strike every ~7 s, flickering twice
      if (cycle < 3 || (cycle > 5 && cycle < 8)) bolt(TILE_CX + 8, TILE_Y + 60);
      cloud(TILE_CX + 4, TILE_Y + 58, 50, 2);
      break;
    }
    case HAZE:
      for (int row = 0; row < 6; row++)
        for (int x = TILE_X + 4; x < TILE_X + TILE_W - 4; x += 2)
          g->drawPixel(x, TILE_Y + 22 + row * 14 + (int)(3 * sinf((x + t * 0.6f) * 0.07f + row)));
      break;
    case WIND:
      for (int i = 0; i < 4; i++) {
        int x = TILE_X + TILE_W - (int)((frame * (3 + i) + i * 47) % (TILE_W + 60));
        int y = TILE_Y + 26 + i * 20;
        int x0 = max(x, TILE_X);
        g->drawHLine(x0, y, max(0, min(x + 50, TILE_X + TILE_W) - x0));
        if (x + 50 < TILE_X + TILE_W && x + 50 > TILE_X) g->drawCircle(x + 50, y - 5, 5, U8G2_DRAW_LOWER_RIGHT | U8G2_DRAW_UPPER_RIGHT);
      }
      break;
    default: break;
  }
}

// ---------- buses ----------

static int animBus = -1;  // index into BUS_SERVICES, -1 when the road is empty
static uint32_t animStart = 0;
static uint32_t pending = 0;  // bitmask of buses waiting to drive by
static const char *demoType = nullptr;

static void drawBus(int x, int ground, const char *no, const char *type) {
  bool dd = !strcmp(type, "DD"), bd = !strcmp(type, "BD");
  int h = dd ? 44 : 30, w = bd ? 156 : 108, top = ground - 5 - h;
  g->setDrawColor(1);
  g->drawRBox(x, top, w, h, 5);
  g->setDrawColor(0);
  // Windscreen at the front (left), then passenger windows.
  g->drawBox(x + 4, top + 5, 14, h - 16);
  for (int row = 0; row < (dd ? 2 : 1); row++)
    for (int wx = x + 24; wx + 13 <= x + w - 6; wx += 17) {
      if (bd && wx > x + 80 && wx < x + 96) continue;  // the bendy joint
      if (row == 1 && wx < x + 64) continue;           // lower deck: keep a panel for the number
      g->drawBox(wx, top + 5 + row * 19, 13, 10);
    }
  if (bd)
    for (int y = top + 3; y < top + h - 3; y += 4) g->drawLine(x + 86, y, x + 90, y + 2), g->drawLine(x + 90, y + 2, x + 86, y + 4);
  // Service number on the side, in paper colour.
  g->setFont(u8g2_font_helvB14_tr);
  g->drawStr(x + 26, top + h - 4, no);
  // Wheels with turning spokes, sitting in arches.
  int wheels[3] = {x + 22, x + w - 22, bd ? x + 72 : -1};
  float spin = -x * 0.25f;
  for (int wx : wheels) {
    if (wx < 0) continue;
    g->setDrawColor(0);
    g->drawDisc(wx, ground - 5, 8);
    g->setDrawColor(1);
    g->drawDisc(wx, ground - 5, 6);
    g->setDrawColor(0);
    g->drawLine(wx + (int)(5 * cosf(spin)), ground - 5 + (int)(5 * sinf(spin)), wx - (int)(5 * cosf(spin)),
                ground - 5 - (int)(5 * sinf(spin)));
    g->drawDisc(wx, ground - 5, 1);
    g->setDrawColor(1);
  }
}

static void drawBusStrip(time_t now) {
  constexpr int top = 244, col = (W - 2 * MARGIN) / NBUS;
  if (WiFi.status() != WL_CONNECTED && !portalActive) {
    g->setFont(u8g2_font_helvR10_tf);
    centered(W / 2, top + 30, "Wi-Fi dropped, reconnecting...");
    return;
  }
  if (ltaKey.isEmpty() && !S.busLive) {
    g->setFont(u8g2_font_helvR10_tf);
    centered(W / 2, top + 22, "Add your LTA DataMall key to see buses:");
    centered(W / 2, top + 40, "hold KEY for 3 seconds, then follow the steps.");
    return;
  }
  for (int i = 0; i < NBUS; i++) {
    int x = MARGIN + col * i, cx = x + col / 2, m = minutesUntil(S.bus[i].eta[0], now);
    char label[8];
    busLabel(m, label, sizeof label);
    bool arriving = m != NO_BUS && m >= -1 && m <= 1;  // a solid block, readable from across the room
    if (arriving) g->drawRBox(x + 3, top + 1, col - 6, 52, 6), g->setDrawColor(0);
    g->setFont(u8g2_font_helvB18_tr);
    centered(cx, top + 25, BUS_SERVICES[i]);
    g->setFont(u8g2_font_helvB12_tr);
    centered(cx, top + 45, label);
    g->setDrawColor(1);
  }
}

static void drawBusAnim(uint32_t t) {
  constexpr int ground = 296;
  const char *type = demoType ? demoType : S.bus[animBus].type[0];
  int bw = !strcmp(type, "BD") ? 156 : 108;
  int stopX = (W - bw) / 2 + 10;
  // The stop: a post with a flag, where the bus pulls in.
  g->drawVLine(stopX - 14, ground - 44, 44);
  g->drawBox(stopX - 22, ground - 44, 16, 10);
  g->setDrawColor(0);
  g->drawHLine(stopX - 20, ground - 40, 12);
  g->setDrawColor(1);
  for (int x = -((int)(t / 30) % 16); x < W; x += 16) g->drawHLine(x, ground + 2, 8);  // road markings
  drawBus(busX(t, W, bw, stopX), ground, BUS_SERVICES[animBus], type);
}

// Queue a drive-by the first time each bus comes within a minute.
static void checkArrivals(time_t now) {
  for (int i = 0; i < NBUS; i++) {
    int m = minutesUntil(S.bus[i].eta[0], now);
    if (m == NO_BUS) continue;
    if (m >= 2) S.bus[i].armed = true;
    else if (m >= -1 && S.bus[i].armed) S.bus[i].armed = false, pending |= 1u << i;
  }
  if (animBus < 0 && pending) {
    animBus = __builtin_ctz(pending);
    pending &= pending - 1;
    animStart = millis();
  }
}

static void showToast(const char *text) {
  strlcpy(toastText, text, sizeof toastText);
  toastUntil = millis() + 3000;
}

static void drawToast() {
  if ((int32_t)(millis() - toastUntil) > 0) return;
  g->setFont(u8g2_font_helvB10_tf);
  int w = g->getStrWidth(toastText) + 24;
  g->setDrawColor(0);
  g->drawRBox((W - w) / 2 - 3, 1, w + 6, 28, 9);  // a paper margin so it reads over anything
  g->setDrawColor(1);
  g->drawRBox((W - w) / 2, 4, w, 22, 7);
  g->setDrawColor(0);
  centered(W / 2, 20, toastText);
  g->setDrawColor(1);
}

static void drawBusBoard(time_t now) {
  g->setFont(u8g2_font_helvB08_tf);
  char header[64];
  snprintf(header, sizeof header, "%s  \xb7  %s", BUS_STOP_NAME, BUS_STOP);
  spaced(MARGIN, 16, header, 1);
  if (ltaKey.isEmpty() && !S.busLive) {
    g->setFont(u8g2_font_helvR10_tf);
    centered(W / 2, H / 2, "Add your LTA DataMall key: hold KEY for 3 seconds.");
    return;
  }
  constexpr int top = 24, row = 54;
  for (int i = 0; i < NBUS; i++) {
    int y = top + i * row;
    for (int x = MARGIN; x < W - MARGIN; x += 3) g->drawPixel(x, y);
    bool mine = false;
    for (const char *b : MY_BUSES) mine |= !strcmp(b, BUS_SERVICES[i]);
    g->setFont(mine ? u8g2_font_helvB24_tf : u8g2_font_helvR24_tf);  // my buses in bold
    g->drawStr(MARGIN, y + 38, BUS_SERVICES[i]);
    for (int k = 0; k < 3; k++) {
      int cx = 172 + k * 84;
      char lab[8];
      busLabel(minutesUntil(S.bus[i].eta[k], now), lab, sizeof lab);
      g->setFont(k ? u8g2_font_helvR14_tf : u8g2_font_helvB18_tf);
      centered(cx, y + 30, lab);
      g->setFont(u8g2_font_helvR08_tf);
      centered(cx, y + 45, S.bus[i].eta[k] ? loadLabel(S.bus[i].load[k]) : "");
    }
  }
}

// ---------- screens ----------

// 12-hour clock, "9:05 PM": no leading zero, small tracked caps on the baseline after the minutes.
static void drawClock(int x, int y, const tm &lt, bool haveTime) {
  int h = HERO_DIGITS[0].h, hr = hour12(lt.tm_hour);
  const Bitmap *seq[5] = {&HERO_DASH, &HERO_COLON, &HERO_DASH, &HERO_DASH};
  int n = 4;
  if (haveTime) {
    n = 0;
    if (hr >= 10) seq[n++] = &HERO_LEAD1;
    seq[n++] = &HERO_DIGITS[hr % 10], seq[n++] = &HERO_COLON;
    seq[n++] = &HERO_DIGITS[lt.tm_min / 10], seq[n++] = &HERO_DIGITS[lt.tm_min % 10];
  }
  for (int i = 0; i < n; i++) blit(x, y + (h - seq[i]->h) / 2, *seq[i]), x += seq[i]->w;
  if (haveTime) blit(x + 5, y + h - HERO_PM.h, lt.tm_hour < 12 ? HERO_AM : HERO_PM);
}

static void drawBattery(int right, int baseline) {
  int pct = batteryPercent(S.batV);
  char buf[8];
  snprintf(buf, sizeof buf, "%d%%", pct);
  int tw = g->getStrWidth(buf), x = right - tw - 22, y = baseline - 8;
  g->drawStr(right - tw, baseline, buf);
  g->drawFrame(x, y, 17, 9);
  g->drawBox(x + 17, y + 2, 2, 5);  // terminal
  g->drawBox(x + 2, y + 2, (13 * pct + 50) / 100, 5);
}

static int quoteSkip = 0;  // 'q' or "next quote" steps through the built-in quotes while there's no live one

static bool inkAt(const Bitmap &b, int x, int y) {
  return x >= 0 && y >= 0 && x < b.w && y < b.h && (b.bits[y * ((b.w + 7) / 8) + x / 8] >> (x & 7) & 1);
}

// A new quote dissolves in over the old one, pixel by pixel in a fixed scattered order.
static void drawQuote(const Bitmap &next) {
  constexpr int top = 166, height = 72;
  constexpr uint32_t FADE_MS = 1500;
  static Bitmap q = {}, p = {};  // shown, and the one it replaced
  static uint32_t since = 0;
  if (next.bits != q.bits) p = q, q = next, since = millis();
  uint32_t t = millis() - since;
  if (!p.bits || t >= FADE_MS) return blit((W - q.w) / 2, top + (height - q.h) / 2, q);
  int threshold = t * 256 / FADE_MS, qx = (W - q.w) / 2, qy = top + (height - q.h) / 2;
  int px = (W - p.w) / 2, py = top + (height - p.h) / 2;
  for (int y = top; y < top + height; y++)
    for (int x = MARGIN; x < W - MARGIN; x++) {
      bool fresh = ((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u) % 256 < (uint32_t)threshold;  // unsigned: wraps, no UB
      if (fresh ? inkAt(q, x - qx, y - qy) : inkAt(p, x - px, y - py)) g->drawPixel(x, y);
    }
}

// World clocks in small type in the strip between the big clock and the rule, spread across its width.
static void drawWorldClocks(int x0, int x1, int y, time_t now) {
  const WorldClock *zones = WORLD_CLOCKS;  // offsets follow each DST rule
  char times[3][10];
  int widths[3], total = 0;
  for (int i = 0; i < 3; i++) {
    int mins = (int)(((now + zoneOffsetMin(zones[i], now) * 60) / 60) % 1440), h = mins / 60;
    snprintf(times[i], sizeof times[i], "%d:%02d%s", hour12(h), mins % 60, h < 12 ? "am" : "pm");
    g->setFont(u8g2_font_helvR10_tf);  // city light, time bold: the number is what you read
    widths[i] = g->getStrWidth(zones[i].label) + 4;
    g->setFont(u8g2_font_helvB10_tf);
    widths[i] += g->getStrWidth(times[i]);
    total += widths[i];
  }
  int gap = (x1 - x0 - total) / 2, x = x0;
  for (int i = 0; i < 3; i++) {
    g->setFont(u8g2_font_helvR10_tf);
    g->drawStr(x, y, zones[i].label);
    int lw = g->getStrWidth(zones[i].label) + 4;
    g->setFont(u8g2_font_helvB10_tf);
    g->drawStr(x + lw, y, times[i]);
    x += widths[i] + gap;
  }
}

// Thin outlines keep the cat's face clear at its small native size.
static void drawFlubber(int x, int ground, Scene sc, bool sleepy, bool happy, bool concentrating) {
  uint32_t t = millis();
  bool wet = sc == RAIN || sc == HEAVY_RAIN || sc == THUNDER;
  CatMotion motion = catMotion(t, wet, sleepy, happy, concentrating);
  bool walking = motion.activity == CAT_WALK, stretching = motion.activity == CAT_STRETCH;
  bool washing = motion.activity == CAT_WASH, napping = motion.activity == CAT_NAP;
  x += motion.dx;
  int bodyX = x - (walking ? 7 * motion.direction : stretching ? 5 : 0);
  x += walking ? 6 * motion.direction : stretching ? 7 : 0;
  if (walking) ground -= t / 180 % 2;
  if (happy && !wet) ground -= (int)(2 * fabsf(sinf(t * 0.006f)));
  int faceY = ground - (wet ? 19 : napping ? 17 : stretching ? 21 : 27);
  if (wet) {
    int base = ground - 39;
    g->drawCircle(x, base, 14, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
    g->drawHLine(x - 14, base, 29);
    g->drawVLine(x + 14, base, 28);
  }
  int tailSide = walking || stretching ? -motion.direction : 1;
  int tailReach = walking ? 11 : 14;
  int tailX = bodyX + tailSide * (tailReach + (napping ? 0 : (int)(2 * sinf(t * 0.0012f))));
  g->drawCircle(tailX, ground - 8, 7,
                (tailSide > 0 ? U8G2_DRAW_UPPER_RIGHT : U8G2_DRAW_UPPER_LEFT) | U8G2_DRAW_LOWER_RIGHT | U8G2_DRAW_LOWER_LEFT);
  g->drawLine(bodyX + tailSide * 8, ground - 2, tailX, ground - 1);
  int bodyH = napping ? 11 : wet ? 15 : stretching ? 13 : walking ? 18 : 21;
  int bodyW = stretching ? 29 : walking ? 27 : napping ? 25 : 21, radius = min(6, (bodyH - 2) / 2);
  g->drawRBox(bodyX - bodyW / 2, ground - bodyH, bodyW, bodyH, radius);
  g->setDrawColor(0);
  g->drawRBox(bodyX - bodyW / 2 + 1, ground - bodyH + 1, bodyW - 2, bodyH - 2, radius - 1);
  g->setDrawColor(1);
  if (walking) {
    for (int i = 0; i < 4; i++) {
      int legX = bodyX - 9 + i * 6, stride = (t / 180 + i) % 2 ? 2 : -2;
      g->drawLine(legX, ground - 7, legX + stride, ground - 1);
      g->drawHLine(legX + stride - 1, ground - 1, 3);
    }
  } else if (stretching) {
    for (int side : {-1, 1})
      g->drawLine(bodyX + side * 4, ground - 7, bodyX + 8 + side * 8, ground - 1);
  }
  for (int side : {-1, 1}) {
    g->drawLine(x + side * 14, faceY - 5, x + side * 13, faceY - 18);
    g->drawLine(x + side * 13, faceY - 18, x + side * 4, faceY - 10);
    g->drawLine(x + side * 11, faceY - 10, x + side * 11, faceY - 14);
  }
  g->drawRBox(x - 15, faceY - 11, 31, 23, 10);
  g->setDrawColor(0);
  g->drawRBox(x - 14, faceY - 10, 29, 21, 9);
  g->setDrawColor(1);
  for (int side : {-1, 1}) {
    int eyeX = x + side * 6;
    if (napping || washing || stretching || t % 8000 < 150) g->drawHLine(eyeX - 2, faceY - 1, 4);
    else if (happy) g->drawCircle(eyeX, faceY, 2, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
    else {
      eyeX += walking ? motion.direction : concentrating ? 1 : 0;
      g->drawDisc(eyeX, faceY - 1, 2);
      g->setDrawColor(0);
      g->drawPixel(eyeX - 1, faceY - 2);
      g->setDrawColor(1);
    }
    g->drawLine(x + side * 10, faceY + 2, x + side * 18, faceY);
    g->drawLine(x + side * 10, faceY + 4, x + side * 17, faceY + 6);
    g->drawVLine(x + side * 3, faceY - 8, 3);
  }
  g->drawVLine(x, faceY - 9, 4);
  g->drawHLine(x - 1, faceY + 1, 3), g->drawPixel(x, faceY + 2);
  for (int side : {-1, 1}) {
    g->drawCircle(x + side * 2, faceY + 3, 2, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
    if (!walking && !stretching && !napping) {
      g->drawVLine(bodyX + side * 4, ground - 5, 4);
      g->drawPixel(bodyX + side * 6, ground - 2);
      g->drawPixel(bodyX + side * 8, ground - 2);
    }
    if (!wet && !napping && !stretching) {
      g->drawHLine(bodyX + (side < 0 ? -9 : 6), ground - 14, 4);
      g->drawHLine(bodyX + (side < 0 ? -9 : 7), ground - 11, 3);
    }
  }
  if (washing) {
    int pawY = faceY + 5 - (int)(t / 350 % 3) * 2;
    g->drawLine(bodyX + 5, ground - 9, x + 10, pawY);
    g->drawDisc(x + 10, pawY, 3);
    g->setDrawColor(0), g->drawDisc(x + 10, pawY, 2), g->setDrawColor(1);
  }
  if (napping) {
    g->drawHLine(x + 19, faceY - 13, 4);
    g->drawLine(x + 22, faceY - 12, x + 19, faceY - 9);
    g->drawHLine(x + 19, faceY - 8, 4);
  }
}

static void drawPlant(int x, int ground, int growth) {
  int stem = 8 + growth * 9, top = ground - 20 - stem;
  g->drawLine(x - 18, ground - 19, x - 12, ground);
  g->drawHLine(x - 12, ground, 25);
  g->drawLine(x + 12, ground, x + 18, ground - 19);
  g->drawHLine(x - 20, ground - 20, 41), g->drawHLine(x - 20, ground - 17, 41);
  g->drawVLine(x, top, stem);
  for (int i = 0; i < growth; i++) {
    int y = ground - 30 - i * 9, side = i % 2 ? 1 : -1;
    g->drawLine(x, y + 4, x + side * 11, y - 4);
    g->drawDisc(x + side * 12, y - 5, 6);
  }
  g->drawDisc(x, top, growth == 4 ? 8 : 3);
  if (growth == 4) {
    for (int i = 0; i < 6; i++) {
      float a = i * (float)M_PI / 3;
      g->drawCircle(x + (int)(11 * cosf(a)), top + (int)(11 * sinf(a)), 5);
    }
  }
}

static void drawTwoLines(const char *text, int center, int baseline, int width) {
  const uint8_t *fonts[] = {u8g2_font_helvB12_tf, u8g2_font_helvB10_tf, u8g2_font_helvR08_tf};
  char first[80], second[80];
  for (const uint8_t *font : fonts) {
    g->setFont(font);
    strlcpy(first, text, sizeof first), second[0] = 0;
    if (g->getStrWidth(first) > width) {
      size_t cut = strlen(first);
      while (cut && g->getStrWidth(first) > width) first[--cut] = 0;
      size_t word = cut;
      while (word && text[word] != ' ') word--;
      if (word && g->getStrWidth(text + word + 1) <= width) cut = word;
      first[cut] = 0;
      while (text[cut] == ' ') cut++;
      strlcpy(second, text + cut, sizeof second);
    }
    if (g->getStrWidth(second) <= width) break;
  }
  centered(center, baseline, first);
  if (*second) centered(center, baseline + 20, second);
}

static bool hasIntention(time_t now) { return *intention && intentionDay == sgtDay(now); }

static bool drawPriority(time_t now, Scene sc) {
  static MiddlePanel panel;
  uint32_t t = millis();
  bool weatherFresh = S.weatherAt && t - S.weatherAt < 25 * 60000UL;
  bool busFresh = ltaKey.isEmpty() || (S.busAt && t - S.busAt < 90000UL);
  bool psiFresh = S.psiAt && t - S.psiAt < 65 * 60000UL;
  bool severeWeather = sc == HEAVY_RAIN || sc == THUNDER;
  uint8_t eligible = priorityMask(WiFi.status() == WL_CONNECTED, weatherFresh, busFresh, routine == ROUTINE_MORNING,
                                  S.psi, psiFresh, severeWeather);
  Priority p = panel.choose(t, eligible, S.psi);
  const char *label = "", *text = "";
  char buf[64], area[48];
  switch (p) {
    case PRIORITY_OFFLINE: label = "CONNECTION"; text = "Offline. Retrying Wi-Fi."; break;
    case PRIORITY_WEATHER_STALE:
      label = "WEATHER"; text = S.weatherAt ? "Weather update overdue" : "Checking the sky..."; break;
    case PRIORITY_BUS_STALE:
      label = "BUSES"; text = S.busAt ? "Bus arrivals are out of date" : "Checking bus arrivals..."; break;
    case PRIORITY_AIR:
      label = "AIR QUALITY - 24-HOUR PSI";
      snprintf(buf, sizeof buf, "PSI %d - %s", S.psi, psiBand(S.psi)); text = buf; break;
    case PRIORITY_RAIN:
      snprintf(area, sizeof area, "%s FORECAST", NEA_AREA);
      for (char *c = area; *c; c++) *c = toupper((unsigned char)*c);
      label = area;
      text = sc == THUNDER ? "Thundery showers in the 2-hour forecast" : "Heavy rain in the 2-hour forecast";
      break;
    default:
      if (!hasIntention(now) || t / 20000 % 2) return false;
      label = "TODAY"; text = intention;
  }
  g->setFont(u8g2_font_helvB08_tf);
  spaced(MARGIN, 180, label, 1);
  drawTwoLines(text, W / 2, 204, W - 2 * MARGIN);
  return true;
}

static void drawFocus(time_t now) {
  uint32_t t = millis();
  g->setFont(u8g2_font_helvB08_tf);
  const char *label = focus.phase == FOCUS_WORK ? "ONE THING AT A TIME" : focus.phase == FOCUS_BREAK ? "A LITTLE BREATHING ROOM" :
                      focus.phase == FOCUS_DONE ? "NICELY DONE" : "GROW A LITTLE FOCUS";
  spaced(MARGIN, 18, label, 1);
  if (hasIntention(now)) drawTwoLines(intention, W / 2, 48, W - 2 * MARGIN);
  else {
    g->setFont(u8g2_font_helvR10_tf);
    centered(W / 2, 51, "One small thing deserves your attention.");
  }
  unsigned secs = focus.active() ? focus.remaining(t) : 25 * 60;
  if (focus.phase == FOCUS_DONE) secs = 0;
  const Bitmap *seq[] = {&HERO_DIGITS[secs / 600], &HERO_DIGITS[secs / 60 % 10], &HERO_COLON,
                         &HERO_DIGITS[secs / 10 % 6], &HERO_DIGITS[secs % 10]};
  int width = 0;
  for (auto b : seq) width += b->w;
  int x = (W - width) / 2;
  for (auto b : seq) blit(x, 87 + (HERO_DIGITS[0].h - b->h) / 2, *b), x += b->w;
  g->setFont(u8g2_font_helvR10_tf);
  centered(W / 2, 180, focus.phase == FOCUS_BREAK ? "Five minutes to stretch. Your flower is ready." :
                            focus.phase == FOCUS_DONE ? "A session finished. A flower earned." : "25 minutes to let something grow.");
  bool happy = focus.phase == FOCUS_DONE || focus.phase == FOCUS_BREAK;
  drawFlubber(86, 268, CLOUDY, focus.phase == FOCUS_BREAK, happy, focus.phase == FOCUS_WORK);
  drawPlant(310, 267, focus.growth(t));
  char kept[32];
  snprintf(kept, sizeof kept, "%lu", (unsigned long)garden.total);
  g->setFont(u8g2_font_helvB14_tr);
  centered(W / 2, 226, kept);
  g->setFont(u8g2_font_helvR08_tf);
  centered(W / 2, 244, garden.total == 1 ? "flower in your garden" : "flowers in your garden");
  g->setFont(u8g2_font_helvR08_tf);
  char hint[64];
  snprintf(hint, sizeof hint, "Tap BOOT or say '%c%s focus' to begin.", toupper((unsigned char)CALL_NAME[0]), CALL_NAME + 1);
  centered(W / 2, 292, focus.active() ? "KEY changes view. Hold BOOT 2s to stop." : hint);
}

static void drawGarden(time_t now) {
  char buf[80];
  g->setFont(u8g2_font_helvB08_tf);
  spaced(MARGIN, 18, "YOUR FOCUS GARDEN", 1);
  g->setFont(u8g2_font_helvB24_tr);
  snprintf(buf, sizeof buf, "%lu %s", (unsigned long)garden.total, garden.total == 1 ? "flower" : "flowers");
  fitText(buf, sizeof buf, W - 2 * MARGIN, [](const char *s) { return g->getStrWidth(s); });
  g->drawStr(MARGIN, 61, buf);
  g->setFont(u8g2_font_helvR10_tf);
  uint64_t minutes = (uint64_t)garden.total * 25;
  if (gardenMonth(now))
    snprintf(buf, sizeof buf, "%lu this month   /   %llu h %u min of focus", (unsigned long)garden.thisMonth(now),
             (unsigned long long)(minutes / 60), (unsigned)(minutes % 60));
  else snprintf(buf, sizeof buf, "%llu h %u min of focus", (unsigned long long)(minutes / 60), (unsigned)(minutes % 60));
  fitText(buf, sizeof buf, W - 2 * MARGIN, [](const char *s) { return g->getStrWidth(s); });
  g->drawStr(MARGIN, 84, buf);
  g->drawHLine(MARGIN, 96, W - 2 * MARGIN);
  unsigned count = garden.total < 30 ? garden.total : 30;
  for (unsigned i = 0; i < count; i++) {
    uint32_t seed = garden.total - count + i;
    int x = 28 + (i % 10) * 38, ground = 147 + (i / 10) * 42, top = ground - 17 - seed % 13;
    g->drawVLine(x, top, ground - top);
    int side = seed % 2 ? 1 : -1;
    g->drawLine(x, ground - 5, x + side * 7, ground - 11);
    g->drawDisc(x + side * 7, ground - 12, 3);
    if (seed % 3 == 0) {
      for (int p = 0; p < 4; p++)
        g->drawCircle(x + (p == 0 ? -4 : p == 1 ? 4 : 0), top + (p == 2 ? -4 : p == 3 ? 4 : 0), 3);
    } else g->drawCircle(x, top, 6 + seed % 2);
    g->drawDisc(x, top, 2);
    g->drawHLine(x - 8, ground + 1, 17);
  }
  g->setFont(u8g2_font_helvR10_tf);
  if (!count) {
    drawPlant(W / 2, 214, 0);
    centered(W / 2, 246, "25 minutes to your first flower.");
  } else {
    g->setFont(u8g2_font_helvR08_tf);
    g->drawStr(MARGIN, 261, garden.total > 30 ? "Your latest 30 flowers. Every session is kept." : "A little more grown, one session at a time.");
  }
  drawFlubber(348, 280, CLOUDY, false, false, false);
  g->setFont(u8g2_font_helvR08_tf);
  g->drawStr(MARGIN, 294, gardenSaved ? "BOOT to focus. KEY to go home." : "Progress not saved. Check storage.");
}

static void drawMain(const tm &lt, bool haveTime, time_t now) {
  char buf[80];
  g->setFont(u8g2_font_helvB08_tf);
  if (haveTime) {
    snprintf(buf, sizeof buf, "%.3s  %d %.3s", DAYS[lt.tm_wday], lt.tm_mday, MONTHS[lt.tm_mon]);  // short: climate needs the row
    spaced(MARGIN, 16, buf, 1);
  }
  g->setFont(u8g2_font_helvR08_tf);
  int edge = W - MARGIN;
  if (!isnan(S.batV)) {
    drawBattery(edge, 16);
    edge -= 56;
  }
  if (!isnan(S.inT)) {
    snprintf(buf, sizeof buf, "IN  %.1f\xc2\xb0  %.0f%%", S.inT, S.inH);
    rightAligned(edge, 16, buf);
    edge -= g->getUTF8Width(buf) + 14;
  }
  if (!isnan(S.airT) && !isnan(S.airH)) {
    int n = snprintf(buf, sizeof buf, "OUT  %.0f\xc2\xb0  %.0f%%", S.airT, S.airH);
    if (!isnan(S.windMs)) snprintf(buf + n, sizeof buf - n, "  FEELS %.0f\xc2\xb0", feelsLike(S.airT, S.airH, S.windMs));
    rightAligned(edge, 16, buf);
  }

  drawClock(MARGIN - 2, 30 + (104 - HERO_DIGITS[0].h) / 2, lt, haveTime);
  if (haveTime) drawWorldClocks(MARGIN - 2, TILE_X - 6, 133, now);

  Scene sc = forcedScene >= 0 ? (Scene)forcedScene : sceneFor(S.nowcast.c_str(), haveTime ? lt.tm_hour : 12);
  if (!S.nowcast.isEmpty() || forcedScene >= 0) drawWeather(sc);

  g->drawHLine(MARGIN, 140, W - 2 * MARGIN);
  g->setFont(u8g2_font_helvR10_tf);
  if (S.nowcast.isEmpty()) snprintf(buf, sizeof buf, "%s", WiFi.status() == WL_CONNECTED ? "Checking the sky..." : "Offline");
  else snprintf(buf, sizeof buf, "%s \xc2\xb7 %s", NEA_AREA, S.nowcast.c_str());

  // Reserve the readings first; even an unfamiliar forecast must stay out of their space.
  String right;
  if (S.uv > 0) right += "UV " + String(S.uv);
  if (S.psi > 50) right += (right.isEmpty() ? "" : "   ") + String("PSI ") + S.psi + " " + psiBand(S.psi);
  int weatherWidth = W - 2 * MARGIN - (right.isEmpty() ? 0 : g->getUTF8Width(right.c_str()) + 12);
  if (!S.nowcast.isEmpty() && g->getUTF8Width(buf) > weatherWidth)
    snprintf(buf, sizeof buf, "%s \xc2\xb7 %s", NEA_AREA, compactWeather(S.nowcast.c_str(), sc));
  fitText(buf, sizeof buf, weatherWidth, [](const char *s) { return g->getUTF8Width(s); });
  g->drawUTF8(MARGIN, 157, buf);
  rightAligned(W - MARGIN, 157, right.c_str());

  if (!drawPriority(now, sc))
    drawQuote(liveQuote.bits ? liveQuote : QUOTES[haveTime ? (quoteAt(now, QUOTE_MINUTES, QUOTE_COUNT) + quoteSkip) % QUOTE_COUNT : 0]);

  g->drawHLine(MARGIN, 240, W - 2 * MARGIN);
  uint32_t t = millis() - animStart;
  if (routine == ROUTINE_MORNING) {
    if (animBus >= 0 && t < BUS_ANIM_MS) drawBusAnim(t);
    else { animBus = -1, demoType = nullptr; drawBusStrip(now); }
  } else {
    animBus = -1, demoType = nullptr;
    uint32_t catNow = millis();
    bool freshWet = S.weatherAt && catNow - S.weatherAt < 25 * 60000UL;
    Scene catScene = freshWet ? sc : CLOUDY;
    bool wet = catScene == RAIN || catScene == HEAVY_RAIN || catScene == THUNDER;
    bool sleepy = haveTime && (lt.tm_hour >= 22 || lt.tm_hour < 7);
    bool happy = celebrateAt && catNow - celebrateAt < 5000;
    CatMotion motion = catMotion(catNow, wet, sleepy, happy, focus.phase == FOCUS_WORK);
    CatCaption caption = catCaption(catNow, motion.activity, wet, sleepy, happy, routine == ROUTINE_EVENING, voiceOk);
    g->setFont(u8g2_font_helvB08_tf);
    g->drawStr(MARGIN, 261, focus.active() ? (focus.phase == FOCUS_WORK ? "SOMETHING IS GROWING" : "A LITTLE BREATHING ROOM") : caption.title);
    g->setFont(u8g2_font_helvR10_tf);
    if (focus.active()) {
      unsigned s = focus.remaining(catNow);
      snprintf(buf, sizeof buf, "%s %u:%02u - hold BOOT 2s to stop", focus.phase == FOCUS_WORK ? "Focus" : "Break", s / 60, s % 60);
    } else strlcpy(buf, caption.text, sizeof buf);
    g->drawStr(MARGIN, 283, buf);
    drawFlubber(353, 294, catScene, sleepy, happy, focus.phase == FOCUS_WORK);
  }
}

static void drawForecast() {
  g->setFont(u8g2_font_helvB08_tf);
  spaced(MARGIN, 18, "NEXT 24 HOURS  \xb7  WEST", 1);
  g->setFont(u8g2_font_helvR10_tf);
  for (int i = 0; i < 3; i++) {
    g->drawUTF8(MARGIN, 42 + i * 20, S.periodLabel[i].c_str());
    rightAligned(W - MARGIN, 42 + i * 20, S.periodText[i].c_str());
  }
  g->drawHLine(MARGIN, 112, W - 2 * MARGIN);
  g->setFont(u8g2_font_helvB08_tf);
  spaced(MARGIN, 134, "NEXT 4 DAYS", 1);
  for (int i = 0; i < 4; i++) {
    int y = 160 + i * 36;
    char temp[16];
    g->setFont(u8g2_font_helvB10_tf);
    g->drawUTF8(MARGIN, y, S.outDay[i].c_str());
    snprintf(temp, sizeof temp, "%d-%d\xc2\xb0", S.outLo[i], S.outHi[i]);  // helv has no en dash
    g->setFont(u8g2_font_helvR10_tf);
    rightAligned(W - MARGIN, y, S.outLo[i] || S.outHi[i] ? temp : "");
    g->drawUTF8(MARGIN, y + 16, S.outText[i].c_str());
  }
}

// Shown values come from raw ones at draw time, so new offsets apply to the whole history.
static float shownT(const Sample &s) { return s.rawT + TEMP_OFFSET_C; }
static float shownH(const Sample &s) { return rhAt(s.rawH, s.rawT, shownT(s)) + RH_OFFSET; }

// One panel per measure: name and current value on the left, the last 24 h on the right as a thick line over a
// light dithered fill. Readings are averaged per pixel column (~6 min), which irons out noise and restarts.
static void drawRoomPanel(const char *title, const char *value, const float *sum, const uint16_t *cnt, int cols,
                          int x0, int top, int bottom, float minSpan, const char *unit) {
  char buf[24];
  g->setFont(u8g2_font_helvB08_tf);
  spaced(MARGIN, top + 9, title, 1);
  g->setFont(u8g2_font_helvB24_tf);
  g->drawStr(MARGIN, top + 44, value);
  float lo = 1e9, hi = -1e9;
  for (int c = 0; c < cols; c++)
    if (cnt[c]) lo = min(lo, sum[c] / cnt[c]), hi = max(hi, sum[c] / cnt[c]);
  g->drawHLine(x0, bottom, cols);
  if (lo > hi) return;
  g->setFont(u8g2_font_helvR10_tf);
  snprintf(buf, sizeof buf, "%.0f to %.0f%s", lo, hi, unit);
  g->drawUTF8(MARGIN, top + 64, buf);
  if (hi - lo < minSpan) lo = (lo + hi - minSpan) / 2, hi = lo + minSpan;
  float pad = (hi - lo) * 0.1f;
  lo -= pad, hi += pad;
  auto yOf = [&](float v) { return bottom - 2 - (int)((v - lo) / (hi - lo) * (bottom - top - 6)); };
  snprintf(buf, sizeof buf, "%.0f%s", hi, unit), g->drawUTF8(x0 + cols + 4, top + 10, buf);
  snprintf(buf, sizeof buf, "%.0f%s", lo, unit), g->drawUTF8(x0 + cols + 4, bottom, buf);
  int px = -1, py = 0;
  for (int c = 0; c < cols; c++) {
    if (!cnt[c]) continue;
    int x = x0 + c, y = yOf(sum[c] / cnt[c]);
    for (int yy = y + 3; yy < bottom; yy++)
      if (((x + yy) & 3) == 0) g->drawPixel(x, yy);  // light fill under the line
    if (px >= 0 && x - px <= 3)                       // join neighbours; a longer gap means no data then
      for (int d = -1; d <= 1; d++) g->drawLine(px, py + d, x, y + d);
    else
      g->drawBox(x - 1, y - 1, 3, 3);
    px = x, py = y;
  }
}

static void drawRoom(time_t now) {
  constexpr int x0 = 108, x1 = W - 40, cols = x1 - x0;
  static float tSum[cols], hSum[cols];
  static uint16_t cnt[cols];
  memset(tSum, 0, sizeof tSum), memset(hSum, 0, sizeof hSum), memset(cnt, 0, sizeof cnt);
  for (int i = 0; i < histCount; i++) {
    const Sample &s = sampleAt(i);
    if (s.t + 86400 < (uint32_t)now || s.t > (uint32_t)now) continue;
    int c = (int)((int64_t)(s.t - (now - 86400)) * cols / 86400);
    if (c < 0 || c >= cols) continue;
    tSum[c] += shownT(s), hSum[c] += shownH(s), cnt[c]++;
  }
  g->setFont(u8g2_font_helvB08_tf);
  spaced(MARGIN, 12, "ROOM  \xb7  LAST 24 HOURS", 1);
  if (!sdOk) g->setFont(u8g2_font_helvR08_tf), rightAligned(W - MARGIN, 12, "no memory card: history resets on restart");

  constexpr int tTop = 20, tBottom = 140, hTop = 150, hBottom = 270;
  char v[16];
  snprintf(v, sizeof v, isnan(S.inT) ? "--" : "%.1f\xb0", S.inT);
  drawRoomPanel("TEMPERATURE", v, tSum, cnt, cols, x0, tTop, tBottom, 2, "\xb0");
  snprintf(v, sizeof v, isnan(S.inH) ? "--" : "%.0f%%", S.inH);
  drawRoomPanel("HUMIDITY", v, hSum, cnt, cols, x0, hTop, hBottom, 10, "%");

  // Six-hourly ticks through both panels, labelled once underneath like the clock.
  g->setFont(u8g2_font_helvR10_tf);
  for (time_t t = now - (now + 8 * 3600) % 21600; t > now - 86400; t -= 21600) {
    int x = x0 + (int)((int64_t)(t - (now - 86400)) * cols / 86400);
    for (int y = tTop; y < hBottom; y += 4)
      if (y < tBottom || y > hTop) g->drawPixel(x, y);
    int h = (int)((t + 8 * 3600) / 3600 % 24);
    char buf[8];
    snprintf(buf, sizeof buf, "%d %s", hour12(h), h < 12 ? "AM" : "PM");
    centered(x, 287, buf);
  }
}

static void drawPhotos(const tm &lt, bool haveTime) {
  if (!photoReady) {
    g->setFont(u8g2_font_helvR10_tf);
    centered(W / 2, H / 2, photoNote);
    return;
  }
  g->drawXBMP(0, 0, PW, PH, photo);
  bool memory = *photoMemory.caption;
  if (memory) {
    g->setDrawColor(0);
    g->drawBox(0, 230, W, 70);
    g->drawRBox(6, 6, 130, 20, 4);
    g->setDrawColor(1);
    g->drawHLine(MARGIN, 230, W - 2 * MARGIN);
    g->setFont(u8g2_font_helvB08_tf);
    g->drawStr(12, 20, memorySeason(photoMemory.date, time(nullptr)) == 2 ? "ON THIS DAY" : "A LITTLE MEMORY");
    char date[24];
    memoryDateLabel(photoMemory.date, date, sizeof date);
    spaced(MARGIN, 248, date, 1);
    drawTwoLines(photoMemory.caption, W / 2, 271, W - 2 * MARGIN);
  } else if (!photoCredit.isEmpty()) {  // Unsplash photos: credit the photographer on a paper tab, bottom left
    String credit = photoCredit + " / Unsplash";
    g->setFont(u8g2_font_helvR08_tf);
    char line[96];
    credit.toCharArray(line, sizeof line);
    fitText(line, sizeof line, W - 142, [](const char *s) { return g->getStrWidth(s); });
    int w = g->getStrWidth(line) + 12;
    g->setDrawColor(0);
    g->drawRBox(6, H - 22, w, 16, 4);
    g->setDrawColor(1);
    g->drawStr(12, H - 10, line);
  }
  if (!haveTime) return;
  char buf[16];  // a small clock on a paper tab, bottom right
  snprintf(buf, sizeof buf, "%d:%02d %s", hour12(lt.tm_hour), lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM");
  g->setFont(u8g2_font_helvB12_tr);
  int w = g->getStrWidth(buf) + 16;
  g->setDrawColor(0);
  g->drawRBox(W - w - 6, memory ? 6 : H - 30, w, 24, 6);
  g->setDrawColor(1);
  g->drawStr(W - w + 2, memory ? 24 : H - 12, buf);
}

static void drawSetup() {
  char buf[64];
  g->setFont(u8g2_font_helvB14_tr);
  g->drawStr(MARGIN, 40, "Set up your daycard");
  g->setFont(u8g2_font_helvR10_tf);
  g->drawUTF8(MARGIN, 76, "1.  On your phone, join this Wi-Fi network:");
  g->setFont(u8g2_font_helvB12_tr);
  snprintf(buf, sizeof buf, "%s", AP_NAME);
  g->drawStr(MARGIN + 22, 100, buf);
  g->setFont(u8g2_font_helvR10_tf);
  snprintf(buf, sizeof buf, "password  %s", apPass);
  g->drawStr(MARGIN + 22, 120, buf);
  g->drawUTF8(MARGIN, 150, "2.  A setup page opens (or visit 192.168.4.1).");
  g->drawUTF8(MARGIN, 172, "3.  Configure WiFi: pick your 2.4 GHz network.");
  g->drawUTF8(MARGIN, 194, "4.  Setup: paste your LTA DataMall key, Save.");
  g->drawHLine(MARGIN, 230, W - 2 * MARGIN);
  g->setFont(u8g2_font_helvR08_tf);
  g->drawUTF8(MARGIN, 252, "Hold KEY for 3 seconds to come back here.");
}

// ---------- input ----------

// Presence. There's no motion sensor, so sound stands in: anything well above the room's own background
// counts as someone there. Quiet evenings turn the card into the photo frame; sound brings it back.
static float noiseFloor = 0;      // tracks the room's steady background (aircon, fan): falls fast, rises slowly
static uint32_t lastActivity = 0;
static bool autoPhotos = false;   // the frame came up on its own, so sound may dismiss it (a chosen one stays)
static int loudness = 0;          // +2 per loud frame, -1 per quiet one: talking builds up, a lone click doesn't

static void checkPresence() {  // caller holds M
  if (view == V_PHOTOS && autoPhotos && routine != ROUTINE_EVENING) view = V_CARD, autoPhotos = false;
  float lvl = i2s.level;
  noiseFloor = noiseFloor == 0 ? lvl : lvl < noiseFloor ? noiseFloor * 0.9f + lvl * 0.1f : noiseFloor * 0.999f + lvl * 0.001f;
  loudness = lvl > noiseFloor * 4 + 40 ? min(loudness + 2, 40) : max(loudness - 1, 0);
  if (voiceOk && loudness >= 10) {  // about half a second of sound well above the room (~12 dB)
    lastActivity = millis();
    if (view == V_PHOTOS && autoPhotos) view = V_CARD, autoPhotos = false;
  }
  if (voiceOk && view == V_CARD && !focus.active() && routine == ROUTINE_EVENING &&
      millis() - lastActivity > IDLE_MINUTES * 60000UL)
    view = V_PHOTOS, autoPhotos = true;
}

static void nextView() {
  view = (view + 1) % V_COUNT;
  viewUntil = millis() + 60000;
  autoPhotos = false;
}

static void startDemoBus() {
  static int n = 0;
  static const char *types[] = {"DD", "SD", "BD"};
  animBus = n % NBUS, demoType = types[n % 3], animStart = millis(), n++;
}

static void injectSample() {  // serial 's': fill every field so the layout can be checked without Wi-Fi
  portalActive = false;       // hides the setup screen until the next reboot
  time_t now = time(nullptr);
  if (now < 1700000000) {
    timeval tv = {1790605812 - 16 * 60, 0};  // 22:14 SGT
    settimeofday(&tv, nullptr);
    now = tv.tv_sec;
  }
  S.nowcast = "Thundery Showers", S.nowcastPeriod = "10.00 pm to Midnight";
  S.uv = 5, S.psi = 104, S.pm25 = 58, S.inT = 27.4f, S.inH = 68;
  S.airT = 30.4f, S.airH = 74, S.windMs = 2.1f;
  const int secs[5] = {185, 7 * 60 + 5, 40, 12 * 60 + 20, 0};  // one per bus-board row; NBUS is at most 5
  const char *types[5] = {"SD", "DD", "BD", "DD", "SD"};
  static const char *loads[3] = {"SEA", "SDA", "LSD"};
  for (int i = 0; i < NBUS; i++)
    for (int k = 0; k < 3; k++) {
      S.bus[i].eta[k] = secs[i] ? now + secs[i] + k * (9 + i) * 60 : 0;
      strlcpy(S.bus[i].type[k], types[i], 3), strlcpy(S.bus[i].load[k], loads[(i + k) % 3], 4);
    }
  const char *pl[3] = {"6 pm 28 Sep to 6 am 29 Sep", "6 am to Midday 29 Sep", "Midday to 6 pm 29 Sep"};
  const char *pt[3] = {"Cloudy", "Cloudy", "Thundery Showers"};
  const char *od[4] = {"Tuesday", "Wednesday", "Thursday", "Friday"};
  const char *ot[4] = {"Afternoon thundery showers", "Late morning and early afternoon thundery showers",
                       "Afternoon thundery showers", "Afternoon thundery showers"};
  for (int i = 0; i < 3; i++) S.periodLabel[i] = pl[i], S.periodText[i] = pt[i];
  for (int i = 0; i < 4; i++) S.outDay[i] = od[i], S.outText[i] = ot[i], S.outLo[i] = 25, S.outHi[i] = 34 - (i == 1);
  S.busLive = true;
  for (int i = 0; i < HIST; i++) {  // a synthetic day for the room chart (memory only, never the card)
    float h = i / 60.0f;
    remember({(uint32_t)(now - (HIST - 1 - i) * 60), 33.5f + 1.6f * sinf((h - 9) / 24 * 2 * (float)M_PI),
              58 + 6 * cosf((h - 3) / 24 * 2 * (float)M_PI), 4.1f});
  }
}

static void dumpFrame() {  // serial 'd': the framebuffer as hex, for tools/grab.py
  static const char hex[] = "0123456789abcdef";
  const uint8_t *b = g->getBufferPtr();
  size_t n = (size_t)g->getBufferTileWidth() * 8 * g->getBufferTileHeight();
  Serial.print("FB:");
  for (size_t i = 0; i < n; i++) Serial.write(hex[b[i] >> 4]), Serial.write(hex[b[i] & 15]);
  Serial.println();
}

static void openPortal() {
  if (portalActive) return;
  wm.startConfigPortal(AP_NAME, apPass);
  portalActive = portalForced = true;
}

static void storeKey(String v);

static void showView(int v) {
  view = v;
  viewUntil = millis() + 60000;
  autoPhotos = false;
  lastActivity = millis();
}

static void updateFocus() {  // caller holds M; catch completion before a stop/start command can discard it
  FocusPhase before = focus.phase;
  uint32_t tick = millis(), workEnd = focus.started + FocusSession::WORK_MS;
  if (!focus.update(tick)) return;
  if (before == FOCUS_WORK) {
    time_t now = time(nullptr);
    time_t finished = now >= 1700000000 ? now - (uint32_t)(tick - workEnd) / 1000 : 0;
    if (garden.earn(finished)) gardenSaved = saveGarden();
  }
  celebrateAt = tick;
  play(SOUND_CHIME);
  showToast(!gardenSaved ? "Flower earned. Storage needs attention." :
            focus.phase == FOCUS_BREAK ? "A flower planted. Take five." : "Break finished. Nicely done.");
}

static void startFocus() {  // caller holds M
  updateFocus();
  if (!focus.active()) focus.start(millis());
  showView(V_FOCUS);
  showToast(focus.phase == FOCUS_BREAK ? "Take a little break" : "One thing at a time");
}

static void stopFocus() {
  updateFocus();
  focus.stop();
  showView(V_CARD);
  showToast("Focus stopped");
}

static void saveIntention(const String &text) {  // never print the user's note
  char clean[sizeof intention];
  time_t now = time(nullptr);
  if (now < 1700000000 || !cleanIntention(text.c_str(), clean, sizeof clean)) {
    Serial.println("intention: refused (clock not set, or use up to 56 printable ASCII characters)");
    return;
  }
  int64_t day = sgtDay(now);
  if (prefs.putString("intention", clean) != strlen(clean) || prefs.putLong64("note-day", day) != sizeof(int64_t)) {
    Serial.println("intention: storage failed");
    return;
  }
  strlcpy(intention, clean, sizeof intention), intentionDay = day;
  showView(V_CARD);
  showToast(*clean ? "Daily intention saved" : "Daily intention cleared");
  Serial.println("intention: saved");
}

// What Flubber heard, acted on from loop() so it never races the renderer.
static void handleVoice() {
  int cmd = heard;
  if (cmd < 0) return;
  heard = -1;
  std::lock_guard<std::mutex> l(M);
  switch (cmd) {
    case VC_BUSES: showView(V_BUSES); break;
    case VC_FORECAST: showView(V_FORECAST); break;
    case VC_ROOM: showView(V_ROOM); break;
    case VC_PHOTOS: showView(V_PHOTOS); break;
    case VC_HOME: showView(V_CARD); break;
    case VC_QUOTE: quoteSkip++, quoteWanted = true, showView(V_CARD); break;
    case VC_FOCUS: startFocus(); break;
    case VC_STOP: stopFocus(); break;
    case VC_GARDEN: showView(V_GARDEN); break;
  }
  play(SOUND_TICK);
}

static void receiveMemory() {  // U<basename>\n<size>\n<bytes>; JPEG first, sidecar last makes a complete postcard
  String name = Serial.readStringUntil('\n'), length = Serial.readStringUntil('\n');
  name.trim(), length.trim();
  bool digits = length.length() && length.length() <= 7;
  for (char ch : length) digits &= isdigit((unsigned char)ch);
  long size = digits ? length.toInt() : 0;
  bool metadata = name.endsWith(".json");
  if (!sdOk || !memoryFilename(name.c_str()) || size <= 0 || size > (metadata ? 256 : 2 * 1024 * 1024)) {
    Serial.println("memory: refused");
    return;
  }
  if (!SD_MMC.mkdir("/memories") && !SD_MMC.exists("/memories")) {
    Serial.println("memory: storage failed");
    return;
  }
  String path = "/memories/" + name;
  if (SD_MMC.exists(path)) { Serial.println("memory: exists"); return; }
  const char *temp = "/memories/.upload";
  File f = SD_MMC.open(temp, FILE_WRITE);
  if (!f) { Serial.println("memory: storage failed"); return; }
  Serial.println("memory: ready");
  uint8_t chunk[256];
  long left = size;
  while (left > 0) {
    size_t wanted = min(left, (long)sizeof chunk), got = Serial.readBytes(chunk, wanted);
    if (got != wanted || f.write(chunk, got) != got) break;
    left -= got;
    esp_task_wdt_reset();
    Serial.println("memory: ack");
  }
  f.flush();
  f.close();
  MemoryCaption caption;
  bool valid = !left && (!metadata || readMemoryFile(temp, caption));
  if (!valid || !SD_MMC.rename(temp, path)) {
    SD_MMC.remove(temp);
    Serial.println("memory: failed");
    return;
  }
  photoDueAt = millis();
  Serial.println("memory: saved");
}

static void handleInput() {
  static bool keyWas = false, bootWas = false, held = false, bootHeld = false;
  static uint32_t keyDown = 0, bootDown = 0;
  bool key = digitalRead(PIN_KEY) == LOW, boot = digitalRead(PIN_BOOT) == LOW;
  // KEY: tap for the next view, hold 3 s for setup (on battery, replugging USB doesn't reboot).
  if ((key && !keyWas) || (boot && !bootWas)) lastActivity = millis();
  if (key && !keyWas) keyDown = millis(), held = false;
  if (key && !held && millis() - keyDown > 3000) held = true, openPortal();
  if (!key && keyWas && !held) nextView();
  if (boot && !bootWas) {
    std::lock_guard<std::mutex> l(M);
    bootDown = millis(), bootHeld = false;
    startFocus();
  }
  if (boot && !bootHeld && millis() - bootDown >= 2000) {
    std::lock_guard<std::mutex> l(M);
    bootHeld = true;
    stopFocus();
  }
  keyWas = key, bootWas = boot;
  if (view != V_CARD && view != V_PHOTOS && view != V_FOCUS && view != V_GARDEN && (int32_t)(millis() - viewUntil) > 0)
    view = focus.active() ? V_FOCUS : V_CARD;

  while (Serial.available()) {
    std::lock_guard<std::mutex> l(M);
    switch (Serial.read()) {
      case 'd': dumpFrame(); break;
      case 'b': startDemoBus(); break;
      case 'r': startFocus(); break;  // same as tapping BOOT
      case 'R': stopFocus(); break;
      case 'g': showView(V_GARDEN); break;
      case 'G': Serial.printf("garden: v1 total %lu month %lu monthly %lu saved %d; memory-v1\n",
                              (unsigned long)garden.total, (unsigned long)garden.month,
                              (unsigned long)garden.monthly, gardenSaved); break;
      case 'U': receiveMemory(); break;
      case 'T': saveIntention(Serial.readStringUntil('\n')); break;
      case 'F': Serial.printf("focus: phase %d, remaining %u s, growth %d, routine %d, intention %s\n",
                              focus.phase, focus.remaining(millis()), focus.growth(millis()), routine,
                              hasIntention(time(nullptr)) ? "set" : "none"); break;
      case 'c': play(SOUND_CHIME); break;
      case 'n': photoDueAt = millis(); break;  // next photo
      case 'z': lastActivity = millis() - IDLE_MINUTES * 60000UL; break;  // pretend the room went quiet
      case 'W': WiFi.disconnect(false); break;  // drop Wi-Fi, to test that health() brings it back
      case 'm': Serial.printf("mic bytes %lu at %lu ms\n", (unsigned long)i2s.bytesRead, (unsigned long)millis()); break;
      case 'C':  // the phrases the recogniser actually holds, with the phonemes it built for each
        for (int i = 0; esp_mn_phrase_t *ph = esp_mn_commands_get_from_index(i); i++)
          Serial.printf("cmd %d id %d \"%s\" -> %s\n", i, ph->command_id, ph->string ? ph->string : "?",
                        ph->phonemes ? ph->phonemes : "(none)");
        Serial.println("cmd end");
        break;
      case 'H': for (;;) {}  // hang the display loop, to test that the watchdog restarts the board
      case 'A':  // record 8 s of mic 1 as the recogniser hears it, with a prompt; loop() prints it as PCM: hex
        if (!i2s.cap) i2s.cap = (int16_t *)ps_malloc(AUDIO_RATE * 8 * sizeof(int16_t));
        i2s.capPos = 0;
        i2s.capLen = AUDIO_RATE * 8;
        strlcpy(toastText, "Recording: speak now", sizeof toastText);
        toastUntil = millis() + 8000;
        view = V_CARD;
        break;
      case 'f':  // feeds: when each job last succeeded, and the weather it holds
        for (auto &j : jobs) Serial.printf("%s %lus ago, ", j.name, j.lastOk ? (millis() - j.lastOk) / 1000 : 0);
        Serial.printf("| outside %.1f C %.0f%% wind %.1f m/s", S.airT, S.airH, S.windMs);
        Serial.printf("| nowcast \"%s\" for %s, uv %d, psi %d | rtc %+ld s vs clock, heap block %u\n", S.nowcast.c_str(),
                      S.nowcastPeriod.c_str(), S.uv, S.psi, (long)(rtcRead() - time(nullptr)), (unsigned)ESP.getMaxAllocHeap());
        break;
      case 'L': {  // how many photos the frame can show
        int n = 0;
        File dir = SD_MMC.open("/photos");
        for (File f = dir ? dir.openNextFile() : File(); f; f = dir.openNextFile()) n += !f.isDirectory() && isJpeg(f.name());
        Serial.printf("photos on card: %d\n", n);
        break;
      }
      case 'D': {  // D<name>\n: delete one photo from /photos
        String name = Serial.readStringUntil('\n');
        name.trim();
        bool ok = name.indexOf('/') < 0 && isJpeg(name.c_str()) && SD_MMC.remove("/photos/" + name);
        Serial.printf("delete %s: %s\n", name.c_str(), ok ? "done" : "failed");
        break;
      }
      case 'x': ESP_SR.end(), voiceOk = false; break;  // voice off until restart
      case 'h': {  // I2C health: who answers, and the SHTC3's wake result
        Serial.print("i2c:");
        for (uint8_t a = 1; a < 127; a++) {
          Wire.beginTransmission(a);
          if (Wire.endTransmission() == 0) Serial.printf(" 0x%02x", a);
        }
        Wire.beginTransmission(0x70);
        Wire.write(0x35), Wire.write(0x17);
        Serial.printf("  shtc3 wake -> %d\n", Wire.endTransmission());
        break;
      }
      case 'P': {  // P<name>\n<size>\n<bytes>: copy a photo onto the card over USB
        String name = Serial.readStringUntil('\n');
        long size = Serial.readStringUntil('\n').toInt();
        name.trim();
        if (!sdOk || name.indexOf('/') >= 0 || !isJpeg(name.c_str()) || size <= 0) {
          Serial.println("upload refused");
          break;
        }
        SD_MMC.mkdir("/photos");
        File f = SD_MMC.open("/photos/" + name, FILE_WRITE);
        if (!f) {
          Serial.println("upload refused");
          break;
        }
        Serial.println("ready");  // the sender holds the bytes until it sees this, so none are read as commands
        static uint8_t chunk[256];  // must match tools/upload.py
        long left = size;
        while (f && left > 0) {  // the sender waits for each '.', so the USB buffer never overflows
          size_t got = Serial.readBytes(chunk, min(left, (long)sizeof chunk));
          if (!got) break;  // 1 s with nothing: give up
          f.write(chunk, got), left -= got;
          esp_task_wdt_reset();
          Serial.write('.');
        }
        if (f) f.close();
        Serial.printf("upload %s: %s\n", name.c_str(), left ? "incomplete" : "saved");
        break;
      }
      case 's': injectSample(); break;
      case 'p': nextView(); break;
      case 'k': openPortal(); break;  // same as holding KEY
      case 'q': quoteSkip++, quoteWanted = true; break;
      case 'i':  // indoor sensor: raw now, first reading after boot, and what the card shows
        Serial.printf("indoor raw %.1f C %.0f%%  first %.1f C %.0f%%  shown %.1f C %.0f%%  up %lus  wifi %d dBm  sd %s %llu MB  history %d  audio %s  voice %s (mic rms %d/%d, %lu listens)  frames %lu  sound %.0f floor %.0f quiet %lus\n",
                      S.rawT, S.rawH, S.firstT, S.firstH, S.inT, S.inH, millis() / 1000,
                      WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0, sdOk ? "ok" : "none",
                      sdOk ? SD_MMC.cardSize() >> 20 : 0ULL, histCount, audioOk ? "ok" : "no codec", voiceOk ? "on" : "off",
                      micRms[0], micRms[1], (unsigned long)voiceListens, (unsigned long)frame, (float)i2s.level, noiseFloor,
                      (unsigned long)((millis() - lastActivity) / 1000));
        break;
      case 'v': {  // battery line spread: a real cell holds steady, an empty holder makes the charger flicker
        uint32_t lo = UINT32_MAX, hi = 0, sum = 0;
        for (int i = 0; i < 200; i++, delay(1)) {
          uint32_t mv = analogReadMilliVolts(PIN_BATTERY);
          lo = min(lo, mv), hi = max(hi, mv), sum += mv;
        }
        Serial.printf("battery min %.2f max %.2f avg %.2f V\n", lo * BATTERY_DIVIDER / 1000, hi * BATTERY_DIVIDER / 1000,
                      sum / 200.0f * BATTERY_DIVIDER / 1000);
        break;
      }
      case 'K': {  // K<key>\n: set the LTA key over USB. Only a base64-looking key, so stray bytes can't clobber it.
        String k = Serial.readStringUntil('\n');
        k.trim();
        bool ok = k.length() >= 20 && k.length() <= 48;
        for (char ch : k) ok &= isalnum((unsigned char)ch) || ch == '+' || ch == '/' || ch == '=';
        if (ok) storeKey(k);
        break;
      }
      case 'w': forcedScene = forcedScene + 1 >= SCENE_COUNT ? -1 : forcedScene + 1; break;
    }
  }
}

// ---------- setup / loop ----------

static void storeKey(String v) {  // caller holds M
  v.trim();
  if (v.length() < 16) return;
  ltaKey = v;
  prefs.putString("lta", ltaKey);
  Serial.println("LTA key saved");  // never the key itself
}

static void saveKey() {
  String v = keyParam.getValue();
  v.trim();
  if (v.isEmpty()) return;
  std::lock_guard<std::mutex> l(M);
  storeKey(v);
  closePortalAt = millis() + 3000;  // let the "saved" page reach the phone before the hotspot goes
}

static void splash(const char *msg) {
  g->clearBuffer();
  g->setFont(u8g2_font_helvR10_tf);
  centered(W / 2, H / 2, msg);
  g->sendBuffer();
}

void setup() {
  Serial.setRxBufferSize(4096);  // uploads arrive faster than the SD card takes them
  Serial.begin(115200);
  pinMode(PIN_KEY, INPUT_PULLUP);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  lcd.begin(0, U8G2_R1);
  g = lcd.getU8g2();
  g->setFontMode(1);
  Wire.begin(PIN_SDA, PIN_SCL);
  prefs.begin("daycard");
  audioOk = audioBegin();
  voiceOk = audioOk && voiceBegin();
  hist = (Sample *)ps_calloc(HIST, sizeof(Sample));
  photo = (uint8_t *)ps_malloc(PSTRIDE * PH), photoSpare = (uint8_t *)ps_malloc(PSTRIDE * PH);
  for (auto &b : quoteBufs) b = (uint8_t *)ps_malloc(QUOTE_BUF);
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
  sdOk = SD_MMC.begin("/sdcard", true);  // true = 1-bit bus, all this board wires up
  if (sdOk) SD_MMC.mkdir("/daycard"), loadHistory(), loadSeenQuotes();
  loadGarden();
  ltaKey = prefs.getString("lta", "");
  prefs.getString("intention", "").toCharArray(intention, sizeof intention);
  intentionDay = prefs.getLong64("note-day", -1);
  snprintf(apPass, sizeof apPass, "%08lu", (unsigned long)(esp_random() % 100000000UL));

  bool forcePortal = digitalRead(PIN_KEY) == LOW;
  splash(forcePortal ? "Opening setup..." : "Connecting to Wi-Fi...");
  WiFi.mode(WIFI_STA);
  wm.addParameter(&keyParam);
  wm.setSaveConfigCallback(saveKey);
  wm.setSaveParamsCallback(saveKey);
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(600);
  wm.setConnectTimeout(20);
  wm.setTitle("Daycard");
  wm.setParamsPage(true);  // the key gets its own "Setup" page, so adding it doesn't mean re-entering Wi-Fi
  if (forcePortal) wm.startConfigPortal(AP_NAME, apPass), portalActive = portalForced = true;
  else if (wm.getWiFiIsSaved()) WiFi.begin();  // a known network: never park on the setup screen, health() retries
  else portalActive = !wm.autoConnect(AP_NAME, apPass);

  // Time: the RTC first, so a restart while offline still has a clock; NTP corrects it and re-arms the RTC.
  if (time_t t = rtcRead()) {
    timeval tv = {t, 0};
    settimeofday(&tv, nullptr);
  }
  sntp_set_time_sync_notification_cb(onNtp);

  configTzTime("SGT-8", "sg.pool.ntp.org", "time.google.com");
  static const char *reasons[] = {"unknown", "power-on", "external", "software", "panic", "interrupt watchdog",
                                  "task watchdog", "watchdog", "deep sleep", "brownout", "sdio", "usb", "jtag",
                                  "efuse", "power glitch", "cpu lockup"};
  esp_reset_reason_t r = esp_reset_reason();
  char why[48];
  snprintf(why, sizeof why, "boot after %s", r < (int)(sizeof reasons / sizeof *reasons) ? reasons[r] : "?");
  noteRestart(why);

  // Watchdog: if the display loop or the network task stops checking in for 2 minutes, restart.
  esp_task_wdt_config_t wdt = {.timeout_ms = 120000, .idle_core_mask = 0, .trigger_panic = true};
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
  esp_task_wdt_add(nullptr);
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  esp_task_wdt_reset();
  if (portalActive) {
    wm.process();
    if (closePortalAt && (int32_t)(millis() - closePortalAt) > 0) closePortalAt = 0, wm.stopConfigPortal();
    // First-time setup ends once we're online; a KEY-opened portal ends when it closes or times out.
    if (!wm.getConfigPortalActive() || (!portalForced && WiFi.status() == WL_CONNECTED)) {
      portalActive = portalForced = false;
      if (WiFi.status() != WL_CONNECTED) WiFi.begin();  // portal timed out: keep trying the saved network
    }
  }
  handleInput();
  handleVoice();
  if (i2s.cap && i2s.capLen && i2s.capPos >= i2s.capLen) {  // a finished 'A' recording: send it once
    static const char hex[] = "0123456789abcdef";
    const uint8_t *b = (const uint8_t *)i2s.cap;
    Serial.print("PCM:");
    for (size_t i = 0; i < i2s.capLen * 2; i++) Serial.write(hex[b[i] >> 4]), Serial.write(hex[b[i] & 15]);
    Serial.println();
    i2s.capLen = 0;
  }

  static uint32_t last = 0;
  if (millis() - last < FRAME_MS) return delay(2);
  last = millis();

  time_t now = time(nullptr);
  tm lt;
  localtime_r(&now, &lt);
  bool haveTime = lt.tm_year + 1900 >= 2025;
  {
    std::lock_guard<std::mutex> l(M);
    if (haveTime) {
      routine = routineFor(lt.tm_hour, MORNING_HOUR, WORK_HOUR, EVENING_HOUR);
      if (routine == ROUTINE_MORNING) checkArrivals(now);
      if (*intention && intentionDay != sgtDay(now)) {
        intention[0] = 0, intentionDay = -1;
        prefs.remove("intention"), prefs.remove("note-day");
      }
    }
    updateFocus();
    checkPresence();
    g->clearBuffer();
    g->setDrawColor(1);
    if (portalActive) drawSetup();
    else if (view == V_BUSES) drawBusBoard(now);
    else if (view == V_ROOM) drawRoom(now);
    else if (view == V_FORECAST) drawForecast();
    else if (view == V_PHOTOS) drawPhotos(lt, haveTime);
    else if (view == V_FOCUS) drawFocus(now);
    else if (view == V_GARDEN) drawGarden(now);
    else drawMain(lt, haveTime, now);
    drawToast();
  }
  g->sendBuffer();
  frame++;
}
