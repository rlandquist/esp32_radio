/*
  ESP32-S3-Touch-LCD-1.54 — Internet Radio + Clock/Weather Display
  ------------------------------------------------------------------
  Board:   Waveshare ESP32-S3-Touch-LCD-1.54-EN
  Screen:  1.54" 240x240 ST7789 (SPI)
  Audio:   ES8311 codec + I2S, driven by the ESP32-audioI2S library
  Extras:  NTP clock, Open-Meteo weather (no API key needed), NOAA
           Weather Radio auto-tune on severe alerts, full-screen alert
           list when more than one alert is active (tap the "N of M"
           tag in the banner, tap Home to return)
  Control: BOOT button (click = next station, long-press = play/pause)
           PLUS touch buttons along the bottom of the screen — tap to
           select a station directly, or drag left/right if there are
           more stations than fit on screen at once

  ---------------------------------------------------------------
  REQUIRED LIBRARIES:
    From Waveshare's example package, under
      examples/ESP32-S3-Touch-LCD-1.54-demo/Arduino-3.2.0/libraries/
    copy these into your Arduino "libraries" folder:
      - GFX_Library_for_Arduino
      - ESP32-audioI2S-master
      - SensorLib          (CST816 touch driver)
      - OneButton

    From that same package, under
      examples/ESP32-S3-Touch-LCD-1.54-demo/Arduino-3.2.0/examples/01_i2s_audio/
    copy these three files into THIS SKETCH'S FOLDER (not libraries):
      - es8311.h
      - es8311.cpp
      - es8311_reg.h

    From the Arduino Library Manager:
      - ArduinoJson        (v6 or v7)

  Board package: esp32 by Espressif Systems, v3.2.0 (per Waveshare docs)
  Arduino IDE board settings: "ESP32S3 Dev Module", 16MB Flash, USB CDC On Boot: Enabled

  NOTE ON TOUCH: the CST816 controller needs its reset and interrupt
  pins (47 / 48) configured via touch.setPins() before touch.begin(),
  otherwise it stays held in reset and never responds on I2C. Taken
  from Waveshare's LVGL example. There's also an I2C bus scan in
  setup() that prints every device found — useful if touch ever stops
  being detected.
  ---------------------------------------------------------------
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <Audio.h>
#include <Wire.h>
#include <OneButton.h>
#include <time.h>
#include "es8311.h"
#include "esp_check.h"
#include <TouchDrvCSTXXX.hpp>   // SensorLib — CST816 touch driver

static const char *TAG = "radio";

// =====================================================================
// USER CONFIG — edit these for your network / location / stations
// =====================================================================

// --- Wi-Fi credentials + NWS contact string ---
// If a secrets.h file is present next to this sketch, its values are used
// instead of the placeholders below. That's how the GitHub Actions build
// injects credentials without ever committing them. Compiling locally
// without a secrets.h just uses whatever you type in here.
#if __has_include("secrets.h")
  #include "secrets.h"
#endif

#ifdef SECRET_WIFI_SSID
  const char *WIFI_SSID     = SECRET_WIFI_SSID;
  const char *WIFI_PASSWORD = SECRET_WIFI_PASSWORD;
#else
  const char *WIFI_SSID     = "YOUR_WIFI_SSID";
  const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
#endif

// --- Location for weather (default: Waukesha, WI) ---
const float WEATHER_LAT = 43.0117;
const float WEATHER_LON = -88.2315;

// --- Timezone (POSIX TZ string). Central Time with automatic DST: ---
const char *TZ_STRING = "CST6CDT,M3.2.0,M11.1.0";

// --- National Weather Service requires a descriptive User-Agent on every
//     request (contact info, not a browser string). US locations only. ---
#ifdef SECRET_NWS_USER_AGENT
  const char *NWS_USER_AGENT = SECRET_NWS_USER_AGENT;
#else
  const char *NWS_USER_AGENT = "(esp32-radio-clock, your-email@example.com)";
#endif

// --- Radio stations (name + short button label + direct stream URL) ---
struct Station {
  const char *name;      // shown above the touch buttons
  const char *shortName; // shown on the touch button — keep to ~6 chars
  const char *url;
};

Station stations[] = {
  { "News/Talk 1130 WISN", "1130",  "http://stream.revma.ihrhls.com/zc4245" },
  { "The Big 920",         "920",   "http://stream.revma.ihrhls.com/zc2681" },
  { "FM 106.1",            "106.1", "http://stream.revma.ihrhls.com/zc2677" },
  { "99.1 The Mix",        "99.1",  "https://live.amperwave.net/direct/audacy-wmyxfmaac-imc" },
  { "95.7 The Big FM",     "95.7",  "http://stream.revma.ihrhls.com/zc2689" },
  { "NOAA Weather Radio",  "NOAA",  "http://radio.weatherusa.net/NWR/KEC60.mp3" },
};
const int NUM_STATIONS = sizeof(stations) / sizeof(stations[0]);
const int NOAA_STATION_INDEX = NUM_STATIONS - 1; // must stay the last entry above

// --- Volume (0-21 for ESP32-audioI2S) ---
const uint8_t DEFAULT_VOLUME = 15;

// =====================================================================
// PIN DEFINITIONS
// LCD pins from Waveshare's 04_gfx_helloworld example; audio/I2C pins
// from their 01_i2s_audio example. Both verified against their source.
// =====================================================================

// LCD (ST7789, 4-wire SPI)
// These are taken from Waveshare's 04_gfx_helloworld example for this
// exact board — verified against their source, not assumed.
#define LCD_DC   45
#define LCD_CS   21
#define LCD_SCK  38
#define LCD_MOSI 39
#define LCD_MISO -1
#define LCD_RST  40
#define LCD_BL   46

// Audio (ES8311 codec + I2S + PA enable)
#define PA_CTRL   7
#define I2S_MCLK  8
#define I2S_BCLK  9
#define I2S_LRC   10
#define I2S_DOUT  12
#define I2C_SDA   42
#define I2C_SCL   41

// User button (BOOT)
#define BTN_BOOT  0

// Touch controller (CST816) — shares the I2C bus above (SDA=42, SCL=41)
// and has its own reset + interrupt pins. These must be set via
// touch.setPins() BEFORE touch.begin(), or the controller stays held in
// reset and never answers on the bus. From Waveshare's LVGL example.
#define TOUCH_RST 47
#define TOUCH_IRQ 48

#define EXAMPLE_SAMPLE_RATE     (16000)
#define EXAMPLE_MCLK_MULTIPLE   (256)
#define EXAMPLE_MCLK_FREQ_HZ    (EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE)
#define EXAMPLE_VOICE_VOLUME    (75)   // ES8311 analog volume, separate from Audio lib volume

// =====================================================================
// GLOBALS
// =====================================================================

// Waveshare's example uses Arduino_ESP32SPI for this panel
Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, LCD_MISO);
// Display rotation. Think of it as: which way you physically turn the
// device, then rotate the CONTENT the opposite way to cancel it out.
//   0 = native, device upright, USB port on the bottom edge
//   1 = device turned 90 CCW (USB ends up on the RIGHT edge)
//   2 = device turned 180 (USB on the top edge)
//   3 = device turned 90 CW (USB ends up on the LEFT edge)
// Set for turning the device 90 CCW, so USB exits the right side.
// If the screen comes up rotated the wrong way, try 3 instead.
#define DISPLAY_ROTATION 0

// Touch coordinates are handled separately — see TOUCH_ROTATION in the
// touch input section further down. Changing DISPLAY_ROTATION does NOT
// change it; they're independent settings.
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, DISPLAY_ROTATION, true /* IPS */, 240, 240);

