/*
  ============================================================
     IoT SPOIL ALERT  -  ESP32  |  VERIFIED-READING EDITION
  ============================================================

  NOTE: this is an ESP32 sketch (not Arduino UNO). In the Arduino IDE
        select  Tools > Board > "ESP32 Dev Module".

  WHAT CHANGED IN THIS VERSION
    1. Serial Monitor no longer floods every second. Nothing is classified
       or printed as a result until the readings have been VERIFIED:
         WARM-UP   -> MQ sensors heat up (progress line every 5 s)
         MEASURING -> 20 samples are collected over a 2-minute window
         VERIFY    -> signal present? values in range? readings stable?
         CALCULATE -> live sensor readings are verified and sent to the app/API
         REPORT    -> ONE clean report block every 2 minutes
    2. Outliers are removed (trimmed mean of 20 samples, each sample is
       itself a 4-read average).
    3. Sensor faults (unplugged MQ, failed DHT22) are detected and shown
       as ERROR instead of producing a fake result.
    4. OLED, LEDs, Serial and the phone app use the same verified sensor values.
    5. Wi-Fi no longer blocks start-up; it connects in the background and
       reconnects automatically.
    6. /api/status exports selected-item type and selection state so the app
       cannot mistake the boot default for a user selection.
    7. Live gas features are raw ADC counts. Workbook-scaled values are never
       presented as measured gas concentrations.

  PINS
    MQ-135 -> GPIO 34   MQ-4 -> GPIO 35   MQ-3 -> GPIO 32   (ADC1 only)
    DHT22  -> GPIO 4    (10k pull-up between DATA and 3.3V)
    OLED   -> SDA 21, SCL 22
    LEDs   -> RED 25, YELLOW 26, GREEN 27  (each with a resistor)

  SERIAL COMMANDS (115200 baud, send with "Newline")
    <item name>    e.g. Banana, Blueberry, Tomato (prefix works)
    <number>       1-10, index in the list
    list           print all produce items
    live           use live sensors (demonstration mode is disabled)
    restart        restart measuring
    help

  HARDWARE SAFETY
    MQ modules run at 5 V, ESP32 ADC pins must never see more than 3.3 V.
    Put a voltage divider (e.g. 10k + 20k) on every MQ analog output.

  NOTE
    ADC values are sensor readings, not ppm. The synthetic workbook model is
    disabled for live predictions. Freshness classification needs real,
    independently labelled samples collected with this same hardware.
*/

// ---- Type definitions MUST stay above the first #include. ----
// The Arduino IDE auto-generates function prototypes near the top of the
// file; if these structs were defined later, 'RefRow' / 'Match' would be
// "not a type" when those prototypes are compiled.
#include <stdint.h>
#include "RequestValidation.h"

struct RefRow {
  uint8_t fruit;
  uint8_t minute;
  float temp;
  float humidity;
  float mq135;
  float mq3;
  float mq4;
  uint8_t status;
};

struct Match {
  uint8_t minute;
  uint8_t status;
  float dist;        // distance to the closest row
  float otherDist;   // distance to the closest row with a DIFFERENT label
};

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>

// ===== OLED CONTROLLER =====
// Scrambled pixels with only the last line readable = wrong driver.
// Most 1.3" 128x64 modules are SH1106 (set 1). 0.96" modules are SSD1306 (set 0).
#define OLED_SH1106    1
#define OLED_ADDRESS   0x3C     // some modules use 0x3D
#define OLED_ROTATION  0        // 0 normal, 2 = upside down
#if OLED_SH1106
  #include <Adafruit_SH110X.h>
#else
  #include <Adafruit_SSD1306.h>
#endif
#include <DHT.h>

// ===== OPTIONAL PRODUCE-TEMPERATURE PROBE (DS18B20) =====
// The DHT22 only measures the AIR around the sensor (room temperature).
// Set USE_DS18B20_PROBE to 1 and wire a waterproof DS18B20 so the "temperature"
// feature is the produce/probe temperature instead. Needs the OneWire and
// DallasTemperature libraries. Data pin: GPIO 18, 4.7k pull-up to 3.3 V.
// IMPORTANT: train and run the model with the SAME temperature source.
#define USE_DS18B20_PROBE 0
#define DS18B20_PIN       18
#if USE_DS18B20_PROBE
  #include <OneWire.h>
  #include <DallasTemperature.h>
  OneWire oneWire(DS18B20_PIN);
  DallasTemperature probe(&oneWire);
#endif
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "secrets.h"

// ===== PINS =====
#define MQ135_PIN 34
#define MQ4_PIN   35
#define MQ3_PIN   32

#define DHT_PIN   4
#define DHT_TYPE  DHT22

#define OLED_SDA 21
#define OLED_SCL 22

#define RED_LED    25
#define YELLOW_LED 26
#define GREEN_LED  27

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1

#if OLED_SH1106
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
  #define OLED_WHITE SH110X_WHITE
  #define OLED_BEGIN() display.begin(OLED_ADDRESS, true)
#else
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
  #define OLED_WHITE SSD1306_WHITE
  #define OLED_BEGIN() display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)
#endif
bool oledOK = false;
DHT dht(DHT_PIN, DHT_TYPE);

// Temperature used as the model feature: probe if enabled, otherwise DHT22 air temp.
// Returns NAN on a failed/disconnected sensor so the existing validity checks reject it.
float readTemperatureC() {
#if USE_DS18B20_PROBE
  probe.requestTemperatures();
  float t = probe.getTempCByIndex(0);
  if (t == DEVICE_DISCONNECTED_C || t < -55.0f || t > 125.0f) return NAN;
  return t;
#else
  return dht.readTemperature();
#endif
}

// ===== Wi-Fi settings: replace with your credentials =====
const char* WIFI_SSID     = "H403";
const char* WIFI_PASSWORD = "floor@403";

// Set to 1 to give the ESP32 a fixed address so the app's BaseURL never changes.
// The address must match your router/hotspot subnet.
#define USE_STATIC_IP 0
#if USE_STATIC_IP
IPAddress STATIC_IP(192, 168, 1, 50);
IPAddress GATEWAY(192, 168, 1, 1);
IPAddress SUBNET(255, 255, 255, 0);
IPAddress DNS_1(8, 8, 8, 8);
#endif

WebServer server(80);
bool serverStarted = false;
bool wifiWasUp = false;
unsigned long lastWifiNoteMs = 0;

// ===== TIMING / VERIFICATION SETTINGS =====
const unsigned long WARMUP_LIVE_MS     = 100000UL; // 100-second stabilization before LIVE readings
const unsigned long WARMUP_DEMO_MS     = 10000UL;  // short "self-check" before the demo
const unsigned long SAMPLE_INTERVAL_MS = 6000UL;   // 20 samples over a 2-minute window
const unsigned long REPORT_INTERVAL_MS = 120000UL; // one verified report every 2 minutes
const unsigned long DHT_INTERVAL_MS    = 2500UL;   // DHT22 can only be read every >= 2 s
const unsigned long DHT_STALE_MS       = 30000UL;  // last good DHT value is usable this long
const unsigned long OLED_REFRESH_MS    = 500UL;

#define WINDOW_SAMPLES   20     // REPORT_INTERVAL_MS / SAMPLE_INTERVAL_MS
#define MIN_SAMPLES      10     // below this the window is discarded
#define TRIM_EACH_SIDE   2      // outliers dropped from each end before averaging
#define ADC_AVG_READS    4      // reads averaged inside one sample

const int   ADC_MIN_OK       = 20;     // below  = sensor unplugged / unpowered
const int   ADC_MAX_OK       = 4080;   // above  = input saturated / wiring fault
const int   ADC_SPREAD_MAX   = 150;    // max peak-to-peak counts for "stable"
const float TEMP_SPREAD_MAX  = 2.0;    // degC
const float HUM_SPREAD_MAX   = 5.0;    // %RH
const int   MAX_UNSTABLE_WINDOWS = 3;  // then the reading is accepted and flagged NOISY

// ===== 2-MINUTE DEMO MODE =====
// DEMO: the selected item ages through its 0-4 minute XLSX rows in 2 real minutes.
// LIVE: real MQ / DHT22 readings.
#define DEMO_DEFAULT_ON false
const unsigned long DEMO_DURATION_MS = 120000UL;
bool demoMode = DEMO_DEFAULT_ON;
unsigned long demoStartMs = 0;

// XLSX operating bands (min/max of the 250-row reference table - verified)
const float TEMP_MIN = 1.2;
const float TEMP_MAX = 30.0;
const float HUM_MIN = 66.5;
const float HUM_MAX = 95.9;
const float MQ135_MIN = 5.0;
const float MQ135_MAX = 195.0;
const float MQ3_MIN = 0.10;
const float MQ3_MAX = 9.50;
const float MQ4_MIN = 320.0;
const float MQ4_MAX = 2600.0;

enum Status { FRESH = 0, EARLY = 1, MEDIUM = 2, SPOILED = 3 };

enum SysState {
  SYS_WARMUP = 0,   // sensors heating, nothing verified yet
  SYS_CHECKING,     // collecting + verifying a window, no verified data for this item
  SYS_READY,        // latest window verified, values are valid
  SYS_FAULT         // a sensor failed verification
};

const char* fruits[] = {
  "Banana",
  "Blueberry",
  "Grape",
  "Green Apple",
  "Kiwi",
  "Mushroom",
  "Pear",
  "Red Apple",
  "Strawberry",
  "Tomato"
};

