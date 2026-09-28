#pragma once
#include <Arduino.h>
#include <math.h>
#include "config.h"

// =====================================================================
//  Tuning rules. From the relay experiment we get the ultimate gain Ku
//  and ultimate period Tu; each rule maps them to Kp, Ti, Td:
//     Kp = kp*Ku     Ti = ti*Tu     Td = td*Tu     Ki = Kp/Ti   Kd = Kp*Td
//  (keep in sync with RULES in web/index.html)
// =====================================================================
struct TuneRule { const char *name; float kp, ti, td; };
static const TuneRule TUNE_RULES[] = {
  {"Ziegler-Nichols",    0.600f, 0.500f, 0.125f},
  {"Tyreus-Luyben",      0.454f, 2.200f, 0.159f},
  {"Pessen Integral",    0.700f, 0.400f, 0.150f},
  {"Some Overshoot",     0.333f, 0.500f, 0.333f},
  {"No Overshoot",       0.200f, 0.500f, 0.333f},
  {"Ziegler-Nichols PI", 0.450f, 0.833f, 0.000f},
};
static const uint8_t TUNE_RULE_COUNT = sizeof(TUNE_RULES) / sizeof(TUNE_RULES[0]);

inline void tuneGains(uint8_t rule, float ku, float tu, float &kp, float &ki, float &kd) {
  const TuneRule &r = TUNE_RULES[rule < TUNE_RULE_COUNT ? rule : 1];
  kp = r.kp * ku;
  float ti = r.ti * tu, td = r.td * tu;
  ki = ti > 0 ? kp / ti : 0;
  kd = kp * td;
}

// =====================================================================
//  PID with derivative-on-measurement, filtered D term and
//  conditional-integration anti-windup. The integral is stored in output
//  units, so changing Ki on the fly is bumpless.
// =====================================================================
class PID {
 public:
  float kp = 0, ki = 0, kd = 0;
  float outMin = 0, outMax = 100;
  bool reverse = false;        // true when a positive output lowers the process value
  float p = 0, i = 0, d = 0;   // last term contributions, reported to the UI

  void reset() { p = i = d = 0; lastPv = NAN; dFilt = 0; }

  float compute(float sp, float pv, float dt) {
    if (dt <= 0) dt = 0.001f;
    float err = reverse ? pv - sp : sp - pv;
    float slope = isnan(lastPv) ? 0 : (pv - lastPv) / dt;
    lastPv = pv;
    if (reverse) slope = -slope;
    dFilt += 0.35f * (slope - dFilt);

    p = kp * err;
    d = -kd * dFilt;
    float iNext = i + ki * err * dt;
    float out = p + iNext + d;
    bool windingUp = (out > outMax && err > 0) || (out < outMin && err < 0);
    if (!windingUp) i = iNext;
    i = constrain(i, outMin, outMax);
    return constrain(p + i + d, outMin, outMax);
  }

 private:
  float lastPv = NAN, dFilt = 0;
};

// =====================================================================
//  Relay auto-tuner (Astrom-Hagglund). The output is switched between
//  outHi and outLo with a hysteresis band around the setpoint, which
//  forces a limit cycle. From its amplitude a and period Tu:
//      Ku = 4d / (pi * sqrt(a^2 - band^2)),   d = (outHi - outLo) / 2
//  Finishes once three consecutive cycles agree within 20 %.
// =====================================================================
class AutoTuner {
 public:
  enum State : uint8_t { IDLE = 0, RUNNING = 1, DONE = 2, FAILED = 3 };
  State state = IDLE;
  char msg[40] = "";
  float ku = 0, tu = 0, lastAmp = 0, lastPeriod = 0;
  uint8_t cycles = 0;
  bool high = false;

  bool running() const { return state == RUNNING; }
  uint32_t elapsed(uint32_t nowS) const { return (state == RUNNING ? nowS : endS) - startS; }

  void start(float setpoint, float noiseBand, float hi, float lo, bool reverse, float pv, uint32_t nowS) {
    sp = setpoint; band = max(noiseBand, 0.1f); outHi = hi; outLo = lo; rev = reverse;
    state = RUNNING; cycles = 0; ku = tu = lastAmp = lastPeriod = 0;
    haveRise = false; cMax = cMin = pv;
    startS = lastSwitchS = endS = nowS;
    high = error(pv) > 0;
    setMsg("Forcing oscillation");
  }

  void abort(const char *why, uint32_t nowS) {
    if (state == RUNNING) finish(false, why, nowS);
  }