Audio audio;
OneButton bootButton(BTN_BOOT, true);
TouchDrvCSTXXX touch;
bool touchAvailable = false;

// --- Touch drag-scroll state for the station button row ---
bool touchDown = false;
bool isDragging = false;
int16_t touchStartX = 0, touchStartY = 0;
int scrollStartOffsetX = 0;
unsigned long lastTouchEnd = 0;
const unsigned long TOUCH_REARM_MS = 150; // ignore a new touch-down this soon after a release
const int DRAG_THRESHOLD_PX = 8;          // movement beyond this counts as a drag, not a tap

int currentStation = 0;
bool isPlaying = true;
uint8_t volume = DEFAULT_VOLUME;

String nowPlayingLine = "";   // set from audio_showstreamtitle callback
String stationStatus  = "Connecting...";

unsigned long lastClockDraw = 0;
unsigned long lastWeatherFetch = 0;
const unsigned long WEATHER_INTERVAL_MS = 15UL * 60UL * 1000UL; // 15 minutes

String weatherText = "Weather: --";
bool weatherValid = false;

// --- Weather alerts (NWS, US only) ---
struct AlertInfo {
  String event;
  String headline;
};
const int MAX_ALERTS = 5;
AlertInfo activeAlerts[MAX_ALERTS];
int numActiveAlerts = 0;

bool alertActive = false;
String alertEvent = "";
String alertHeadline = "";
unsigned long lastAlertCheck = 0;
const unsigned long ALERT_CHECK_INTERVAL_MS = 2UL * 60UL * 1000UL; // 2 minutes
unsigned long lastAlertFlash = 0;
bool alertFlashOn = true;
int preAlertStationIndex = -1; // station to restore once the alert clears
bool stationChangedDuringAlert = false; // if true, don't override the user's choice on clear
uint8_t preAlertVolume = 0;    // volume to restore once the alert clears
bool volumeChangedDuringAlert = false; // if true, don't override the user's choice on clear
const uint8_t ALERT_VOLUME = 21; // true max — this is a safety alert

// --- Screen mode: the alert list is a separate full-screen view ---
enum ScreenMode { SCREEN_MAIN, SCREEN_ALERT_LIST };
ScreenMode currentScreen = SCREEN_MAIN;

// Screen layout regions (x, y, w, h) so we only redraw what changed
const int TIME_Y    = 30;
const int DATE_Y    = 70;
const int WEATHER_Y = 110;
const int STATION_Y = 132;
const int STATUS_Y  = 152;
const int BUTTON_ROW_TOP    = 184;
const int BUTTON_ROW_BOTTOM = 236;
const int BUTTON_WIDTH      = 50;  // fixed width — row scrolls horizontally if
                                    // NUM_STATIONS * BUTTON_WIDTH > 240
