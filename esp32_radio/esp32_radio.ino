/*
  ESP32-S3-Touch-LCD-1.54 — Internet Radio + Clock/Weather Display
  ------------------------------------------------------------------
  Board:   Waveshare ESP32-S3-Touch-LCD-1.54-EN
  Screen:  1.54" 240x240 ST7789 (SPI)
  Audio:   ES8311 codec + I2S, driven by the ESP32-audioI2S library
  Extras:  NTP clock, Open-Meteo weather (no API key needed), NOAA
           Weather Radio auto-tune on severe alerts, full-screen alert
           list when several alerts are active
  Control: BOOT button  — click = next station, long-press = play/pause
           Button GPIO5  — volume down (hold to repeat)
           Button GPIO4  — volume up   (hold to repeat)
           Touch (upper) — tap anywhere above the bottom band to MUTE
           Touch (lower) — tap the station name to open the station list;
                           during a weather alert that band becomes the
                           alert banner, and tapping it opens the alert
                           list

  Threading: network fetches run on a task pinned to core 0 so blocking
  HTTPS calls can't starve the audio decoder on core 1. That task only
  fetches; the main loop does all drawing and audio changes.

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
      .../Arduino-3.2.0/libraries/GFX_Library_for_Arduino/examples/HelloWorldGfxfont/
    copy this into THIS SKETCH'S FOLDER:
      - FreeSansBold10pt7b.h

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
#include "FreeSansBold10pt7b.h" // body text — from GFX library's HelloWorldGfxfont example
#include "FreeSansBold12pt7b.h" // alert titles — ~1.2x the body font
#include "FreeSansBold24pt7b.h" // clock — native size, so it isn't pixel-scaled

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

// --- Alert test mode ---------------------------------------------------
// 0 = off (normal operation)
// 1 = inject ONE fake alert            — tests banner, NOAA auto-tune,
//                                        volume boost, and the restore
// 3 = inject THREE fake alerts         — also tests the "tap for all N
//                                        alerts" banner and the list screen
// In test mode no network call is made, and the fake alerts TOGGLE on and
// off on each 2-minute poll, so you get to watch both the alert starting
// and it clearing without waiting for real weather.
#define ALERT_TEST_MODE 0

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
const uint8_t DEFAULT_VOLUME = 12;

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

// Buttons. All three are on Waveshare's 02_button_example: BOOT plus
// two more, which now drive volume.
#define BTN_BOOT      0    // click = next station, long-press = play/pause
#define BTN_VOL_DOWN  5
#define BTN_VOL_UP    4

// Touch controller (CST816) — shares the I2C bus above (SDA=42, SCL=41)
// and has its own reset + interrupt pins. These must be set via
// touch.setPins() BEFORE touch.begin(), or the controller stays held in
// reset and never answers on the bus. From Waveshare's LVGL example.
#define TOUCH_RST 47
#define TOUCH_IRQ 48

// The ESP32-audioI2S library always outputs 48kHz, regardless of the
// source. Waveshare's example configures the codec for 16000 because it
// plays a local voice file, but for streaming radio the codec's clock
// should match what's actually coming out of I2S.
// IF AUDIO BREAKS OR SOUNDS WRONG AFTER THIS CHANGE, put it back to
// 16000 — that's the value Waveshare ship and it demonstrably works.
#define EXAMPLE_SAMPLE_RATE     (48000)
#define EXAMPLE_MCLK_MULTIPLE   (256)
#define EXAMPLE_MCLK_FREQ_HZ    (EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE)
// ES8311 analog output volume, 0-100. This is a SEPARATE gain stage from
// audio.setVolume() — think of it as the amplifier's own level, set once
// at boot. Waveshare's example uses 75; 90 turned out to be pushing the
// small speaker hard enough to add harshness, so 80 is the compromise.
#define EXAMPLE_VOICE_VOLUME    (80)

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
#define DISPLAY_ROTATION 1

// Touch coordinates are handled separately — see TOUCH_ROTATION in the
// touch input section further down. Changing DISPLAY_ROTATION does NOT
// change it; they're independent settings.
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, DISPLAY_ROTATION, true /* IPS */, 240, 240);

Audio audio;
OneButton bootButton(BTN_BOOT, true);
OneButton volDownButton(BTN_VOL_DOWN, true);
OneButton volUpButton(BTN_VOL_UP, true);
TouchDrvCSTXXX touch;
bool touchAvailable = false;


int currentStation = 0;
bool isPlaying = true;
uint8_t volume = DEFAULT_VOLUME;

// Mute is preferred over pause for quick silencing: pause tears down the
// stream and resuming costs several seconds of reconnect, while mute is
// instant both ways. Muting is shown persistently on screen, not just as
// a transient overlay, so an accidental tap can't leave you wondering
// why the radio is silent.
bool isMuted = false;
uint8_t preMuteVolume = DEFAULT_VOLUME;


unsigned long lastClockDraw = 0;
const unsigned long WEATHER_INTERVAL_MS = 15UL * 60UL * 1000UL; // 15 minutes

// Fixed char buffers rather than Arduino String. String reallocates on
// every update, and on a device meant to run for weeks that repeated
// churn fragments the heap until a big allocation (like the audio
// buffer) fails. Fixed buffers never fragment.
char weatherText[48] = "Weather: --";     // read by main loop only
bool weatherValid = false;
char pendingWeatherText[48] = "";         // written by netTask only
bool pendingWeatherValid = false;

// --- Weather alerts (NWS, US only) ---
struct AlertInfo {
  char event[40];
  char headline[128];
};
const int MAX_ALERTS = 5;
AlertInfo activeAlerts[MAX_ALERTS];
int numActiveAlerts = 0;

// Set by netTask when Wi-Fi comes up after having been down at boot;
// the main loop sees it and starts playback (audio calls must not be
// made from the network task).
volatile bool wifiJustCameUp = false;

// Network work runs on a separate task (see netTask) so a slow HTTPS
// fetch can't stall audio decoding. The task only ever writes to the
// "pending" copies and sets a dirty flag; the main loop picks them up
// and does all the drawing and audio changes. The mutex guards the
// pending buffers during the handoff.
AlertInfo pendingAlerts[MAX_ALERTS];
int pendingNumAlerts = 0;
volatile bool alertsDirty = false;
volatile bool weatherDirty = false;
SemaphoreHandle_t netMutex = NULL;