  // Returns the output (controller domain) to apply this step. 0 once finished.
  float update(float pv, uint32_t nowS) {
    if (state != RUNNING) return 0;
    float e = error(pv);
    if (pv > cMax) cMax = pv;
    if (pv < cMin) cMin = pv;

    if (high && e < -band) {
      high = false; lastSwitchS = nowS;
    } else if (!high && e > band) {
      high = true; lastSwitchS = nowS;
      if (haveRise) cycleDone(nowS);
      haveRise = true; riseS = nowS; cMax = cMin = pv;
    }

    if (state == RUNNING) {
      if (nowS - startS > AUTOTUNE_TIMEOUT_S) finish(false, "Timed out", nowS);
      else if (nowS - lastSwitchS > AUTOTUNE_STALL_S)
        finish(false, high ? "Setpoint unreachable" : "Process not recovering", nowS);
    }
    if (state != RUNNING) return 0;
    return high ? outHi : outLo;
  }

 private:
  float sp = 0, band = 1, outHi = 100, outLo = 0;
  bool rev = false, haveRise = false;
  uint32_t startS = 0, endS = 0, riseS = 0, lastSwitchS = 0;
  float cMax = 0, cMin = 0;
  float amps[3] = {0, 0, 0}, pers[3] = {0, 0, 0};

  float error(float pv) const { return rev ? pv - sp : sp - pv; }
  void setMsg(const char *m) { strlcpy(msg, m, sizeof(msg)); }

  void finish(bool ok, const char *m, uint32_t nowS) {
    state = ok ? DONE : FAILED;
    endS = nowS;
    setMsg(m);
  }

  static float spread(const float *v) {
    float lo = min(v[0], min(v[1], v[2])), hi = max(v[0], max(v[1], v[2]));
    float mean = (v[0] + v[1] + v[2]) / 3;
    return mean > 0 ? (hi - lo) / mean : 1;
  }

  void cycleDone(uint32_t nowS) {
    lastPeriod = (float)(nowS - riseS);
    lastAmp = (cMax - cMin) / 2;
    cycles++;
    for (int k = 0; k < 2; k++) { amps[k] = amps[k + 1]; pers[k] = pers[k + 1]; }
    amps[2] = lastAmp; pers[2] = lastPeriod;
    if (cycles < 3) return;

    bool converged = spread(amps) < 0.20f && spread(pers) < 0.20f;
    if (!converged && cycles < 8) return;

    float a = (amps[0] + amps[1] + amps[2]) / 3;
    float T = (pers[0] + pers[1] + pers[2]) / 3;
    if (a < 0.05f || T < 1) { finish(false, "No usable oscillation", nowS); return; }
    float aEff = a > band * 1.1f ? sqrtf(a * a - band * band) : a;
    float d = (outHi - outLo) / 2;
    ku = 4 * d / (PI * aEff);
    tu = T;
    finish(true, converged ? "Tuned successfully" : "Tuned (noisy cycles)", nowS);
  }
};

// =====================================================================
//  Time-proportioning ("slow PWM") relay output with minimum on/off
//  times to protect relays and compressors.
// =====================================================================
class SlowPWM {
 public:
  bool on = false;
  uint32_t switches = 0;
  uint64_t onMs = 0;

  explicit SlowPWM(uint8_t p) : pin(p) {}

  void begin() {
    write(false);
    pinMode(pin, OUTPUT);
    write(false);
    lastChange = lastAcc = windowStart = millis();  // enforce min-off after boot
  }

  void update(float duty, uint32_t now, uint32_t windowMs, uint32_t minOnMs, uint32_t minOffMs) {
    if (on) onMs += now - lastAcc;
    lastAcc = now;
    if (windowMs < 1000) windowMs = 1000;
    if (now - windowStart >= windowMs) windowStart += windowMs * ((now - windowStart) / windowMs);

    bool want;
    if (duty <= 0.5f) want = false;
    else if (duty >= 99.5f) want = true;
    else want = (now - windowStart) < (uint32_t)(duty * 0.01f * windowMs);

    if (want != on && now - lastChange >= (on ? minOnMs : minOffMs)) {
      on = want;
      lastChange = now;
      if (on) switches++;
      write(on);
    }
  }

  void force(bool v) {  // immediate, ignores min times (used for shutdown)
    on = v; write(v); lastChange = millis();
  }

 private:
  uint8_t pin;
  uint32_t lastChange = 0, lastAcc = 0, windowStart = 0;
  void write(bool v) { digitalWrite(pin, (v != (bool)RELAY_ACTIVE_LOW) ? HIGH : LOW); }
};
