#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include "config.h"

enum Mode : uint8_t { MODE_OFF = 0, MODE_HUMIDIFY = 1, MODE_DEHUMIDIFY = 2, MODE_AUTO = 3 };
static const char *const MODE_NAMES[] = {"OFF", "HUMIDIFY", "DEHUMIDIFY", "AUTO"};

#define SETTINGS_FILE  "/settings.bin"
#define SETTINGS_MAGIC 0x48504C31UL  // "HPL1" — bump when the struct layout changes

struct Settings {
  uint32_t magic;
  float    setpoint;          // %RH
  uint8_t  mode;              // Mode
  uint8_t  tuneRule;          // index into TUNE_RULES
  uint8_t  oledFlip;
  uint8_t  reserved;
  float    kp, ki, kd;        // Kp: %out/%RH, Ki: %out/(%RH*s), Kd: %out*s/%RH
  float    ku, tu;            // last auto-tune result (0 = never tuned)
  uint16_t windowSec;         // time-proportioning window of the relay outputs
  uint16_t minOnSec;          // relay protection
  uint16_t minOffSec;
  int16_t  tzMinutes;         // browser timezone offset, used for the OLED clock
  float    tuneBand;          // relay hysteresis used during auto-tune, %RH
  float    alarmLow, alarmHigh;
  float    tempOffset, humOffset;  // sensor calibration
  char     apSsid[33];
  char     apPass[65];
};

inline void settingsDefaults(Settings &s) {
  memset(&s, 0, sizeof(s));
  s.magic     = SETTINGS_MAGIC;
  s.setpoint  = 55;
  s.mode      = MODE_HUMIDIFY;
  s.tuneRule  = 1;  // Tyreus-Luyben: gentle, suits slow humidity loops
  s.kp = 8; s.ki = 0.02f; s.kd = 30;
  s.windowSec = 20; s.minOnSec = 3; s.minOffSec = 5;
  s.tuneBand  = 0.8f;
  s.alarmLow  = 25; s.alarmHigh = 85;
  strlcpy(s.apSsid, DEFAULT_AP_SSID, sizeof(s.apSsid));
  strlcpy(s.apPass, DEFAULT_AP_PASS, sizeof(s.apPass));
}

inline float clampF(float v, float lo, float hi) { return isnan(v) ? lo : (v < lo ? lo : (v > hi ? hi : v)); }

inline void settingsSanitize(Settings &s) {
  s.setpoint  = clampF(s.setpoint, 5, 95);
  if (s.mode > MODE_AUTO) s.mode = MODE_OFF;
  if (s.tuneRule > 5) s.tuneRule = 1;
  s.kp = clampF(s.kp, 0, 1000); s.ki = clampF(s.ki, 0, 100); s.kd = clampF(s.kd, 0, 100000);
  s.ku = clampF(s.ku, 0, 10000); s.tu = clampF(s.tu, 0, 100000);
  s.windowSec = constrain(s.windowSec, 2, 600);
  s.minOnSec  = constrain(s.minOnSec, 0, 600);
  s.minOffSec = constrain(s.minOffSec, 0, 1800);
  s.tuneBand  = clampF(s.tuneBand, 0.1f, 10);
  s.alarmLow  = clampF(s.alarmLow, 0, 100);
  s.alarmHigh = clampF(s.alarmHigh, s.alarmLow + 1, 100);
  s.tempOffset = clampF(s.tempOffset, -10, 10);
  s.humOffset  = clampF(s.humOffset, -20, 20);
  s.apSsid[sizeof(s.apSsid) - 1] = 0;
  s.apPass[sizeof(s.apPass) - 1] = 0;
  if (!s.apSsid[0]) strlcpy(s.apSsid, DEFAULT_AP_SSID, sizeof(s.apSsid));
  size_t pl = strlen(s.apPass);
  if (pl > 0 && pl < 8) strlcpy(s.apPass, DEFAULT_AP_PASS, sizeof(s.apPass));
}

inline bool settingsLoad(Settings &s) {
  File f = LittleFS.open(SETTINGS_FILE, "r");
  bool ok = f && f.size() == sizeof(Settings) && f.read((uint8_t *)&s, sizeof(s)) == sizeof(s) &&
            s.magic == SETTINGS_MAGIC;
  if (f) f.close();
  if (!ok) settingsDefaults(s);
  settingsSanitize(s);
  return ok;
}

inline bool settingsSave(const Settings &s) {
  File f = LittleFS.open(SETTINGS_FILE, "w");
  if (!f) return false;
  bool ok = f.write((const uint8_t *)&s, sizeof(s)) == sizeof(s);
  f.close();
  return ok;
}