const int FRUIT_COUNT = 10;

// Map picker/API item indices to the matching produce IDs in referenceData,
// which retains the original 50-item workbook ordering. Both apple varieties
// use the workbook's single Apple profile.
const uint8_t referenceFruitIds[FRUIT_COUNT] = {
  1, 44, 8, 0, 16, 39, 13, 0, 9, 2
};

// User-facing culinary category: tomatoes, cucumbers, peppers, etc. are shown
// as vegetables even though some are botanically fruits.
bool isVegetable(int index) {
  switch (index) {
    case 5: case 9:
      return true;
    default:
      return false;
  }
}

const char* produceType(int index) {
  return isVegetable(index) ? "Vegetable" : "Fruit";
}

// Exact XLSX rows
const RefRow referenceData[] = {
  {0, 0, 3.80, 91.50, 10.80, 1.31, 494.70, 0},
  {0, 1, 6.20, 92.00, 17.80, 1.90, 625.80, 0},
  {0, 2, 11.10, 93.10, 31.90, 3.08, 887.90, 1},
  {0, 3, 15.10, 94.00, 43.60, 4.07, 1106.40, 2},
  {0, 4, 20.00, 95.00, 57.60, 5.26, 1368.50, 3},
  {1, 0, 19.80, 88.00, 15.30, 1.10, 569.60, 0},
  {1, 1, 21.30, 89.00, 24.30, 1.60, 734.40, 0},
  {1, 2, 24.40, 91.20, 42.40, 2.60, 1063.90, 1},
  {1, 3, 26.90, 92.90, 57.40, 3.43, 1338.50, 2},
  {1, 4, 30.00, 95.00, 75.50, 4.43, 1668.00, 3},
  {2, 0, 10.90, 91.50, 9.70, 0.38, 453.10, 0},
  {2, 1, 12.60, 92.00, 16.20, 0.55, 565.50, 0},
  {2, 2, 15.90, 93.10, 29.30, 0.90, 790.10, 1},
  {2, 3, 18.70, 94.00, 40.10, 1.18, 977.40, 2},
  {2, 4, 22.00, 95.00, 53.20, 1.53, 1202.00, 3},
  {3, 0, 7.90, 86.50, 7.20, 0.48, 386.60, 0},
  {3, 1, 10.00, 87.80, 12.60, 0.70, 471.40, 0},
  {3, 2, 14.20, 90.30, 23.50, 1.14, 641.20, 1},
  {3, 3, 17.80, 92.50, 32.50, 1.50, 782.70, 2},
  {3, 4, 22.00, 95.00, 43.40, 1.94, 952.40, 3},
  {4, 0, 1.20, 66.50, 45.10, 0.25, 320.00, 0},
  {4, 1, 4.30, 68.50, 67.60, 0.36, 372.40, 0},
  {4, 2, 10.60, 72.60, 112.60, 0.59, 477.30, 1},
  {4, 3, 15.80, 76.00, 150.00, 0.78, 564.60, 2},
  {4, 4, 22.00, 80.00, 195.00, 1.01, 669.50, 3},
  {5, 0, 5.80, 91.50, 5.20, 0.19, 353.30, 0},
  {5, 1, 7.90, 92.00, 9.70, 0.27, 420.70, 0},
  {5, 2, 12.20, 93.10, 18.80, 0.45, 555.50, 1},
  {5, 3, 15.70, 94.00, 26.30, 0.59, 667.80, 2},
  {5, 4, 20.00, 95.00, 35.30, 0.77, 802.60, 3},
  {6, 0, 19.80, 88.00, 15.30, 1.10, 569.60, 0},
  {6, 1, 21.30, 89.00, 24.30, 1.60, 734.40, 0},
  {6, 2, 24.40, 91.20, 42.40, 2.60, 1063.90, 1},
  {6, 3, 26.90, 92.90, 57.40, 3.43, 1338.50, 2},
  {6, 4, 30.00, 95.00, 75.50, 4.43, 1668.00, 3},
  {7, 0, 19.80, 88.00, 15.30, 1.10, 569.60, 0},
  {7, 1, 21.30, 89.00, 24.30, 1.60, 734.40, 0},
  {7, 2, 24.40, 91.20, 42.40, 2.60, 1063.90, 1},
  {7, 3, 26.90, 92.90, 57.40, 3.43, 1338.50, 2},
  {7, 4, 30.00, 95.00, 75.50, 4.43, 1668.00, 3},
  {8, 0, 1.20, 91.50, 5.90, 2.37, 578.00, 0},
  {8, 1, 3.30, 92.00, 10.70, 3.44, 746.50, 0},
  {8, 2, 7.40, 93.10, 20.30, 5.58, 1083.50, 1},
  {8, 3, 10.90, 94.00, 28.40, 7.36, 1364.30, 2},
  {8, 4, 15.00, 95.00, 38.00, 9.50, 1701.30, 3},
  {9, 0, 1.20, 91.50, 5.90, 2.37, 578.00, 0},
  {9, 1, 3.30, 92.00, 10.70, 3.44, 746.50, 0},
  {9, 2, 7.40, 93.10, 20.30, 5.58, 1083.50, 1},
  {9, 3, 10.90, 94.00, 28.40, 7.36, 1364.30, 2},
  {9, 4, 15.00, 95.00, 38.00, 9.50, 1701.30, 3},
  {10, 0, 11.50, 86.50, 5.00, 0.87, 802.60, 0},
  {10, 1, 13.50, 87.80, 9.40, 1.26, 1072.20, 0},
  {10, 2, 17.60, 90.30, 18.20, 2.05, 1611.40, 1},
  {10, 3, 20.90, 92.50, 25.60, 2.71, 2060.80, 2},
  {10, 4, 25.00, 95.00, 34.40, 3.50, 2600.00, 3},
  {11, 0, 11.50, 86.50, 5.00, 0.87, 802.60, 0},
  {11, 1, 13.50, 87.80, 9.40, 1.26, 1072.20, 0},
  {11, 2, 17.60, 90.30, 18.20, 2.05, 1611.40, 1},
  {11, 3, 20.90, 92.50, 25.60, 2.71, 2060.80, 2},
  {11, 4, 25.00, 95.00, 34.40, 3.50, 2600.00, 3},
  {12, 0, 12.90, 86.50, 11.90, 0.94, 494.70, 0},
  {12, 1, 14.70, 87.80, 19.40, 1.37, 625.80, 0},
  {12, 2, 18.30, 90.30, 34.50, 2.23, 887.90, 1},
  {12, 3, 21.40, 92.50, 47.00, 2.94, 1106.40, 2},
  {12, 4, 25.00, 95.00, 62.10, 3.79, 1368.50, 3},
  {13, 0, 3.80, 91.50, 10.80, 1.31, 494.70, 0},
  {13, 1, 6.20, 92.00, 17.80, 1.90, 625.80, 0},
  {13, 2, 11.10, 93.10, 31.90, 3.08, 887.90, 1},
  {13, 3, 15.10, 94.00, 43.60, 4.07, 1106.40, 2},
  {13, 4, 20.00, 95.00, 57.60, 5.26, 1368.50, 3},
  {14, 0, 3.80, 91.50, 10.80, 1.31, 494.70, 0},
  {14, 1, 6.20, 92.00, 17.80, 1.90, 625.80, 0},
  {14, 2, 11.10, 93.10, 31.90, 3.08, 887.90, 1},
  {14, 3, 15.10, 94.00, 43.60, 4.07, 1106.40, 2},
  {14, 4, 20.00, 95.00, 57.60, 5.26, 1368.50, 3},
  {15, 0, 3.80, 91.50, 10.80, 1.31, 494.70, 0},
  {15, 1, 6.20, 92.00, 17.80, 1.90, 625.80, 0},
  {15, 2, 11.10, 93.10, 31.90, 3.08, 887.90, 1},
  {15, 3, 15.10, 94.00, 43.60, 4.07, 1106.40, 2},
  {15, 4, 20.00, 95.00, 57.60, 5.26, 1368.50, 3},
  {16, 0, 12.90, 86.50, 11.90, 0.94, 494.70, 0},
  {16, 1, 14.70, 87.80, 19.40, 1.37, 625.80, 0},
  {16, 2, 18.30, 90.30, 34.50, 2.23, 887.90, 1},
  {16, 3, 21.40, 92.50, 47.00, 2.94, 1106.40, 2},
  {16, 4, 25.00, 95.00, 62.10, 3.79, 1368.50, 3},
  {17, 0, 7.90, 86.50, 7.20, 0.48, 386.60, 0},
  {17, 1, 10.00, 87.80, 12.60, 0.70, 471.40, 0},
  {17, 2, 14.20, 90.30, 23.50, 1.14, 641.20, 1},
  {17, 3, 17.80, 92.50, 32.50, 1.50, 782.70, 2},
  {17, 4, 22.00, 95.00, 43.40, 1.94, 952.40, 3},
  {18, 0, 7.90, 86.50, 7.20, 0.48, 386.60, 0},
  {18, 1, 10.00, 87.80, 12.60, 0.70, 471.40, 0},
  {18, 2, 14.20, 90.30, 23.50, 1.14, 641.20, 1},
  {18, 3, 17.80, 92.50, 32.50, 1.50, 782.70, 2},
  {18, 4, 22.00, 95.00, 43.40, 1.94, 952.40, 3},
  {19, 0, 7.90, 86.50, 7.20, 0.48, 386.60, 0},
  {19, 1, 10.00, 87.80, 12.60, 0.70, 471.40, 0},
  {19, 2, 14.20, 90.30, 23.50, 1.14, 641.20, 1},
  {19, 3, 17.80, 92.50, 32.50, 1.50, 782.70, 2},
  {19, 4, 22.00, 95.00, 43.40, 1.94, 952.40, 3},
  {20, 0, 10.90, 91.50, 9.70, 0.38, 453.10, 0},
  {20, 1, 12.60, 92.00, 16.20, 0.55, 565.50, 0},
  {20, 2, 15.90, 93.10, 29.30, 0.90, 790.10, 1},
  {20, 3, 18.70, 94.00, 40.10, 1.18, 977.40, 2},
  {20, 4, 22.00, 95.00, 53.20, 1.53, 1202.00, 3},
  {21, 0, 5.80, 91.50, 5.20, 0.19, 353.30, 0},
  {21, 1, 7.90, 92.00, 9.70, 0.27, 420.70, 0},
  {21, 2, 12.20, 93.10, 18.80, 0.45, 555.50, 1},
  {21, 3, 15.70, 94.00, 26.30, 0.59, 667.80, 2},
  {21, 4, 20.00, 95.00, 35.30, 0.77, 802.60, 3},
  {22, 0, 1.20, 95.90, 24.20, 0.10, 411.50, 0},
  {22, 1, 3.30, 95.80, 37.20, 0.15, 505.10, 0},
  {22, 2, 7.40, 95.50, 63.30, 0.25, 692.40, 1},
  {22, 3, 10.90, 95.30, 85.10, 0.33, 848.40, 2},
  {22, 4, 15.00, 95.00, 111.20, 0.43, 1035.60, 3},
  {23, 0, 1.20, 95.90, 24.20, 0.10, 411.50, 0},
  {23, 1, 3.30, 95.80, 37.20, 0.15, 505.10, 0},
  {23, 2, 7.40, 95.50, 63.30, 0.25, 692.40, 1},
  {23, 3, 10.90, 95.30, 85.10, 0.33, 848.40, 2},
  {23, 4, 15.00, 95.00, 111.20, 0.43, 1035.60, 3},
  {24, 0, 1.20, 95.90, 24.20, 0.10, 411.50, 0},
  {24, 1, 3.30, 95.80, 37.20, 0.15, 505.10, 0},
  {24, 2, 7.40, 95.50, 63.30, 0.25, 692.40, 1},
  {24, 3, 10.90, 95.30, 85.10, 0.33, 848.40, 2},
  {24, 4, 15.00, 95.00, 111.20, 0.43, 1035.60, 3},
  {25, 0, 1.20, 95.90, 24.20, 0.10, 411.50, 0},
  {25, 1, 3.30, 95.80, 37.20, 0.15, 505.10, 0},
  {25, 2, 7.40, 95.50, 63.30, 0.25, 692.40, 1},
  {25, 3, 10.90, 95.30, 85.10, 0.33, 848.40, 2},
  {25, 4, 15.00, 95.00, 111.20, 0.43, 1035.60, 3},
  {26, 0, 1.20, 95.90, 24.20, 0.10, 411.50, 0},
  {26, 1, 3.30, 95.80, 37.20, 0.15, 505.10, 0},
  {26, 2, 7.40, 95.50, 63.30, 0.25, 692.40, 1},
  {26, 3, 10.90, 95.30, 85.10, 0.33, 848.40, 2},
  {26, 4, 15.00, 95.00, 111.20, 0.43, 1035.60, 3},
  {27, 0, 10.90, 91.50, 9.70, 0.38, 453.10, 0},
  {27, 1, 12.60, 92.00, 16.20, 0.55, 565.50, 0},
  {27, 2, 15.90, 93.10, 29.30, 0.90, 790.10, 1},
  {27, 3, 18.70, 94.00, 40.10, 1.18, 977.40, 2},
  {27, 4, 22.00, 95.00, 53.20, 1.53, 1202.00, 3},
  {28, 0, 10.90, 91.50, 9.70, 0.38, 453.10, 0},
  {28, 1, 12.60, 92.00, 16.20, 0.55, 565.50, 0},
  {28, 2, 15.90, 93.10, 29.30, 0.90, 790.10, 1},
  {28, 3, 18.70, 94.00, 40.10, 1.18, 977.40, 2},
  {28, 4, 22.00, 95.00, 53.20, 1.53, 1202.00, 3},
  {29, 0, 1.20, 66.50, 45.10, 0.25, 320.00, 0},
  {29, 1, 4.30, 68.50, 67.60, 0.36, 372.40, 0},
  {29, 2, 10.60, 72.60, 112.60, 0.59, 477.30, 1},
  {29, 3, 15.80, 76.00, 150.00, 0.78, 564.60, 2},
  {29, 4, 22.00, 80.00, 195.00, 1.01, 669.50, 3},
  {30, 0, 5.80, 91.50, 5.20, 0.19, 353.30, 0},
  {30, 1, 7.90, 92.00, 9.70, 0.27, 420.70, 0},
  {30, 2, 12.20, 93.10, 18.80, 0.45, 555.50, 1},
  {30, 3, 15.70, 94.00, 26.30, 0.59, 667.80, 2},
  {30, 4, 20.00, 95.00, 35.30, 0.77, 802.60, 3},
  {31, 0, 5.80, 91.50, 5.20, 0.19, 353.30, 0},
  {31, 1, 7.90, 92.00, 9.70, 0.27, 420.70, 0},
  {31, 2, 12.20, 93.10, 18.80, 0.45, 555.50, 1},
  {31, 3, 15.70, 94.00, 26.30, 0.59, 667.80, 2},
  {31, 4, 20.00, 95.00, 35.30, 0.77, 802.60, 3},
  {32, 0, 5.80, 91.50, 5.20, 0.19, 353.30, 0},
  {32, 1, 7.90, 92.00, 9.70, 0.27, 420.70, 0},
  {32, 2, 12.20, 93.10, 18.80, 0.45, 555.50, 1},
  {32, 3, 15.70, 94.00, 26.30, 0.59, 667.80, 2},
  {32, 4, 20.00, 95.00, 35.30, 0.77, 802.60, 3},
  {33, 0, 10.90, 91.50, 9.70, 0.38, 453.10, 0},
  {33, 1, 12.60, 92.00, 16.20, 0.55, 565.50, 0},
  {33, 2, 15.90, 93.10, 29.30, 0.90, 790.10, 1},
  {33, 3, 18.70, 94.00, 40.10, 1.18, 977.40, 2},
  {33, 4, 22.00, 95.00, 53.20, 1.53, 1202.00, 3},
  {34, 0, 5.80, 91.50, 5.20, 0.19, 353.30, 0},
  {34, 1, 7.90, 92.00, 9.70, 0.27, 420.70, 0},
  {34, 2, 12.20, 93.10, 18.80, 0.45, 555.50, 1},
  {34, 3, 15.70, 94.00, 26.30, 0.59, 667.80, 2},
  {34, 4, 20.00, 95.00, 35.30, 0.77, 802.60, 3},
  {35, 0, 10.90, 91.50, 9.70, 0.38, 453.10, 0},
  {35, 1, 12.60, 92.00, 16.20, 0.55, 565.50, 0},
  {35, 2, 15.90, 93.10, 29.30, 0.90, 790.10, 1},
  {35, 3, 18.70, 94.00, 40.10, 1.18, 977.40, 2},
  {35, 4, 22.00, 95.00, 53.20, 1.53, 1202.00, 3},
  {36, 0, 10.90, 91.50, 9.70, 0.38, 453.10, 0},
  {36, 1, 12.60, 92.00, 16.20, 0.55, 565.50, 0},
  {36, 2, 15.90, 93.10, 29.30, 0.90, 790.10, 1},
  {36, 3, 18.70, 94.00, 40.10, 1.18, 977.40, 2},
  {36, 4, 22.00, 95.00, 53.20, 1.53, 1202.00, 3},
  {37, 0, 1.20, 91.50, 7.50, 0.26, 453.10, 0},
  {37, 1, 3.70, 92.00, 13.00, 0.38, 565.50, 0},
  {37, 2, 8.80, 93.10, 24.00, 0.63, 790.10, 1},
  {37, 3, 13.00, 94.00, 33.20, 0.84, 977.40, 2},
  {37, 4, 18.00, 95.00, 44.20, 1.09, 1202.00, 3},
  {38, 0, 1.20, 91.50, 7.50, 0.26, 453.10, 0},
  {38, 1, 3.70, 92.00, 13.00, 0.38, 565.50, 0},
  {38, 2, 8.80, 93.10, 24.00, 0.63, 790.10, 1},
  {38, 3, 13.00, 94.00, 33.20, 0.84, 977.40, 2},
  {38, 4, 18.00, 95.00, 44.20, 1.09, 1202.00, 3},
  {39, 0, 1.20, 91.50, 35.80, 0.16, 386.60, 0},
  {39, 1, 3.30, 92.00, 54.00, 0.24, 471.40, 0},
  {39, 2, 7.40, 93.10, 90.60, 0.39, 641.20, 1},
  {39, 3, 10.90, 94.00, 121.00, 0.52, 782.70, 2},
  {39, 4, 15.00, 95.00, 157.50, 0.67, 952.40, 3},
  {40, 0, 19.80, 88.00, 15.30, 1.10, 569.60, 0},
  {40, 1, 21.30, 89.00, 24.30, 1.60, 734.40, 0},
  {40, 2, 24.40, 91.20, 42.40, 2.60, 1063.90, 1},
  {40, 3, 26.90, 92.90, 57.40, 3.43, 1338.50, 2},
  {40, 4, 30.00, 95.00, 75.50, 4.43, 1668.00, 3},
  {41, 0, 3.80, 91.50, 10.80, 1.31, 494.70, 0},
  {41, 1, 6.20, 92.00, 17.80, 1.90, 625.80, 0},
  {41, 2, 11.10, 93.10, 31.90, 3.08, 887.90, 1},
  {41, 3, 15.10, 94.00, 43.60, 4.07, 1106.40, 2},
  {41, 4, 20.00, 95.00, 57.60, 5.26, 1368.50, 3},
  {42, 0, 3.80, 91.50, 10.80, 1.31, 494.70, 0},
  {42, 1, 6.20, 92.00, 17.80, 1.90, 625.80, 0},
  {42, 2, 11.10, 93.10, 31.90, 3.08, 887.90, 1},
  {42, 3, 15.10, 94.00, 43.60, 4.07, 1106.40, 2},
  {42, 4, 20.00, 95.00, 57.60, 5.26, 1368.50, 3},
  {43, 0, 1.20, 91.50, 5.90, 2.37, 578.00, 0},
  {43, 1, 3.30, 92.00, 10.70, 3.44, 746.50, 0},
  {43, 2, 7.40, 93.10, 20.30, 5.58, 1083.50, 1},
  {43, 3, 10.90, 94.00, 28.40, 7.36, 1364.30, 2},
  {43, 4, 15.00, 95.00, 38.00, 9.50, 1701.30, 3},
  {44, 0, 1.20, 91.50, 5.90, 2.37, 578.00, 0},
  {44, 1, 3.30, 92.00, 10.70, 3.44, 746.50, 0},
  {44, 2, 7.40, 93.10, 20.30, 5.58, 1083.50, 1},
  {44, 3, 10.90, 94.00, 28.40, 7.36, 1364.30, 2},
  {44, 4, 15.00, 95.00, 38.00, 9.50, 1701.30, 3},
  {45, 0, 1.20, 91.50, 5.90, 2.37, 578.00, 0},
  {45, 1, 3.30, 92.00, 10.70, 3.44, 746.50, 0},
  {45, 2, 7.40, 93.10, 20.30, 5.58, 1083.50, 1},
  {45, 3, 10.90, 94.00, 28.40, 7.36, 1364.30, 2},
  {45, 4, 15.00, 95.00, 38.00, 9.50, 1701.30, 3},
  {46, 0, 12.90, 86.50, 11.90, 0.94, 494.70, 0},
  {46, 1, 14.70, 87.80, 19.40, 1.37, 625.80, 0},
  {46, 2, 18.30, 90.30, 34.50, 2.23, 887.90, 1},
  {46, 3, 21.40, 92.50, 47.00, 2.94, 1106.40, 2},
  {46, 4, 25.00, 95.00, 62.10, 3.79, 1368.50, 3},
  {47, 0, 1.20, 91.50, 5.90, 2.37, 578.00, 0},
  {47, 1, 3.30, 92.00, 10.70, 3.44, 746.50, 0},
  {47, 2, 7.40, 93.10, 20.30, 5.58, 1083.50, 1},
  {47, 3, 10.90, 94.00, 28.40, 7.36, 1364.30, 2},
  {47, 4, 15.00, 95.00, 38.00, 9.50, 1701.30, 3},
  {48, 0, 11.50, 86.50, 5.00, 0.87, 802.60, 0},
  {48, 1, 13.50, 87.80, 9.40, 1.26, 1072.20, 0},
  {48, 2, 17.60, 90.30, 18.20, 2.05, 1611.40, 1},
  {48, 3, 20.90, 92.50, 25.60, 2.71, 2060.80, 2},
  {48, 4, 25.00, 95.00, 34.40, 3.50, 2600.00, 3},
  {49, 0, 12.90, 86.50, 11.90, 0.94, 494.70, 0},
  {49, 1, 14.70, 87.80, 19.40, 1.37, 625.80, 0},
  {49, 2, 18.30, 90.30, 34.50, 2.23, 887.90, 1},
  {49, 3, 21.40, 92.50, 47.00, 2.94, 1106.40, 2},
  {49, 4, 25.00, 95.00, 62.10, 3.79, 1368.50, 3}
};

