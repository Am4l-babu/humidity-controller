/*
 * HygroPilot — ESP8266 PID humidity controller
 * ---------------------------------------------------------------
 *  - SHT41 (or SHT31 / DHT) sensor, SSD1306 128x64 OLED
 *  - Humidifier relay (+ optional dehumidifier relay) driven by a PID
 *    loop through time-proportioning outputs
 *  - Relay (Astrom-Hagglund) auto-tune with selectable tuning rules
 *  - Own Wi-Fi access point + captive portal serving an interactive
 *    dashboard with live data, tuning and history analysis
 *  - 20 min / 24 h / 7 day history, the latter two persisted to flash
 *
 *  Web UI source: web/index.html -> gzipped into webpage.h by
 *  tools/embed_web.py (runs automatically when building with PlatformIO).
 */
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "config.h"
#if SENSOR_TYPE == SENSOR_DHT22
#include <DHT.h>
#elif SENSOR_TYPE == SENSOR_SHT31
#include <Adafruit_SHT31.h>
#elif SENSOR_TYPE == SENSOR_SHT41
#include <Adafruit_SHT4x.h>
#else
#error "Select a sensor in config.h"
#endif

#include "settings.h"
#include "control.h"
#include "history.h"
#include "jsonw.h"
#include "webpage.h"

// ------------------------------------------------------------------ globals
Settings cfg;
ESP8266WebServer server(80);
DNSServer dns;
Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);
#if SENSOR_TYPE == SENSOR_DHT22
DHT dht(PIN_DHT, DHT_MODEL);
#elif SENSOR_TYPE == SENSOR_SHT31
Adafruit_SHT31 sht;
#else
Adafruit_SHT4x sht4;
#endif

PID pid;
AutoTuner tuner;
SlowPWM humOut(PIN_HUMIDIFIER), dehOut(PIN_DEHUMIDIFIER);

Ring<FINE_SAMPLES> histFine;
Ring<MEDIUM_SAMPLES> histMed;
Ring<LONG_SAMPLES> histLong;
Accumulator accFine, accMed, accLong;

const IPAddress AP_IP(192, 168, 4, 1);

struct Live {
  float t = NAN, h = NAN, hf = NAN;  // raw temperature/humidity and filtered humidity
  bool ok = false, everOk = false;
  uint8_t fails = 0;
  float u = 0;                       // net drive: + humidify, - dehumidify
  float humDuty = 0, dehDuty = 0;
  uint8_t alarm = 0;                 // 0 none, 1 too dry, 2 too humid
} live;

// Device clock: Unix epoch once a browser synced it, otherwise estimated from flash.
uint32_t clockSec = 0, bootClock = 0, uptimeSec = 0;
bool timeSynced = false;

uint32_t lastTickMs = 0, lastSensorMs = 0, lastOledMs = 0;
uint32_t lastPersistS = 0, settingsDirtyAt = 0, restartAt = 0;
bool settingsDirty = false, factoryReset = false;

bool oledOk = false;
uint8_t oledPage = 0;
uint32_t pageSince = 0, manualPageUntil = 0;

#define HIST_FILE  "/hist.bin"
#define HIST_MAGIC 0x48495354UL  // "HIST"

// ================================================================== helpers
void markDirty() { settingsDirty = true; settingsDirtyAt = millis(); }

float dewPoint(float t, float rh) {
  if (isnan(t) || isnan(rh) || rh <= 0) return NAN;
  const float a = 17.62f, b = 243.12f;
  float g = logf(rh / 100.0f) + a * t / (b + t);
  return b * g / (a - g);
}

void tickClock() {
  uint32_t ms = millis();
  while (ms - lastTickMs >= 1000) { lastTickMs += 1000; clockSec++; uptimeSec++; }
}

void syncClock(uint32_t epoch) {
  int32_t delta = (int32_t)(epoch - clockSec);
  histFine.shift(delta, bootClock);
  histMed.shift(delta, bootClock);
  histLong.shift(delta, bootClock);
  bootClock += delta;
  clockSec = epoch;
  timeSynced = true;
}

