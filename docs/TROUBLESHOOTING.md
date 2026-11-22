# Troubleshooting Guide

## Table of Contents

- [Motor Does Not Start](#motor-does-not-start)
- [Display Stays Black After Flashing (Waveshare 1.64 V2)](#display-stays-black-after-flashing-waveshare-164-v2)
- [Display Goes Black While the Grinder Keeps Working](#display-goes-black-while-the-grinder-keeps-working)
- [Wi-Fi Setup Does Not Connect](#wi-fi-setup-does-not-connect)
- [HX711 Not Detected / Wrong Sample Rate](#hx711-not-detected--wrong-sample-rate)
- [Suspected HX711 or Load Cell Damage](#suspected-hx711-or-load-cell-damage)
- [Unknown board ID 'esp32-s3-devkitc-1'](#unknown-board-id-esp32-s3-devkitc-1)
- [PlatformIO Project Initialization Issues](#platformio-project-initialization-issues)
- [Unexpected Net Weight Error](#unexpected-net-weight-error)
- [Grind Timeout Screen](#grind-timeout-screen)
- [Unreliable Pulse Corrections](#unreliable-pulse-corrections)
- [Getting Diagnostic Reports](#getting-diagnostic-reports)

---

## Display Stays Black After Flashing (Waveshare 1.64 V2)

**Applies to:** ESP32-S3-Touch-AMOLED-1.64 boards that still accept firmware and produce serial/BLE diagnostics, but show no pixels after Smart Grind starts.

### Symptoms
- The display worked with the factory firmware, then became completely black after flashing Smart Grind.
- The device still boots, connects, or produces a diagnostic report.
- Reflashing the official V2 demo restores the display.

### Root Cause
Waveshare's V2 revision is not display-compatible with the original board. It uses an SH8601 panel controller, GPIO 46 for display chip select, a 40 MHz QSPI bus, and a 20-pixel X offset. The original V1 target uses a different display path and can boot successfully without lighting a V2 panel.

### Confirm the Revision
Do **not** identify this display generation from the PCB revision text alone. A physically verified newer SH8601 board is silkscreened `Rev1.1`, despite requiring what this project calls the V2 firmware and wiring. The V1/V2 names in this project distinguish incompatible display generations, not a reliable marking printed on the PCB.

Flash Waveshare's [official V2 demo](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.64-v2). If that works but the original CO5300 demo or normal V1 Smart Grind image remains black, treat the board as the newer SH8601 generation and use the V2 target. Do this test before connecting the grinder wiring so display compatibility is isolated from the rest of the installation.

### Resolution
Build and flash the V2 target:

```bash
python3 tools/venv/bin/python -m platformio run --target upload -e waveshare-esp32s3-touch-amoled-164-v2
```

For a complete V2 installation, wire HX711 SCK to GPIO 1 and the grinder motor-control lead to GPIO 16. GPIO 18 is connected to the V2 touchscreen interrupt (`TP_INT`) and must not be used for motor control. The V1 connections remain HX711 SCK on GPIO 2 and motor control on GPIO 18.

Do not change only the chip-select pin in a V1 build. Hardware validation showed that an Arduino_GFX SH8601 attempt remained black; the working V2 target uses Waveshare's native Espressif `esp_lcd` SH8601 driver and initialization sequence.

If using the Web Flasher, select **Newer SH8601 (V2 firmware; may say Rev1.1)** for this generation. Do not select the original CO5300 image merely because the PCB says `Rev1.1`.

---

## Display Goes Black While the Grinder Keeps Working

**Applies to:** V2 boards whose display worked after flashing.

### Symptoms
- The screen goes black during or just after a grind, or while the web page is in use.
- The grinder carries on: a running grind finishes and the web page still answers.
- Or the grinder restarts in the middle of a grind, and the web page then shows `BROWNOUT` as the reason for the last restart.
- Switching the grinder off and on brings the screen back. A restart without cutting power does not.

### Cause
The AMOLED panel needs at least 2.7 V on its supply, and the ESP32 keeps running at a lower voltage than that. When the supply sags, the panel therefore goes dark while everything else carries on. Where it sags far enough to restart the ESP32 as well, the web page shows `BROWNOUT` as the reason for the last restart.

The Eureka control board feeds the Waveshare board from a small 5 V regulator (78L05A) with less than 100 mA to spare (see the [power board notes](eureka-specialita-reverse-engineering.md)). The board draws about that with Wi-Fi on, and more while the radio transmits, while a web page is open and above all during a firmware upload. The 1000 µF capacitor covers short transmissions only. A weak Wi-Fi signal makes them long: the radio sends slowly, at full power, and repeats what was not received.

A panel that has gone dark this way does not come back by itself. In the cases examined, a reset of the panel and a restart of the grinder both left it dark; switching the grinder off and on brought it back.

### What the Firmware Does
The firmware cannot give the panel voltage. It notices the fault, reports it, and brings back a panel that has only lost its setup.
- **Follows the panel's frame signal**, so it knows when the panel has stopped. **Settings → System & updates** on the web page shows the state of the signal and counts the faults.
- **Resends the panel's setup** every three seconds and when the screen is touched. A healthy panel shows no change.
- **Resets the panel while the display is on** when its frame signal has stopped and the resend did not bring it back. This blanks the screen for about a quarter of a second, and waits until nobody has touched the screen for a second and a half, because the same line resets the touch controller. A screen that shows a picture is never reset as a precaution.
- **Sets the panel up afresh each time the display wakes from off.** Switching the display off puts the panel to sleep and resets it, and waking sets it up from scratch, as at power-on. None of this shows on the screen. It needs **Menu → Display → Turn off** enabled (or the same setting on the web page), which it is not by default.
- **Resets the touch controller's bus** if it stops answering.
- **Pauses Wi-Fi while grinding**, if **Menu → Wi-Fi → Pause** is on. The radio is off from the start of a grind until about 15 seconds after it. The switch turns itself on once a grinder has restarted on a brownout, until you choose otherwise.
- **Records what was running at a brownout restart.** **Last restart** on the web page shows it: whether a grind was running, whether the motor was on and what Wi-Fi was doing.
- **Draws less power when it can**: the display sleeps during a firmware upload, the processor runs at a third of its speed while the display is dimmed or off, the online update check waits for a quiet moment, and an idle grinder sends an open web page far fewer messages.

### If the Screen Is Black Now
Switch the grinder off, wait ten seconds, and switch it on again.

**Reset display** under **Settings → System & updates** on the web page (`http://smartgrind.local`) resets the panel without a power cycle. It helps a panel that has lost its setup, not one whose supply is too low. It is refused while the display is switched off after a long idle time.

Do not tap a black screen to find out whether it responds: the controls under your finger still work.

### If It Happens Often
1. Look at **Settings → System & updates**. **Last restart** shows the reason for the last restart and how many restarts so far were brownouts. **Display** shows the state of the frame signal, how often the panel was reset while the display was on and how often the frame signal was lost. **Connectivity → Signal** shows the Wi-Fi signal. Brownout restarts, or a frame signal that is lost, point at the supply.
2. Switch **Menu → Wi-Fi → Pause** on if the grinder restarts or goes black during grinds. If that is not enough, switch **Enabled** off there and only switch Wi-Fi on when you want the web page or an update: without Wi-Fi the board draws about what the original firmware does.
3. Measure the voltage between the board's 5 V and ground pins while the grinder is on and a web page is open. Clearly under 4.75 V means the regulator is overloaded. The lasting cure is a 5 V supply that can deliver 500 mA or more.
4. Check that the 1000 µF capacitor from the [installation guide](HARDWARE_INSTALLATION.md) is fitted across 5 V and ground, close to the board.
5. Improve the Wi-Fi signal at the grinder if it is weaker than about -75 dBm. That shortens every transmission.
6. Close web pages you are not using. A page in a hidden tab already pauses its connection.
7. Enable **Menu → Display → Turn off** with a short extra delay. The panel is then asleep, and draws next to nothing, whenever the grinder is idle.
8. Expect a firmware upload to be the heaviest load of all. If the screen is black after one, switch the grinder off and on.

---

## Motor Does Not Start

**Applies to:** Eureka Mignon installations where the grinder motor does not activate when commanded.

### Symptoms
- Motor test function (Menu → Motor Test) does not start the motor
- GRIND button does not start the motor
- All other functionality works normally (screen, weight readings, etc.)

**Note:** If you hear a click and humming sound but no coffee grounds come out, the motor **is** starting correctly - your grinder is clogged, not a wiring issue.

### Root Cause
The motor control wire (Pin 3) and button signal wire (Pin 2) from the Eureka 4-pin plug may be reversed on the Waveshare board connection. The button signal wire does not control the motor.

### Diagnosis

**Option 1: Use Motor Test Function (Recommended)**

Access **Menu → Motor Test** (Tools section) to verify motor connectivity. This button activates the motor for a short test pulse. If the motor does not run during the test, the motor wire is incorrectly connected.

**Option 2: Manual Wire Identification (Advanced - Use with Extreme Caution)**

⚠️ **DANGER:** This test involves live wires and can cause the grinder to start unexpectedly, potentially causing injury or damage. Only attempt if comfortable working with electrical systems.

Using the 4-pin Eureka plug pinout (see `../media/4-pin_Eureka_plug_pinout.png` in documentation):
- **Pin 1**: 5V power supply
- **Pin 2**: Button signal (unused in this project)
- **Pin 3**: Motor control signal *(active-high — motor engages when driven to 3.3-5V)*
- **Pin 4**: Ground

The motor control wire (Pin 3) is the wire that **starts the motor when briefly driven HIGH (touch it to Pin 1 / 5V)**. Test carefully:
1. Ensure grinder is powered and hopper has minimal beans
2. Disconnect the suspect wire from the Waveshare board
3. Briefly touch the wire to 5V (Pin 1) - motor should start
4. **Be prepared for the powerful motor to cause the grinder to twist/jump**
5. **Avoid shorts that could damage the board or cause injury**

### Resolution

**Swap the wire connections:**
- The motor control wire (Pin 3) should connect to Waveshare **GPIO 18 on V1** or **GPIO 16 on V2**
- The button signal wire (Pin 2) can remain disconnected (unused in this project)

If wires were reversed, swap them so Pin 3 connects to the correct GPIO for the board revision. On V2, never use GPIO 18 for motor control because it is shared with `TP_INT`. The Waveshare board has reverse polarity protection for power connections, but the motor/button wires should be correctly identified for proper operation.

**Important:** Wire colors vary significantly between Eureka units - always refer to pin positions rather than wire colors when troubleshooting. On the physically verified V2 Specialita, Pin 3 was the grey lead and Pin 2 was white, contrary to the earlier assumed assignment; the white Pin 2 lead was left disconnected and insulated.

---

## HX711 Not Detected / Wrong Sample Rate

**Applies to:** First-boot issues when the load cell board is missing, miswired, or strapped for 80 SPS.

### Symptoms
- Startup log shows `HX711_NOT_CONNECTED` or `HX711_SAMPLE_RATE_INVALID`.
- Weight display stays at `0.00 g`; grinding and calibration remain disabled.

### Quick Fixes
- **NOT_CONNECTED:** Verify VCC/GND/SCK/DOUT wiring and that the HX711 board is powered.
- **SAMPLE_RATE_INVALID:** Ensure the HX711 `RATE` pin is tied to GND for 10 SPS; a floating/high pin forces 80 SPS and will now block startup.
- After correcting hardware, reboot the scale. The diagnostic clears automatically when healthy samples are detected.

---

## Suspected HX711 or Load Cell Damage

**Applies to:** A reversed connector, short circuit, overheated HX711 module, or
a scale that stopped responding immediately after a wiring mistake.

> [!CAUTION]
> Disconnect the grinder from mains power before opening it. Remove USB power
> and any battery connection before moving wires or measuring resistance. Do
> not reconnect an HX711 module that became unusually hot; a damaged module can
> place another fault on the ESP32's 3.3 V rail.

### What the symptoms establish

- An HX711 IC that became too hot to touch after a reversed connection should
  be treated as failed and replaced, even if it later appears to cool normally.
- A four-wire load cell is a passive Wheatstone bridge and will often survive a
  fault on the ESP32-to-HX711 cable, but this is not guaranteed.
- An ESP32 that still boots, runs the display and controls the motor is broadly
  functional, but those tests do not prove that its HX711 GPIO pins survived.
- `HX711_NOT_CONNECTED` means the firmware received no valid samples. It cannot
  distinguish a failed HX711, broken load cell, damaged GPIO or bad wiring by
  itself.

### Isolate the components

1. **Remove the suspect HX711.** Photograph and label every connection first,
   then disconnect both the ESP32 cable and load cell. Check for a cracked IC,
   discolouration, lifted tracks or a burnt smell. Do not use this module for
   further testing if it overheated.
2. **Check the disconnected load cell with a multimeter.** Use its datasheet or
   the documented wiring labels rather than relying only on wire colours.
   Measure resistance across the excitation pair (`E+` to `E-`) and signal pair
   (`A+` to `A-`). Both readings should be finite, stable and normally in the
   hundreds of ohms. An open circuit or near-short indicates a damaged cable or
   bridge. The two values need not be identical because load cells can contain
   compensation resistors.
3. **Check insulation.** With the load cell still disconnected, there should be
   no low-resistance continuity from any of its four bridge wires to the metal
   body or cable shield. The shield itself is excluded from this test.
4. **Fit a known-good replacement HX711.** Verify all four ESP32 connections
   before applying power: V1 uses GPIO 2 for SCK and GPIO 3 for DOUT; V2 uses
   GPIO 1 for SCK and GPIO 3 for DOUT. VCC is 3.3 V and GND is GND. Perform the
   first test with the grinder disconnected from mains and the motor-control
   lead disconnected and insulated; USB power is sufficient to test the scale.
5. **Interpret the result.** If valid readings return, recalibrate and the ESP32
   and load cell have passed the practical test. If the new HX711 is still not
   detected, recheck the cable and pin mapping, then test with a known-good load
   cell or ESP32 board to isolate the remaining component. Do not keep swapping
   parts onto a module that overheats or pulls down the 3.3 V rail.

The normal wiring map is in
[Hardware and installation](HARDWARE_INSTALLATION.md#installation-and-wiring).
After installing the replacement, reboot and check **Menu → Diagnostics** or a
[diagnostic report](#getting-diagnostic-reports). A healthy 10 SPS HX711 clears
the hardware fault automatically; calibration is still required before weight
mode can grind.

### Temporary operation without a load cell

The maintained firmware can still operate the grinder while replacement scale
parts are in transit. Install the image that matches the display generation—in
particular, an original 1.64-inch V1/CO5300 board must use the **V1** image—then
use either:

- **Manual mode** for target-free start/stop operation with its independent
  30-second safety cutoff; or
- **Time mode** for a repeatable timed dose.

These modes control the motor through the normal guarded grind controller and
do not require a working HX711. Weight mode, live weight, tare, calibration,
start-on-cup and weight-based stopping remain unavailable until the scale is
repaired. The matching V1/V2 images are available from the
[Web Flasher](https://s31teg.github.io/smart-grind-by-weight/).

The resistance checks above follow standard Wheatstone-bridge fault isolation:
compare excitation and signal resistance with the load-cell specification and
check insulation between the bridge, shield and body. See Analog Devices'
[bridge measurement overview](https://www.analog.com/en/resources/reference-designs/circuits-from-the-lab/cn0600.html)
and the [HX711 datasheet](https://datasheet.lcsc.com/lcsc/2011051703_Avia-Semicon-Xiamen-HX711_C43656.pdf)
for the underlying circuit and supply limits.

## Unknown board ID 'esp32-s3-devkitc-1'

**Applies to:** Windows development environments using PlatformIO with ESP32-S3 boards.

### Symptoms
- `platformio run` fails with `UnknownBoard: Unknown board ID 'esp32-s3-devkitc-1'`.
- `pio boards espressif32 | findstr "esp32-s3"` shows the board, suggesting it exists in the registry.

### Root Cause
The local PlatformIO `espressif32` platform package was pinned to an older release (e.g. 3.1.0 from 2021) that predates ESP32-S3 board definitions. The registry lookup reports current boards, but the outdated local package lacks the corresponding JSON definition.

### Resolution
1. Update the global `espressif32` platform package so it includes ESP32-S3 board definitions:
   ```powershell
   C:\Users\<user>\.platformio\penv\Scripts\platformio.exe pkg install -g -p platformio/espressif32@^6.8.0
   ```
   *(Alternatively, run `pio platform update espressif32`.)*
2. Re-run the build:
   ```powershell
   C:\Users\<user>\.platformio\penv\Scripts\platformio.exe run
   ```
3. Optional: Pin the desired platform version in `platformio.ini` to ensure consistency across machines:
   ```ini
   [env:waveshare-esp32s3-touch-amoled-164]
   platform = platformio/espressif32@^6.8.0
   ```

### Notes
- `monitor_options` is not a supported PlatformIO setting. Current firmware uses
  the project-local `monitor/filter_hiding_filter.py` through `monitor_filters`
  instead; remove any old local override that still defines `monitor_options`.
- If multiple PlatformIO installations are present, ensure you are updating the instance used for the project.

---

## PlatformIO Project Initialization Issues

**Applies to:** General PlatformIO project setup issues, especially when using PlatformIO (as this project uses the pioarduino fork as a platform).

### Symptoms
- `Error: Could not find one of 'package.json' manifest files in the package`
- PlatformIO fails to initialize the project properly
- Vague platform-related errors during project setup

### Root Cause
The use of the pioarduino platform fork can sometimes cause PlatformIO's cache or project state to become inconsistent, leading to initialization failures.

### Resolution
1. **Close VS Code completely**
2. **Delete the PlatformIO cache folder:**
   ```bash
   # On Windows
   rmdir /s "%USERPROFILE%\.platformio"
   
   # On macOS/Linux  
   rm -rf ~/.platformio
   ```
3. **Restart VS Code**
4. **Reopen the project** - PlatformIO will reinitialize and download the correct platform packages
5. **Perform a clean build** - Use PlatformIO's "Clean" then "Build" to ensure a fresh compilation

### Alternative (Less Nuclear)
If you want to try a less aggressive approach first:
1. Close VS Code
2. Delete only the platforms cache: `~/.platformio/platforms/` (or `%USERPROFILE%\.platformio\platforms\` on Windows)
3. Restart VS Code and reopen project
4. Perform a clean build in PlatformIO

**Note:** This issue is specific to the pioarduino platform fork usage and the way PlatformIO handles custom platform URLs.

---

## Unexpected Net Weight Error

**Applies to:** A grind stopping with **STOPPED** and "Portafilter removed"
(older firmware: "Err: neg wt") even though the cup or portafilter is still in
place.

The firmware works out the vessel weight as the grind starts. It only treats a
drop as removal when the reading falls by about 90% of that vessel weight and
stays there across several samples, so pushing a portafilter up in its holder
or a brief negative spike does not stop the grind.

If the error still occurs, download the diagnostic log from the grinder web UI
and include the vessel reference, removal threshold and reported weight in a
GitHub issue.

### Portafilter lifted off, but the grind carried on

The check needs to know what the cup or portafilter weighs. On a scale that
was zeroed empty, that is what the scale reads as the grind starts. Every
weight grind zeroes the scale with the portafilter on, though, so the
firmware carries the weight forward from one grind's zero to the next. Where
neither tells it, it falls back on the level the scale rests at when empty.

That leaves one case without a weight: **the grinder was switched on with the
portafilter on the scale**, and it has not been lifted off since. Lift it off
for a second; every grind after that is covered. If a tray or holder that
normally sits on the scale was off at the time, also switch the grinder off
and on once with the scale as you normally leave it.

The diagnostic log shows what was used for each grind: `[SCALE] Vessel
reference:` with the weight, where it came from and the removal threshold, or
`[SCALE] No vessel reference` when the check was off.

---

## Grind Timeout Screen

**Applies to:** A grind stopping after 60 seconds with **TIMEOUT** and "Target
not reached" (older firmware: "Timeout:" followed by four letters such as
"PULS" or "PRED").

### Symptoms
- The grind stops with the timeout screen before the target weight is reached
- Grinds take much longer than usual
- Long taring process, unstable weight readings

### Root Cause
A weight grind has 60 seconds to reach its target. It runs out of time when
no coffee, or too little, reaches the scale: an empty hopper, beans bridging
above the burrs, or a target too large for the grinder's flow rate.

A grind also runs out of time when the scale never reads still. It waits for
a still reading before it tares, before each correction and before it reports
the result, so it can hang with the coffee already in the basket (older
firmware: "Timeout:PULS"). A bench that vibrates, for example with a 3D
printer on it, a hand on the portafilter or a noisy load cell causes this.
The diagnostic log then has `[SCALE] Not still within ... waiting for a still
reading`, and the screen says "Could not tare" when the wait was for the
zero.

### Shaky-bench handling

For a grinder that has to stand on a vibrating bench, build the firmware with
shaky-bench handling on:

```bash
PLATFORMIO_BUILD_FLAGS="-DGRIND_SHAKY_BENCH_DEFAULT=1" python3 tools/grinder.py build --hardware v2
```

The diagnostic log then has `[SCALE] Shaky-bench handling is on` at the start
of every grind, and:

- The waits after taring, after the prime and after the motor stops end after
  a few seconds (`carrying on` in the log).
- A grind takes its bench for a shaking one when the scale came to rest
  neither after taring nor soon after the prime. Its corrections and its
  result then use the average of the last two seconds of readings once they
  have waited 2.5 seconds (`using the 2.0s average`). Such a grind finishes,
  only more slowly.
- An average is used only while the readings look like one weight being
  shaken. Otherwise the log has `no steady average to use, waiting`: a hand
  was on the portafilter, grounds were still falling, or the bench shakes
  harder than the firmware allows for. For the last case, raise
  `GRIND_UNSTEADY_SCATTER_LIMIT_G` in `src/config/grind_control.h`.
- A grind that did not take its bench for a shaking one waits for a still
  reading as it would without the handling (`waiting for a still reading`).

It is off as shipped because a hand on the portafilter also keeps the scale
from reading still, and unlike vibration it does not average out. With the
handling on, keep your hands off the portafilter until the result is on the
screen: a steady hand can be weighed as coffee.

### Diagnosis
First check that coffee is coming out: beans in the hopper and nothing
bridging above the burrs.

For slow grinds, check **Menu → Diagnostics → "Noise Floor"** (see
[Diagnostics System](FIRMWARE_SETUP.md#diagnostics-system) for details). If
"Noise level: Too High" (red text) appears persistently, your load cell has
sustained noise issues that will cause slow taring (>2 seconds). A warning icon
(⚠) will appear in the top-right corner when sustained noise is detected.

### Resolution
0. **Keep vibration away from the scale:** move the grinder off a surface it
   shares with a 3D printer or another machine that shakes, or stand either
   one on a damping mat. Where that cannot be done, see
   [Shaky-bench handling](#shaky-bench-handling).

1. **Check load cell wiring:**
   - Verify shielding wire connection per wiring diagram
   - Shorten load cell cables if possible
   - Ensure clean, solid connections

2. **Check calibration factor (reference examples only):**

   Calibration factors vary between individual load cells, but extreme deviations may indicate hardware issues. Check your calibration factor in **Menu → Diagnostics → Load Cell Status**. Example values from tested units:
   - **1KG T70 load cell**: ±4400 (example)
   - **0.3KG Mavin Load Cell**: ±6580 (example)

   **Note:** These are examples only and can differ significantly between load cells. Use them as rough reference points, not expected values.

3. **If wiring issues persist:**
   - Increase `GRIND_SCALE_SETTLING_TOLERANCE_G` parameter for more noise tolerance

---

## Unreliable Pulse Corrections

**Applies to:** Pulse corrections failing to produce grounds consistently, requiring multiple correction cycles.

### Symptoms
- Multiple pulse attempts needed to reach target weight
- Inconsistent grounds production during pulse phase
- Overshooting or undershooting target weight frequently

### Root Cause
Motor response latency mismatch between firmware settings and actual hardware characteristics. Default 50ms may not match your specific grinder's relay type (solid-state vs mechanical), voltage (110V vs 220V), or burr inertia.

### Resolution

**Recommended:** Use the Auto-Tune Motor Response feature to automatically calibrate optimal pulse duration for your hardware:

1. **Access auto-tune**: Menu → Tune Pulses (Tools section)
2. **Prepare system**:
   - Ensure beans are in hopper
   - Place dosing cup on scale
   - System will automatically tare
3. **Run calibration**: Process takes 1-2 minutes
   - Priming phase: 500ms pulse to position beans
   - Binary search: Finds minimum reliable pulse duration
   - Verification: Confirms 80%+ success rate across 5 test pulses
4. **Check result**: New motor latency value displayed on completion
   - Typical range: 30-200ms depending on hardware
   - Value saved automatically to device preferences
   - Displayed in **Menu → Diagnostics**

**Verify calibration**: Check **Menu → Diagnostics → Motor Latency** to see current value. Re-run auto-tune if you change grinders, modify relay hardware, or continue experiencing unreliable pulse corrections.

**Manual fallback:** If auto-tune fails, system reverts to safe 50ms default. Check grinder power connection, hopper bean level, and scale setup.

---

## Getting Diagnostic Reports

**Applies to:** Reporting issues on GitHub or troubleshooting system behavior.

### What is a Diagnostic Report?

The diagnostic report is a comprehensive text dump of your device's current state, including:
- Firmware version, build number, and git commit
- System information (uptime, CPU frequency, RAM, flash storage)
- All compile-time constants (grind settings, thresholds, timeouts)
- Runtime statistics (lifetime grinds, total weight)
- Load cell calibration status and noise diagnostics
- Motor latency settings
- Autotune results (if available)

This information helps identify configuration issues, hardware problems, or firmware bugs.

### How to Get a Diagnostic Report

#### Option 1: Web Flasher (Recommended)

1. Visit the **[Web Flasher Diagnostics Tool](https://s31teg.github.io/smart-grind-by-weight/)**
2. Click the **"Diagnostics"** tab
3. Click **"Connect & Get Diagnostics"**
4. Select your device from the Bluetooth pairing dialog
5. Wait for the report to complete (~5-10 seconds)
6. Click **"Copy to Clipboard"** or **"Download Report"**
7. Paste the report into your GitHub issue

**Browser Requirements:** Chrome, Edge, or Opera on desktop/Android (Web Bluetooth API required)

#### Option 2: Command Line Tool

If you have the development environment set up:

```bash
# Get diagnostics and display in terminal
python3 tools/grinder.py diagnostics

# Save diagnostics to a file
python3 tools/grinder.py diagnostics --save diagnostics.txt
```

### When to Include a Diagnostic Report

**Always include a diagnostic report when reporting:**
- Grinding accuracy issues (overshooting/undershooting)
- Timeout errors or slow taring
- Load cell calibration problems
- Motor response issues
- Any unexpected behavior or crashes
- Feature requests that depend on your hardware configuration

**The diagnostic report is anonymous** and contains no personal information. It only includes technical device settings and statistics.

### Including Grind Results in Your Report

**For grind-related issues, follow these steps before generating the diagnostic report:**

1. **Enable logging:** Menu → Data → Enable Logging
2. **Reproduce the issue:** Perform at least one grind where the issue occurs
3. **Generate diagnostics:** Use one of the methods above (Web Flasher or Command Line)

The diagnostic report will automatically include your recent grind session data, which is essential for troubleshooting accuracy issues, pulse corrections, timeout problems, and other grind behavior issues. Without recent grind data, it's much harder to diagnose what's going wrong.

---

## Wi-Fi Setup Does Not Connect

**Applies to:** the grinder brings its `SmartGrind-xxxxxx` setup network back about 15 seconds after you save your Wi-Fi details.

### Read the reason

After a failed attempt the grinder says why. The full explanation is on the sign-in page (rejoin the `SmartGrind-xxxxxx` network) and on **Menu → Wi-Fi**; the ready screen's Wi-Fi tab shows a one-line summary.

| Message | What to check |
|---|---|
| Network not found | The name is exact, including capitals. The network has 2.4 GHz switched on, because the grinder cannot use 5 GHz. The grinder is within range. |
| Password rejected | The password. |
| Signal too weak | Move the grinder or the access point closer together. |
| Connection refused | Usually a weak signal. Also check router features that turn away weak or unknown devices, such as UniFi's **Minimum RSSI** or MAC filtering. |
| Security type not supported | Use WPA2 or WPA3 Personal security. Enterprise and WEP networks are not supported. |
| No response | As for "Network not found". |

### Hidden networks

Hidden networks never appear in the scanned list. Open **Hidden network or manual entry** on the sign-in page and type the name exactly.

### Signal strength

The scanned list shows each network's signal in dBm. Around -70 dBm or stronger (closer to zero) is reliable; weaker than about -80 dBm, joining can fail or the connection can drop. A metal grinder body weakens the signal considerably, so test with the grinder near the access point if a network is missing or weak.