const int REF_COUNT = sizeof(referenceData) / sizeof(referenceData[0]);

// ===== RUNTIME STATE =====
int selectedFruit = 0;
bool itemSelected = false;  // false until the user explicitly picks an item
SysState sysState = SYS_WARMUP;
unsigned long bootMs = 0;
unsigned long warmupEndMs = 0;
unsigned long lastWarmPrintMs = 0;
unsigned long lastSampleMs = 0;
unsigned long lastDhtMs = 0;
unsigned long lastOledMs = 0;
unsigned long windowStartMs = 0;
unsigned long reportNo = 0;
unsigned long lastVerifiedMs = 0;

// measurement window
uint16_t wMQ135[WINDOW_SAMPLES], wMQ4[WINDOW_SAMPLES], wMQ3[WINDOW_SAMPLES];
int   wCount = 0;
float dSumT = 0, dSumH = 0, dMinT = 0, dMaxT = 0, dMinH = 0, dMaxH = 0;
int   dCount = 0;
bool  windowAnnounced = false;
int   unstableStreak = 0;
bool  lastWindowStable = false;  // only expose a LIVE prediction input after a stable window

// last good DHT reading
bool  haveDht = false;
float lastT = 0, lastH = 0;
unsigned long lastDhtOkMs = 0;

