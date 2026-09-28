#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ESP8266WebServer.h>

// One logged point: 10 bytes, packed.
struct __attribute__((packed)) Sample {
  uint32_t ts;   // device clock, seconds (Unix epoch once the browser has synced it)
  int16_t  t10;  // temperature x10 (°C)
  uint16_t h10;  // humidity x10 (%RH)
  int8_t   u;    // net drive -100..100 (+ humidify, - dehumidify)
  uint8_t  sp2;  // setpoint x2 (%RH)
};

template <uint16_t N>
struct Ring {
  Sample buf[N];
  uint16_t head = 0, count = 0;  // head = next write position

  void push(const Sample &s) {
    buf[head] = s;
    head = (head + 1) % N;
    if (count < N) count++;
  }
  // 0 = oldest
  const Sample &at(uint16_t i) const { return buf[(head + N - count + i) % N]; }
  Sample &at(uint16_t i) { return buf[(head + N - count + i) % N]; }
  const Sample *last() const { return count ? &at(count - 1) : nullptr; }
  void clear() { head = count = 0; }

  // After a clock sync: move samples taken with the estimated clock onto real time.
  void shift(int32_t delta, uint32_t since) {
    for (uint16_t k = 0; k < count; k++) {
      Sample &s = at(k);
      if (s.ts >= since || s.ts < 1000000000UL) {
        int64_t v = (int64_t)s.ts + delta;
        s.ts = v > 0 ? (uint32_t)v : 0;
      }
    }
  }

  bool save(File &f) const {
    uint16_t n = N;
    return f.write((const uint8_t *)&n, 2) == 2 && f.write((const uint8_t *)&head, 2) == 2 &&
           f.write((const uint8_t *)&count, 2) == 2 &&
           f.write((const uint8_t *)buf, sizeof(buf)) == sizeof(buf);
  }
  bool load(File &f) {
    uint16_t n = 0, h = 0, c = 0;
    if (f.read((uint8_t *)&n, 2) != 2 || f.read((uint8_t *)&h, 2) != 2 ||
        f.read((uint8_t *)&c, 2) != 2 || n != N || h >= N || c > N)
      return false;
    if (f.read((uint8_t *)buf, sizeof(buf)) != sizeof(buf)) return false;
    head = h; count = c;
    return true;
  }
};

// Averages sensor readings between two logged samples.
struct Accumulator {
  float t = 0, h = 0, u = 0;
  uint16_t n = 0;
  uint32_t since = 0;

  void add(float T, float H, float U) { t += T; h += H; u += U; n++; }

  Sample take(uint32_t ts, float sp) {
    Sample s;
    s.ts  = ts;
    s.t10 = (int16_t)lroundf(t / n * 10);
    s.h10 = (uint16_t)constrain(lroundf(h / n * 10), 0L, 1000L);
    s.u   = (int8_t)constrain(lroundf(u / n), -100L, 100L);
    s.sp2 = (uint8_t)constrain(lroundf(sp * 2), 0L, 200L);
    t = h = u = 0; n = 0;
    return s;
  }
};

// Streams a ring as CSV:   #iv=60,now=1727500000,sync=1,n=1440
//                          ts,t10,h10,u,sp2
template <uint16_t N>
void streamRing(ESP8266WebServer &srv, const Ring<N> &r, uint16_t interval, uint32_t now, bool synced) {
  srv.sendHeader(F("Cache-Control"), F("no-store"));
  srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
  srv.send(200, "text/plain", "");
  String chunk;
  chunk.reserve(1200);
  char line[48];
  snprintf(line, sizeof(line), "#iv=%u,now=%lu,sync=%d,n=%u\n", interval, (unsigned long)now, synced ? 1 : 0, r.count);
  chunk += line;
  for (uint16_t k = 0; k < r.count; k++) {
    const Sample &s = r.at(k);
    snprintf(line, sizeof(line), "%lu,%d,%u,%d,%u\n", (unsigned long)s.ts, s.t10, s.h10, s.u, s.sp2);
    chunk += line;
    if (chunk.length() > 1000) {
      srv.sendContent(chunk);
      chunk = "";
      yield();
    }
  }
  if (chunk.length()) srv.sendContent(chunk);
  srv.sendContent("");  // terminate chunked transfer
}