void configurePid() {
  pid.kp = cfg.kp; pid.ki = cfg.ki; pid.kd = cfg.kd;
  pid.reverse = cfg.mode == MODE_DEHUMIDIFY;
  pid.outMin = cfg.mode == MODE_AUTO ? -100 : 0;
  pid.outMax = 100;
}

// ================================================================== history persistence
void persistHistory() {
  File f = LittleFS.open(HIST_FILE, "w");
  if (!f) return;
  uint32_t magic = HIST_MAGIC;
  uint8_t synced = timeSynced;
  f.write((uint8_t *)&magic, 4);
  f.write((uint8_t *)&clockSec, 4);
  f.write(&synced, 1);
  histMed.save(f);
  histLong.save(f);
  f.close();
  lastPersistS = uptimeSec;
}

void restoreHistory() {
  File f = LittleFS.open(HIST_FILE, "r");
  if (!f) return;
  uint32_t magic = 0, savedClock = 0;
  uint8_t synced = 0;
  bool ok = f.read((uint8_t *)&magic, 4) == 4 && magic == HIST_MAGIC &&
            f.read((uint8_t *)&savedClock, 4) == 4 && f.read(&synced, 1) == 1 &&
            histMed.load(f) && histLong.load(f);
  f.close();
  if (!ok) { histMed.clear(); histLong.clear(); return; }
  // Resume the clock just after the last save; the browser corrects it on first visit.
  clockSec = savedClock + 60;
  Serial.printf("History restored: %u + %u samples\n", histMed.count, histLong.count);
}

// ================================================================== sensor + control
void sensorBegin() {
#if SENSOR_TYPE == SENSOR_DHT22
  dht.begin();
#elif SENSOR_TYPE == SENSOR_SHT31
  sht.begin(0x44);
#else
  if (sht4.begin(&Wire)) {
    sht4.setPrecision(SHT4X_HIGH_PRECISION);
    sht4.setHeater(SHT4X_NO_HEATER);
  } else {
    Serial.println(F("SHT41 not found"));
  }
#endif
}

bool readSensor(float &t, float &h) {
#if SENSOR_TYPE == SENSOR_DHT22
  h = dht.readHumidity();
  t = dht.readTemperature();
#elif SENSOR_TYPE == SENSOR_SHT31
  t = sht.readTemperature();
  h = sht.readHumidity();
#else
  sensors_event_t hev, tev;
  if (!sht4.getEvent(&hev, &tev)) return false;
  h = hev.relative_humidity;
  t = tev.temperature;
#endif
  if (isnan(t) || isnan(h) || h < 0 || h > 100.5f || t < -40 || t > 85) return false;
  t += cfg.tempOffset;
  h = clampF(h + cfg.humOffset, 0, 100);
  return true;
}

const char *startTune(float band) {
  if (cfg.mode == MODE_OFF) return "Select a mode first";
  if (!live.ok) return "Sensor not ready";
  cfg.tuneBand = band;
  float lo = cfg.mode == MODE_AUTO ? -100 : 0;
  tuner.start(cfg.setpoint, band, 100, lo, cfg.mode == MODE_DEHUMIDIFY, live.hf, uptimeSec);
  markDirty();
  return nullptr;
}

