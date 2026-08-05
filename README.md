# ESP32-S3-Touch-LCD-1.54 — Internet Radio + Clock/Weather

A single-sketch project for the Waveshare ESP32-S3-Touch-LCD-1.54-EN that:

- Plays a hard-coded list of internet radio stations over the onboard
  ES8311 codec + speaker, including NOAA Weather Radio as a regular
  selectable station
- Shows a large clock (12-hour with AM/PM), the date, and current
  weather conditions from the free Open-Meteo API (no key needed)
- Polls the National Weather Service for severe weather alerts, and
  when one fires: **auto-tunes to NOAA Weather Radio, unmutes, and
  boosts to max volume**, then restores everything afterward
- Runs network fetches on a separate CPU core so they can't interrupt
  audio

## Controls

| Control | Action |
|---|---|
| **BOOT** button (GPIO 0) | Click = next station · Long-press = play/pause |
| **Button GPIO 5** | Volume down (hold to repeat) |
| **Button GPIO 4** | Volume up (hold to repeat) |
| **Tap upper screen** | Toggle mute |
| **Tap bottom band** | Open the station list (or the alert list, during a multi-alert warning) |

Volume changes pop a temporary overlay that fades after 2 seconds.
Mute shows a persistent **MUTED** indicator where the station name
normally sits — deliberately persistent, so an accidental tap can't
leave you thinking the radio is broken.

## Screen layout

The main screen is a single vertical stack, all full width:

```
  clock (24pt, no seconds, AM/PM to the right)
  date            <- also shows "NO NTP" if time hasn't synced
  weather         <- also shows "NO WI-FI" if disconnected
  ----- divider -----
  station name    <- tap to open the station list
  "tap to change"
```

There are two other full-screen views: the **station list** (tap a row
to select and return; tap the header to back out) and the **alert
list** (tap Home to return).

---

## 1. Before you flash

Edit the top of `esp32_radio.ino`:

- `WIFI_SSID` / `WIFI_PASSWORD` — your network credentials. If a
  `secrets.h` file is present next to the sketch, its values are used
  instead; that's how the GitHub Actions build injects credentials
  without committing them.
- `NWS_USER_AGENT` — NWS requires a descriptive User-Agent with real
  contact info on every request, e.g.
  `"(esp32-radio-clock, yourname@example.com)"`. They may block
  generic or browser-like strings.
- `WEATHER_LAT` / `WEATHER_LON` — defaults to Mukwonago, WI. Used for
  **both** the weather forecast and the alert query.
- `TZ_STRING` — defaults to US Central with automatic DST
  (`CST6CDT,M3.2.0,M11.1.0`).