// verified values shown on Serial / OLED / app
float temperature = 0, humidity = 0;
float mq135Value = 0, mq3Value = 0, mq4Value = 0;
int   rawMQ135 = 0, rawMQ3 = 0, rawMQ4 = 0;

// classification result
Status currentStatus = FRESH;
int   closestMinute = 0;
float bestDistance = 0;
float labelConfidence = 0;
bool  haveConfirmed = false;
Status pendingStatus = FRESH;
int   pendingCount = 0;

// verification text
char verifyMsg[64] = "-";
char faultMsg[96]  = "";

// serial input
char cmdBuf[40];
int  cmdLen = 0;

// forward declarations
const char* statusText(Status s);
void selectFruit(int i, bool markSelected = true);
void clearItemSelection();
void beginMode();
unsigned long demoSecondsLeft();

// ============================================================
//  SMALL HELPERS
// ============================================================
float mapFloat(float x, float inMin, float inMax, float outMin, float outMax) {
  return (x - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

float sqf(float x) { return x * x; }

float norm01(float v, float lo, float hi) {
  return constrain((v - lo) / (hi - lo), 0.0f, 1.0f);
}

const char* statusText(Status s) {
  if (s == FRESH)   return "FRESH";
  if (s == EARLY)   return "EARLY";
  if (s == MEDIUM)  return "MEDIUM";
  if (s == SPOILED) return "SPOILED";
  return "UNKNOWN";
}

// Text sent to the app / shown on screen
const char* stateText() {
  if (sysState == SYS_WARMUP)  return "WARM-UP";
  if (sysState == SYS_CHECKING) return "CHECKING";
  if (sysState == SYS_FAULT)   return "ERROR";
  if (!itemSelected) return "SELECT ITEM";
  return demoMode ? statusText(currentStatus) : "ML IN APP";
}

void fmtUptime(char* out, size_t n, unsigned long ms) {
  unsigned long s = ms / 1000UL;
  snprintf(out, n, "%02lu:%02lu:%02lu", s / 3600UL, (s / 60UL) % 60UL, s % 60UL);
}

void makeBar(char* out, int width, int pct) {
  int fill = (pct * width) / 100;
  for (int i = 0; i < width; i++) out[i] = (i < fill) ? '#' : '-';
  out[width] = '\0';
}

unsigned long demoSecondsLeft() {
  if (!demoMode) return 0;
  if (sysState == SYS_WARMUP) return DEMO_DURATION_MS / 1000UL;
  unsigned long elapsed = millis() - demoStartMs;
  if (elapsed >= DEMO_DURATION_MS) return 0;
  return (DEMO_DURATION_MS - elapsed + 999UL) / 1000UL;
}

const RefRow* findRefRow(int fruit, int minute) {
  for (int i = 0; i < REF_COUNT; i++) {
    if (referenceData[i].fruit == fruit && referenceData[i].minute == minute)
      return &referenceData[i];
  }
  return &referenceData[0];
}

// ============================================================
//  SERIAL REPORT FORMATTING  (fixed-width box, ASCII only)
// ============================================================
const int BOX_W = 54;

void boxRule(char c) {
  Serial.print('+');
  for (int i = 0; i < BOX_W; i++) Serial.print(c);
  Serial.println('+');
}

void boxText(const char* fmt, ...) {
  char v[80];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(v, sizeof(v), fmt, ap);
  va_end(ap);
  char line[100];
  snprintf(line, sizeof(line), " %-*s", BOX_W - 1, v);
  Serial.print('|');
  Serial.print(line);
  Serial.println('|');
}

void boxRow(const char* label, const char* fmt, ...) {
  char v[80];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(v, sizeof(v), fmt, ap);
  va_end(ap);
  char line[120];
  snprintf(line, sizeof(line), " %-13s: %-*s", label, BOX_W - 16, v);
  Serial.print('|');
  Serial.print(line);
  Serial.println('|');
}

void printReport() {
  char up[20];
  fmtUptime(up, sizeof(up), millis());

  Serial.println();
  boxRule('=');
  boxText("SPOIL ALERT   REPORT #%lu   uptime %s", reportNo, up);
  boxRule('=');
  boxRow("Item", "%s: %s", produceType(selectedFruit), fruits[selectedFruit]);
  if (demoMode) {
    boxRow("Mode", "DEMO  (%lu s left)", demoSecondsLeft());
  } else {
    boxRow("Mode", "LIVE sensors");
  }
  boxRow("Verification", "%s", verifyMsg);
  boxRule('-');
  boxRow("Temperature", "%.2f C", temperature);
  boxRow("Humidity", "%.2f %%", humidity);
  if (demoMode) {
    boxRow("MQ-135", "%.2f", mq135Value);
    boxRow("MQ-3", "%.2f", mq3Value);
    boxRow("MQ-4", "%.1f", mq4Value);
  } else {
    boxRow("MQ-135", "ADC %d", rawMQ135);
    boxRow("MQ-3", "ADC %d", rawMQ3);
    boxRow("MQ-4", "ADC %d", rawMQ4);
  }
  boxRule('-');
  if (demoMode) {
    boxRow("Demo row", "minute %d of 4", closestMinute);
    boxRow("Demo match", "%.0f %%  (distance %.4f)", labelConfidence, bestDistance);
    if (pendingCount > 0) {
      boxRow("Confirming", "%s  %d/2", statusText(pendingStatus), pendingCount);
    }
    boxRow(">> DEMO STATUS", "%s", statusText(currentStatus));
    boxText("Reference-table demonstration only.");
  } else {
    boxRow("Sensor state", "VERIFIED LIVE");
    boxRow("ML result", "Request via SPOIL ALERT app/API");
  }
  boxRule('=');
}

void printFault() {
  char up[20];
  fmtUptime(up, sizeof(up), millis());
  Serial.println();
  boxRule('=');
  boxText("SPOIL ALERT   SENSOR ERROR   uptime %s", up);
  boxRule('-');
  boxRow("Item", "%s", itemSelected ? fruits[selectedFruit] : "No item selected");
  boxRow("Problem", "%s", faultMsg);
  boxRow("Action", "Check wiring; retrying every %lu s", REPORT_INTERVAL_MS / 1000UL);
  boxRule('=');
}

void printFruitList() {
  Serial.println();
  Serial.println("Available fruit/vegetable items (type name or number):");
  for (int i = 0; i < FRUIT_COUNT; i++) {
    char b[48];
    snprintf(b, sizeof(b), "  %2d. %-10s %s", i + 1, produceType(i), fruits[i]);
    Serial.println(b);
  }
}

void printHelp() {
  Serial.println();
  Serial.println("Commands: <item name> | <1-10> | list | live | restart | help");
}

// ============================================================
//  CLASSIFICATION (nearest XLSX row of the selected item)
// ============================================================
Match classifyReading(int fruit, float t, float h, float a, float b, float c) {
  float nt = norm01(t, TEMP_MIN, TEMP_MAX);
  float nh = norm01(h, HUM_MIN, HUM_MAX);
  float na = norm01(a, MQ135_MIN, MQ135_MAX);
  float nb = norm01(b, MQ3_MIN, MQ3_MAX);
  float nc = norm01(c, MQ4_MIN, MQ4_MAX);

  float   dist[8];
  uint8_t mins[8];
  uint8_t stat[8];
  int n = 0;

  for (int i = 0; i < REF_COUNT && n < 8; i++) {
    if (referenceData[i].fruit != fruit) continue;
    const RefRow& r = referenceData[i];
    // Gas sensors are weighted higher: spoilage is mainly a gas change.
    dist[n] = sqf(nt - norm01(r.temp, TEMP_MIN, TEMP_MAX)) * 1.0f +
              sqf(nh - norm01(r.humidity, HUM_MIN, HUM_MAX)) * 0.75f +
              sqf(na - norm01(r.mq135, MQ135_MIN, MQ135_MAX)) * 2.0f +
              sqf(nb - norm01(r.mq3, MQ3_MIN, MQ3_MAX)) * 2.0f +
              sqf(nc - norm01(r.mq4, MQ4_MIN, MQ4_MAX)) * 2.0f;
    mins[n] = r.minute;
    stat[n] = r.status;
    n++;
  }

  Match m;
  m.minute = 0; m.status = FRESH; m.dist = 999999.0f; m.otherDist = 999999.0f;
  int best = -1;
  for (int i = 0; i < n; i++) {
    if (dist[i] < m.dist) { m.dist = dist[i]; best = i; }
  }
  if (best >= 0) {
    m.minute = mins[best];
    m.status = stat[best];
    for (int i = 0; i < n; i++) {
      if (stat[i] != m.status && dist[i] < m.otherDist) m.otherDist = dist[i];
    }
  }
  return m;
}

// Boot self-test: every XLSX row must match itself, and lie inside the bands.
int selfTestReferenceTable(int* outOfBand) {
  int passed = 0;
  *outOfBand = 0;
  for (int i = 0; i < REF_COUNT; i++) {
    const RefRow& r = referenceData[i];
    Match m = classifyReading(r.fruit, r.temp, r.humidity, r.mq135, r.mq3, r.mq4);
    if (m.minute == r.minute && m.status == r.status) passed++;
    if (r.temp < TEMP_MIN || r.temp > TEMP_MAX || r.humidity < HUM_MIN || r.humidity > HUM_MAX ||
        r.mq135 < MQ135_MIN || r.mq135 > MQ135_MAX || r.mq3 < MQ3_MIN || r.mq3 > MQ3_MAX ||
        r.mq4 < MQ4_MIN || r.mq4 > MQ4_MAX)
      (*outOfBand)++;
  }
  return passed;
}

// ============================================================
//  MEASUREMENT WINDOW
// ============================================================
void resetWindow() {
  wCount = 0;
  dCount = 0;
  dSumT = dSumH = 0;
  dMinT = dMinH = 1e9f;
  dMaxT = dMaxH = -1e9f;
  windowStartMs = millis();
  windowAnnounced = false;
}

int readAdcAvg(int pin) {
  long sum = 0;
  for (int i = 0; i < ADC_AVG_READS; i++) sum += analogRead(pin);
  return (int)(sum / ADC_AVG_READS);
}

void trimmedStats(const uint16_t* src, int n, float& mean, int& spread) {
  uint16_t a[WINDOW_SAMPLES];
  for (int i = 0; i < n; i++) a[i] = src[i];
  for (int i = 1; i < n; i++) {           // insertion sort
    uint16_t k = a[i];
    int j = i - 1;
    while (j >= 0 && a[j] > k) { a[j + 1] = a[j]; j--; }
    a[j + 1] = k;
  }
  int lo = TRIM_EACH_SIDE, hi = n - TRIM_EACH_SIDE;
  long sum = 0;
  for (int i = lo; i < hi; i++) sum += a[i];
  mean = (float)sum / (float)(hi - lo);
  spread = (int)a[hi - 1] - (int)a[lo];
}

bool dhtValid(float t, float h) {
  return !isnan(t) && !isnan(h) && t > -40.0f && t < 80.0f && h >= 0.0f && h <= 100.0f;
}

// One sample tick: reads the real sensors (LIVE). Also feeds the live
// preview during warm-up so you can see the sensors react.
void sampleOnce(unsigned long now) {
  if (demoMode) {
    if (sysState == SYS_WARMUP) {              // preview = minute-0 reference row
      const RefRow* r = findRefRow(referenceFruitIds[selectedFruit], 0);
      temperature = r->temp; humidity = r->humidity;
      mq135Value = r->mq135; mq3Value = r->mq3; mq4Value = r->mq4;
    }
    return;
  }

  int r135 = readAdcAvg(MQ135_PIN);
  int r4   = readAdcAvg(MQ4_PIN);
  int r3   = readAdcAvg(MQ3_PIN);

  if (now - lastDhtMs >= DHT_INTERVAL_MS || lastDhtMs == 0) {
    lastDhtMs = now;
    float t = readTemperatureC();
    float h = dht.readHumidity();
    if (dhtValid(t, h)) {
      haveDht = true; lastT = t; lastH = h; lastDhtOkMs = now;
      if (sysState != SYS_WARMUP) {
        dSumT += t; dSumH += h; dCount++;
        if (t < dMinT) dMinT = t;
        if (t > dMaxT) dMaxT = t;
        if (h < dMinH) dMinH = h;
        if (h > dMaxH) dMaxH = h;
      }
    }
  }

  if (sysState == SYS_WARMUP) {
    rawMQ135 = r135; rawMQ4 = r4; rawMQ3 = r3;
    mq135Value = (float)r135;
    mq4Value   = (float)r4;
    mq3Value   = (float)r3;
    temperature = haveDht ? lastT : 0;
    humidity    = haveDht ? lastH : 0;
  } else if (wCount < WINDOW_SAMPLES) {
    wMQ135[wCount] = (uint16_t)r135;
    wMQ4[wCount]   = (uint16_t)r4;
    wMQ3[wCount]   = (uint16_t)r3;
    wCount++;
  }
}

// ---- confirmation (debounce) of the label ----
void confirmStatus(const Match& m) {
  Status cand = (Status)m.status;
  int need = demoMode ? 1 : 2;

  if (!haveConfirmed) {
    currentStatus = cand; closestMinute = m.minute;
    haveConfirmed = true; pendingCount = 0;
  } else if (cand == currentStatus) {
    closestMinute = m.minute;
    pendingCount = 0;
  } else {
    if (cand == pendingStatus && pendingCount > 0) pendingCount++;
    else { pendingStatus = cand; pendingCount = 1; }
    if (pendingCount >= need) {
      currentStatus = cand; closestMinute = m.minute; pendingCount = 0;
    }
  }
}

void updateLEDs();
void showOLED();

// Unlabelled LIVE telemetry only. It deliberately contains no class label;
// add independently verified labels and a physical sample_id in training_data.csv.
void printMLCsvRow() {
  Serial.printf("MLDATA,%lu,%d,\"%s\",\"%s\",%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,LIVE\n",
    reportNo, selectedFruit, produceType(selectedFruit), fruits[selectedFruit], temperature, humidity,
    mq135Value, mq3Value, mq4Value, rawMQ135, rawMQ3, rawMQ4);
}

// Verified sensor window: classify only an explicit DEMO selection; LIVE
// readings are forwarded to the external model API instead.
void publishVerified() {
  sysState = SYS_READY;
  lastVerifiedMs = millis();
  unstableStreak = 0;
  reportNo++;

  // Never classify the boot-time placeholder (Apple/index 0). In LIVE mode,
  // the API model is the only prediction source. The reference-table matcher
  // remains available for an explicitly selected DEMO item only.
  if (!itemSelected) {
    haveConfirmed = false;
    pendingCount = 0;
    labelConfidence = 0;
    bestDistance = 0;
    updateLEDs();
    showOLED();
    Serial.println("[READY] Live sensors verified; select a fruit or vegetable before requesting a prediction.");
    return;
  }

  if (demoMode) {
    Match m = classifyReading(referenceFruitIds[selectedFruit], temperature, humidity, mq135Value, mq3Value, mq4Value);
    bestDistance = m.dist;
    labelConfidence = (m.otherDist <= 0.000001f || m.otherDist > 999990.0f)
                        ? 100.0f
                        : constrain(100.0f * (1.0f - m.dist / m.otherDist), 0.0f, 100.0f);
    confirmStatus(m);
  } else {
    haveConfirmed = false;
    pendingCount = 0;
    labelConfidence = 0;
    bestDistance = 0;
  }

  updateLEDs();
  showOLED();
  printReport();
  if (!demoMode && itemSelected) printMLCsvRow();
}

void enterFault(const char* msg) {
  strncpy(faultMsg, msg, sizeof(faultMsg) - 1);
  faultMsg[sizeof(faultMsg) - 1] = '\0';
  sysState = SYS_FAULT;
  lastWindowStable = false;
  haveConfirmed = false;
  pendingCount = 0;
  updateLEDs();
  showOLED();
  printFault();
}

void finalizeDemoWindow() {
  int minute = (int)((millis() - demoStartMs) / (DEMO_DURATION_MS / 5));
  if (minute > 4) minute = 4;
  const RefRow* r = findRefRow(referenceFruitIds[selectedFruit], minute);
  temperature = r->temp;
  humidity    = r->humidity;
  mq135Value  = r->mq135;
  mq3Value    = r->mq3;
  mq4Value    = r->mq4;
  rawMQ135 = rawMQ3 = rawMQ4 = 0;
  snprintf(verifyMsg, sizeof(verifyMsg), "PASSED - XLSX reference row (minute %d)", minute);
  publishVerified();
  if (minute == 4 && demoSecondsLeft() == 0) {
    static unsigned long doneFor = 0;
    if (doneFor != demoStartMs) {
      doneFor = demoStartMs;
      Serial.println(">> Demo finished. Type 'restart' to run it again.");
    }
  }
  resetWindow();
}

void finalizeLiveWindow() {
  if (wCount < MIN_SAMPLES) { resetWindow(); return; }

  float m135, m4, m3;
  int s135, s4, s3;
  trimmedStats(wMQ135, wCount, m135, s135);
  trimmedStats(wMQ4,   wCount, m4,   s4);
  trimmedStats(wMQ3,   wCount, m3,   s3);

  // ---- 1. signal present? ----
  char problems[64] = "";
  auto addProblem = [&](const char* p) {
    if (problems[0]) strlcat(problems, ", ", sizeof(problems));
    strlcat(problems, p, sizeof(problems));
  };
  if (m135 < ADC_MIN_OK || m135 > ADC_MAX_OK) addProblem("MQ-135");
  if (m4   < ADC_MIN_OK || m4   > ADC_MAX_OK) addProblem("MQ-4");
  if (m3   < ADC_MIN_OK || m3   > ADC_MAX_OK) addProblem("MQ-3");

  float t = 0, h = 0;
  bool dhtStale = false;
  if (dCount > 0) {
    t = dSumT / dCount; h = dSumH / dCount;
  } else if (haveDht && (millis() - lastDhtOkMs) <= DHT_STALE_MS) {
    t = lastT; h = lastH; dhtStale = true;
  } else {
    addProblem("DHT22");
  }

  if (problems[0]) {
    char msg[96];
    snprintf(msg, sizeof(msg), "No valid signal: %s", problems);
    resetWindow();
    enterFault(msg);
    return;
  }

  // ---- 2. stable? ----
  bool stable = (s135 <= ADC_SPREAD_MAX) && (s4 <= ADC_SPREAD_MAX) && (s3 <= ADC_SPREAD_MAX);
  if (dCount >= 2) {
    stable = stable && ((dMaxT - dMinT) <= TEMP_SPREAD_MAX) && ((dMaxH - dMinH) <= HUM_SPREAD_MAX);
  }

  if (!stable && unstableStreak < MAX_UNSTABLE_WINDOWS) {
    unstableStreak++;
    if (sysState != SYS_READY) sysState = SYS_CHECKING;
    Serial.println();
    Serial.printf("[VERIFY] Readings still settling (spread ADC: MQ135=%d MQ3=%d MQ4=%d) - re-measuring %d/%d\n",
                  s135, s3, s4, unstableStreak, MAX_UNSTABLE_WINDOWS);
    resetWindow();
    return;
  }

  // ---- 3. calculate ----
  // A window that remains noisy after the retry limit is retained for
  // diagnostics, but it must not be advertised as prediction-ready.
  lastWindowStable = stable;
  rawMQ135 = (int)(m135 + 0.5f);
  rawMQ4   = (int)(m4 + 0.5f);
  rawMQ3   = (int)(m3 + 0.5f);
  mq135Value = (float)rawMQ135;
  mq4Value   = (float)rawMQ4;
  mq3Value   = (float)rawMQ3;
  temperature = t;
  humidity    = h;

  snprintf(verifyMsg, sizeof(verifyMsg), "PASSED - %d samples, %s%s", wCount,
           stable ? "stable" : "noisy", dhtStale ? ", DHT held" : "");
  resetWindow();
  publishVerified();
}

void finalizeWindow() {
  if (demoMode) finalizeDemoWindow();
  else          finalizeLiveWindow();
}

// ============================================================
//  MODE / ITEM CONTROL
// ============================================================
// Decide whether we still need the sensor warm-up (measured from power-up).
void beginMode() {
  unsigned long need = demoMode ? WARMUP_DEMO_MS : WARMUP_LIVE_MS;
  warmupEndMs = bootMs + need;
  haveConfirmed = false;
  pendingCount = 0;
  unstableStreak = 0;
  lastWindowStable = false;
  if (millis() < warmupEndMs) {
    sysState = SYS_WARMUP;
    lastWarmPrintMs = 0;
  } else {
    sysState = SYS_CHECKING;
    demoStartMs = millis();
  }
  resetWindow();
  lastDhtMs = 0;
}

void selectFruit(int i, bool markSelected) {
  if (i < 0 || i >= FRUIT_COUNT) return;
  selectedFruit = i;
  if (markSelected) itemSelected = true;
  haveConfirmed = false;
  pendingCount = 0;
  unstableStreak = 0;
  lastWindowStable = false;
  labelConfidence = 0;
  bestDistance = 0;
  if (sysState != SYS_WARMUP) {
    sysState = SYS_CHECKING;
    demoStartMs = millis();      // a new item restarts the 2-minute demo from FRESH
  }
  resetWindow();
  updateLEDs();
  showOLED();
}

void clearItemSelection() {
  itemSelected = false;
  lastWindowStable = false;
  haveConfirmed = false;
  pendingCount = 0;
  labelConfidence = 0;
  bestDistance = 0;
  updateLEDs();
  showOLED();
}

// ============================================================
//  LEDs  (non-blocking blink while not verified)
// ============================================================
void updateLEDs() {
  unsigned long now = millis();
  bool r = false, y = false, g = false;

  if (sysState == SYS_WARMUP || sysState == SYS_CHECKING) {
    y = ((now / 500UL) % 2UL) == 0;            // slow yellow blink = working
  } else if (sysState == SYS_FAULT) {
    r = ((now / 250UL) % 2UL) == 0;            // fast red blink = sensor error
  } else if (!itemSelected || !demoMode) {
    y = ((now / 1000UL) % 2UL) == 0;            // waiting for selection / ML app
  } else if (currentStatus == FRESH) {
    g = true;
  } else if (currentStatus == EARLY || currentStatus == MEDIUM) {
    y = true;
  } else {
    r = true;
  }
  digitalWrite(RED_LED, r);
  digitalWrite(YELLOW_LED, y);
  digitalWrite(GREEN_LED, g);
}

// ============================================================
//  OLED
// ============================================================
void oledBar(int y, int pct) {
  pct = constrain(pct, 0, 100);
  display.drawRect(0, y, 128, 8, OLED_WHITE);
  display.fillRect(2, y + 2, (124 * pct) / 100, 4, OLED_WHITE);
}

void oledItemLine(int y) {
  char name[28];
  if (itemSelected) snprintf(name, sizeof(name), "%s: %s", produceType(selectedFruit), fruits[selectedFruit]);
  else snprintf(name, sizeof(name), "Select fruit/vegetable");
  display.setCursor(0, y);
  display.print(name);
}

void showOLED() {
  if (!oledOK) return;
  lastOledMs = millis();

  display.clearDisplay();
  display.setTextColor(OLED_WHITE);
  display.setTextSize(1);

  if (sysState == SYS_WARMUP) {
    unsigned long need = warmupEndMs - bootMs;
    unsigned long el = millis() - bootMs;
    if (el > need) el = need;
    int pct = (int)((el * 100UL) / need);
    unsigned long left = (need - el + 999UL) / 1000UL;
    display.setCursor(0, 0);  display.print("SPOIL ALERT");
    display.setCursor(0, 14); display.print(demoMode ? "Demo self-check..." : "Sensors warming up...");
    oledItemLine(26);
    oledBar(38, pct);
    display.setCursor(0, 52);
    display.print(pct); display.print("%   "); display.print(left); display.print("s left");
  } else if (sysState == SYS_CHECKING) {
    int pct;
    if (demoMode) pct = (int)(((millis() - windowStartMs) * 100UL) / REPORT_INTERVAL_MS);
    else          pct = (wCount * 100) / WINDOW_SAMPLES;
    display.setCursor(0, 0);  display.print("SPOIL ALERT");
    display.setCursor(0, 14); display.print("Verifying readings...");
    oledItemLine(26);
    oledBar(38, pct);
    display.setCursor(0, 52);
    if (demoMode) display.print("Reading reference");
    else { display.print(wCount); display.print("/"); display.print(WINDOW_SAMPLES); display.print(" samples"); }
  } else if (sysState == SYS_FAULT) {
    display.setTextSize(2);
    display.setCursor(0, 0); display.print("SENSOR");
    display.setCursor(0, 17); display.print("ERROR");
    display.setTextSize(1);
    display.setCursor(0, 40); display.print(faultMsg);
  } else {
    oledItemLine(0);

    display.setCursor(0, 11);
    display.print("T:"); display.print(temperature, 1);
    display.print("C H:"); display.print(humidity, 1); display.print("%");

    display.setCursor(0, 21);
    display.print("MQ135:"); display.print(rawMQ135);
    display.print(" MQ3:"); display.print(rawMQ3);

    display.setCursor(0, 31);
    display.print("MQ4 ADC:"); display.print(rawMQ4);
    if (demoMode && itemSelected) {
      display.print("  Min:"); display.print(closestMinute);
    }

    display.setTextSize(2);
    display.setCursor(0, 43);
    if (!itemSelected) {
      display.setTextSize(1);
      display.print("Select an item");
    } else if (demoMode) {
      display.print(statusText(currentStatus));
    } else {
      display.setTextSize(1);
      display.print("ML result in app");
    }

    display.setTextSize(1);
    display.setCursor(98, 43);
    if (demoMode) {
      unsigned long left = demoSecondsLeft();
      char tb[24];
      snprintf(tb, sizeof(tb), "%lu:%02lu", left / 60UL, left % 60UL);
      display.print("DEMO");
      display.setCursor(98, 52);
      display.print(tb);

      unsigned long elapsed = millis() - demoStartMs;
      if (elapsed > DEMO_DURATION_MS) elapsed = DEMO_DURATION_MS;
      int w = (int)((elapsed * 126UL) / DEMO_DURATION_MS);
      display.drawRect(0, 61, 128, 3, OLED_WHITE);
      display.fillRect(1, 61, w, 3, OLED_WHITE);
    } else {
      display.print("LIVE");
    }
  }
  display.display();
}

// ============================================================
//  WEB API  (compatible with the existing MIT App Inventor app)
// ============================================================
void handleApiStatus() {
  char json[1100];
  const bool dhtFresh = haveDht && (millis() - lastDhtOkMs <= 15000UL);
  const bool dataReady = (sysState == SYS_READY && dhtFresh && !demoMode &&
                          itemSelected && lastWindowStable);
  const char* itemName = itemSelected ? fruits[selectedFruit] : "";
  const char* itemCategory = itemSelected ? produceType(selectedFruit) : "";
  const char* reportedStatus = !itemSelected ? "SELECT_ITEM" :
      (demoMode ? statusText(currentStatus) : "AWAITING_ML");
  const char* statusSource = !itemSelected ? "none" :
      (demoMode ? "reference_table_demo" : "ml_api");
  char referenceConfidence[20];
  if (demoMode && itemSelected) snprintf(referenceConfidence, sizeof(referenceConfidence), "%.0f", labelConfidence);
  else snprintf(referenceConfidence, sizeof(referenceConfidence), "null");
  const unsigned long readingAgeMs = lastVerifiedMs == 0
      ? 0xFFFFFFFFUL
      : millis() - lastVerifiedMs;
  snprintf(json, sizeof(json),
    "{\"item\":\"%s\",\"itemType\":\"%s\",\"itemSelected\":%s,"
    "\"fruit\":\"%s\",\"fruitIndex\":%d,"
    "\"temperature\":%.2f,\"humidity\":%.2f,"
    "\"mq135\":%.2f,\"mq3\":%.2f,\"mq4\":%.2f,"
    "\"rawMq135\":%d,\"rawMq3\":%d,\"rawMq4\":%d,"
    "\"dhtValid\":%s,\"dataReady\":%s,\"measurementStable\":%s,\"featuresVersion\":2,\"sensorFeatureScale\":\"raw_adc\","
    "\"mode\":\"%s\",\"demoSecondsLeft\":%lu,"
    "\"status\":\"%s\",\"statusSource\":\"%s\",\"state\":\"%s\","
    "\"verified\":%s,\"sensorVerified\":%s,"
    "\"matchedMinute\":%d,\"matchError\":%.6f,\"referenceConfidence\":%s,\"confidence\":null,"
    "\"shelfLifeDaysEstimate\":null,\"shelfLifeBaselineDays\":null,"
    "\"report\":%lu,\"readingAgeMs\":%lu,\"uptimeMs\":%lu,\"rssi\":%d,\"wifi\":\"connected\"}",
    itemName, itemCategory, itemSelected ? "true" : "false", itemName, selectedFruit,
    temperature, humidity, mq135Value, mq3Value, mq4Value,
    rawMQ135, rawMQ3, rawMQ4, dhtFresh ? "true" : "false", dataReady ? "true" : "false",
    lastWindowStable ? "true" : "false", demoMode ? "DEMO" : "LIVE", demoSecondsLeft(),
    reportedStatus, statusSource,
    sysState == SYS_WARMUP ? "WARMUP" : sysState == SYS_CHECKING ? "CHECKING" :
    sysState == SYS_FAULT ? "FAULT" : "READY",
     dataReady ? "true" : "false",
     dataReady ? "true" : "false",
    closestMinute, bestDistance, referenceConfidence,
    reportNo, readingAgeMs, millis(), (int)WiFi.RSSI());
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleApiFruits() {
  String json = "[";
  for (int i = 0; i < FRUIT_COUNT; i++) {
    if (i > 0) json += ",";
    json += "\"" + String(fruits[i]) + "\"";
  }
  json += "]";
  server.send(200, "application/json", json);
}

// App sends a zero-based produce index: /api/item?id=0
void handleApiItem() {
  if (server.hasArg("clear") && server.arg("clear").equals("1")) {
    clearItemSelection();
    server.send(200, "application/json", "{\"ok\":true,\"itemSelected\":false,\"item\":\"\",\"itemType\":\"\"}");
    return;
  }
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\":\"Missing id\"}");
    return;
  }
  String idArg = server.arg("id");
  int id = -1;
  if (!parseFruitId(idArg.c_str(), FRUIT_COUNT, &id)) {
    server.send(400, "application/json", "{\"error\":\"Invalid fruit id\"}");
    return;
  }
  selectFruit(id);
  char json[240];
  snprintf(json, sizeof(json), "{\"ok\":true,\"itemSelected\":true,\"itemIndex\":%d,\"fruitIndex\":%d,\"item\":\"%s\",\"fruit\":\"%s\",\"itemType\":\"%s\"}",
           id, id, fruits[id], fruits[id], produceType(id));
  server.send(200, "application/json", json);
  Serial.println();
  Serial.printf("[APP] Selected %s: %s - re-measuring...\n", produceType(id), fruits[id]);
}

// Backward-compatible route for older app blocks.
void handleApiFruit() { handleApiItem(); }

// /api/mode?set=demo  or  /api/mode?set=live  (no arg = report mode)
void handleApiMode() {
  if (server.hasArg("set")) {
    String m = server.arg("set");
    if (m.equalsIgnoreCase("demo")) {
      server.send(409, "application/json", "{\"error\":\"Demonstration predictions are disabled in this live firmware.\"}");
      return;
    }
    else if (m.equalsIgnoreCase("live")) demoMode = false;
    else {
      server.send(400, "application/json", "{\"error\":\"Use set=live\"}");
      return;
    }
    beginMode();
    showOLED();
  }
  char json[60];
  snprintf(json, sizeof(json), "{\"ok\":true,\"mode\":\"%s\"}", demoMode ? "DEMO" : "LIVE");
  server.send(200, "application/json", json);
}

void startWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
#if USE_STATIC_IP
  WiFi.config(STATIC_IP, GATEWAY, SUBNET, DNS_1);
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain", "SPOIL ALERT ESP32 API. Open /api/status or /api/fruits.");
  });
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/items", HTTP_GET, handleApiFruits);
  server.on("/api/fruits", HTTP_GET, handleApiFruits);
  server.on("/api/item",  HTTP_GET, handleApiItem);
  server.on("/api/fruit",  HTTP_GET, handleApiFruit);
  server.on("/api/mode",   HTTP_GET, handleApiMode);
  server.onNotFound([]() {
    server.send(404, "application/json", "{\"error\":\"Not found\"}");
  });
  Serial.printf("[WIFI] Connecting to \"%s\" in the background...\n", WIFI_SSID);
}

