#pragma once
// =====================================================================
//  HygroPilot — ESP8266 PID humidity controller : hardware configuration
// =====================================================================

#define FW_VERSION "1.0.0"

// ---- Sensor (pick ONE) ------------------------------------------------
#define SENSOR_DHT22 1
#define SENSOR_SHT31 2
#define SENSOR_TYPE  SENSOR_DHT22
// Model used when SENSOR_TYPE is SENSOR_DHT22 (the DHT family): DHT11 or DHT22.
#define DHT_MODEL    DHT11

// ---- Pins (GPIO numbers; NodeMCU / D1 mini silkscreen in brackets) ----
#define PIN_SDA           4   // D2  OLED SDA (+ SHT31)
#define PIN_SCL           5   // D1  OLED SCL (+ SHT31)
#define PIN_DHT           2   // D4  DHT data (10k pull-up to 3V3)
#define PIN_HUMIDIFIER   14   // D5  relay / MOSFET driving the humidifier
#define PIN_DEHUMIDIFIER 12   // D6  relay driving a dehumidifier or exhaust fan
#define PIN_BUTTON        0   // D3  on-board FLASH button: next OLED page
#define PIN_LED          -1   // on-board LED (active LOW) is on GPIO2 / D4, shared with the DHT data
                              // pin, so it is disabled. Set to 2 only if the DHT is on another pin.

// Most cheap relay boards switch ON when their input is pulled LOW.
// Set to 0 for active-HIGH relay boards or a logic-level MOSFET.
#define RELAY_ACTIVE_LOW 1

// ---- OLED (SSD1306 128x64, I2C) --------------------------------------
#define OLED_ADDR 0x3C
#define OLED_W    128
#define OLED_H    64

// ---- Access point defaults (can be changed from the web page) --------
#define DEFAULT_AP_SSID "HygroPilot"
#define DEFAULT_AP_PASS "hygro1234"   // 8..63 chars, or "" for an open network
#define AP_CHANNEL      6
#define MDNS_NAME       "hygropilot"  // http://hygropilot.local

// ---- Timing & history -------------------------------------------------
#define SENSOR_INTERVAL_MS 2000       // DHT22 cannot be read faster than 0.5 Hz
#define FINE_INTERVAL_S    5          // fine   : 240 x 5 s   = 20 min
#define MEDIUM_INTERVAL_S  60         // medium : 1440 x 1 min = 24 h
#define LONG_INTERVAL_S    1800       // long   : 336 x 30 min = 7 days
#define FINE_SAMPLES       240
#define MEDIUM_SAMPLES     1440
#define LONG_SAMPLES       336
#define PERSIST_INTERVAL_S 600        // save medium/long history to flash every 10 min

#define SENSOR_FAIL_LIMIT  5          // consecutive bad reads -> fault, outputs off
#define AUTOTUNE_TIMEOUT_S (4UL * 3600UL)
#define AUTOTUNE_STALL_S   (90UL * 60UL)   // no relay switch for this long -> abort
