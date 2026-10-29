// Photo frame: JPEGs from /photos on the card, decoded by the ESP32-S3's ROM TJpgDec straight from the file
// (a phone photo never has to fit in RAM), fitted to the panel and Atkinson-dithered to 1 bit.
#pragma once
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include <vector>
#include "keepsakes.h"
#include "esp32s3/rom/tjpgd.h"

constexpr int PW = 400, PH = 300, PSTRIDE = PW / 8;

struct JpegSrc {
  File *f;
  uint8_t *gray;  // decoded image at the chosen 1/2^n scale, one byte per pixel
  int w;
};

static UINT jpegIn(JDEC *jd, BYTE *buf, UINT n) {
  File *f = ((JpegSrc *)jd->device)->f;
  if (buf) return f->read(buf, n);
  return f->seek(f->position() + n) ? n : 0;
}

static UINT jpegOut(JDEC *jd, void *bitmap, JRECT *r) {
  JpegSrc *src = (JpegSrc *)jd->device;
  const uint8_t *rgb = (const uint8_t *)bitmap;
  for (int y = r->top; y <= r->bottom; y++)
    for (int x = r->left; x <= r->right; x++, rgb += 3)
      src->gray[y * src->w + x] = (rgb[0] * 77 + rgb[1] * 150 + rgb[2] * 29) >> 8;  // Rec. 601 luma
  return 1;
}

static bool isJpeg(const char *name) {
  const char *dot = strrchr(name, '.');
  return name[0] != '.' && dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"));  // skips macOS ._ files
}

// Decode /photos/<name> into a PW x PH 1-bit XBM (rows of PSTRIDE bytes, LSB = leftmost, 1 = ink).
// Landscape photos fill the screen (cropped); portrait ones are shown whole on paper.
static bool loadPhoto(const char *path, uint8_t *out, int height = PH) {
  if (height < 1 || height > PH) return false;
  File f = SD_MMC.open(path);
  if (!f) return false;
  static uint8_t pool[4096];
  JDEC jd;
  JpegSrc src = {&f, nullptr, 0};
  if (jd_prepare(&jd, jpegIn, pool, sizeof pool, &src) != JDR_OK) return false;  // progressive or not a JPEG
  bool cover = jd.width >= jd.height;
  float fit = cover ? max((float)PW / jd.width, (float)height / jd.height) : min((float)PW / jd.width, (float)height / jd.height);
  int s = 0;
  while (s < 3 && fit * (2 << s) <= 1) s++;  // decode as small as possible without going below the fitted size
  int dw = (jd.width + (1 << s) - 1) >> s, dh = (jd.height + (1 << s) - 1) >> s;
  src.w = dw;
  src.gray = (uint8_t *)ps_calloc((size_t)dw * dh, 1);
  uint8_t *pix = (uint8_t *)ps_malloc(PW * PH);
  bool ok = src.gray && pix && jd_decomp(&jd, jpegOut, s) == JDR_OK;
  if (ok) {
    // Box-filter the decoded image into the panel, centred.
    float k = fit * (1 << s);  // decoded px -> panel px
    float ox = (PW - dw * k) / 2, oy = (height - dh * k) / 2;
    for (int y = 0; y < PH; y++)
      for (int x = 0; x < PW; x++) {
        float fx = (x - ox) / k, fy = (y - oy) / k;
        if (y >= height || fx < 0 || fy < 0 || fx >= dw || fy >= dh) {
          pix[y * PW + x] = 255;  // paper around a portrait photo
          continue;
        }
        int x0 = (int)fx, y0 = (int)fy, x1 = min((int)((x + 1 - ox) / k), dw), y1 = min((int)((y + 1 - oy) / k), dh);
        uint32_t sum = 0, n = 0;
        for (int sy = y0; sy < max(y1, y0 + 1); sy++)
          for (int sx = x0; sx < max(x1, x0 + 1); sx++) sum += src.gray[sy * dw + sx], n++;
        pix[y * PW + x] = sum / n;
      }
    // Auto-levels on the 2nd-98th percentile, then lift the midtones: the panel's white is a light grey.
    uint32_t hist[256] = {};
    for (int i = 0; i < PW * PH; i++) hist[pix[i]]++;
    int lo = 0, hi = 255;
    for (uint32_t c = 0; lo < 255 && (c += hist[lo]) < PW * PH / 50;) lo++;
    for (uint32_t c = 0; hi > 0 && (c += hist[hi]) < PW * PH / 50;) hi--;
    uint8_t lut[256];
    for (int v = 0; v < 256; v++) {
      float t = constrain((v - lo) / (float)max(hi - lo, 1), 0.0f, 1.0f);
      lut[v] = (uint8_t)(255 * powf(t, 0.8f));
    }
    // Atkinson dithering: spreads 3/4 of the error, which keeps contrast crisp on a 1-bit screen.
    int16_t *e = (int16_t *)ps_malloc(sizeof(int16_t) * PW * (PH + 2));
    if (!e) ok = false;
    else {
      for (int i = 0; i < PW * PH; i++) e[i] = lut[pix[i]];
      memset(e + PW * PH, 0, sizeof(int16_t) * PW * 2);
      memset(out, 0, PSTRIDE * PH);
      for (int y = 0; y < PH; y++)
        for (int x = 0; x < PW; x++) {
          int16_t old = e[y * PW + x], ink = old < 128, err = (old - (ink ? 0 : 255)) / 8;
          if (ink) out[y * PSTRIDE + x / 8] |= 1 << (x & 7);
          static const int8_t spread[6][2] = {{1, 0}, {2, 0}, {-1, 1}, {0, 1}, {1, 1}, {0, 2}};
          for (auto &d : spread) {
            int nx = x + d[0], ny = y + d[1];
            if (nx >= 0 && nx < PW) e[ny * PW + nx] += err;
          }
        }
      free(e);
    }
  }
  free(src.gray);
  free(pix);
  return ok;
}