// Non-blocking: reports connect / disconnect once, starts the server on first connect.
void wifiTick(unsigned long now) {
  bool up = (WiFi.status() == WL_CONNECTED);

  if (up && !wifiWasUp) {
    Serial.println();
    Serial.println("[WIFI] Connected.");
    Serial.print("[WIFI] ESP32 IP : http://");
    Serial.println(WiFi.localIP());
    Serial.println("[WIFI] Put this address in the app's BaseURL block.");
    if (!serverStarted) {
      server.begin();
      serverStarted = true;
      if (MDNS.begin("spoilalert")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[WIFI] Also reachable as http://spoilalert.local");
      }
      Serial.println("[WIFI] API ready: /api/status  /api/items  /api/item?id=  /api/mode");
    }
  } else if (!up && wifiWasUp) {
    Serial.println("[WIFI] Connection lost - reconnecting automatically.");
    lastWifiNoteMs = now;
  } else if (!up && now - lastWifiNoteMs >= 30000UL) {
    lastWifiNoteMs = now;
    Serial.println("[WIFI] Not connected yet (check SSID/password, 2.4 GHz only). Retrying...");
    WiFi.reconnect();
  }
  wifiWasUp = up;
}

// ============================================================
//  SERIAL COMMANDS
// ============================================================
int findFruitByText(const char* s) {
  for (int i = 0; i < FRUIT_COUNT; i++)           // exact (case-insensitive)
    if (strcasecmp(s, fruits[i]) == 0) return i;
  int len = strlen(s);
  if (len >= 3) {
    for (int i = 0; i < FRUIT_COUNT; i++)         // prefix
      if (strncasecmp(s, fruits[i], len) == 0) return i;
  }
  return -1;
}