bool alertActive = false;
char alertEvent[40] = "";
char alertHeadline[128] = "";
const unsigned long ALERT_CHECK_INTERVAL_MS = 2UL * 60UL * 1000UL; // 2 minutes
unsigned long lastAlertFlash = 0;
bool alertFlashOn = true;
int preAlertStationIndex = -1; // station to restore once the alert clears
bool stationChangedDuringAlert = false; // if true, don't override the user's choice on clear
uint8_t preAlertVolume = 0;    // volume to restore once the alert clears
bool volumeChangedDuringAlert = false; // if true, don't override the user's choice on clear
const uint8_t ALERT_VOLUME = 21; // true max — this is a safety alert

// --- Screen mode: station list and alert list are separate full screens ---
enum ScreenMode { SCREEN_MAIN, SCREEN_STATION_LIST, SCREEN_ALERT_LIST };
ScreenMode currentScreen = SCREEN_MAIN;

// ---------------------------------------------------------------------
// SCREEN LAYOUT (240x240)
// Each value is the TOP edge of a band; bands don't overlap, so each can
// be erased and redrawn independently.
// ---------------------------------------------------------------------
const int CLOCK_TOP    = 14;   // large time, no seconds
const int CLOCK_H      = 58;   // 24pt font is ~56px tall — erase band must cover it
const int DATE_TOP     = 78;
const int DATE_H       = 18;
const int WEATHER_TOP  = 104;
const int WEATHER_H    = 22;
// (CLOCK_H / DATE_H / WEATHER_H are the erase heights for each band)

// The bottom band shows the station name (tap it to open the station
// list) — or, during a weather alert, the alert banner takes it over.
const int BOTTOM_TOP   = 166;
const int DIVIDER_Y    = 162;
const int STATION_TOP  = 176;
const int HINT_TOP     = 208;

// Volume overlay — a temporary box that appears when the volume buttons
// are pressed, then disappears. Volume has no permanent screen space.
const int VOL_OVERLAY_TOP    = 88;
const int VOL_OVERLAY_H      = 56;
const unsigned long VOL_OVERLAY_MS = 2000;
unsigned long volOverlayUntil = 0;

// Station list screen
const int LIST_HEADER_H = 26;

// =====================================================================
// FORWARD DECLARATIONS
// Several functions below call others defined further down the file.
// The Arduino IDE usually auto-generates prototypes, but it doesn't
// always get it right — declaring them explicitly removes the risk.
// =====================================================================

void drawTextCentered(const char *s, int topY, uint16_t color, uint8_t size,
                      const GFXfont *font);
void drawStaticUI();
void invalidateClockCache();
bool timeIsSynced(struct tm &timeinfo);
void drawClock(struct tm &timeinfo, bool force = false);
void drawWeather();
void drawBottomBand();
void drawVolumeOverlay();
void clearVolumeOverlay();
void drawAlertBanner();
void prepareAlertBannerText();
int wrapText(const char *text, char out[][48], int maxLines,
             const GFXfont *font, int maxWidth);
int drawTextWrapped(const char *text, int topY, int lineH, int maxLines,
                    uint16_t color, const GFXfont *font, int maxWidth);
void drawAlertListFlashStrip();
void drawAlertListScreen();
void drawStationListScreen();
void switchToAlertListScreen();
void switchToStationListScreen();
void returnToMainScreen();
void fetchWeather();
void fetchAlerts();
void applyAlertState();
void netTask(void *param);
void toggleMute();
void playCurrentStation();
void nextStation();
void selectStation(int index);
void togglePlayPause();
void volumeUp();
void volumeDown();
void onVolDown();
void onVolUp();
void drawDate(struct tm &timeinfo, bool force = false);
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

// Draw a string horizontally centred, with its TOP edge at topY.
// Custom GFX fonts position by BASELINE, not top-left, so this uses
// getTextBounds to work out the offset rather than guessing.
//
// Pass the font you want. Always draw at size 1 where possible:
// setTextSize(n) on a bitmap font just replicates pixels n x n, so a
// scaled font has 1/n the effective resolution and looks blocky. Using a
// font that's natively the right size keeps full panel resolution.
void drawTextCentered(const char *s, int topY, uint16_t color, uint8_t size,
                      const GFXfont *font) {
  gfx->setFont(font);
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((240 - (int)w) / 2 - x1, topY - y1);
  gfx->print(s);
  gfx->setFont(NULL);
  gfx->setTextSize(1);
}

void drawStaticUI() {
  gfx->fillScreen(RGB565_BLACK);
  invalidateClockCache();   // screen wiped, so cached text is stale
  gfx->drawFastHLine(20, DIVIDER_Y, 200, gfx->color565(60, 60, 60));
}

// Both of these are called once a second, but the text they show only
// changes once a minute / once a day. Redrawing regardless means an
// erase-then-repaint 60x more often than needed, which reads as a
// flicker. So they cache what was last drawn and no-op if it matches.
// Pass force=true after a full-screen repaint, when the cache is stale.
static char lastTimeStr[12] = "";
static char lastDateStr[24] = "";

// The RTC starts at the epoch, so anything before 2020 means NTP hasn't
// come back yet. Showing a confident 1970 time would be worse than
// admitting we don't know.
bool timeIsSynced(struct tm &timeinfo) {
  return (timeinfo.tm_year + 1900) >= 2020;
}