void controlStep(float dt) {
  float t, h;
  if (readSensor(t, h)) {
    Serial.printf("Sensor: %.2f C  %.1f %%RH\n", t, h);
    live.t = t;
    live.h = h;
    live.hf = (!live.ok || isnan(live.hf)) ? h : live.hf + 0.5f * (h - live.hf);
    live.fails = 0;
    live.ok = live.everOk = true;
  } else {
    Serial.printf("Sensor read failed (%u in a row)\n", live.fails + 1);
    if (live.fails < 255) live.fails++;
    if (live.fails >= SENSOR_FAIL_LIMIT && live.ok) {
      live.ok = false;
      pid.reset();
      tuner.abort("Sensor fault", uptimeSec);
    }
    if (!live.ok) live.u = live.humDuty = live.dehDuty = 0;
    return;  // keep the previous output through isolated glitches
  }

  live.alarm = live.hf >= cfg.alarmHigh ? 2 : (live.hf <= cfg.alarmLow ? 1 : 0);

  float u = 0;  // controller-domain output
  if (tuner.running()) {
    u = tuner.update(live.hf, uptimeSec);
    if (tuner.state == AutoTuner::DONE) {
      cfg.ku = tuner.ku;
      cfg.tu = tuner.tu;
      tuneGains(cfg.tuneRule, cfg.ku, cfg.tu, cfg.kp, cfg.ki, cfg.kd);
      configurePid();
      pid.reset();
      markDirty();
      Serial.printf("Autotune: Ku=%.3f Tu=%.1fs -> Kp=%.3f Ki=%.5f Kd=%.2f\n", cfg.ku, cfg.tu, cfg.kp, cfg.ki, cfg.kd);
    }
  } else if (cfg.mode != MODE_OFF) {
    u = pid.compute(cfg.setpoint, live.hf, dt);
  } else {
    pid.p = pid.i = pid.d = 0;
  }

  float hum = 0, deh = 0;
  switch (cfg.mode) {
    case MODE_HUMIDIFY:   hum = max(u, 0.0f); break;
    case MODE_DEHUMIDIFY: deh = max(u, 0.0f); break;
    case MODE_AUTO:       hum = max(u, 0.0f); deh = max(-u, 0.0f); break;
  }
  // Hard safety limits, independent of the controller
  if (live.hf >= cfg.alarmHigh) hum = 0;
  if (live.hf <= cfg.alarmLow) deh = 0;

  live.humDuty = hum;
  live.dehDuty = deh;
  live.u = hum - deh;
}

void logStep() {
  if (!live.ok || live.fails) return;
  accFine.add(live.t, live.h, live.u);
  accMed.add(live.t, live.h, live.u);
  accLong.add(live.t, live.h, live.u);
  if (uptimeSec - accFine.since >= FINE_INTERVAL_S && accFine.n) {
    accFine.since = uptimeSec;
    histFine.push(accFine.take(clockSec, cfg.setpoint));
  }
  if (uptimeSec - accMed.since >= MEDIUM_INTERVAL_S && accMed.n) {
    accMed.since = uptimeSec;
    histMed.push(accMed.take(clockSec, cfg.setpoint));
  }
  if (uptimeSec - accLong.since >= LONG_INTERVAL_S && accLong.n) {
    accLong.since = uptimeSec;
    histLong.push(accLong.take(clockSec, cfg.setpoint));
  }
}

// Humidity change over the last minute (%RH/min), from the fine history.
float trendPerMin() {
  if (histFine.count < 2) return 0;
  uint16_t back = min<uint16_t>(histFine.count - 1, 60 / FINE_INTERVAL_S);
  const Sample &a = histFine.at(histFine.count - 1 - back), &b = histFine.at(histFine.count - 1);
  float dt = (float)(b.ts - a.ts);
  return dt > 0 ? (b.h10 - a.h10) / 10.0f * 60.0f / dt : 0;
}

// ================================================================== OLED
void splash() {
  oled.clearDisplay();
  // droplet
  oled.fillCircle(22, 38, 13, SSD1306_WHITE);
  oled.fillTriangle(10, 32, 34, 32, 22, 6, SSD1306_WHITE);
  oled.fillCircle(17, 40, 3, SSD1306_BLACK);
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2);
  oled.setCursor(44, 16);
  oled.print(F("Hygro"));
  oled.setCursor(44, 34);
  oled.print(F("Pilot"));
  oled.setTextSize(1);
  oled.setCursor(44, 54);
  oled.print(F("v" FW_VERSION));
  oled.display();
}