void runCommand(char* s) {
  while (*s == ' ') s++;
  int n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\r')) s[--n] = '\0';
  if (n == 0) return;

  if (strcasecmp(s, "help") == 0) { printHelp(); return; }
  if (strcasecmp(s, "list") == 0) { printFruitList(); return; }

  if (strcasecmp(s, "demo") == 0) {
    Serial.println("[MODE] Demonstration mode is disabled. Use live sensor readings.");
    return;
  }
  if (strcasecmp(s, "live") == 0) {
    demoMode = false; beginMode(); showOLED();
    Serial.println("[MODE] LIVE - real sensors. Verified reports follow.");
    return;
  }
  if (strcasecmp(s, "restart") == 0) {
    selectFruit(selectedFruit, false);
    Serial.println("[MODE] Restarted - re-measuring.");
    return;
  }

  int idx = -1;
  bool allDigits = true;
  for (int i = 0; s[i]; i++) if (s[i] < '0' || s[i] > '9') allDigits = false;
  if (allDigits) {
    int num = -1;
    // parseFruitId bounds the decimal value before multiplication, avoiding
    // atoi overflow on an overlong numeric serial command.
    if (parseFruitId(s, FRUIT_COUNT + 1, &num) && num >= 1 && num <= FRUIT_COUNT)
      idx = num - 1;
  } else {
    idx = findFruitByText(s);
  }

  if (idx >= 0) {
    selectFruit(idx);
    Serial.printf("[ITEM] Selected: %s - re-measuring...\n", fruits[idx]);
  } else {
    Serial.println("[ITEM] Not found. Type 'list' to see all items, or 'help'.");
  }
}