void drawClock(struct tm &timeinfo, bool force) {
  char timeStr[8], ampm[4];
  // 12-hour, no leading zero, no seconds — seconds are what forced the
  // clock to be small before, and they aren't useful on a wall clock.
  bool synced = timeIsSynced(timeinfo);
  if (synced) {
    strftime(timeStr, sizeof(timeStr), "%l:%M", &timeinfo);
    strftime(ampm, sizeof(ampm), "%p", &timeinfo);
  } else {
    strlcpy(timeStr, "--:--", sizeof(timeStr));
    ampm[0] = '\0';
  }
  char *t = timeStr;
  while (*t == ' ') t++;   // strip %l's leading space

  // Cache on the combined string so an AM->PM flip still repaints
  char combined[12];
  snprintf(combined, sizeof(combined), "%s%s", t, ampm);
  if (!force && strcmp(combined, lastTimeStr) == 0) return;
  strlcpy(lastTimeStr, combined, sizeof(lastTimeStr));

  gfx->fillRect(0, CLOCK_TOP, 240, CLOCK_H, RGB565_BLACK);

  // The DIGITS stay centred on screen; AM/PM hangs off to the right of
  // them rather than shifting them left. Both share a baseline, which is
  // why this doesn't use drawTextCentered — that positions by top edge,
  // and two different font sizes with the same top don't line up.
  gfx->setFont(&FreeSansBold24pt7b);
  gfx->setTextSize(1);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(t, 0, 0, &x1, &y1, &w, &h);
  int digitsX  = (240 - (int)w) / 2 - x1;
  int baselineY = CLOCK_TOP - y1;      // y1 is negative: baseline -> top

  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(digitsX, baselineY);
  gfx->print(t);

  if (ampm[0]) {
    gfx->setFont(&FreeSansBold10pt7b);
    gfx->setTextColor(gfx->color565(136, 135, 128));
    gfx->setCursor(digitsX + (int)w + 6, baselineY);
    gfx->print(ampm);
  }

  gfx->setFont(NULL);
  gfx->setTextSize(1);
}

void drawDate(struct tm &timeinfo, bool force) {
  char dateStr[24];
  if (timeIsSynced(timeinfo)) {
    strftime(dateStr, sizeof(dateStr), "%A, %b %d", &timeinfo);
  } else {
    strlcpy(dateStr, "NO NTP", sizeof(dateStr));
  }

  if (!force && strcmp(dateStr, lastDateStr) == 0) return;
  strncpy(lastDateStr, dateStr, sizeof(lastDateStr) - 1);
  lastDateStr[sizeof(lastDateStr) - 1] = '\0';

  gfx->fillRect(0, DATE_TOP, 240, DATE_H, RGB565_BLACK);
  uint16_t col = timeIsSynced(timeinfo) ? gfx->color565(133, 183, 235)
                                        : gfx->color565(239, 159, 39);
  drawTextCentered(dateStr, DATE_TOP, col, 1, &FreeSansBold10pt7b);
}

// Call after anything that wipes the screen, so the next draw repaints
// instead of thinking it's already up to date.
void invalidateClockCache() {
  lastTimeStr[0] = '\0';
  lastDateStr[0] = '\0';
}

void drawWeather() {
  gfx->fillRect(0, WEATHER_TOP, 240, WEATHER_H, RGB565_BLACK);
  // Doubles as the Wi-Fi status line: with no connection there's no
  // weather AND no weather alerts, which is worth knowing about.
  if (WiFi.status() != WL_CONNECTED) {
    drawTextCentered("NO WI-FI", WEATHER_TOP, gfx->color565(226, 75, 74),
                     1, &FreeSansBold10pt7b);
    return;
  }
  drawTextCentered(weatherText, WEATHER_TOP,
                   weatherValid ? gfx->color565(239, 159, 39) : RGB565_DARKGREY,
                   1, &FreeSansBold10pt7b);
}

// The bottom band is shared: station name normally, alert banner during
// a weather alert. Only one of them is ever drawn.
void drawBottomBand() {
  if (alertActive) {
    drawAlertBanner();
    return;
  }

  gfx->fillRect(0, BOTTOM_TOP, 240, 240 - BOTTOM_TOP, RGB565_BLACK);

  if (!isPlaying) {
    drawTextCentered("PAUSED", STATION_TOP, gfx->color565(136, 135, 128), 1, &FreeSansBold10pt7b);
    drawTextCentered(stations[currentStation].name, HINT_TOP,
                     gfx->color565(95, 94, 90), 1, NULL);
  } else if (isMuted) {
    // Persistent, not a transient overlay — an accidental mute should be
    // obvious at a glance rather than looking like a broken radio.
    drawTextCentered("MUTED", STATION_TOP, gfx->color565(239, 159, 39), 1, &FreeSansBold10pt7b);
    drawTextCentered(stations[currentStation].name, HINT_TOP,
                     gfx->color565(95, 94, 90), 1, NULL);
  } else {
    drawTextCentered(stations[currentStation].name, STATION_TOP,
                     gfx->color565(93, 202, 165), 1, &FreeSansBold10pt7b);
    drawTextCentered("tap to change", HINT_TOP, gfx->color565(95, 94, 90), 1, NULL);
  }
}

// ---------------------------------------------------------------------
// VOLUME OVERLAY — temporary, shown when the volume buttons are pressed
// ---------------------------------------------------------------------

void drawVolumeOverlay() {
  int x = 24, w = 192;
  gfx->fillRect(x, VOL_OVERLAY_TOP, w, VOL_OVERLAY_H, gfx->color565(28, 28, 28));
  gfx->drawRect(x, VOL_OVERLAY_TOP, w, VOL_OVERLAY_H, gfx->color565(80, 80, 80));

  char buf[20];
  snprintf(buf, sizeof(buf), "Volume %d", volume);
  drawTextCentered(buf, VOL_OVERLAY_TOP + 8, RGB565_WHITE, 1, &FreeSansBold10pt7b);

  int barX = x + 14, barW = w - 28, barY = VOL_OVERLAY_TOP + 34, barH = 10;
  gfx->fillRect(barX, barY, barW, barH, gfx->color565(44, 44, 44));
  int fillW = (int)((float)barW * volume / 21.0f);
  if (fillW > 0) gfx->fillRect(barX, barY, fillW, barH, gfx->color565(29, 158, 117));

  volOverlayUntil = millis() + VOL_OVERLAY_MS;
}

void clearVolumeOverlay() {
  volOverlayUntil = 0;
  if (currentScreen != SCREEN_MAIN) return;
  // Repaint whatever the overlay was covering
  gfx->fillRect(24, VOL_OVERLAY_TOP, 192, VOL_OVERLAY_H, RGB565_BLACK);
  drawWeather();
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5)) {
    drawClock(timeinfo, true);   // force — the overlay wiped these bands
    drawDate(timeinfo, true);
  }
}