// A random JPEG from /photos other than `avoid` (unless it's the only one), as a full path; empty if none.
static String randomPhoto(const String &avoid) {
  File dir = SD_MMC.open("/photos");
  if (!dir || !dir.isDirectory()) return "";
  std::vector<String> paths;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile())
    if (!f.isDirectory() && isJpeg(f.name())) paths.push_back(String("/photos/") + f.name());
  if (paths.empty()) return "";
  size_t i = esp_random() % paths.size();
  if (paths[i] == avoid && paths.size() > 1) i = (i + 1 + esp_random() % (paths.size() - 1)) % paths.size();  // any other
  return paths[i];
}

// "123 - Jane Doe.jpg", as tools/nature.py names Unsplash photos, credits Jane Doe. Your own photos get no credit.
static String creditFor(const String &path) {
  int slash = path.lastIndexOf('/'), dash = path.indexOf(" - ", slash), dot = path.lastIndexOf('.');
  if (dash <= slash + 1 || dot < dash) return "";
  for (int i = slash + 1; i < dash; i++)
    if (!isdigit((unsigned char)path[i])) return "";
  return path.substring(dash + 3, dot);
}

// A sidecar travels with its JPEG: /memories/trip.jpg.json contains date and caption.
static bool readMemoryFile(const String &path, MemoryCaption &out) {
  File f = SD_MMC.open(path);
  if (!f || f.size() > 256) return false;
  JsonDocument doc;
  if (deserializeJson(doc, f) || !doc["date"].is<const char *>() || !doc["caption"].is<const char *>()) return false;
  return cleanMemory(doc["date"].as<const char *>(), doc["caption"].as<const char *>(), out);
}

static bool readMemory(const String &path, MemoryCaption &out) { return readMemoryFile(path + ".json", out); }

static String randomMemory(const String &avoid, time_t now, bool seasonal, MemoryCaption &out) {
  File dir = SD_MMC.open("/memories");
  if (!dir || !dir.isDirectory()) return "";
  String picked, fallback;
  MemoryCaption fallbackCaption;
  int best = -1;
  uint32_t seen = 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory() || !isJpeg(f.name())) continue;
    String path = String("/memories/") + f.name();
    MemoryCaption caption;
    if (!readMemory(path, caption)) continue;
    if (path == avoid) { fallback = path; fallbackCaption = caption; continue; }
    int rank = seasonal ? memorySeason(caption.date, now) : 0;
    if (rank > best) best = rank, seen = 0;
    if (rank == best && esp_random() % ++seen == 0) picked = path, out = caption;
  }
  if (picked.isEmpty() && !fallback.isEmpty()) out = fallbackCaption, picked = fallback;
  return picked;
}