const int BUTTON_GAP        = 2;
int buttonRowOffsetX = 0;          // 0 = scrolled all the way left

// Volume bar occupies a reserved column on the right edge, separate
// from the button row below it (no y-range overlap between the two)
const int VOLUME_BAR_X        = 214;
const int VOLUME_BAR_Y_TOP    = 4;
const int VOLUME_BAR_Y_BOTTOM = 176;
const int CONTENT_WIDTH       = VOLUME_BAR_X - 2; // clock/weather/station text
                                                   // stays clear of the bar

// During an alert, the top 60px splits into a slim time strip (so you
// still see the clock) above a slightly shrunk banner — same total
// footprint as before, nothing else on screen has to move
const int ALERT_TIME_STRIP_HEIGHT = 16;
const int ALERT_BANNER_HEIGHT     = 44;

// "N of M" tag in the banner's corner — tap it to open the full alert
// list (only drawn/active when there's more than one active alert)
const int ALERT_TAG_X = 196;
const int ALERT_TAG_Y = ALERT_TIME_STRIP_HEIGHT + 2;
const int ALERT_TAG_W = 40;
const int ALERT_TAG_H = 14;

// Full-screen alert list — a thin flashing strip up top (so the sense
// of urgency never fully disappears) and a Home button pinned at the
// bottom to return manually; it also auto-returns on the next 2-minute
// alert poll if you don't tap anything
const int ALERT_LIST_FLASH_HEIGHT = 6;
const int ALERT_LIST_HOME_HEIGHT  = 40;

int maxScrollOffset() {
  int totalWidth = NUM_STATIONS * BUTTON_WIDTH;
  return totalWidth > 240 ? totalWidth - 240 : 0;
}

// =====================================================================
// FORWARD DECLARATIONS
// Several functions below call others defined further down the file.
// The Arduino IDE usually auto-generates prototypes, but it doesn't
// always get it right — declaring them explicitly removes the risk.
// =====================================================================

void drawStaticUI();
void drawClock(struct tm &timeinfo);
void drawWeather();
void drawStation();
void drawStationButtons();
void drawVolumeBar();
void drawAlertBanner();
void drawAlertTimeStrip(struct tm &timeinfo);
void drawAlertListFlashStrip();
void drawAlertListScreen();
void switchToAlertListScreen();
void returnToMainScreen();
void ensureStationVisible();
void fetchWeather();
void checkWeatherAlerts();
void playCurrentStation();
void nextStation();
void selectStation(int index);
void togglePlayPause();
void rotateTouch(int16_t &x, int16_t &y);
void handleTouch();

// =====================================================================
// ES8311 CODEC INIT (adapted from Waveshare's 01_audio_out example)
// =====================================================================

static esp_err_t es8311_codec_init(void) {
  es8311_handle_t es_handle = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  ESP_RETURN_ON_FALSE(es_handle, ESP_FAIL, TAG, "es8311 create failed");

  const es8311_clock_config_t es_clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = EXAMPLE_MCLK_FREQ_HZ,
    .sample_frequency = EXAMPLE_SAMPLE_RATE
  };

  ESP_ERROR_CHECK(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
  ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(es_handle, EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE, EXAMPLE_SAMPLE_RATE), TAG, "set es8311 sample frequency failed");
  ESP_RETURN_ON_ERROR(es8311_voice_volume_set(es_handle, EXAMPLE_VOICE_VOLUME, NULL), TAG, "set es8311 volume failed");
  ESP_RETURN_ON_ERROR(es8311_microphone_config(es_handle, false), TAG, "set es8311 microphone failed");
  return ESP_OK;
}

// =====================================================================
// DISPLAY HELPERS
// =====================================================================

void drawStaticUI() {
  gfx->fillScreen(RGB565_BLACK);
  // Dividers must sit in the gaps BETWEEN the per-region erase rects,
  // or they get wiped on the first redraw and never come back.
  // Occupied bands: 10-44 (clock), 60-76 (date), 96-116 (weather),
  // 120-138 (station), 142-172 (status), 184-236 (buttons).
  gfx->drawFastHLine(10, 88, CONTENT_WIDTH - 20, RGB565_DARKGREY);
  gfx->drawFastHLine(10, 178, 220, RGB565_DARKGREY);
}