// ---------------------------------------------------------------------
// ALERT BANNER — occupies the bottom band, so the clock stays intact
// ---------------------------------------------------------------------

// Split text into up to maxLines lines that each fit maxWidth in the
// given font. Returns the line count. Used both by drawTextWrapped and
// by prepareAlertBannerText, which caches the result so the flashing
// banner doesn't redo this word-by-word measuring every 600ms.
int wrapText(const char *text, char out[][48], int maxLines,
             const GFXfont *font, int maxWidth) {
  char buf[160];
  strlcpy(buf, text, sizeof(buf));
  gfx->setFont(font);
  gfx->setTextSize(1);

  int line = 0;
  char *start = buf;
  while (*start && line < maxLines) {
    char *lastFit = NULL;
    char *p = start;
    while (*p) {
      while (*p && *p != ' ') p++;
      char saved = *p;
      *p = '\0';
      int16_t x1, y1; uint16_t w, h;
      gfx->getTextBounds(start, 0, 0, &x1, &y1, &w, &h);
      bool fits = ((int)w <= maxWidth);
      *p = saved;
      if (fits) lastFit = p;
      else break;
      if (*p) p++;
    }
    if (!lastFit) lastFit = p;   // single word too long — let it overflow

    char saved = *lastFit;
    *lastFit = '\0';
    strlcpy(out[line], start, 48);
    *lastFit = saved;

    start = lastFit;
    while (*start == ' ') start++;
    line++;
  }
  gfx->setFont(NULL);
  gfx->setTextSize(1);
  return line;
}

int drawTextWrapped(const char *text, int topY, int lineH, int maxLines,
                    uint16_t color, const GFXfont *font, int maxWidth) {
  char lines[4][48];
  if (maxLines > 4) maxLines = 4;
  int n = wrapText(text, lines, maxLines, font, maxWidth);
  for (int i = 0; i < n; i++) {
    drawTextCentered(lines[i], topY + i * lineH, color, 1, font);
  }
  return n;
}

// Cached, pre-wrapped banner text. Recomputed only when the alert
// changes (prepareAlertBannerText), not on every 600ms flash.
static char bannerTitle[2][48];
static int  bannerTitleLines = 0;
static char bannerSub[2][48];
static int  bannerSubLines = 0;

void prepareAlertBannerText() {
  bannerTitleLines = 0;
  bannerSubLines = 0;
  if (!alertActive) return;

  char titleUpper[40];
  strlcpy(titleUpper, alertEvent, sizeof(titleUpper));
  for (char *c = titleUpper; *c; c++) *c = toupper((unsigned char)*c);
  bannerTitleLines = wrapText(titleUpper, bannerTitle, 2,
                              &FreeSansBold12pt7b, 232);

  int subMax = (bannerTitleLines >= 2) ? 1 : 2;
  if (numActiveAlerts > 1) {
    snprintf(bannerSub[0], sizeof(bannerSub[0]), "tap for all %d alerts",
             numActiveAlerts);
    bannerSubLines = 1;
  } else {
    bannerSubLines = wrapText(alertHeadline, bannerSub, subMax,
                              &FreeSansBold10pt7b, 232);
  }
}

void drawAlertBanner() {
  uint16_t bg = alertFlashOn ? RGB565_RED : gfx->color565(80, 20, 20);
  gfx->fillRect(0, BOTTOM_TOP, 240, 240 - BOTTOM_TOP, bg);

  // Title: ALL CAPS, white, 12pt — one consistent size for every alert.
  // setTextSize() can only scale by whole numbers (it replicates
  // pixels), so a "1.5x" doesn't exist; a natively larger font is how
  // you get an in-between size. Long names like "SEVERE THUNDERSTORM
  // WARNING" don't fit on one line at any size here, so they wrap.
  for (int i = 0; i < bannerTitleLines; i++) {
    drawTextCentered(bannerTitle[i], BOTTOM_TOP + 6 + i * 20,
                     RGB565_WHITE, 1, &FreeSansBold12pt7b);
  }

  // Subtext: grey, smaller font, so the hierarchy stays clear.
  uint16_t subColor = gfx->color565(210, 190, 190);
  int subTop = BOTTOM_TOP + 6 + bannerTitleLines * 20 + 4;
  for (int i = 0; i < bannerSubLines; i++) {
    drawTextCentered(bannerSub[i], subTop + i * 17, subColor, 1,
                     &FreeSansBold10pt7b);
  }
}

void updateAlertBanner() {
  if (!alertActive) return;
  unsigned long now = millis();
  if (now - lastAlertFlash < 600) return;
  lastAlertFlash = now;
  alertFlashOn = !alertFlashOn;
  if (currentScreen == SCREEN_MAIN) {
    drawAlertBanner();
  } else if (currentScreen == SCREEN_ALERT_LIST) {
    drawAlertListFlashStrip();
  }
}

// ---------------------------------------------------------------------
// FULL-SCREEN LISTS
// ---------------------------------------------------------------------

void drawAlertListFlashStrip() {
  uint16_t bg = alertFlashOn ? RGB565_RED : RGB565_BLACK;
  gfx->fillRect(0, 0, 240, 6, bg);
}

void drawAlertListScreen() {
  gfx->fillScreen(RGB565_BLACK);
  int top = 6, bottom = 240 - 36;
  int rows = numActiveAlerts > 0 ? numActiveAlerts : 1;
  int rowH = (bottom - top) / rows;

  for (int i = 0; i < numActiveAlerts; i++) {
    int y = top + i * rowH;
    if (i > 0) gfx->drawFastHLine(0, y, 240, gfx->color565(50, 50, 50));
    char ev[40];
    strlcpy(ev, activeAlerts[i].event, sizeof(ev));
    for (char *c = ev; *c; c++) *c = toupper((unsigned char)*c);
    drawTextWrapped(ev, y + rowH / 2 - 18, 20, 1, RGB565_WHITE,
                    &FreeSansBold12pt7b, 232);
    // maxLines 1 — rows are too short to wrap, but going through the
    // wrapper still breaks at a word boundary that fits rather than
    // chopping mid-word.
    drawTextWrapped(activeAlerts[i].headline, y + rowH / 2 + 4, 18, 1,
                    gfx->color565(160, 160, 160), &FreeSansBold10pt7b, 232);
  }

  int homeY = 240 - 36;
  gfx->fillRect(0, homeY, 240, 36, gfx->color565(28, 28, 28));
  gfx->drawFastHLine(0, homeY, 240, gfx->color565(80, 80, 80));
  drawTextCentered("Home", homeY + 10, RGB565_WHITE, 1, &FreeSansBold10pt7b);

  drawAlertListFlashStrip();
}

