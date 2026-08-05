# Devlog — ESP32 Radio

Running record of decisions, dead ends, and hard-won findings. The
README says how the thing works; this says *why*, and what already got
tried and rejected.

---

## Gotchas that cost real time

These are the ones worth re-reading before touching anything.

**LCD pins are 45/21/38/39/40/46, not what you'd guess.** The first
build had audio working and a black screen for a while because the
display pins were wrong. Audio and I2C pins happened to be right, which
made it look like a display-init problem rather than a pinout one.
Always check Waveshare's actual example source
(`04_gfx_helloworld` for LCD, `01_i2s_audio` for audio) rather than
assuming.

**Core 3.2.0 will not compile, despite Waveshare's docs.** Their
bundled ESP32-audioI2S (3.4.7) calls `dsps_biquad_sf32`, which only
exists in the esp-dsp shipped with core 3.3.x. Their own library
folder is even named `Arduino-3.2.0`. Using 3.3.11.

**PSRAM must be enabled in the FQBN.** Without `PSRAM=opi` the audio
library can't allocate its ~720KB buffer — the symptom is
`OOM: failed to allocate 720896 bytes for AudioBuffer` and total
silence, with everything else working normally.

**The touch controller is held in reset until you set its pins.** It
never appeared in the I2C scan at all until `touch.setPins(47, 48)` was
called *before* `touch.begin()`. Easy to misread as a wrong I2C
address, which is exactly what happened for a while.

**Touch rotation does not follow display rotation.** The CST816's axes
are independent of the panel's. No amount of reasoning about rotation
conventions solved this; tapping four corners and reading the raw
values did, in about ten minutes. `TOUCH_ROTATION` is deliberately a
separate 0–3 knob for this reason.

**`setBufferSizes()` doesn't exist on ESP32's TLS client.** That's an
ESP8266/BearSSL API. There's no direct way to shrink the TLS buffer
here; the fix for memory pressure was streaming the JSON with a filter
instead.

**Removing seconds from the clock caused flicker.** The 1Hz redraw
stayed, so both the clock and date were erasing and repainting ~60x and
~86,400x more often than their content actually changed. Fixed with
change-detection caching, not by slowing the loop.

---

## Decisions and rationale

**Mute rather than pause for quick silencing.** Pause tears down the
stream and resuming costs several seconds of reconnect; mute is instant
both ways. Mute shows a *persistent* indicator (not a transient
overlay) specifically so an accidental tap can't look like a broken
radio.

**Alert banner at the bottom, not the top.** It originally replaced the
clock, which meant losing the time for the entire duration of a
warning. Moving it to the station band keeps the clock intact and gives
the banner more room. The station name is redundant during an alert
anyway, since it always auto-tunes to NOAA.

**Auto-tune the whole station rather than ducking volume.** An earlier
design lowered the radio volume during an alert so you'd "hear it
over" the stream — but there was nothing to hear, since the alert audio
lives on NOAA Weather Radio, not on whatever was playing.

**Alert list auto-returns; station list does not.** The alert list gets
bounced back on the next poll so you can't sit on a calm screen during
an ongoing warning. Applying the same rule to the station list was a
bug — it kicked you out mid-browse every 2 minutes.

**Manual overrides during an alert stick.** Change station or volume
mid-alert and it won't be reverted when the alert clears. Muting is
exempt from this, since mute isn't a volume *preference*.

**Network on core 0.** Blocking HTTPS fetches were starving the audio
decoder on core 1. The task only fetches — never draws, never touches
audio — and hands off through pending buffers plus a mutex.

**The network task is created unconditionally.** It used to be created
only inside the "Wi-Fi connected" branch, so a router that was down at
power-on silently disabled weather *and weather alerts* for the entire
session. That's the worst class of bug for this project, since the
alert feature is the point.

**Fixed `char` buffers instead of `String`.** Repeated `String` churn
fragments the heap; on a device meant to run for weeks that eventually
kills a large allocation. Shows up as "it crashed after 11 days."

**Clock has no seconds.** That's what allows the 24pt font. A wall
clock doesn't need seconds, and the larger digits were worth more.

---

## Font notes