- `DEFAULT_VOLUME` — 14 out of 21.
- `stations[]` — six entries: News/Talk 1130 WISN (boots to this),
  The Big 920, FM 106.1, 99.1 The Mix, 95.7 The Big FM, and NOAA
  Weather Radio. Each has a full `name` and a stream `url`. Look for
  direct `.mp3`/`.aac` stream links — not webpages — via
  [Radio Browser](https://www.radio-browser.info/).

  The struct also has a `shortName` field, which is currently **unused**
  — it labelled the old horizontal button row that the station-list
  screen replaced. Harmless, but it can go if you're tidying up.

  **NOAA Weather Radio must stay the LAST entry** —
  `NOAA_STATION_INDEX` is computed as `NUM_STATIONS - 1`.

## 2. Weather alerts

Polls `api.weather.gov/alerts/active` every 2 minutes for your
lat/lon point. US only, no API key.

**What triggers it:** severity `Extreme` or `Severe`, or any event
with "Warning" in the name — Tornado, Severe Thunderstorm, Flash
Flood, Winter Storm, Red Flag, and so on. Watches, Advisories, and
Outlooks are deliberately filtered out so the display isn't flashing
constantly. Note the query isn't weather-restricted, so non-weather
civil alerts (AMBER, Civil Danger) can also trigger it if they carry
matching severity or wording.

**Precision:** the query is point-based, not county-based. Short-fused
warnings are issued as storm-tracked polygons often smaller than a
county, so you get warnings for where the device actually sits.

**What happens on an alert:**

- The bottom band becomes a flashing red banner with the alert title
  in ALL CAPS (12pt, wrapping if long) and the headline below in grey.
  **The clock stays fully intact** — the banner takes the station area,
  not the clock area.
- The radio switches to the NOAA Weather Radio entry in `stations[]`,
  unmutes if muted, and jumps to `ALERT_VOLUME` (21/21).
- When NWS marks it expired, the previous station and volume come back.

**Manual override:** if you change station or volume during an alert,
your choice sticks and won't be reverted when it clears. It won't
re-tune or re-boost unless a *new* alert starts. Muting is exempt —
mute isn't treated as a volume preference, so the pre-alert level
still returns.

### Multiple simultaneous alerts

Up to `MAX_ALERTS` (5) are collected per poll. NWS returns them in
priority order, so the banner always shows the most urgent. When more
than one is active the banner reads "tap for all N alerts" and tapping
it opens a full-screen list.

On that list screen: a **thin red strip keeps flashing at the top** so
the urgency cue never disappears, and if you don't tap **Home** it
**auto-returns on the next alert poll** (up to 2 minutes). The station
list is deliberately *not* subject to that auto-return.

### Testing alerts

Set `ALERT_TEST_MODE` near the top of the sketch:

- `0` — off
- `1` — one fake alert
- `3` — three fake alerts (also exercises the list screen)

In test mode no network call is made and the fake alerts **toggle on
and off each poll**, so you can watch both the alert firing and it
clearing without waiting for real weather. Set back to `0` when done.

### NOAA Weather Radio coverage

The default URL is the Milwaukee feed
(`radio.weatherusa.net/NWR/KEC60.mp3`). NOAA Weather Radio is analog
broadcast — these internet streams are community rebroadcasts, so
coverage is patchy and there's no directory mapping transmitters to
stream URLs. For a different area, search "[your city] NOAA weather
radio stream" or check
[radio.weatherusa.net](http://radio.weatherusa.net/).

## 3. Display and touch rotation

`DISPLAY_ROTATION` is `1`, for a device physically turned **90°
counter-clockwise** (USB-C ends up on the right edge). Turning the
device one way makes content appear rotated that way, so the content
rotates the opposite way to cancel it out: `0` = native/USB bottom,
`1` = device turned 90° CCW, `2` = 180°, `3` = 90° CW.

The panel is square, so rotating costs nothing — every layout
coordinate stays valid.

**`TOUCH_ROTATION` is separate and cannot be derived from
`DISPLAY_ROTATION`** — the CST816's axes don't follow the display's.
It's set to `3`, calibrated by tapping all four corners: raw X runs
239 at the top to 0 at the bottom, raw Y runs 0 at left to 239 at
right, so display X = rawY and display Y = 239 − rawX.

If you remount the device, set `TOUCH_DEBUG` to `1` to print raw and
mapped coordinates on every touch, tap the corners, and pick whichever
of the four `TOUCH_ROTATION` values lines up.

## 4. Libraries and files

| Library | Where |
|---|---|
| GFX_Library_for_Arduino | Waveshare's [example package](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54) |
| ESP32-audioI2S-master | Same package |
| SensorLib (CST816 touch) | Same package |
| OneButton | Same package |
| ArduinoJson (v7) | Library Manager |

**Where they live in that repo:**

```
ESP32-S3-Touch-LCD-1.54-main/
  examples/
    ESP32-S3-Touch-LCD-1.54-demo/     <- the Touch version
      Arduino-3.2.0/
        libraries/                     <- copy the folders INSIDE this
        examples/01_i2s_audio/         <- es8311 files here
```

`FreeSansBold10pt7b.h` lives under
`libraries/GFX_Library_for_Arduino/examples/HelloWorldGfxfont/`.

Copy the individual library folders (not the `libraries` folder
itself) into your Arduino `libraries` folder. `U8g2`, `arduinoVNC`,
and `lvgl` are in there too but aren't used.

**Files that go in the sketch folder, not libraries:**

```
esp32_radio/
  esp32_radio.ino
  es8311.h              <- from examples/01_i2s_audio/
  es8311.cpp            <- same
  es8311_reg.h          <- same
  FreeSansBold10pt7b.h  <- body text; from HelloWorldGfxfont example
  FreeSansBold12pt7b.h  <- alert titles; from Adafruit's GFX library
  FreeSansBold24pt7b.h  <- clock; from Adafruit's GFX library
```

The ES8311 driver isn't a library — it's three loose files, which is
why the sketch includes it with quotes. The two larger fonts come from
[Adafruit's GFX library](https://github.com/adafruit/Adafruit-GFX-Library)
`Fonts/` folder, with their `#include <Adafruit_GFX.h>` line swapped
for the portable guard block Waveshare's copy uses.

The folder name must match the `.ino` filename.

## 5. Board settings

- Board: **ESP32S3 Dev Module**
- Board package: esp32 by Espressif Systems, **3.3.11**
- Flash Size: **16MB**
- Partition Scheme: **16M Flash (3MB APP/9.9MB FATFS)** —
  `app3M_fat9M_16MB`
- **PSRAM: OPI PSRAM** — required. The audio library allocates a
  ~720KB buffer that won't fit without it; the symptom is
  `OOM: failed to allocate 720896 bytes for AudioBuffer` and silence.
- USB CDC On Boot: **Enabled**

Note the core version: Waveshare's docs say 3.2.0, but the
ESP32-audioI2S version they bundle calls `dsps_biquad_sf32`, which
only exists in the newer esp-dsp shipped with 3.3.x. 3.2.0 will not
compile.

## 6. Building via GitHub Actions

The repo includes `.github/workflows/build.yml`, which compiles on
GitHub's runners and produces a single flashable binary. Useful if
your machine's security software quarantines the Arduino toolchain
(CrowdStrike and similar EDR treat the compiler's rapid file churn as
ransomware-like).

1. Push the repo with `libraries/` and the sketch folder committed
2. Add repository secrets: `WIFI_SSID`, `WIFI_PASSWORD`,
   `NWS_USER_AGENT` — the workflow writes them into a `secrets.h` on
   the runner, and `.gitignore` blocks that file as a backstop
3. Run the workflow, download the `esp32-radio-firmware` artifact
4. Flash `esp32_radio-merged.bin` at offset **0x0** using
   [esptool-js](https://espressif.github.io/esptool-js/) in Chrome or
   Edge — no local install needed

The merged binary already contains bootloader, partition table, and
app, so it's one file at one offset.

## 7. Pin reference

All verified against Waveshare's own example code.

```
LCD (ST7789, SPI):   DC=45 CS=21 SCK=38 MOSI=39 RST=40 Backlight=46
Audio (ES8311/I2S):  PA_CTRL=7  MCLK=8  BCLK=9  LRC=10  DOUT=12
I2C (codec + touch): SDA=42  SCL=41
Touch (CST816):      RST=47  IRQ=48   (address from CST816_SLAVE_ADDRESS)
Buttons:             BOOT=0  VOL_DOWN=5  VOL_UP=4
```

**The touch reset and interrupt pins must be set via `touch.setPins()`
before `touch.begin()`** — otherwise the controller stays held in
reset and never answers on I2C at all. `setup()` also runs an I2C bus
scan and prints every device found, which is the fastest way to
diagnose a missing device (expect the ES8311 at 0x18, the QMI8658 IMU,
and the CST816 at 0x15).

## 8. Audio tuning

Three separate gain stages, worth not confusing:

- `audio.setVolume()` — 0–21, the digital volume the buttons control
- `EXAMPLE_VOICE_VOLUME` (80) — the ES8311's own analog output level,
  set once at boot. Waveshare uses 75; 90 was audibly harsh on this
  small speaker.
- `audio.setTone(2, 1, -1)` — 3-band EQ in dB, currently slightly
  warm. Speech-focused curves that cut bass and lifted treble
  (`-8,2,4` and `-2,2,1`) both came out tinny.

`audio.forceMono(true)` is on: the board has one speaker, and without
it a stereo stream loses whatever sits only on the discarded channel.

`EXAMPLE_SAMPLE_RATE` is 48000 to match the audio library's fixed
output rate. Waveshare's example uses 16000 because it plays a local
voice file — **if audio ever sounds wrong, that's the first constant
to revert.**

## 9. Architecture notes

- **Network runs on core 0.** Arduino's `loop()` runs on core 1;
  `netTask` is pinned to core 0 so blocking HTTPS fetches can't starve
  the audio decoder. That task only fetches — it never draws and never
  touches audio. It writes to "pending" buffers, sets a dirty flag, and
  the main loop picks it up under a mutex.
- **The task is created unconditionally**, even if Wi-Fi is down at
  boot, and handles its own reconnection. It used to live inside the
  connected branch, which silently disabled weather *and alerts* for
  the whole session if the router happened to be down at power-on.
- **Fixed `char` buffers, not `String`**, for anything stored
  long-term. Repeated `String` churn fragments the heap, and on a
  device meant to run for weeks that eventually kills a large
  allocation.
- **Alert JSON is parsed straight off the socket with an ArduinoJson
  filter**, not buffered via `getString()`. NWS responses can exceed
  100KB and that allocation sits next to the 720KB audio buffer.
- **Touch reads are gated on the IRQ pin** — no I2C traffic at all
  while nobody's touching, with a 250ms fallback poll so a misbehaving
  IRQ line can't silently kill touch.
- **Only changed regions redraw.** The clock and date cache their last
  drawn string and skip the repaint if it matches, which is what
  stopped them flickering once seconds were removed. The alert banner
  caches its wrapped text so the 600ms flash doesn't re-measure it.

## 10. Reliability

- Dropped streams reconnect on `audio_eof_stream`, with backoff after
  repeated failures (none for the first 3, then 2s, then 10s, resetting
  after a minute of stable playback). A deliberately paused stream is
  not resurrected.
- Wi-Fi reconnects automatically, retried every 30 seconds, with
  **NO WI-FI** shown on the weather line.
- **NO NTP** appears on the date line and the clock shows `--:--`
  until time syncs, rather than confidently displaying a 1970 time.
- Weather refreshes every 15 minutes; alerts every 2.
- The iHeart `stream.revma.ihrhls.com` URLs are a reverse-engineered
  pattern, not officially documented. They've been stable for years but
  could change without notice.

## 11. Possible next steps

- **Persist last station and volume** to flash (Preferences) so they
  survive a power cycle
- **Weather icons** instead of the plain text line
- **Backlight dimming or a schedule** for overnight
- **A digital pet / fish tank mode** using the touchscreen and IMU
- **OTA updates** so reflashing doesn't need the USB cable