void drawStationListScreen() {
  gfx->fillScreen(RGB565_BLACK);

  gfx->fillRect(0, 0, 240, LIST_HEADER_H, gfx->color565(24, 24, 24));
  gfx->drawFastHLine(0, LIST_HEADER_H - 1, 240, gfx->color565(60, 60, 60));
  drawTextCentered("Select station", 5, gfx->color565(136, 135, 128), 1, NULL);

  int rowH = (240 - LIST_HEADER_H) / NUM_STATIONS;
  for (int i = 0; i < NUM_STATIONS; i++) {
    int y = LIST_HEADER_H + i * rowH;
    bool active = (i == currentStation);
    if (active) gfx->fillRect(0, y, 240, rowH, gfx->color565(20, 80, 60));
    if (i > 0) gfx->drawFastHLine(0, y, 240, gfx->color565(40, 40, 40));

    gfx->setFont(&FreeSansBold10pt7b);
    gfx->setTextSize(1);
    gfx->setTextColor(active ? RGB565_WHITE : gfx->color565(200, 200, 200));
    int16_t x1, y1; uint16_t w, h;
    gfx->getTextBounds(stations[i].name, 0, 0, &x1, &y1, &w, &h);
    gfx->setCursor(14 - x1, y + (rowH - (int)h) / 2 - y1);
    gfx->print(stations[i].name);
    gfx->setFont(NULL);
  }
}

void switchToAlertListScreen() {
  currentScreen = SCREEN_ALERT_LIST;
  drawAlertListScreen();
}

void switchToStationListScreen() {
  currentScreen = SCREEN_STATION_LIST;
  drawStationListScreen();
}

void returnToMainScreen() {
  if (currentScreen == SCREEN_MAIN) return;
  currentScreen = SCREEN_MAIN;
  volOverlayUntil = 0;
  drawStaticUI();
  drawWeather();
  drawBottomBand();
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5)) {
    drawClock(timeinfo);
    drawDate(timeinfo);
  }
}


// =====================================================================
// WEATHER (Open-Meteo — free, no API key)
// =====================================================================

const char *weatherCodeToText(int code) {
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

// Runs on the network task — must not draw or touch audio.
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure(); // no cert pinning — fine for a hobby display
  // Note: ESP32's TLS client has no setBufferSizes() — that's an
  // ESP8266/BearSSL API. The TLS session here is short-lived, so the
  // allocation spike is brief, but it IS happening alongside the ~720KB
  // audio buffer. If OOM ever shows up during a fetch, this is where to
  // look.

  HTTPClient http;
  char url[220];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current_weather=true&temperature_unit=fahrenheit",
           WEATHER_LAT, WEATHER_LON);

  if (!http.begin(client, url)) {
    snprintf(pendingWeatherText, sizeof(pendingWeatherText), "Weather: unavailable");
    pendingWeatherValid = false;
    xSemaphoreTake(netMutex, portMAX_DELAY);
    weatherDirty = true;
    xSemaphoreGive(netMutex);
    return;
  }

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      float temp = doc["current_weather"]["temperature"] | 0.0;
      int code = doc["current_weather"]["weathercode"] | -1;
      snprintf(pendingWeatherText, sizeof(pendingWeatherText), "%s, %.0f F",
               weatherCodeToText(code), temp);
      pendingWeatherValid = true;
    } else {
      snprintf(pendingWeatherText, sizeof(pendingWeatherText), "Weather: parse error");
      pendingWeatherValid = false;
    }
  } else {
    snprintf(pendingWeatherText, sizeof(pendingWeatherText), "Weather: unavailable");
    pendingWeatherValid = false;
  }
  http.end();
  xSemaphoreTake(netMutex, portMAX_DELAY);
  weatherDirty = true;
  xSemaphoreGive(netMutex);
}

// =====================================================================
// WEATHER ALERTS (National Weather Service — US only, no API key)
// =====================================================================

// Runs on the network task — fetches into pendingAlerts and flags it.
// Must not draw or touch audio; applyAlertState() does that on the main
// loop once it sees the flag.
void fetchAlerts() {
#if ALERT_TEST_MODE > 0
  // Fake alerts that flip on and off each poll — see ALERT_TEST_MODE.
  static bool testOn = false;
  testOn = !testOn;
  if (xSemaphoreTake(netMutex, portMAX_DELAY) != pdTRUE) return;
  pendingNumAlerts = 0;
  if (testOn) {
    const char *events[3]   = { "Tornado Warning", "Severe T-Storm Warning",
                                "Flash Flood Warning" };
    const char *headlines[3] = { "TEST until 3:15 PM", "TEST until 3:00 PM",
                                 "TEST until 5:45 PM" };
    int n = ALERT_TEST_MODE > MAX_ALERTS ? MAX_ALERTS : ALERT_TEST_MODE;
    for (int i = 0; i < n && i < 3; i++) {
      strlcpy(pendingAlerts[i].event, events[i], sizeof(pendingAlerts[0].event));
      strlcpy(pendingAlerts[i].headline, headlines[i], sizeof(pendingAlerts[0].headline));
      pendingNumAlerts++;
    }
  }
  alertsDirty = true;
  xSemaphoreGive(netMutex);
  Serial.printf("ALERT TEST MODE: %d fake alerts\n", pendingNumAlerts);
  return;
#endif

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
  if (httpCode != HTTP_CODE_OK) { http.end(); return; }

  // Parse straight from the socket with a filter, rather than
  // http.getString() into one big String first. An NWS response for a
  // busy point can run well over 100KB, and allocating that alongside
  // the ~720KB audio buffer is the most likely remaining cause of an
  // out-of-memory crash. The filter keeps only the three fields we
  // actually read, so the JsonDocument stays tiny too.
  JsonDocument filter;
  filter["features"][0]["properties"]["severity"] = true;
  filter["features"][0]["properties"]["event"]    = true;
  filter["features"][0]["properties"]["headline"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(),
                                             DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("alert JSON parse failed: %s\n", err.c_str());
    return;   // keep the previous alert state rather than clearing it
  }

  if (xSemaphoreTake(netMutex, portMAX_DELAY) != pdTRUE) return;
  pendingNumAlerts = 0;
  JsonArray features = doc["features"].as<JsonArray>();
  for (JsonObject f : features) {
    if (pendingNumAlerts >= MAX_ALERTS) break;
    const char *severity = f["properties"]["severity"] | "Unknown";
    const char *event    = f["properties"]["event"] | "";
    const char *headline = f["properties"]["headline"] | "";
    // Only surface the alert types worth interrupting the display for.
    // NWS returns these already in priority order, so collecting them in
    // order gives us a correctly-ranked list for free.
    if (!strcmp(severity, "Extreme") || !strcmp(severity, "Severe") ||
        strstr(event, "Warning") != NULL) {
      strlcpy(pendingAlerts[pendingNumAlerts].event, event,
              sizeof(pendingAlerts[0].event));
      strlcpy(pendingAlerts[pendingNumAlerts].headline, headline,
              sizeof(pendingAlerts[0].headline));
      pendingNumAlerts++;
    }
  }
  alertsDirty = true;
  xSemaphoreGive(netMutex);
}

