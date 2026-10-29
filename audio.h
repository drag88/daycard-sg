// Speaker: ES8311 codec on the shared I2C bus, fed over I2S. The register sequence mirrors Espressif's
// esp_codec_dev es8311.c (open, set_fs, start) for slave mode, 16 kHz, 16-bit I2S, MCLK = 256 x fs.
#pragma once
#include <ESP_I2S.h>
#include <Wire.h>
#include <math.h>

constexpr int PIN_I2S_MCLK = 16, PIN_I2S_BCLK = 9, PIN_I2S_WS = 45, PIN_I2S_DOUT = 8, PIN_I2S_DIN = 10, PIN_PA = 46;
constexpr int AUDIO_RATE = 16000;
static const float CHIME_VOLUME = 0.35f;  // of full scale; the 2 W speaker is loud

// Every mic read (including the speech recogniser's) passes through here, so room loudness comes for free.
class MeteredI2S : public I2SClass {
public:
  volatile float level = 0;  // RMS of mic 1 over the latest read
  int16_t *cap = nullptr;    // diagnostics: a copy of mic 1 exactly as the recogniser gets it (serial 'A')
  volatile size_t capLen = 0, capPos = 0;
  volatile uint32_t bytesRead = 0;  // diagnostics: should grow at 64 KB/s (16 kHz x 2 ch x 16 bit)

  size_t readBytes(char *buffer, size_t size) override {
    size_t got = I2SClass::readBytes(buffer, size), n = got / 4;  // 16-bit stereo frames
    bytesRead = bytesRead + got;
    const int16_t *s = (const int16_t *)buffer;
    if (n) {
      int64_t sum = 0;
      for (size_t i = 0; i < n; i++) sum += (int32_t)s[2 * i] * s[2 * i];
      level = sqrtf((float)sum / n);
    }
    size_t pos = capPos;
    for (size_t i = 0; cap && i < n && pos < capLen; i++) cap[pos++] = s[2 * i];
    capPos = pos;
    return got;
  }
};
static MeteredI2S i2s;
static QueueHandle_t sounds;
enum Sound { SOUND_TICK, SOUND_CHIME };

static bool es8311(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(0x18);
  Wire.write(reg), Wire.write(v);
  return Wire.endTransmission() == 0;
}

static bool es8311Update(uint8_t reg, uint8_t keep, uint8_t set) {
  Wire.beginTransmission(0x18);
  Wire.write(reg);
  if (Wire.endTransmission(false) || Wire.requestFrom(0x18, 1) != 1) return false;
  return es8311(reg, (Wire.read() & keep) | set);
}

static bool codecBegin() {
  bool ok = es8311(0x44, 0x08) | es8311(0x44, 0x08);  // the first write after power-up can fail; the driver repeats it
  static const uint8_t open[][2] = {{0x01, 0x30}, {0x02, 0x00}, {0x03, 0x10}, {0x16, 0x24}, {0x04, 0x10}, {0x05, 0x00},
                                    {0x0B, 0x00}, {0x0C, 0x00}, {0x10, 0x1F}, {0x11, 0x7F}, {0x00, 0x80}};
  for (auto &r : open) ok &= es8311(r[0], r[1]);
  ok &= es8311Update(0x00, 0xBF, 0x00);  // slave
  ok &= es8311(0x01, 0x3F);              // clocks from the MCLK pin, not inverted
  ok &= es8311Update(0x06, (uint8_t)~0x20, 0x00);
  ok &= es8311(0x13, 0x10) & es8311(0x1B, 0x0A) & es8311(0x1C, 0x6A) & es8311(0x44, 0x58);
  // 16-bit, standard I2S
  ok &= es8311Update(0x09, 0xFF, 0x0C) & es8311Update(0x0A, 0xFF, 0x0C);
  ok &= es8311Update(0x09, 0xFC, 0x00) & es8311Update(0x0A, 0xFC, 0x00);
  // 16 kHz from 4.096 MHz: pre_div 1, mult 1, adc/dac div 1, fs_mode 0, lrck 0x00FF, bclk div 4, osr 0x10/0x20
  ok &= es8311Update(0x02, 0x07, 0x00) & es8311(0x05, 0x00);
  ok &= es8311Update(0x03, 0x80, 0x10) & es8311Update(0x04, 0x80, 0x20);
  ok &= es8311Update(0x07, 0xC0, 0x00) & es8311(0x08, 0xFF) & es8311Update(0x06, 0xE0, 0x03);
  // start, DAC only: DAC port on (bit 6 clear), ADC port muted (bit 6 set)
  ok &= es8311(0x00, 0x80) & es8311(0x01, 0x3F);
  ok &= es8311Update(0x09, 0xBF, 0x00) & es8311Update(0x0A, 0xBF, 0x40);
  static const uint8_t start[][2] = {{0x17, 0xBF}, {0x0E, 0x02}, {0x12, 0x00}, {0x14, 0x1A}, {0x0D, 0x01},
                                     {0x15, 0x40}, {0x37, 0x08}, {0x45, 0x00}, {0x32, 0xBF}};  // 0x32: 0 dB
  for (auto &r : start) ok &= es8311(r[0], r[1]);
  return ok;
}