// Non-blocking line reader (no 1-second readStringUntil stalls)
void checkSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      cmdBuf[cmdLen] = '\0';
      cmdLen = 0;
      runCommand(cmdBuf);
    } else if (c != '\r' && cmdLen < (int)sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
}

// ============================================================
//  SETUP / LOOP
// ============================================================
void printWarmupLine() {
  unsigned long need = warmupEndMs - bootMs;
  unsigned long el = millis() - bootMs;
  if (el > need) el = need;
  int pct = (int)((el * 100UL) / need);
  unsigned long left = (need - el + 999UL) / 1000UL;
  char bar[24];
  makeBar(bar, 20, pct);
  if (demoMode) {
    Serial.printf("[WARM-UP] [%s] %3d%%  %2lus left | demo self-check, no readings yet\n", bar, pct, left);
  } else {
    Serial.printf("[WARM-UP] [%s] %3d%%  %2lus left | ADC 135=%d 3=%d 4=%d | T=%.1f H=%.1f\n",
                  bar, pct, left, rawMQ135, rawMQ3, rawMQ4, temperature, humidity);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(RED_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  digitalWrite(RED_LED, LOW);
  digitalWrite(YELLOW_LED, LOW);
  digitalWrite(GREEN_LED, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(MQ135_PIN, ADC_11db);
  analogSetPinAttenuation(MQ4_PIN,   ADC_11db);
  analogSetPinAttenuation(MQ3_PIN,   ADC_11db);

  dht.begin();
#if USE_DS18B20_PROBE
  probe.begin();
  probe.setResolution(10);   // ~190 ms conversion, keeps the sample tick short
#endif

  Wire.begin(OLED_SDA, OLED_SCL);
  oledOK = OLED_BEGIN();
  if (!oledOK) {
    Serial.println("OLED initialization failed! Check SDA/SCL/VCC/GND and address (0x3C/0x3D).");
    Serial.println("Continuing without the display - LEDs, Serial and the app API still work.");
  } else {
    Wire.setClock(400000);
    display.setRotation(OLED_ROTATION);
    display.clearDisplay();
    display.setTextColor(OLED_WHITE);
    display.setTextSize(1);
    display.setCursor(10, 6);  display.println("SPOIL ALERT");
    display.setCursor(10, 21); display.println("VERIFIED READINGS");
    display.setCursor(10, 36); display.println(DEMO_DEFAULT_ON ? "2-MIN DEMO" : "LIVE MODE");
    display.setCursor(10, 51); display.println("Starting...");
    display.display();
  }

  Serial.println();
  boxRule('=');
  boxText("SPOIL ALERT - VERIFIED LIVE READINGS");
  boxRule('=');
  boxText("Commands: <item> | <1-10> | list | live");
  boxText("          restart | help");
  boxRow("Selected item", "%s", itemSelected ? fruits[selectedFruit] : "None - choose an item");
  boxRow("Mode", "%s", demoMode ? "DEMO (2 minutes)" : "LIVE sensors");

  int outOfBand = 0;
  int ok = selfTestReferenceTable(&outOfBand);
  boxRow("Table test", "%d/%d rows OK, %d out of band", ok, REF_COUNT, outOfBand);
  boxRule('-');
  if (ok != REF_COUNT || outOfBand != 0) Serial.println("!! CHECK THE REFERENCE TABLE / BANDS !!");

  startWiFi();
  delay(1200);

  bootMs = millis();
  demoStartMs = bootMs;
  lastWifiNoteMs = bootMs;
  beginMode();
  showOLED();

  Serial.printf("[START] No results are shown until the readings are verified. Warm-up: %lu s.\n",
                (demoMode ? WARMUP_DEMO_MS : WARMUP_LIVE_MS) / 1000UL);
}

void loop() {
  unsigned long now = millis();

  wifiTick(now);
  if (serverStarted) server.handleClient();
  checkSerial();

  // sample tick (live sensors; also warm-up preview)
  if (now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = now;
    sampleOnce(now);
  }

  if (sysState == SYS_WARMUP) {
    if (now >= warmupEndMs) {
      sysState = SYS_CHECKING;
      demoStartMs = now;
      resetWindow();
      Serial.println("[WARM-UP] Complete. Collecting the first verified window...");
    } else if (now - lastWarmPrintMs >= 5000UL || lastWarmPrintMs == 0) {
      lastWarmPrintMs = now;
      printWarmupLine();
    }
  } else {
    if (!windowAnnounced && sysState != SYS_READY) {
      windowAnnounced = true;
      if (sysState == SYS_CHECKING)
        Serial.printf("[MEASURING] Collecting %d samples over %lu s, then verifying...\n",
                      WINDOW_SAMPLES, REPORT_INTERVAL_MS / 1000UL);
    }
    bool done = demoMode ? (now - windowStartMs >= REPORT_INTERVAL_MS)
                         : (wCount >= WINDOW_SAMPLES || now - windowStartMs >= REPORT_INTERVAL_MS + 1500UL);
    if (done) finalizeWindow();
  }

  updateLEDs();
  if (now - lastOledMs >= OLED_REFRESH_MS) showOLED();
  delay(2);
}