// Runs on the MAIN loop. Picks up whatever the network task fetched and
// applies it: station switch, volume boost, and all drawing.
void applyAlertState() {
  if (xSemaphoreTake(netMutex, portMAX_DELAY) != pdTRUE) return;
  numActiveAlerts = pendingNumAlerts;
  for (int i = 0; i < numActiveAlerts; i++) activeAlerts[i] = pendingAlerts[i];
  alertsDirty = false;
  xSemaphoreGive(netMutex);

  // Auto-return from the ALERT LIST only, so the flashing urgency cue
  // isn't missable for more than one poll interval. The station list is
  // deliberately excluded — being kicked out of it mid-browse would be
  // obnoxious.
  if (currentScreen == SCREEN_ALERT_LIST) returnToMainScreen();

  bool foundSevere = numActiveAlerts > 0;

  // Populate the banner text FIRST. The branches below trigger draws
  // (via playCurrentStation -> drawBottomBand), and if these were still
  // set from the previous state the banner would flash the wrong title
  // — or an empty one, on the very first alert.
  if (numActiveAlerts > 0) {
    strlcpy(alertEvent, activeAlerts[0].event, sizeof(alertEvent));
    strlcpy(alertHeadline, activeAlerts[0].headline, sizeof(alertHeadline));
  } else {
    alertEvent[0] = '\0';
    alertHeadline[0] = '\0';
  }

  if (foundSevere && !alertActive) {
    // New alert — auto-tune to NOAA Weather Radio, unmute, and boost to
    // max volume so the alert is actually heard.
    preAlertStationIndex = currentStation;
    stationChangedDuringAlert = false;
    preAlertVolume = isMuted ? preMuteVolume : volume;
    volumeChangedDuringAlert = false;
    isMuted = false;               // a muted radio during a warning is useless
    currentStation = NOAA_STATION_INDEX;
    alertActive = true;
    playCurrentStation();
    volume = ALERT_VOLUME;
    audio.setVolume(volume);
    alertFlashOn = true;
    lastAlertFlash = millis();
  } else if (!foundSevere && alertActive) {
    // Cleared — restore station and volume unless you overrode them.
    if (preAlertStationIndex >= 0 && !stationChangedDuringAlert) {
      currentStation = preAlertStationIndex;
    }
    if (!volumeChangedDuringAlert) {
      volume = preAlertVolume;
      audio.setVolume(volume);
    }
    alertActive = false;
    drawStaticUI();
    playCurrentStation();
    drawWeather();
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5)) { drawClock(timeinfo); drawDate(timeinfo); }
  }

  alertActive = foundSevere;
  prepareAlertBannerText();
  if (alertActive && currentScreen == SCREEN_MAIN) drawAlertBanner();
}

// =====================================================================
// NETWORK TASK — runs on the other core so blocking HTTPS fetches can't
// starve audio decoding. Only fetches; never draws, never touches audio.
// =====================================================================