void drawRelayIcon(int x, int y, char c, bool on) {
  if (on) {
    oled.fillRoundRect(x, y, 11, 10, 2, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
  } else {
    oled.drawRoundRect(x, y, 11, 10, 2, SSD1306_WHITE);
    oled.setTextColor(SSD1306_WHITE);
  }
  oled.setCursor(x + 3, y + 1);
  oled.print(c);
  oled.setTextColor(SSD1306_WHITE);
}

void printDegC(int x, int y) {
  oled.drawCircle(x + 1, y + 1, 1, SSD1306_WHITE);
  oled.setCursor(x + 4, y);
  oled.print('C');
}

void drawMain() {
  bool blink = (millis() / 500) & 1;
  char buf[24];

  // --- header
  if (live.alarm && blink) {
    oled.fillRect(0, 0, 128, 11, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    oled.setCursor(2, 2);
    oled.print(live.alarm == 2 ? F("ALARM: TOO HUMID") : F("ALARM: TOO DRY"));
    oled.setTextColor(SSD1306_WHITE);
  } else {
    if (tuner.running()) snprintf(buf, sizeof(buf), "AUTOTUNE #%u", tuner.cycles + 1);
    else strlcpy(buf, MODE_NAMES[cfg.mode], sizeof(buf));
    oled.setCursor(0, 2);
    oled.print(buf);
    drawRelayIcon(103, 0, 'H', humOut.on);
    drawRelayIcon(116, 0, 'D', dehOut.on);
  }
  oled.drawFastHLine(0, 12, 128, SSD1306_WHITE);

  if (!live.ok) {
    oled.setTextSize(2);
    oled.setCursor(4, 22);
    oled.print(live.everOk ? F("SENSOR") : F("STARTING"));
    oled.setCursor(4, 42);
    oled.print(live.everOk ? F("FAULT!") : F("..."));
    oled.setTextSize(1);
    return;
  }

  // --- big humidity
  if (live.h >= 99.95f) snprintf(buf, sizeof(buf), "%d", (int)lroundf(live.h));
  else snprintf(buf, sizeof(buf), "%.1f", live.h);
  oled.setTextSize(3);
  oled.setCursor(0, 17);
  oled.print(buf);
  int x = oled.getCursorX() + 2;
  oled.setTextSize(1);
  oled.setCursor(x, 17);
  oled.print('%');
  oled.setCursor(x, 27);
  oled.print(F("RH"));

  // --- right column: setpoint + temperature
  oled.setCursor(93, 16);
  oled.print(F("SET"));
  snprintf(buf, sizeof(buf), "%.1f", cfg.setpoint);
  oled.setCursor(93, 26);
  oled.print(buf);
  snprintf(buf, sizeof(buf), "%.1f", live.t);
  oled.setCursor(93, 38);
  oled.print(buf);
  printDegC(oled.getCursorX() + 1, 38);

  // --- trend line
  float tr = trendPerMin();
  oled.setCursor(0, 43);
  if (tuner.running()) {
    snprintf(buf, sizeof(buf), "a=%.1f T=%.0fs", tuner.lastAmp, tuner.lastPeriod);
    oled.print(buf);
  } else {
    int ty = 46;
    if (tr > 0.05f) oled.fillTriangle(0, ty + 2, 6, ty + 2, 3, ty - 2, SSD1306_WHITE);
    else if (tr < -0.05f) oled.fillTriangle(0, ty - 2, 6, ty - 2, 3, ty + 2, SSD1306_WHITE);
    else oled.drawFastHLine(0, ty, 7, SSD1306_WHITE);
    snprintf(buf, sizeof(buf), "%+.1f%%/min", tr);
    oled.setCursor(10, 43);
    oled.print(buf);
  }

  // --- output bar
  oled.setCursor(0, 55);
  oled.print(live.u < 0 ? F("DRY") : F("HUM"));
  oled.drawRect(20, 54, 80, 9, SSD1306_WHITE);
  int w = (int)(fabsf(live.u) * 78 / 100);
  if (w > 0) oled.fillRect(21, 55, w, 7, SSD1306_WHITE);
  snprintf(buf, sizeof(buf), "%3d%%", (int)lroundf(fabsf(live.u)));
  oled.setCursor(104, 55);
  oled.print(buf);
}

void drawGraph() {
  oled.setCursor(0, 0);
  oled.print(F("RH  last 20 min"));
  uint16_t n = histFine.count;
  if (n < 2) {
    oled.setCursor(10, 30);
    oled.print(F("collecting data..."));
    return;
  }
  const int X0 = 24, Y0 = 11, W = 104, H = 52;
  float lo = cfg.setpoint, hi = cfg.setpoint;
  for (uint16_t k = 0; k < n; k++) {
    float v = histFine.at(k).h10 / 10.0f;
    lo = min(lo, v); hi = max(hi, v);
  }
  float mid = (lo + hi) / 2, span = max(hi - lo, 4.0f) * 1.15f;
  lo = mid - span / 2; hi = mid + span / 2;
  auto yOf = [&](float v) { return Y0 + H - 1 - (int)((v - lo) / (hi - lo) * (H - 1)); };

  char buf[8];
  snprintf(buf, sizeof(buf), "%.0f", hi);
  oled.setCursor(0, Y0);
  oled.print(buf);
  snprintf(buf, sizeof(buf), "%.0f", lo);
  oled.setCursor(0, Y0 + H - 8);
  oled.print(buf);
  oled.drawFastVLine(X0 - 2, Y0, H, SSD1306_WHITE);

  int ys = yOf(cfg.setpoint);
  for (int x = X0; x < X0 + W; x += 4) oled.drawPixel(x, ys, SSD1306_WHITE);

  int px = -1, py = -1;
  for (uint16_t k = 0; k < n; k++) {
    int x = X0 + (int)((long)k * (W - 1) / (n - 1));
    int y = yOf(histFine.at(k).h10 / 10.0f);
    if (px >= 0) oled.drawLine(px, py, x, y, SSD1306_WHITE);
    px = x; py = y;
  }
}

void drawNet() {
  char buf[32];
  oled.setCursor(0, 0);
  oled.print(F("Wi-Fi access point"));
  oled.drawFastHLine(0, 9, 128, SSD1306_WHITE);
  oled.setCursor(0, 13);
  oled.print(F("SSID "));
  oled.print(cfg.apSsid);
  oled.setCursor(0, 23);
  oled.print(F("PASS "));
  oled.print(cfg.apPass[0] ? cfg.apPass : "(open)");
  oled.setCursor(0, 33);
  oled.print(F("http://192.168.4.1"));
  oled.setCursor(0, 44);
  snprintf(buf, sizeof(buf), "Clients %d  Up %luh%02lum", WiFi.softAPgetStationNum(),
           (unsigned long)(uptimeSec / 3600), (unsigned long)(uptimeSec / 60 % 60));
  oled.print(buf);
  oled.setCursor(0, 55);
  if (timeSynced) {
    uint32_t local = clockSec + cfg.tzMinutes * 60;
    snprintf(buf, sizeof(buf), "Time %02lu:%02lu", (unsigned long)(local / 3600 % 24), (unsigned long)(local / 60 % 60));
    oled.print(buf);
  } else {
    oled.print(F("Open page to set time"));
  }
}

void drawOled() {
  static const uint16_t PAGE_MS[3] = {9000, 5000, 5000};
  uint32_t ms = millis();
  if ((int32_t)(ms - manualPageUntil) > 0 && ms - pageSince > PAGE_MS[oledPage]) {
    oledPage = (oledPage + 1) % 3;
    pageSince = ms;
  }
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  if (oledPage == 0 || !live.ok || live.alarm) drawMain();
  else if (oledPage == 1) drawGraph();
  else drawNet();
  oled.display();
}

void handleButton() {
  static bool last = true;
  static uint32_t lastMs = 0;
  bool b = digitalRead(PIN_BUTTON);
  if (b != last && millis() - lastMs > 40) {
    lastMs = millis();
    last = b;
    if (!b) {
      oledPage = (oledPage + 1) % 3;
      pageSince = millis();
      manualPageUntil = millis() + 30000;
    }
  }
}

void updateLed() {
  if (PIN_LED < 0) return;
  uint32_t phase = millis() % 2000;
  bool on = live.ok ? phase < 40 : phase < 1000;  // heartbeat, slow blink on fault
  if (humOut.on || dehOut.on) on = !on;
  digitalWrite(PIN_LED, on ? LOW : HIGH);
}

// ================================================================== web
void sendJson(const String &body, int code = 200) {
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(code, "application/json", body);
}
void sendOk() { sendJson(F("{\"ok\":1}")); }
void sendErr(const char *m) {
  JsonW j;
  j.i("ok", 0).str("err", m);
  sendJson(j.end(), 400);
}

bool argF(const char *k, float &v, float lo, float hi) {
  if (!server.hasArg(k)) return false;
  String a = server.arg(k);
  if (!a.length()) return false;
  v = clampF(a.toFloat(), lo, hi);
  return true;
}

// Captive portal: anything not addressed to us gets redirected to the dashboard.
bool captiveRedirect() {
  String host = server.hostHeader();
  if (host.length() == 0 || host == AP_IP.toString() || host.startsWith(MDNS_NAME)) return false;
  server.sendHeader(F("Location"), String(F("http://")) + AP_IP.toString() + '/', true);
  server.send(302, "text/plain", "");
  return true;
}

void handleRoot() {
  if (captiveRedirect()) return;
  server.sendHeader(F("Content-Encoding"), F("gzip"));
  server.sendHeader(F("Cache-Control"), F("no-cache"));
  server.send_P(200, "text/html", (PGM_P)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
}

void handleNotFound() {
  if (captiveRedirect()) return;
  server.send(404, "text/plain", "Not found");
}

void handleStatus() {
  JsonW at;
  at.i("s", tuner.state).i("c", tuner.cycles).u("el", tuner.elapsed(uptimeSec)).i("hi", tuner.high)
    .f("a", tuner.lastAmp, 2).f("per", tuner.lastPeriod, 0).str("m", tuner.msg);

  JsonW j;
  j.str("v", FW_VERSION)
   .i("ok", live.ok).i("ev", live.everOk).f("t", live.t, 2).f("h", live.h, 1).f("hf", live.hf, 2)
   .f("dp", dewPoint(live.t, live.h), 1)
   .f("sp", cfg.setpoint, 1).i("mode", cfg.mode)
   .f("u", live.u, 1).f("hd", live.humDuty, 1).f("dd", live.dehDuty, 1)
   .i("hr", humOut.on).i("dr", dehOut.on)
   .f("p", pid.p, 2).f("i", pid.i, 2).f("d", pid.d, 2)
   .f("kp", cfg.kp, 4).f("ki", cfg.ki, 6).f("kd", cfg.kd, 3)
   .f("ku", cfg.ku, 4).f("tu", cfg.tu, 1).i("rule", cfg.tuneRule)
   .i("win", cfg.windowSec).i("al", live.alarm)
   .raw("at", at.end())
   .u("hon", (unsigned long)(humOut.onMs / 1000)).u("don", (unsigned long)(dehOut.onMs / 1000))
   .u("hsw", humOut.switches).u("dsw", dehOut.switches)
   .u("now", clockSec).i("sync", timeSynced).u("up", uptimeSec)
   .u("heap", ESP.getFreeHeap()).i("cli", WiFi.softAPgetStationNum());
  sendJson(j.end());
}

void handleConfig() {
  JsonW j;
  j.f("sp", cfg.setpoint, 1).i("mode", cfg.mode)
   .f("kp", cfg.kp, 4).f("ki", cfg.ki, 6).f("kd", cfg.kd, 3)
   .i("win", cfg.windowSec).i("mon", cfg.minOnSec).i("moff", cfg.minOffSec)
   .f("band", cfg.tuneBand, 2).i("rule", cfg.tuneRule).f("ku", cfg.ku, 4).f("tu", cfg.tu, 1)
   .f("alo", cfg.alarmLow, 1).f("ahi", cfg.alarmHigh, 1)
   .f("toff", cfg.tempOffset, 2).f("hoff", cfg.humOffset, 2)
   .i("flip", cfg.oledFlip).i("tz", cfg.tzMinutes)
   .str("ssid", cfg.apSsid).str("pass", cfg.apPass)
   .str("fw", FW_VERSION)
   .u("flash", ESP.getFlashChipRealSize()).u("chip", ESP.getChipId());
  sendJson(j.end());
}

void handleHistory() {
  String r = server.arg("r");
  if (r == "f") streamRing(server, histFine, FINE_INTERVAL_S, clockSec, timeSynced);
  else if (r == "l") streamRing(server, histLong, LONG_INTERVAL_S, clockSec, timeSynced);
  else streamRing(server, histMed, MEDIUM_INTERVAL_S, clockSec, timeSynced);
}

void handleSet() {
  float v;
  if (argF("sp", v, 5, 95)) { cfg.setpoint = roundf(v * 2) / 2; markDirty(); }
  if (server.hasArg("mode")) {
    int m = server.arg("mode").toInt();
    if (m >= MODE_OFF && m <= MODE_AUTO && m != cfg.mode) {
      tuner.abort("Mode changed", uptimeSec);
      cfg.mode = m;
      configurePid();
      pid.reset();
      markDirty();
    }
  }
  sendOk();
}

void handlePid() {
  float v;
  if (argF("kp", v, 0, 1000)) cfg.kp = v;
  if (argF("ki", v, 0, 100)) cfg.ki = v;
  if (argF("kd", v, 0, 100000)) cfg.kd = v;
  if (argF("win", v, 2, 600)) cfg.windowSec = (uint16_t)v;
  if (argF("mon", v, 0, 600)) cfg.minOnSec = (uint16_t)v;
  if (argF("moff", v, 0, 1800)) cfg.minOffSec = (uint16_t)v;
  configurePid();
  markDirty();
  sendOk();
}

void handleTune() {
  String act = server.arg("act");
  float v;
  if (argF("rule", v, 0, TUNE_RULE_COUNT - 1)) cfg.tuneRule = (uint8_t)v;
  if (act == "start") {
    float band = cfg.tuneBand;
    argF("band", band, 0.1f, 10);
    const char *err = startTune(band);
    if (err) return sendErr(err);
  } else if (act == "stop") {
    tuner.abort("Stopped by user", uptimeSec);
  } else if (act == "apply") {
    if (cfg.ku <= 0 || cfg.tu <= 0) return sendErr("No auto-tune result yet");
    tuneGains(cfg.tuneRule, cfg.ku, cfg.tu, cfg.kp, cfg.ki, cfg.kd);
    configurePid();
  } else {
    return sendErr("Unknown action");
  }
  markDirty();
  sendOk();
}

void handleSettings() {
  float v;
  if (argF("alo", v, 0, 99)) cfg.alarmLow = v;
  if (argF("ahi", v, 1, 100)) cfg.alarmHigh = v;
  if (argF("toff", v, -10, 10)) cfg.tempOffset = v;
  if (argF("hoff", v, -20, 20)) cfg.humOffset = v;
  if (server.hasArg("flip")) {
    cfg.oledFlip = server.arg("flip").toInt() ? 1 : 0;
    if (oledOk) oled.setRotation(cfg.oledFlip ? 2 : 0);
  }
  bool netChanged = false;
  if (server.hasArg("ssid")) {
    String s = server.arg("ssid");
    s.trim();
    String p = server.hasArg("pass") ? server.arg("pass") : String(cfg.apPass);
    if (s.length() < 1 || s.length() > 32) return sendErr("SSID must be 1-32 characters");
    if (p.length() != 0 && (p.length() < 8 || p.length() > 63)) return sendErr("Password must be 8-63 characters or empty");
    if (s != cfg.apSsid || p != cfg.apPass) {
      strlcpy(cfg.apSsid, s.c_str(), sizeof(cfg.apSsid));
      strlcpy(cfg.apPass, p.c_str(), sizeof(cfg.apPass));
      netChanged = true;
    }
  }
  settingsSanitize(cfg);
  settingsSave(cfg);
  settingsDirty = false;
  if (netChanged) restartAt = millis() + 1500;
  JsonW j;
  j.i("ok", 1).i("restart", netChanged);
  sendJson(j.end());
}

void handleTime() {
  if (!server.hasArg("epoch")) return sendErr("epoch missing");
  uint32_t e = strtoul(server.arg("epoch").c_str(), nullptr, 10);
  if (e < 1600000000UL) return sendErr("bad epoch");
  if (server.hasArg("tz")) {
    cfg.tzMinutes = constrain(server.arg("tz").toInt(), -840, 840);
    markDirty();
  }
  syncClock(e);
  sendOk();
}

void handleClear() {
  histFine.clear(); histMed.clear(); histLong.clear();
  LittleFS.remove(HIST_FILE);
  humOut.switches = dehOut.switches = 0;
  humOut.onMs = dehOut.onMs = 0;
  sendOk();
}

void handleReboot() {
  sendOk();
  restartAt = millis() + 800;
}

void handleFactory() {
  LittleFS.remove(SETTINGS_FILE);
  LittleFS.remove(HIST_FILE);
  factoryReset = true;
  sendOk();
  restartAt = millis() + 800;
}

void setupWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/config", HTTP_GET, handleConfig);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/set", HTTP_POST, handleSet);
  server.on("/api/pid", HTTP_POST, handlePid);
  server.on("/api/tune", HTTP_POST, handleTune);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/time", HTTP_POST, handleTime);
  server.on("/api/clear", HTTP_POST, handleClear);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/factory", HTTP_POST, handleFactory);
  server.onNotFound(handleNotFound);
  server.begin();
}

void startWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  bool ok = WiFi.softAP(cfg.apSsid, cfg.apPass[0] ? cfg.apPass : nullptr, AP_CHANNEL, 0, 4);
  Serial.printf("AP '%s' %s, IP %s\n", cfg.apSsid, ok ? "up" : "FAILED", WiFi.softAPIP().toString().c_str());
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", AP_IP);
  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
}

