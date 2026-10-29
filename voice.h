// Offline voice commands with Espressif's ESP-SR (MultiNet 7, English), from the ES7210 mic pair.
// There's no trained wake word, so every phrase starts with CALL_NAME (config.h, "flubber" by default) and the
// recogniser runs continuously: the name does the wake word's job. ponytail: phrase-level matching only; if false
// triggers or misses are a problem, train a real wake word with microWakeWord and gate MultiNet behind it.
#pragma once
#include "ESP_SR.h"
#include "audio.h"

// Keep action phrases short and distinct; long shared prefixes previously caused false bus reminders.
enum VoiceCmd { VC_BUSES, VC_FORECAST, VC_ROOM, VC_PHOTOS, VC_HOME, VC_QUOTE, VC_FOCUS, VC_STOP, VC_GARDEN };

static const sr_cmd_t VOICE_COMMANDS[] = {
  {VC_BUSES, CALL_NAME " next bus"},          {VC_BUSES, CALL_NAME " show the buses"},   {VC_BUSES, CALL_NAME " bus times"},
  {VC_FORECAST, CALL_NAME " show the forecast"}, {VC_FORECAST, CALL_NAME " will it rain"}, {VC_FORECAST, CALL_NAME " weather"},
  {VC_ROOM, CALL_NAME " show the room"},      {VC_ROOM, CALL_NAME " show room"},         {VC_ROOM, CALL_NAME " room temperature"},
  {VC_PHOTOS, CALL_NAME " show photos"},      {VC_PHOTOS, CALL_NAME " photo frame"},
  {VC_HOME, CALL_NAME " go home"},            {VC_HOME, CALL_NAME " main screen"},
  {VC_QUOTE, CALL_NAME " next quote"},
  {VC_FOCUS, CALL_NAME " focus"},            {VC_STOP, CALL_NAME " stop"},
  {VC_GARDEN, CALL_NAME " show garden"},     {VC_GARDEN, CALL_NAME " my garden"},
};

static volatile int heard = -1;  // set by the recogniser's task, consumed by loop()
static volatile uint32_t voiceListens = 0;
static int micRms[2] = {-1, -1};

static void onVoice(sr_event_t event, int command, int phrase) {
  if (event == SR_EVENT_COMMAND) {
    heard = command;
    constexpr int n = sizeof VOICE_COMMANDS / sizeof *VOICE_COMMANDS;
    Serial.printf("voice: \"%s\"\n", phrase >= 0 && phrase < n ? VOICE_COMMANDS[phrase].str : "?");
  }
  voiceListens = voiceListens + 1;
  ESP_SR.setMode(SR_MODE_COMMAND);  // after a command or a timeout, listen again straight away
}

// Half a second of raw audio, so a silent or miswired mic shows up as 0 rather than as "it never hears me".
static void measureMics() {
  static int16_t buf[1024];
  int64_t sum[2] = {};
  int n = 0;
  for (uint32_t end = millis() + 500; millis() < end;) {
    size_t got = i2s.readBytes((char *)buf, sizeof buf) / sizeof(int16_t);
    for (size_t i = 0; i + 1 < got; i += 2, n++) sum[0] += buf[i] * buf[i], sum[1] += buf[i + 1] * buf[i + 1];
  }
  for (int c = 0; c < 2; c++) micRms[c] = n ? (int)sqrtf((float)sum[c] / n) : 0;
}

static bool voiceBegin() {
  measureMics();
  ESP_SR.onEvent(onVoice);
  // One mic ("MN"): two would add ESP-SR's two-mic separation stage, which alone ate the display's core.
  if (!ESP_SR.begin(i2s, VOICE_COMMANDS, sizeof VOICE_COMMANDS / sizeof *VOICE_COMMANDS, SR_CHANNELS_STEREO,
                    SR_MODE_COMMAND, "MN"))
    return false;
  // ESP-SR's detector needs core 1 to itself to keep up in real time; the build puts loop() on core 0 (LoopCore=0).
  return true;
}