void drawClock(struct tm &timeinfo) {
  char timeStr[9];
  char dateStr[24];
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  strftime(dateStr, sizeof(dateStr), "%a %b %d, %Y", &timeinfo);

  gfx->fillRect(0, TIME_Y - 20, CONTENT_WIDTH, 34, RGB565_BLACK);
  gfx->setTextSize(3);
  gfx->setTextColor(RGB565_WHITE);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((240 - w) / 2, TIME_Y - 12);
  gfx->print(timeStr);

  gfx->fillRect(0, DATE_Y - 10, CONTENT_WIDTH, 16, RGB565_BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_CYAN);
  gfx->getTextBounds(dateStr, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((240 - w) / 2, DATE_Y - 6);
  gfx->print(dateStr);
}

void drawWeather() {
  gfx->fillRect(0, WEATHER_Y - 14, CONTENT_WIDTH, 20, RGB565_BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(weatherValid ? RGB565_YELLOW : RGB565_DARKGREY);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(weatherText.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((240 - w) / 2, WEATHER_Y - 8);
  gfx->print(weatherText);
}

void drawAlertBanner() {
  uint16_t bg = alertFlashOn ? RGB565_RED : RGB565_BLACK;
  gfx->fillRect(0, ALERT_TIME_STRIP_HEIGHT, 240, ALERT_BANNER_HEIGHT, bg);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(alertEvent.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((240 - (int)w) / 2, ALERT_TIME_STRIP_HEIGHT + 8);
  gfx->print(alertEvent);

  gfx->setTextSize(1);
  String headTrim = alertHeadline;
  if (headTrim.length() > 34) headTrim = headTrim.substring(0, 34);
  gfx->getTextBounds(headTrim.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((240 - (int)w) / 2, ALERT_TIME_STRIP_HEIGHT + 34);
  gfx->print(headTrim);

  if (numActiveAlerts > 1) {
    gfx->drawRect(ALERT_TAG_X, ALERT_TAG_Y, ALERT_TAG_W, ALERT_TAG_H, RGB565_WHITE);
    char tagBuf[8];
    snprintf(tagBuf, sizeof(tagBuf), "1/%d", numActiveAlerts);
    gfx->getTextBounds(tagBuf, 0, 0, &x1, &y1, &w, &h);
    gfx->setCursor(ALERT_TAG_X + (ALERT_TAG_W - (int)w) / 2, ALERT_TAG_Y + ALERT_TAG_H - 4);
    gfx->print(tagBuf);
  }
}

void drawAlertTimeStrip(struct tm &timeinfo) {
  char timeStr[9];
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  gfx->fillRect(0, 0, CONTENT_WIDTH, ALERT_TIME_STRIP_HEIGHT, RGB565_BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_WHITE);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((240 - (int)w) / 2, ALERT_TIME_STRIP_HEIGHT - 4);
  gfx->print(timeStr);
}

void updateAlertBanner() {
  if (!alertActive) return;
  unsigned long now = millis();
  if (now - lastAlertFlash < 600) return;
  lastAlertFlash = now;
  alertFlashOn = !alertFlashOn;
  if (currentScreen == SCREEN_MAIN) {
    drawAlertBanner();
  } else {
    drawAlertListFlashStrip();
  }
}

void drawAlertListFlashStrip() {
  uint16_t bg = alertFlashOn ? RGB565_RED : RGB565_BLACK;
  gfx->fillRect(0, 0, 240, ALERT_LIST_FLASH_HEIGHT, bg);
}

void drawAlertListScreen() {
  gfx->fillScreen(RGB565_BLACK);

  int rowsTop = ALERT_LIST_FLASH_HEIGHT;
  int rowsBottom = 240 - ALERT_LIST_HOME_HEIGHT;
  int rowCount = numActiveAlerts > 0 ? numActiveAlerts : 1;
  int rowH = (rowsBottom - rowsTop) / rowCount;

  for (int i = 0; i < numActiveAlerts; i++) {
    int y = rowsTop + i * rowH;
    if (i > 0) gfx->drawFastHLine(0, y, 240, gfx->color565(50, 50, 50));

    gfx->setTextSize(1);
    gfx->setTextColor(RGB565_WHITE);
    int16_t x1, y1; uint16_t w, h;
    gfx->getTextBounds(activeAlerts[i].event.c_str(), 0, 0, &x1, &y1, &w, &h);
    gfx->setCursor((240 - (int)w) / 2, y + rowH / 2 - 6);
    gfx->print(activeAlerts[i].event);

    String head = activeAlerts[i].headline;
    if (head.length() > 34) head = head.substring(0, 34);
    gfx->setTextColor(gfx->color565(150, 150, 150));
    gfx->getTextBounds(head.c_str(), 0, 0, &x1, &y1, &w, &h);
    gfx->setCursor((240 - (int)w) / 2, y + rowH / 2 + 10);
    gfx->print(head);
  }

  int homeY = 240 - ALERT_LIST_HOME_HEIGHT;
  gfx->fillRect(0, homeY, 240, ALERT_LIST_HOME_HEIGHT, gfx->color565(28, 28, 28));
  gfx->drawFastHLine(0, homeY, 240, gfx->color565(60, 60, 60));
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_WHITE);
  const char *homeLabel = "Home";
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(homeLabel, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((240 - (int)w) / 2, homeY + (ALERT_LIST_HOME_HEIGHT + (int)h) / 2 - 2);
  gfx->print(homeLabel);

  drawAlertListFlashStrip();
}

void switchToAlertListScreen() {
  currentScreen = SCREEN_ALERT_LIST;
  drawAlertListScreen();
}

void returnToMainScreen() {
  if (currentScreen == SCREEN_MAIN) return;
  currentScreen = SCREEN_MAIN;
  drawStaticUI();
  drawStation();
  drawStationButtons();
  drawWeather();
  drawVolumeBar();
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5)) {
    if (alertActive) {
      drawAlertTimeStrip(timeinfo);
    } else {
      drawClock(timeinfo);
    }
  }
  if (alertActive) drawAlertBanner();
}

void drawStation() {
  gfx->fillRect(0, STATION_Y - 12, CONTENT_WIDTH, 18, RGB565_BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_GREEN);
  String line = stations[currentStation].name;
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(line.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((240 - w) / 2, STATION_Y - 6);
  gfx->print(line);

  gfx->fillRect(0, STATUS_Y - 10, CONTENT_WIDTH, 30, RGB565_BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_WHITE);
  gfx->getTextBounds(stationStatus.c_str(), 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((240 - w) / 2, STATUS_Y);
  gfx->print(stationStatus);

  if (nowPlayingLine.length() > 0) {
    gfx->setTextColor(RGB565_ORANGE);
    String trimmed = nowPlayingLine;
    if (trimmed.length() > 30) trimmed = trimmed.substring(0, 30);
    gfx->getTextBounds(trimmed.c_str(), 0, 0, &x1, &y1, &w, &h);
    gfx->setCursor((240 - w) / 2, STATUS_Y + 14);
    gfx->print(trimmed);
  }
}

void drawVolumeBar() {
  int labelH = 14;
  int trackX = VOLUME_BAR_X + 2;
  int trackW = 240 - trackX - 4;
  int trackYTop = VOLUME_BAR_Y_TOP + labelH;
  int trackH = VOLUME_BAR_Y_BOTTOM - trackYTop;

  gfx->fillRect(VOLUME_BAR_X, VOLUME_BAR_Y_TOP, 240 - VOLUME_BAR_X,
                VOLUME_BAR_Y_BOTTOM - VOLUME_BAR_Y_TOP, RGB565_BLACK);

  char buf[4];
  snprintf(buf, sizeof(buf), "%d", volume);
  gfx->setTextSize(1);
  gfx->setTextColor(gfx->color565(150, 150, 150));
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(VOLUME_BAR_X + (240 - VOLUME_BAR_X - (int)w) / 2, VOLUME_BAR_Y_TOP + (int)h);
  gfx->print(buf);

  gfx->drawRect(trackX, trackYTop, trackW, trackH, gfx->color565(60, 60, 60));

  int fillH = (int)((float)(trackH - 2) * volume / 21.0f);
  if (fillH > 0) {
    gfx->fillRect(trackX + 1, trackYTop + (trackH - 1 - fillH), trackW - 2, fillH,
                  gfx->color565(20, 90, 60));
  }
}

void drawStationButtons() {
  int btnH = BUTTON_ROW_BOTTOM - BUTTON_ROW_TOP;
  gfx->fillRect(0, BUTTON_ROW_TOP, 240, btnH, RGB565_BLACK);
  for (int i = 0; i < NUM_STATIONS; i++) {
    int x = i * BUTTON_WIDTH - buttonRowOffsetX;
    if (x + BUTTON_WIDTH < 0 || x > 240) continue; // fully off-screen — skip
    bool active = (i == currentStation);
    uint16_t bg = active ? gfx->color565(20, 90, 60) : gfx->color565(28, 28, 28);
    uint16_t fg = active ? RGB565_WHITE : gfx->color565(150, 150, 150);
    int w = BUTTON_WIDTH - BUTTON_GAP;
    gfx->fillRect(x, BUTTON_ROW_TOP, w, btnH, bg);
    gfx->drawRect(x, BUTTON_ROW_TOP, w, btnH, gfx->color565(60, 60, 60));
    gfx->setTextSize(1);
    gfx->setTextColor(fg);
    String label = stations[i].shortName;
    int16_t x1, y1; uint16_t tw, th;
    gfx->getTextBounds(label.c_str(), 0, 0, &x1, &y1, &tw, &th);
    gfx->setCursor(x + (w - (int)tw) / 2, BUTTON_ROW_TOP + (btnH + (int)th) / 2 - 2);
    gfx->print(label);
  }
}

void ensureStationVisible() {
  int btnX = currentStation * BUTTON_WIDTH;
  if (btnX < buttonRowOffsetX) {
    buttonRowOffsetX = btnX;
  } else if (btnX + BUTTON_WIDTH > buttonRowOffsetX + 240) {
    buttonRowOffsetX = btnX + BUTTON_WIDTH - 240;
  }
  buttonRowOffsetX = constrain(buttonRowOffsetX, 0, maxScrollOffset());
}

// =====================================================================
// WEATHER (Open-Meteo — free, no API key)
// =====================================================================

String weatherCodeToText(int code) {
  if (code == 0) return "Clear";
  if (code == 1 || code == 2) return "Partly Cloudy";
  if (code == 3) return "Overcast";
  if (code == 45 || code == 48) return "Fog";
  if (code >= 51 && code <= 57) return "Drizzle";
  if (code >= 61 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Rain Showers";
  if (code >= 85 && code <= 86) return "Snow Showers";
  if (code >= 95) return "Thunderstorm";
  return "Unknown";
}

void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure(); // no cert pinning — fine for a hobby display

  HTTPClient http;
  char url[220];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current_weather=true&temperature_unit=fahrenheit",
           WEATHER_LAT, WEATHER_LON);

  if (!http.begin(client, url)) {
    weatherText = "Weather: unavailable";
    weatherValid = false;
    return;
  }

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    JsonDocument doc; // ArduinoJson v7 style
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      float temp = doc["current_weather"]["temperature"] | 0.0;
      int code = doc["current_weather"]["weathercode"] | -1;
      char buf[40];
      snprintf(buf, sizeof(buf), "%s, %.0f F", weatherCodeToText(code).c_str(), temp);
      weatherText = String(buf);
      weatherValid = true;
    } else {
      weatherText = "Weather: parse error";
      weatherValid = false;
    }
  } else {
    weatherText = "Weather: unavailable";
    weatherValid = false;
  }
  http.end();
}

// =====================================================================
// WEATHER ALERTS (National Weather Service — US only, no API key)
// =====================================================================

void checkWeatherAlerts() {
  // Every poll is also the auto-return point for the alert list screen —
  // if you're on it and haven't tapped Home, this is where it kicks you
  // back to the main screen (so the flashing urgency cue isn't missable
  // for more than one poll interval)
  returnToMainScreen();

  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  char url[140];
  snprintf(url, sizeof(url),
           "https://api.weather.gov/alerts/active?point=%.4f,%.4f&status=actual&message_type=alert",
           WEATHER_LAT, WEATHER_LON);

  if (!http.begin(client, url)) return;
  http.addHeader("User-Agent", NWS_USER_AGENT);
  http.addHeader("Accept", "application/geo+json");

  int httpCode = http.GET();
  numActiveAlerts = 0;

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      JsonArray features = doc["features"].as<JsonArray>();
      for (JsonObject f : features) {
        if (numActiveAlerts >= MAX_ALERTS) break;
        const char *severity = f["properties"]["severity"] | "Unknown";
        const char *event    = f["properties"]["event"] | "";
        const char *headline = f["properties"]["headline"] | "";
        String sevStr = String(severity);
        String evStr  = String(event);
        // Only surface the alert types worth interrupting the display for.
        // NWS returns these already in priority order, so collecting them
        // in order gives us a correctly-ranked list for free.
        if (sevStr == "Extreme" || sevStr == "Severe" || evStr.indexOf("Warning") >= 0) {
          activeAlerts[numActiveAlerts].event = evStr;
          activeAlerts[numActiveAlerts].headline = String(headline);
          numActiveAlerts++;
        }
      }
    }
  }
  http.end();

  bool foundSevere = numActiveAlerts > 0;

  if (foundSevere && !alertActive) {
    // New alert just appeared — auto-tune to the live NOAA Weather Radio
    // broadcast and boost to max volume so the alert is actually heard
    preAlertStationIndex = currentStation;
    stationChangedDuringAlert = false;
    preAlertVolume = volume;
    volumeChangedDuringAlert = false;
    currentStation = NOAA_STATION_INDEX;
    playCurrentStation();
    volume = ALERT_VOLUME;
    audio.setVolume(volume);
    drawVolumeBar();
    alertFlashOn = true;
    lastAlertFlash = millis();
  } else if (!foundSevere && alertActive) {
    // Alert cleared — switch back to whatever was playing before, and
    // restore the volume too, unless you manually changed it mid-alert
    if (preAlertStationIndex >= 0 && !stationChangedDuringAlert) currentStation = preAlertStationIndex;
    if (!volumeChangedDuringAlert) {
      volume = preAlertVolume;
      audio.setVolume(volume);
    }
    drawStaticUI();
    playCurrentStation();
    drawWeather();
    drawVolumeBar();
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5)) drawClock(timeinfo);
  }

  alertActive = foundSevere;
  alertEvent = numActiveAlerts > 0 ? activeAlerts[0].event : "";
  alertHeadline = numActiveAlerts > 0 ? activeAlerts[0].headline : "";

  if (alertActive) {
    drawAlertBanner();
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5)) drawAlertTimeStrip(timeinfo);
  }
}