// ================================================================== setup / loop
void setup() {
  humOut.begin();  // relays off as early as possible
  dehOut.begin();
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  if (PIN_LED >= 0) {
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH);
  }

  Serial.begin(115200);
  Serial.println(F("\n\nHygroPilot " FW_VERSION));

  Wire.begin(PIN_SDA, PIN_SCL);
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledOk) splash();
  else Serial.println(F("OLED not found"));

  if (!LittleFS.begin()) {
    Serial.println(F("Formatting LittleFS..."));
    LittleFS.format();
    LittleFS.begin();
  }
  if (!settingsLoad(cfg)) Serial.println(F("Using default settings"));
  if (oledOk) oled.setRotation(cfg.oledFlip ? 2 : 0);
  restoreHistory();
  bootClock = clockSec;

  sensorBegin();
  configurePid();
  pid.reset();
  startWiFi();
  setupWeb();

  delay(1200);  // let the splash be seen and the sensor settle
  lastTickMs = lastSensorMs = millis();
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
  MDNS.update();
  tickClock();

  uint32_t ms = millis();
  if (ms - lastSensorMs >= SENSOR_INTERVAL_MS) {
    float dt = min((ms - lastSensorMs) / 1000.0f, 10.0f);
    lastSensorMs = ms;
    controlStep(dt);
    logStep();
  }

  humOut.update(live.humDuty, ms, cfg.windowSec * 1000UL, cfg.minOnSec * 1000UL, cfg.minOffSec * 1000UL);
  dehOut.update(live.dehDuty, ms, cfg.windowSec * 1000UL, cfg.minOnSec * 1000UL, cfg.minOffSec * 1000UL);

  handleButton();
  updateLed();
  if (oledOk && ms - lastOledMs >= 250) {
    lastOledMs = ms;
    drawOled();
  }

  if (settingsDirty && ms - settingsDirtyAt > 3000) {
    settingsSave(cfg);
    settingsDirty = false;
  }
  if (uptimeSec - lastPersistS >= PERSIST_INTERVAL_S) persistHistory();

  if (restartAt && (int32_t)(ms - restartAt) > 0) {
    humOut.force(false);
    dehOut.force(false);
    if (!factoryReset) {
      if (settingsDirty) settingsSave(cfg);
      persistHistory();
    }
    ESP.restart();
  }
}