// Microphones: ES7210 ADC at 0x40, mics 1 and 2 as a standard-I2S stereo pair on DIN. Sequence mirrors
// esp_codec_dev es7210.c (open, set_fs, start) in slave mode, 16-bit, 30 dB gain.
static bool es7210(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(0x40);
  Wire.write(reg), Wire.write(v);
  return Wire.endTransmission() == 0;
}

static bool es7210Update(uint8_t reg, uint8_t mask, uint8_t bits) {
  Wire.beginTransmission(0x40);
  Wire.write(reg);
  if (Wire.endTransmission(false) || Wire.requestFrom(0x40, 1) != 1) return false;
  return es7210(reg, (Wire.read() & ~mask) | (bits & mask));
}

static bool micSelect() {  // mics 1 and 2, 30 dB (gain code 10: 3 dB steps)
  bool ok = true;
  for (int i = 0; i < 4; i++) ok &= es7210Update(0x43 + i, 0x10, 0x00);
  ok &= es7210(0x4B, 0xFF) & es7210(0x4C, 0xFF);
  for (uint8_t reg : {0x43, 0x44}) {
    ok &= es7210Update(0x01, 0x0B, 0x00) & es7210(0x4B, 0x00);
    ok &= es7210Update(reg, 0x10, 0x10) & es7210Update(reg, 0x0F, 10);
  }
  return ok & es7210(0x12, 0x00);  // two mics: plain I2S, not TDM
}

static bool micsBegin() {
  static const uint8_t open[][2] = {{0x00, 0xFF}, {0x00, 0x41}, {0x01, 0x3F}, {0x09, 0x30}, {0x0A, 0x30},
                                    {0x23, 0x2A}, {0x22, 0x0A}, {0x20, 0x0A}, {0x21, 0x2A}};
  bool ok = true;
  for (auto &r : open) ok &= es7210(r[0], r[1]);
  ok &= es7210Update(0x08, 0x01, 0x00);  // slave
  static const uint8_t analog[][2] = {{0x40, 0x43}, {0x41, 0x70}, {0x42, 0x70}, {0x07, 0x20}, {0x02, 0xC1}};
  for (auto &r : analog) ok &= es7210(r[0], r[1]);
  ok &= micSelect();
  Wire.beginTransmission(0x40);
  Wire.write(0x01);
  uint8_t clockOff = Wire.endTransmission(false) == 0 && Wire.requestFrom(0x40, 1) == 1 ? Wire.read() : 0x3F;
  ok &= es7210Update(0x11, 0xE0, 0x60) & es7210Update(0x11, 0x03, 0x00);  // 16-bit, standard I2S
  static const uint8_t start[][2] = {{0x06, 0x00}, {0x40, 0x43}, {0x47, 0x08}, {0x48, 0x08}, {0x49, 0x08}, {0x4A, 0x08}};
  ok &= es7210(0x01, clockOff);
  for (auto &r : start) ok &= es7210(r[0], r[1]);
  ok &= micSelect();
  return ok & es7210(0x40, 0x43) & es7210(0x00, 0x71) & es7210(0x00, 0x41);
}

// Plays queued sounds so the renderer never waits on audio. The amplifier is only on while a sound plays.
static void soundTask(void *) {
  static int16_t buf[256 * 2];
  for (;;) {
    int s;
    if (xQueueReceive(sounds, &s, portMAX_DELAY) != pdTRUE) continue;
    // A chime is two bell-like notes (E6 then C6); a tick is one short blip.
    const float notes[2] = {1318.5f, 1046.5f};
    int nNotes = s == SOUND_CHIME ? 2 : 1, len = s == SOUND_CHIME ? AUDIO_RATE * 6 / 10 : AUDIO_RATE / 20;
    digitalWrite(PIN_PA, HIGH);
    for (int n = 0; n < nNotes; n++)
      for (int i = 0; i < len; i += 256) {
        for (int j = 0; j < 256; j++) {
          float t = (float)(i + j) / AUDIO_RATE, env = expf(-t * (s == SOUND_CHIME ? 5.0f : 40.0f));
          float f = s == SOUND_CHIME ? notes[n] : 1760.0f;
          int16_t v = (int16_t)(32767 * CHIME_VOLUME * env * (sinf(2 * (float)M_PI * f * t) + 0.3f * sinf(4 * (float)M_PI * f * t)) / 1.3f);
          buf[2 * j] = buf[2 * j + 1] = v;
        }
        i2s.write((uint8_t *)buf, sizeof buf);
      }
    memset(buf, 0, sizeof buf);
    for (int i = 0; i < 12; i++) i2s.write((uint8_t *)buf, sizeof buf);  // flush the DMA ring so nothing repeats
    digitalWrite(PIN_PA, LOW);
  }
}

static bool audioBegin() {
  pinMode(PIN_PA, OUTPUT);
  digitalWrite(PIN_PA, LOW);
  i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
  if (!i2s.begin(I2S_MODE_STD, AUDIO_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) return false;
  bool ok = codecBegin() & micsBegin();  // after i2s.begin: both chips need MCLK running
  sounds = xQueueCreate(4, sizeof(int));
  xTaskCreatePinnedToCore(soundTask, "sound", 4096, nullptr, 2, nullptr, 0);
  return ok;
}

static void play(Sound s) {
  int v = s;
  if (sounds) xQueueSend(sounds, &v, 0);
}
