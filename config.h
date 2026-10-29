// Everything you'd want to change about your card lives here. The values below are examples (a stop at Orchard);
// replace them with your own. To keep your settings out of git, copy this file to config.local.h and edit that
// instead: the build uses config.local.h when it exists, and .gitignore keeps it private.
// On-screen text is Latin-1 only: no en dashes, curly quotes or emoji.
#pragma once
#include "logic.h"

// ---- Buses (LTA DataMall) ----
// The five-digit code is printed on the bus stop pole. The board shows up to five services, one row each.
static const char *BUS_STOP = "09047";
static const char *BUS_STOP_NAME = "ORCHARD STN / TANG PLAZA";  // header of the bus board
static const char *BUS_SERVICES[] = {"36", "77", "124", "167", "174"};
static const char *MY_BUSES[] = {"36", "77"};  // bold on the bus board; {""} for none

// ---- Weather (NEA, data.gov.sg) ----
// NEA_AREA must match an area name in https://api-open.data.gov.sg/v2/real-time/api/two-hr-forecast exactly
// (e.g. "Ang Mo Kio", "Bedok", "City", "Tampines"). REGION is the nearest of north, south, east, west, central;
// it picks the PSI reading and the 24-hour forecast.
static const char *NEA_AREA = "City";
static const char *REGION = "south";
// Weather stations for outside temperature, humidity and wind, nearest first; the second fills any minute the
// first misses. IDs and coordinates: https://api-open.data.gov.sg/v2/real-time/api/air-temperature
static const char *const AIR_STATIONS[] = {"S111", "S60"};  // Scotts Road, Sentosa

// ---- World clocks under the big clock ----
// Exactly three {label, standard UTC offset in minutes, daylight-saving rule}. Three-letter labels fit best. Rules: NO_DST,
// US_DST (US, Canada), EU_DST (Europe, UK), AU_DST (Sydney, Melbourne, Canberra, Hobart), NZ_DST.
// Examples: {"IND", 330, NO_DST}, {"SYD", 600, AU_DST}, {"SFO", -480, US_DST}, {"DXB", 240, NO_DST}.
static const WorldClock WORLD_CLOCKS[] = {{"LON", 0, EU_DST}, {"NYC", -300, US_DST}, {"TYO", 540, NO_DST}};

// ---- Voice companion ----
// Every voice command starts with this name, which stands in for a wake word; it's also the name on screen.
// Lower case, letters and spaces only. Two or three syllables that sound like nothing else in the room work best.
// After changing it, check what the recogniser heard with the serial command C and tools/voicetest.py.
#define CALL_NAME "flubber"

// ---- Daily routine (Singapore time) ----
// 7-10 AM shows buses along the bottom; 10 AM-6 PM favours the clock and focus; 6 PM-7 AM allows the photo frame.
static const int MORNING_HOUR = 7, WORK_HOUR = 10, EVENING_HOUR = 18;
static const int IDLE_MINUTES = 5;   // quiet evenings turn the card into the photo frame after this long
static const int PHOTO_MINUTES = 5;  // a new photo this often
static const int QUOTE_MINUTES = 30; // a new quote this often

// ---- Quotes ----
// Live quotes come from these Wikiquote pages, a random sourced line each time.
// {page title as in the URL, name on the card}. A page with fewer than six short sourced lines repeats itself.
static const char *const QUOTE_AUTHORS[][2] = {
  {"Richard_Feynman", "Richard Feynman"}, {"Marie_Curie", "Marie Curie"}, {"Carl_Sagan", "Carl Sagan"},
  {"Albert_Einstein", "Albert Einstein"}, {"Isaac_Newton", "Isaac Newton"}, {"Charles_Darwin", "Charles Darwin"},
  {"Michael_Faraday", "Michael Faraday"}, {"Grace_Hopper", "Grace Hopper"}, {"Donald_Knuth", "Donald Knuth"},
  {"Edsger_W._Dijkstra", "Edsger W. Dijkstra"}, {"Alan_Kay", "Alan Kay"}, {"Richard_Hamming", "Richard Hamming"},
  {"Ken_Thompson", "Ken Thompson"}, {"Bjarne_Stroustrup", "Bjarne Stroustrup"}, {"Steve_Wozniak", "Steve Wozniak"},
  {"Buckminster_Fuller", "Buckminster Fuller"}, {"Freeman_Dyson", "Freeman Dyson"},
  {"Stephen_Hawking", "Stephen Hawking"}, {"A._P._J._Abdul_Kalam", "A. P. J. Abdul Kalam"},
  {"Socrates", "Socrates"}, {"Seneca_the_Younger", "Seneca"}, {"Marcus_Aurelius", "Marcus Aurelius"},
  {"Michel_de_Montaigne", "Michel de Montaigne"}, {"Blaise_Pascal", "Blaise Pascal"}, {"Baruch_Spinoza", "Baruch Spinoza"},
  {"Bertrand_Russell", "Bertrand Russell"}, {"Ludwig_Wittgenstein", "Ludwig Wittgenstein"}, {"Simone_Weil", "Simone Weil"},
  {"Confucius", "Confucius"}, {"Laozi", "Laozi"}, {"Zhuangzi", "Zhuangzi"}, {"Mencius", "Mencius"},
  {"Gautama_Buddha", "Gautama Buddha"}, {"Thiruvalluvar", "Thiruvalluvar"}, {"Chanakya", "Chanakya"}, {"Kabir", "Kabir"},
  {"Rumi", "Rumi"}, {"Rabindranath_Tagore", "Rabindranath Tagore"}, {"Swami_Vivekananda", "Swami Vivekananda"},
  {"Ramana_Maharshi", "Ramana Maharshi"}, {"Jiddu_Krishnamurti", "Jiddu Krishnamurti"}, {"D%C5%8Dgen", "Dogen"},
  {"Shunryu_Suzuki", "Shunryu Suzuki"}, {"Th%C3%ADch_Nh%E1%BA%A5t_H%E1%BA%A1nh", "Thich Nhat Hanh"},
  {"Miyamoto_Musashi", "Miyamoto Musashi"}, {"Alan_Watts", "Alan Watts"}};

// ---- Setup and calibration ----
static const char *AP_NAME = "Daycard-Setup";  // the Wi-Fi network the board opens for setup
// The SHTC3 sensor sits next to the warm ESP32 and reads high. To calibrate, put a room thermometer beside the
// card, read the serial command i at the same moment, set TEMP_OFFSET_C = meter - raw, flash, then set
// RH_OFFSET = old RH_OFFSET + meter humidity - shown humidity (shown already includes the old offset).
// These defaults came from one board with voice on, plugged in.
// ponytail: fixed offsets; the board runs cooler just after boot and warmer while charging, add an uptime/charge
// model if that matters.
static const float TEMP_OFFSET_C = -5.4f, RH_OFFSET = -4.0f;
// The battery reaches GPIO4 through a 3:1 divider (Waveshare's ADC example). Tune if a multimeter disagrees.
static const float BATTERY_DIVIDER = 3.0f;