- Scaling a bitmap font with `setTextSize(n)` replicates each pixel
  n×n, so a scaled font has 1/n the effective resolution and looks
  blocky. Always prefer a natively-sized font at size 1.
- `setTextSize()` is integer-only — there is no 1.5×. In-between sizes
  come from a different font file (hence 10pt for body, 12pt for alert
  titles, 24pt for the clock).
- Custom GFX fonts position by **baseline**, not top-left. That's why
  `drawTextCentered()` uses `getTextBounds` to compute the offset, and
  why the clock's AM/PM is drawn manually — two different font sizes
  sharing a top edge don't line up along the bottom.
- Even at 1×, "SEVERE THUNDERSTORM WARNING" is ~243px against 232
  available. Alert titles wrap rather than scale down.

---

## Audio tuning history

Three attempts, in order:

1. `setTone(-8, 2, 4)` — cut bass hard, boosted treble, aiming for
   speech intelligibility. **Sounded tinny.** In hindsight a 12dB tilt
   toward the high end is close to the definition of tinny.
2. `setTone(-2, 2, 1)` — gentler version of the same idea. Still not
   right.
3. `setTone(2, 1, -1)` — slightly *warm*. This is what stuck.

Also: `EXAMPLE_VOICE_VOLUME` went 75 → 90 → 80. At 90 the small speaker
was being pushed hard enough to add harshness that no EQ could fix.

`forceMono(true)` is a genuine win, unrelated to EQ — the board has one
speaker, and without it a stereo stream loses whatever sits only on the
discarded channel.

`EXAMPLE_SAMPLE_RATE` changed 16000 → 48000 to match the library's
fixed output. Waveshare uses 16000 because their example plays a local
voice file. **This is the first thing to revert if audio ever sounds
wrong.**

---

## Build toolchain

Building locally on the work laptop failed: **CrowdStrike quarantined
Arduino toolchain files mid-compile** (EDR reads the compiler's rapid
file create/write/delete as ransomware-like). OneDrive also caused
"permission denied" errors on library headers before that.

Tried and rejected:

- **Arduino Cloud Editor** — compiles server-side, which would have
  sidestepped it, but it's pinned to core 2.x and the GFX library needs
  3.x (`esp32-hal-periman.h` missing). No way to choose the core
  version.
- **Flashing from a phone** — Android apps do this over USB OTG, but
  they only flash pre-compiled `.bin` files. Doesn't solve compiling.

Settled on: **GitHub Actions compiles, esptool-js flashes from the
browser.** Nothing installs locally, the core version is pinned
explicitly, and credentials come from repo secrets rather than the
committed sketch.

---

## Things deliberately not done

- **SAME code decoding** from the NOAA stream. Technically possible —
  the tones survive MP3 encoding and `multimon-ng` does it on desktop —
  but it's real-time DSP competing for CPU with audio decoding, and it
  wouldn't give better alert coverage than the `api.weather.gov` polling
  already does. The Si4707 chip does this in hardware if it ever
  matters.
- **Dynamic NOAA station selection by location.** The transmitter list
  is public, but there's no directory mapping transmitters to internet
  stream URLs — those are community rebroadcasts with patchy coverage.
  Only a hand-curated table would work.
- **IMU-based auto-rotation.** Works only when the screen is roughly
  vertical; flat on a desk gives no orientation signal at all. Not
  worth it for a device that sits in one place.
- **LVGL.** Would give properly anti-aliased fonts, but it's an
  architectural rewrite of all the drawing code.

---

## Open items

- `shortName` in the `Station` struct is dead code — it labelled the
  old horizontal button row.
- Alert list rows are capped at one line of headline; with 3+ alerts
  the rows are too short to wrap, so long titles get cut at a word
  boundary. The banner shows the full text.
- The mute tap zone is everything above y=166 — a large accidental-tap
  target, mitigated by the persistent MUTED indicator.
- iHeart `stream.revma.ihrhls.com` URLs are a reverse-engineered
  pattern, not officially documented. Stable for years, but could break
  without notice.
- Not yet built: persisting station/volume across reboots, weather
  icons, overnight backlight dimming, OTA updates, fish-tank pet mode.
