#pragma once
#include <Arduino.h>

// Minimal JSON object writer (avoids an ArduinoJson dependency).
class JsonW {
 public:
  JsonW() { s.reserve(900); s = '{'; }

  JsonW &f(const char *k, float v, uint8_t dec = 2) {
    key(k);
    if (isnan(v) || isinf(v)) s += F("null");
    else s += String(v, (unsigned int)dec);
    return *this;
  }
  JsonW &i(const char *k, long v) { key(k); s += v; return *this; }
  JsonW &u(const char *k, unsigned long v) { key(k); s += v; return *this; }
  JsonW &str(const char *k, const char *v) {
    key(k);
    s += '"';
    for (const char *p = v; *p; p++) {
      char c = *p;
      if (c == '"' || c == '\\') { s += '\\'; s += c; }
      else if ((uint8_t)c < 0x20) s += ' ';
      else s += c;
    }
    s += '"';
    return *this;
  }
  JsonW &raw(const char *k, const String &v) { key(k); s += v; return *this; }
  String end() { s += '}'; return s; }

 private:
  String s;
  bool first = true;
  void key(const char *k) {
    if (!first) s += ',';
    first = false;
    s += '"'; s += k; s += F("\":");
  }
};