// =====================================================================
// RADIO CONTROL
// =====================================================================

void playCurrentStation() {
  stationStatus = "Connecting...";
  nowPlayingLine = "";
  drawStation();
  ensureStationVisible();
  drawStationButtons();
  audio.connecttohost(stations[currentStation].url);
  isPlaying = true;
}

void nextStation() {
  currentStation = (currentStation + 1) % NUM_STATIONS;
  if (alertActive) stationChangedDuringAlert = true;
  playCurrentStation();
}

void selectStation(int index) {
  if (index < 0 || index >= NUM_STATIONS || index == currentStation) return;
  currentStation = index;
  if (alertActive) stationChangedDuringAlert = true;
  playCurrentStation();
}

void togglePlayPause() {
  if (isPlaying) {
    audio.stopSong();
    isPlaying = false;
    stationStatus = "Paused";
    drawStation();
  } else {
    playCurrentStation();
  }
}

// OneButton callbacks
void onBootClick()     { nextStation(); }
void onBootLongPress() { togglePlayPause(); }

// =====================================================================
// TOUCH INPUT — tap a button to select it, or drag the row to scroll
// =====================================================================

// Touch coordinate rotation, INDEPENDENT of DISPLAY_ROTATION.
// The CST816 may or may not already return coordinates in the display's
// orientation, so this can't be derived from DISPLAY_ROTATION reliably —
// it has to be found by testing.
//
// IF TAPS LAND IN THE WRONG PLACE: just try the next value. The four
// values are the four 90-degree steps, so one of them is correct.
//   0 = no change      1 = 90 degrees      2 = 180 degrees      3 = 270 degrees
// Off by 90 in either direction? Try 1 or 3. Off by 180? Try 2.
// Uncomment the Serial.printf lines in handleTouch() to watch raw vs
// mapped coordinates while testing.
#define TOUCH_ROTATION 0