void netTask(void *param) {
  unsigned long lastWeather = 0, lastAlerts = 0, lastReconnectTry = 0;
  bool radioStartedOnce = (WiFi.status() == WL_CONNECTED);
  for (;;) {
    unsigned long now = millis();

    if (WiFi.status() != WL_CONNECTED) {
      // Nudge a reconnect every 30s. setAutoReconnect usually handles
      // this, but an explicit retry covers the case where the AP was
      // unreachable at boot and never seen at all.
      if (now - lastReconnectTry >= 30000UL) {
        lastReconnectTry = now;
        Serial.println("WiFi down — retrying");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // First time we ever get a connection, make sure time and the radio
    // actually got started — they're skipped in setup() if Wi-Fi was
    // down then.
    if (!radioStartedOnce) {
      radioStartedOnce = true;
      configTzTime(TZ_STRING, "pool.ntp.org", "time.nist.gov");
      wifiJustCameUp = true;    // main loop starts playback; audio is its job
    }

    {
      if (lastAlerts == 0 || now - lastAlerts >= ALERT_CHECK_INTERVAL_MS) {
        lastAlerts = now;
        fetchAlerts();
      }
      if (lastWeather == 0 || now - lastWeather >= WEATHER_INTERVAL_MS) {
        lastWeather = now;
        fetchWeather();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// =====================================================================
// RADIO CONTROL
// =====================================================================

void playCurrentStation() {
  isPlaying = true;
  if (currentScreen == SCREEN_MAIN) drawBottomBand();
  audio.connecttohost(stations[currentStation].url);
}

void nextStation() {
  currentStation = (currentStation + 1) % NUM_STATIONS;
  if (alertActive) stationChangedDuringAlert = true;
  playCurrentStation();
  // If the list happens to be open, repaint it so the highlight follows
  // the button rather than showing a stale selection.
  if (currentScreen == SCREEN_STATION_LIST) drawStationListScreen();
}

void selectStation(int index) {
  if (index < 0 || index >= NUM_STATIONS) return;
  if (index != currentStation) {
    currentStation = index;
    if (alertActive) stationChangedDuringAlert = true;
    playCurrentStation();
  }
  // Picking from the list always returns to the main screen, even if
  // you tapped the station that was already playing.
  if (currentScreen == SCREEN_STATION_LIST) returnToMainScreen();
}

void togglePlayPause() {
  if (isPlaying) {
    audio.stopSong();
    isPlaying = false;
    if (currentScreen == SCREEN_MAIN) drawBottomBand();
  } else {
    playCurrentStation();
  }
}

// --- Volume, driven by the two physical buttons ---
void volumeUp() {
  // First press while muted just unmutes — it doesn't also step the
  // volume, so you get back exactly the level you had.
  if (isMuted) {
    isMuted = false;
    volume = preMuteVolume;
    audio.setVolume(volume);
    if (currentScreen == SCREEN_MAIN) { drawBottomBand(); drawVolumeOverlay(); }
    return;
  }
  if (volume < 21) volume++;
  audio.setVolume(volume);
  if (alertActive) volumeChangedDuringAlert = true;
  if (currentScreen == SCREEN_MAIN) drawVolumeOverlay();
}

void volumeDown() {
  // First press while muted just unmutes — see volumeUp().
  if (isMuted) {
    isMuted = false;
    volume = preMuteVolume;
    audio.setVolume(volume);
    if (currentScreen == SCREEN_MAIN) { drawBottomBand(); drawVolumeOverlay(); }
    return;
  }
  if (volume > 0) volume--;
  audio.setVolume(volume);
  if (alertActive) volumeChangedDuringAlert = true;
  if (currentScreen == SCREEN_MAIN) drawVolumeOverlay();
}

void toggleMute() {
  if (isMuted) {
    isMuted = false;
    volume = preMuteVolume;
  } else {
    isMuted = true;
    preMuteVolume = volume;
    volume = 0;
  }
  audio.setVolume(volume);
  // Deliberately does NOT set volumeChangedDuringAlert — muting isn't a
  // volume preference, so the pre-alert level should still come back
  // when the alert clears.
  if (currentScreen == SCREEN_MAIN) drawBottomBand();  // shows MUTED
}

// OneButton callbacks
void onBootClick()     { nextStation(); }
void onBootLongPress() { togglePlayPause(); }
// attachDuringLongPress gives hold-to-repeat for free
void onVolDown()       { volumeDown(); }
void onVolUp()         { volumeUp(); }

// =====================================================================
// TOUCH INPUT
// Main screen: tap the bottom band to open the station list (or the
// alert list during a multi-alert warning). Station list: tap a row.
// =====================================================================

// Set to 1 to print raw and mapped touch coordinates to Serial on every
// touch, for recalibrating if the mounting orientation ever changes.
#define TOUCH_DEBUG 0

// Touch coordinate rotation, INDEPENDENT of DISPLAY_ROTATION — the
// CST816's axes don't follow the display's, so this has to be found by
// testing. If taps land in the wrong place, try the next value; the
// four values are the four 90-degree steps, so one of them is right.

// Calibrated by tapping all four screen corners and reading the raw
// values: raw X runs 239 at the top down to 0 at the bottom, and raw Y
// runs 0 at the left to 239 at the right. So display X = rawY and
// display Y = 239 - rawX, which is the rotation-3 mapping below.
#define TOUCH_ROTATION 3

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
  // responsive for taps.
  static unsigned long lastPoll = 0;
  unsigned long nowMs = millis();
  if (nowMs - lastPoll < 20) return;

  // The CST816 pulls TOUCH_IRQ low while a finger is down, so when it's
  // high there is nothing to read and the I2C transaction can be skipped
  // entirely. That removes essentially all bus traffic while idle.
  // A slow fallback poll every 250ms covers the case where the IRQ line
  // misbehaves — without it, a bad IRQ would silently kill touch.
  static unsigned long lastFullPoll = 0;
  bool irqActive = (digitalRead(TOUCH_IRQ) == LOW);
  if (!irqActive && (nowMs - lastFullPoll < 250)) return;
  if (irqActive) lastFullPoll = nowMs;
  lastPoll = nowMs;

  int16_t x, y;
  // SensorLib's getPoint takes a third argument: how many touch points
  // to read. This board is single-touch, so 1.
  uint8_t touched = touch.getPoint(&x, &y, 1);

  // Everything is a discrete tap now, so act on the press EDGE only —
  // this both debounces and stops a held finger from firing repeatedly.
  static bool wasTouched = false;
  if (!touched) { wasTouched = false; return; }
  if (wasTouched) return;
  wasTouched = true;

  int16_t rawX = x, rawY = y;
  rotateTouch(x, y);
#if TOUCH_DEBUG
  Serial.printf("touch raw=(%3d,%3d) mapped=(%3d,%3d)\n", rawX, rawY, x, y);
#else
  (void)rawX; (void)rawY;
#endif

  switch (currentScreen) {

    case SCREEN_STATION_LIST: {
      if (y < LIST_HEADER_H) {          // tapping the header just goes back
        returnToMainScreen();
        break;
      }
      int rowH = (240 - LIST_HEADER_H) / NUM_STATIONS;
      int index = (y - LIST_HEADER_H) / rowH;
      selectStation(index);             // this returns to the main screen
      break;
    }

    case SCREEN_ALERT_LIST: {
      if (y >= 240 - 36) returnToMainScreen();   // Home button
      break;
    }

    case SCREEN_MAIN: {
      // A tap anywhere while the volume overlay is up just dismisses it
      if (volOverlayUntil != 0) {
        clearVolumeOverlay();
        break;
      }
      if (y >= BOTTOM_TOP) {
        // The bottom band is either the alert banner or the station name
        if (alertActive) {
          if (numActiveAlerts > 1) switchToAlertListScreen();
        } else {
          switchToStationListScreen();
        }
      } else {
        // Anywhere above the bottom band is a quick mute toggle. Mute
        // rather than pause, so unmuting is instant instead of needing a
        // stream reconnect.
        toggleMute();
      }
      break;
    }
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
  // Logged only — the layout no longer shows stream titles, since these
  // stations don't reliably send them.
  Serial.print("streamtitle "); Serial.println(info);
}

void audio_bitrate(const char *info) {
  // Informational only — status is driven by audio.isRunning() in loop(),
  // since this callback doesn't fire for streams without bitrate metadata.
  Serial.print("bitrate     "); Serial.println(info);
}

void audio_eof_stream(const char *info) {
  Serial.print("eof_stream  "); Serial.println(info);
  // Don't resurrect a stream the user deliberately paused.
  if (!isPlaying) return;
  // Back off on repeated failures rather than hammering a dead URL in a
  // tight loop. Resets once a stream has been up for a while.
  static unsigned long lastReconnect = 0;
  static uint8_t failCount = 0;
  unsigned long now = millis();
  if (now - lastReconnect > 60000UL) failCount = 0;   // it was stable, reset
  lastReconnect = now;
  if (failCount < 20) failCount++;
  unsigned long backoff = (failCount <= 3) ? 0 : (failCount <= 8 ? 2000 : 10000);
  if (backoff) {
    Serial.printf("stream reconnect backoff %lums (fail %u)\n", backoff, failCount);
    vTaskDelay(pdMS_TO_TICKS(backoff));
  }
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
  gfx->fillScreen(RGB565_BLACK);
  gfx->setCursor(30, 120);
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_WHITE);
  gfx->print("Connecting to Wi-Fi...");

  // --- Buttons ---
  bootButton.attachClick(onBootClick);
  bootButton.attachLongPressStart(onBootLongPress);
  // attachDuringLongPress repeats while held, so holding ramps the volume
  volDownButton.attachClick(onVolDown);
  volDownButton.attachDuringLongPress(onVolDown);
  volUpButton.attachClick(onVolUp);
  volUpButton.attachDuringLongPress(onVolUp);

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

  // The board has ONE mono speaker. Without this, a stereo stream loses
  // whatever is only on the discarded channel; forceMono sums both, so
  // nothing is lost and the result is effectively louder.
  audio.forceMono(true);

  // 3-band tone control, gains in dB (-40 to +6). Running flat — earlier
  // attempts at a speech-focused curve (-8/2/4, then -2/2/1) both came
  // out tinny, so no EQ at all. If you ever want to experiment again:
  //   (0, 1, 0)  slight presence bump
  //   (2, 1, -1) slightly warm, if flat feels thin
  audio.setTone(2, 1, -1);

  audio.setVolume(volume);

  // --- Touch controller (shares the I2C bus above) ---
  // setPins() must come before begin() — see the pin definitions above.
  pinMode(TOUCH_IRQ, INPUT_PULLUP);   // handleTouch() reads this directly
  touch.setPins(TOUCH_RST, TOUCH_IRQ);
  touchAvailable = touch.begin(Wire, CST816_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);
  if (touchAvailable) {
    Serial.println("Touch controller initialized.");
  } else {
    Serial.println("Touch controller not found. BOOT button still works normally.");
  }

  // --- Wi-Fi ---
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
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

    // --- Start radio ---
    playCurrentStation();
  } else {
    Serial.println("\nWiFi connect failed");
  }

  // --- Network task on the OTHER core ---
  // Arduino's loop() runs on core 1, so pin this to core 0. That keeps
  // blocking HTTPS fetches off the core feeding the audio decoder.
  // 12KB stack — TLS needs a lot more than the default.
  //
  // Created UNCONDITIONALLY, even if Wi-Fi didn't come up at boot. It
  // used to live inside the connected branch, which meant a router that
  // happened to be down at power-on permanently disabled weather AND
  // weather alerts for that whole session — silently. The task handles
  // reconnection itself.
  netMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(netTask, "net", 12288, NULL, 1, NULL, 0);

  drawWeather();
  drawBottomBand();
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5)) { drawClock(timeinfo); drawDate(timeinfo); }
}

void loop() {
  // Keep audio decoding responsive — call as often as possible
  audio.loop();
  bootButton.tick();
  volDownButton.tick();
  volUpButton.tick();
  handleTouch();

  unsigned long now = millis();

  // Repaint the clock once per second. Skipped on the list screens so
  // it doesn't draw over them. The date is also skipped while the volume
  // overlay is up, since the overlay covers that band.
  if (currentScreen == SCREEN_MAIN && now - lastClockDraw >= 1000) {
    lastClockDraw = now;
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5)) {
      drawClock(timeinfo);
      if (volOverlayUntil == 0) drawDate(timeinfo);
    }
  }

  // The weather band doubles as the Wi-Fi indicator, but it otherwise
  // only repaints when new weather arrives — which never happens while
  // offline. So watch for the state flipping and repaint on the edge.
  static bool lastWifiOk = true;
  static unsigned long lastWifiCheck = 0;
  if (now - lastWifiCheck >= 2000) {
    lastWifiCheck = now;
    bool ok = (WiFi.status() == WL_CONNECTED);
    if (ok != lastWifiOk) {
      lastWifiOk = ok;
      if (currentScreen == SCREEN_MAIN && volOverlayUntil == 0) drawWeather();
    }
  }

  // Hide the volume overlay once its time is up
  if (volOverlayUntil != 0 && millis() > volOverlayUntil) {
    clearVolumeOverlay();
  }

  // Wi-Fi came up after a failed boot-time connect — start the radio.
  if (wifiJustCameUp) {
    wifiJustCameUp = false;
    playCurrentStation();
  }

  // Pick up anything the network task fetched. All the drawing and
  // audio changes happen here on the main loop, never on that task.
  if (weatherDirty) {
    // Copy under the mutex — the task could be mid-write otherwise.
    if (xSemaphoreTake(netMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      strlcpy(weatherText, pendingWeatherText, sizeof(weatherText));
      weatherValid = pendingWeatherValid;
      weatherDirty = false;
      xSemaphoreGive(netMutex);
      if (currentScreen == SCREEN_MAIN && volOverlayUntil == 0) drawWeather();
    }
  }
  if (alertsDirty) {
    applyAlertState();
  }

  // Flash the alert banner while one is active
  updateAlertBanner();

  vTaskDelay(1);
}