void rotateTouch(int16_t &x, int16_t &y) {
  int16_t rawX = x, rawY = y;
#if TOUCH_ROTATION == 1
  x = 239 - rawY;
  y = rawX;
#elif TOUCH_ROTATION == 2
  x = 239 - rawX;
  y = 239 - rawY;
#elif TOUCH_ROTATION == 3
  x = rawY;
  y = 239 - rawX;
#else
  (void)rawX; (void)rawY;   // TOUCH_ROTATION 0 — coordinates used as-is
#endif
}

void handleTouch() {
  if (!touchAvailable) return;

  // Rate-limit the I2C read. loop() spins very fast to keep audio
  // decoding fed, and doing an I2C transaction every iteration floods
  // the bus and steals time from audio.loop(). 50Hz is plenty
  // responsive for taps and drags.
  static unsigned long lastPoll = 0;
  if (millis() - lastPoll < 20) return;
  lastPoll = millis();

  int16_t x, y;
  // SensorLib's getPoint takes a third argument: how many touch points to
  // read. This board is single-touch, so 1. (The library marks this call
  // deprecated in favour of getTouchPoints, but it still works and this
  // signature is confirmed against the version in use — the deprecation
  // note in the build log is harmless.)
  uint8_t touched = touch.getPoint(&x, &y, 1);
  if (touched) {
    // Uncomment while testing touch alignment:
    // Serial.printf("raw %d,%d -> ", x, y);
    rotateTouch(x, y);
    // Serial.printf("mapped %d,%d\n", x, y);
  }
  unsigned long now = millis();

  // Alert list screen — only the Home button is interactive here;
  // everything else on this screen is read-only
  if (currentScreen == SCREEN_ALERT_LIST) {
    static bool homePressed = false;
    if (touched && y >= (240 - ALERT_LIST_HOME_HEIGHT) && !homePressed) {
      homePressed = true;
      returnToMainScreen();
    } else if (!touched) {
      homePressed = false;
    }
    return;
  }

  // "N of M" alert tag — a single tap opens the full alert list
  static bool tagPressed = false;
  bool inTag = touched && alertActive && numActiveAlerts > 1 &&
               x >= ALERT_TAG_X && x <= ALERT_TAG_X + ALERT_TAG_W &&
               y >= ALERT_TAG_Y && y <= ALERT_TAG_Y + ALERT_TAG_H;
  if (inTag && !tagPressed) {
    tagPressed = true;
    switchToAlertListScreen();
    return;
  } else if (!touched) {
    tagPressed = false;
  } else if (tagPressed) {
    return; // still holding on the tag, ignore until release
  }

  // Volume bar — a separate reserved column; tap or drag both just set
  // the level directly under your finger, no drag-vs-tap distinction needed
  if (touched && x >= VOLUME_BAR_X && x <= 240 &&
      y >= VOLUME_BAR_Y_TOP && y <= VOLUME_BAR_Y_BOTTOM) {
    float t = (float)(VOLUME_BAR_Y_BOTTOM - y) / (VOLUME_BAR_Y_BOTTOM - VOLUME_BAR_Y_TOP);
    int newVolume = constrain((int)(t * 21.0f + 0.5f), 0, 21);
    if (newVolume != volume) {
      volume = newVolume;
      audio.setVolume(volume);
      drawVolumeBar();
      if (alertActive) volumeChangedDuringAlert = true;
    }
    return;
  }

  if (touched) {
    if (!touchDown) {
      // A new touch just started
      if (now - lastTouchEnd < TOUCH_REARM_MS) return; // debounce re-trigger
      if (y < BUTTON_ROW_TOP || y > BUTTON_ROW_BOTTOM) return; // outside the row
      touchDown = true;
      isDragging = false;
      touchStartX = x;
      touchStartY = y;
      scrollStartOffsetX = buttonRowOffsetX;
    } else {
      // Finger still down — check whether it's turned into a drag
      int dx = x - touchStartX;
      if (!isDragging && abs(dx) > DRAG_THRESHOLD_PX) {
        isDragging = true;
      }
      if (isDragging) {
        int newOffset = constrain(scrollStartOffsetX - dx, 0, maxScrollOffset());
        if (newOffset != buttonRowOffsetX) {
          buttonRowOffsetX = newOffset;
          drawStationButtons();
        }
      }
    }
  } else if (touchDown) {
    // Finger just lifted
    if (isDragging) {
      // Snap to the nearest button boundary
      buttonRowOffsetX = constrain(
        ((buttonRowOffsetX + BUTTON_WIDTH / 2) / BUTTON_WIDTH) * BUTTON_WIDTH,
        0, maxScrollOffset());
      drawStationButtons();
    } else {
      // A clean tap — select whichever button is under the start point
      int index = (touchStartX + buttonRowOffsetX) / BUTTON_WIDTH;
      selectStation(index);
    }
    touchDown = false;
    isDragging = false;
    lastTouchEnd = now;
  }
}

// =====================================================================
// AUDIO LIBRARY CALLBACKS (optional but useful for the display)
// =====================================================================

void audio_info(const char *info) {
  Serial.print("info        "); Serial.println(info);
}

void audio_showstation(const char *info) {
  Serial.print("station     "); Serial.println(info);
}

void audio_showstreamtitle(const char *info) {
  Serial.print("streamtitle "); Serial.println(info);
  nowPlayingLine = String(info);
  if (currentScreen == SCREEN_MAIN) drawStation();
}

void audio_bitrate(const char *info) {
  Serial.print("bitrate     "); Serial.println(info);
  stationStatus = "Playing";
  if (currentScreen == SCREEN_MAIN) drawStation();
}

void audio_eof_stream(const char *info) {
  Serial.print("eof_stream  "); Serial.println(info);
  stationStatus = "Reconnecting...";
  if (currentScreen == SCREEN_MAIN) drawStation();
  audio.connecttohost(stations[currentStation].url);
}

// =====================================================================
// SETUP / LOOP
// =====================================================================

void setup() {
  Serial.begin(115200);

  // --- Display ---
  // Waveshare's example calls gfx->begin() first, then enables the
  // backlight — keeping that order.
  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
  drawStaticUI();
  gfx->setCursor(30, 120);
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_WHITE);
  gfx->print("Connecting to Wi-Fi...");

  // --- Button ---
  bootButton.attachClick(onBootClick);
  bootButton.attachLongPressStart(onBootLongPress);

  // --- Audio codec / amp ---
  Wire.begin(I2C_SDA, I2C_SCL);

  // Scan the I2C bus and report what's actually out there. Handy for
  // diagnosing a device that isn't being detected — expect the ES8311
  // codec, the QMI8658 IMU, and the CST816 touch controller.
  Serial.println("Scanning I2C bus...");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  I2C device found at 0x%02X\n", addr);
    }
  }
  Serial.println("I2C scan complete.");

  pinMode(PA_CTRL, OUTPUT);
  digitalWrite(PA_CTRL, HIGH);
  es8311_codec_init();
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK);
  audio.setVolume(volume);

  // --- Touch controller (shares the I2C bus above) ---
  // setPins() must come before begin() — see the pin definitions above.
  touch.setPins(TOUCH_RST, TOUCH_IRQ);
  touchAvailable = touch.begin(Wire, CST816_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);
  if (touchAvailable) {
    Serial.println("Touch controller initialized.");
  } else {
    Serial.println("Touch controller not found. BOOT button still works normally.");
  }

  // --- Wi-Fi ---
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(250);
    Serial.print(".");
  }

  drawStaticUI();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());

    // --- Time (NTP) ---
    configTzTime(TZ_STRING, "pool.ntp.org", "time.nist.gov");

    // --- Weather ---
    fetchWeather();
    lastWeatherFetch = millis();

    // --- Weather alerts ---
    checkWeatherAlerts();
    lastAlertCheck = millis();

    // --- Start radio ---
    playCurrentStation();
  } else {
    stationStatus = "No WiFi - radio disabled";
    Serial.println("\nWiFi connect failed");
  }

  drawStation();
  drawStationButtons();
  drawWeather();
  drawVolumeBar();
}

void loop() {
  // Keep audio decoding responsive — call as often as possible
  audio.loop();
  bootButton.tick();
  handleTouch();

  unsigned long now = millis();

  // Update the time display once per second — full clock normally,
  // or the slim time strip above the alert banner while one is active.
  // Skipped entirely while the alert list screen is showing, so it
  // doesn't draw over that screen's content.
  if (currentScreen == SCREEN_MAIN && now - lastClockDraw >= 1000) {
    lastClockDraw = now;
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5)) {
      if (alertActive) {
        drawAlertTimeStrip(timeinfo);
      } else {
        drawClock(timeinfo);
      }
    }
  }

  // Refresh weather periodically — the fetch itself always runs on
  // schedule, but the redraw is skipped while alertActive or while
  // the alert list screen is showing
  if (now - lastWeatherFetch >= WEATHER_INTERVAL_MS) {
    lastWeatherFetch = now;
    fetchWeather();
    if (!alertActive && currentScreen == SCREEN_MAIN) drawWeather();
  }

  // Check for new/cleared weather alerts periodically
  if (now - lastAlertCheck >= ALERT_CHECK_INTERVAL_MS) {
    lastAlertCheck = now;
    checkWeatherAlerts();
  }

  // Flash the alert banner while one is active
  updateAlertBanner();

  vTaskDelay(1);
}
