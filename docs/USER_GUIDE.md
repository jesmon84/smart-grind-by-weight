# Everyday Use

This guide covers the touchscreen, local web interface and normal grinder
workflow after installation and calibration. New installations should begin at
the [documentation home](DOC.md).

**On this page:** [Manual grinding](#manual-grinding) ·
[Profiles](#grinding-profiles) · [Navigation](#navigation) ·
[Grind settings](#grind-settings) · [Automation](#automated-grind-flow) ·
[Bluetooth](#bluetooth-connectivity) · [Screensaver](#screensaver)

## Usage guide

### Manual Grinding

**Manual** is the first page in the main-screen carousel. Tap **START** to run
the grinder without a weight or time target, then tap **STOP** when the desired
amount has been ground. When a healthy load cell is fitted, the page shows a
large live weight and a large **TARE** button. Put the empty cup in place and
tap **TARE** if you want to weigh the result; taring is never automatic in
Manual mode. During grinding, the screen shows live weight and elapsed motor
time.

Manual start/stop works without a load cell, and while the scale is still
starting; the scale readout shows **STARTING** or reports that the scale is
unavailable, and the TARE button stays disabled. Manual mode deliberately skips
automatic taring, purging, predictive stopping, finishing pulses and
grind-history recording. It still adds the real motor run time to lifetime
statistics and stops automatically after 30 seconds. Use Single, Double or
Custom when you want a repeatable target and a recorded result.

### Grinding Profiles
All profiles are fully customizable. Default grind-by-weight targets (fallback time values shown in parenthesis):
- **Single**: 9 g (5 s)
- **Double**: 18 g (10 s)
- **Custom**: 21.5 g (12 s)

> 💡 **Tip** – the target label always shows the active unit (`g` or `s`). Long-press to edit in whichever mode you are currently using.

### Navigation
- **Swipe left/right** to navigate between menu tabs
- **Swipe up/down** on the ready screen to toggle between grind-by-weight and grind-by-time modes (when enabled in Menu → Grind Settings)
- **Tap** to select profiles or buttons
- **Long press** on profile targets to edit/customize them
- **After a restart** the grinder returns to the profile you last stayed on for a couple of seconds or ground with. Profiles you only swipe across on the way to the Wi-Fi or menu page do not replace it

> **Color cues:** The GRIND button background turns **red** in weight mode and **blue** in time mode, so you always know which behaviour is armed.

### Grind Settings
Access **Menu → Grind Settings** to configure:
- **Swipe Gestures**: Enable/disable vertical swipe gestures for mode switching (default: disabled)
- **Time Mode**: Directly toggle between Weight and Time modes regardless of swipe setting
- **Grind Screen**: Choose the layout every grind starts with: Arc (default), Chart or Circle
- **Hold**: Keep the final weight on the finished screen after the cup is lifted (default: on). Turn it off to show the live scale reading instead
- **Show for**: How long the finished screen stays after you lift the cup before returning to Ready: 15 s, 30 s, 1 min (default), 2 min or 5 min. While the cup is still on the scale the finished screen stays up, for at most 30 minutes. An error screen always stays for 1 minute
- **Start on Cup**: Start the active profile automatically once a cup or portafilter at least the configured cup threshold (50 g by default) heavier than the empty scale has come to rest on it
- **Return on Removal**: Leave the completion screen as soon as that cup weight drops back off the scale
- **Motor Latency** *(Advanced)*: View or manually adjust the minimum reliable
  motor pulse from 30–300 ms in 5 ms steps. Use **Menu → Tune Pulses** first;
  this manual control is a fallback when automatic tuning cannot complete.
- **Purging** *(Advanced)*: Control how the grinder saturates itself before weight-mode grinding
  - **Prime mode**: Keeps the coffee used to saturate the grinder, continues immediately
  - **Purge mode** (default): Prompts you to discard stale grinds before continuing
  - **Amount slider**: Configure purge/prime amount (0.1g-5.0g, default 1.0g). Amount is a minimum target; actual output will be slightly higher.
  - **"Keep purge grinds from now on" checkbox**: Appears during purge confirmation - switches to Prime mode when checked

  *Explanation:* The grinder stops its motor a little before the target, by what will still land afterwards (the coast). It learns that from every grind: what landed after the motor stopped, as a time, which it multiplies by the flow of the grind in progress. So the main run ends just under the target and one short pulse finishes it, and new beans that grind faster or slower are allowed for at once. Until a first grind has shown the coast, it is estimated from the time between motor start and grinds hitting the cup, scaled by **Coast → Ratio**. Purging clears stale coffee and saturates the grinder with fresh grounds, so what lands is the coffee just ground. If you prefer to keep all coffee without manual intervention, select Prime mode.

### Basic Operation

After power-on the scale takes about seven seconds to start and zero itself,
and two seconds more for its first readings to settle. Until then the profile
pages show a spinner with **SCALE STARTING**, the GRIND
button is grey and does nothing, and Start on Cup stays off. Keep the scale
clear while it starts: anything on it becomes part of the zero. If the scale
has not started after 20 seconds, the pages show **SCALE UNAVAILABLE** in weight
mode; time-mode grinds still work without the scale.

These steps describe the default grind-by-weight workflow:
1. Select profile by tapping on the main screen
2. Long press the profile target to edit/customize the weight if needed
3. Place the dosing cup on the scale platform
4. Press the GRIND button – the scale will tare automatically
5. The system grinds to the precise target weight using the predictive algorithm
6. GRIND COMPLETE shows the final settled weight in grams (with statistics), which stays on screen after you lift the cup
7. Under the weight, a timer counts up from the moment the grind finished, so you can see how long the grounds have been waiting. It stops when you lift the cup. Until then the finished screen stays up and the display stays bright, for at most 30 minutes. In time mode, a PULSE restarts the timer

If the portafilter is lifted off the scale during a grind, the grind stops and the screen shows **STOPPED** with "Portafilter removed". This works in weight, time and manual modes and on every grind: the grinder keeps track of how heavy the portafilter is, even though each weight grind zeroes the scale with the portafilter on. If you switch the grinder on with the portafilter already on the scale, the check starts once the portafilter has been lifted off for a second. A time grind that you have paused can be lifted, for example to distribute the grounds; the check starts again when you resume.

The scale weighs best on a bench that stands still. A weight grind waits for the scale to read still before it tares, before each correction and before it reports the result. If something shakes the bench, such as a 3D printer on the same surface, the scale never reads still and the grind can wait until its 60-second limit. Keep your hands off the portafilter while a grind runs for the same reason. A weight grind that has not reached its target after 60 seconds stops with **TIMEOUT** and "Target not reached", for example when the hopper is empty, or with "Could not tare" when the scale never came to rest for its zero.

For a grinder that has to stand on a vibrating bench, switch on **Menu → Grind Settings → Shaky Bench → Enabled**: each wait then ends after a few seconds, and a grind that finds its bench shaking works with the average of its readings, so it takes longer but still ends on target. It is off as shipped and applies from the next grind. With it on, keep your hands off the portafilter while it grinds: a hand does not average out and can be weighed as coffee. See [Troubleshooting](TROUBLESHOOTING.md#shaky-bench-handling) for what it does.

> Optional automation (Menu → Grind Settings): enable auto-start and set the cup threshold below the empty cup or portafilter weight. The grinder remembers the weight the scale last rested at and starts as soon as the cup has come to rest at least that much heavier, however long positioning it takes. Auto-return then jumps back to Ready whenever that cup is lifted off again.

Need the stock timed run? Enable swipe gestures in **Menu → Grind Settings**, then swipe up or down on the ready screen before you start; the GRIND button background turns blue to confirm time mode is active (red = weight). Alternatively, use the direct **Time Mode** toggle in the menu.

> **Time mode pulse button:** In time mode completion, a "+" button appears next to OK for 100ms additional grinding pulses.

From the local web dashboard, choose Single, Double or Custom and use the round
play/stop button to request the same target grind remotely. The request passes
through the firmware's normal checks and state machine; Manual mode remains an
on-device control.

The dashboard is updated ten times a second during a grind and twice a second
otherwise. A browser tab that is hidden or in the background pauses its
connection and reconnects when you return to it, so a page left open does not
keep the grinder's Wi-Fi busy.

### Quick Scale View
Need a simple live readout? Open **Menu → Scale** to jump into a full-screen weight display. Entering the page automatically tares the scale (using the same blocking overlay as the main workflow), and a large `TARE` button at the bottom lets you re-zero manually whenever you need.

### Display Modes
- **Arc Layout**: Clean, minimal arc-based interface
- **Chart Layout**: Detailed charts showing flow rates and real-time grinding analytics
- **Circle Layout**: In the style of the Mahlkönig E64, a light circle around the weight grows from the middle as the grind progresses and covers the display at the target. When the grind finishes, the whole display turns green (amber if the grind failed)
- **Switching**: Choose the layout in **Menu → Grind Settings → Grind Screen**; every grind starts with it. Tapping anywhere on the grind screen cycles Arc → Chart → Circle for that grind only
- **Screensaver**: A custom or built-in design can show on startup or when the
  display dims. An optional later panel-off stage protects the AMOLED during
  long idle periods.

---

## User interface navigation

```
Main Screen (swipe left/right between tabs, up/down to toggle weight/time mode if enabled)
|
+-- Manual
|   |-- Live elapsed motor time
|   \-- START / STOP button (30s safety limit)
|
+-- Single Profile
|   |-- Weight display (long press to edit)
|   \-- GRIND button (red=weight, blue=time)
|
+-- Double Profile
|   |-- Weight display (long press to edit)
|   \-- GRIND button (red=weight, blue=time)
|
+-- Custom Profile
|   |-- Weight display (long press to edit)
|   \-- GRIND button (red=weight, blue=time)
|   \-- Time mode completion: OK + PULSE buttons
|
\-- Menu (scrollable hub)
    |
    +-- Tools (quick actions)
    |   |-- Scale (live weight view with Tare action)
    |   |-- Calibrate (launch calibration workflow)
    |   |-- Tune Pulses (auto-tune motor latency)
    |   \-- Motor Test (1s safety pulse)
    |
    +-- Settings
    |   +-- Bluetooth
    |   |   |-- Bluetooth toggle (30m timer)
    |   |   |-- Bluetooth startup toggle (configurable auto-enable)
    |   |   |-- Connection status display
    |   |   \-- Auto-disable timer display
    |   |
    |   +-- Wi-Fi
    |   |   |-- Enabled toggle (Wi-Fi on or off)
    |   |   |-- Pause toggle (Wi-Fi off while a grind runs)
    |   |   |-- Connection status, address and setup QR code
    |   |   \-- Firmware update status and install button
    |   |
	    |   +-- Display
	    |   |   |-- Normal brightness slider
	    |   |   |-- Screensaver brightness slider
	    |   |   |-- Screensaver startup/idle toggles
	    |   |   \-- Turn off toggle (display off after a long idle time)
    |   |
    |   \-- Grind Settings
    |       |-- Swipe Gestures toggle (enable/disable vertical swipes)
    |       |-- Time Mode toggle (direct weight/time mode selection)
    |       |-- Grind Screen (Arc/Chart/Circle radio buttons)
    |       |-- Hold toggle (keep the final weight after the cup is lifted)
    |       |-- Show for slider (finished screen time, 15 s-5 min)
    |       |-- Start on Cup toggle and configurable cup threshold
    |       |-- Return on Removal toggle (drop back to Ready when that weight leaves)
    |       |-- Purging (Prime/Purge radio buttons)
    |       |-- Amount slider (0.1g-5.0g for purge/prime operation)
    |       |-- Latency slider (30-300ms manual Pulse Tune fallback)
    |       |-- Coast ratio slider (only until a first grind has taught the coast)
    |       \-- Shaky Bench toggle (for a bench that vibrates; off as shipped)
    |
    \-- Info
        +-- Diagnostics
        |   |-- Load Cell Status (calibration flag, calibration factor)
        |   |-- Noise Floor (std dev g/ADC, noise level indicator)
        |   |-- Active diagnostic warnings
        |   \-- Clear Warnings button
        |
        +-- System Info
        |   |-- Firmware version & build number
        |   |-- Real-time weight sensor data (instant, samples, raw)
        |   |-- Uptime display
        |   \-- Memory usage
        |
        +-- Logs & Data
        |   |-- Logging toggle (enable/disable session file writing)
        |   |-- Sessions / Events / Measurements counters
        |   |-- Purge Logs button
        |   \-- Factory Reset button
        |
        \-- Lifetime Stats
            |-- Refresh Stats button
            |-- Total grinds, shots, weight
            |-- Motor runtime, uptime, accuracy
            \-- Pulse counts

During Grinding:
|-- Weight/elapsed display & progress
|-- Tap anywhere: cycle Arc → Chart → Circle layouts for this grind
|-- STOP button
\-- Purge Confirmation (appears in Purge mode after grinder saturation)
    |-- "Grinder Purged" title
    |-- Instruction message
    |-- "Keep purge grinds from now on" checkbox
    \-- CONTINUE button
```

---

## Automated grind flow

Want the scale to run itself? Enable the automation toggles in **Menu → Grind Settings**:

- **Start on Cup**: As soon as a cup or portafilter adds at least the configured threshold, the active profile tares and begins grinding automatically. The default is 50 g; set it safely below the empty accessory weight but above incidental touches or vibration. The scale only needs to come to rest within about a gram for a second, so there is no time limit on positioning the cup, and sensor noise or a knock merely delays the start. The grinder watches for the cup only while the Ready screen shows a profile; a cup placed while you are in the menu or on another tab counts as already there, so lift it off and set it down again.
- **Return on Removal**: When the cup weight drops away after completion, the grinder exits the results screen and returns to Ready. Useful for keeping the workflow hands-free between shots.

Both automation settings work from the averaged scale reading, so no extra calibration is required. Leave them disabled if you prefer manual control or experience false triggers with lighter accessories.

---

## Bluetooth connectivity

Bluetooth can be configured in **Menu → Bluetooth** with optional auto-startup (5-minute timer) or manual control (30-minute timer when manually enabled). The blue Bluetooth symbol in the top-right corner indicates when active. The status icons appear on the main screen and while grinding; menus, editors and pop-ups hide them so their titles stay readable. Bluetooth enables wireless firmware updates via BLE OTA, legacy grind-data export and device management. Grind session logging is configurable in **Menu → Data → Logging** and is enabled by default so the local web History page works immediately; disable it if you do not want sessions written to flash.

---

## Screensaver

Open `http://smartgrind.local` and choose **Settings → Display & screensaver**.
Select the built-in Minimal, Orbit or Black AMOLED design, the live GaggiMate
status view, or upload a normal photo; the browser crops and converts custom
images to the display's 280 × 456 RGB565 format before sending them to the
grinder.

- **Custom image**: Upload a normal photo from the web interface; conversion
  happens in the browser before it is sent to the grinder.
- **GaggiMate status**: Enter the machine hostname (normally
  `gaggimate.local`) or its IP address. While both devices are on the same
  Wi-Fi network, the screensaver shows temperature and readiness, then live
  shot time, phase, pressure and flow during an extraction. It uses
  GaggiMate's existing WebSocket API and falls back to its compact HTTP status
  endpoint, so no modified GaggiMate firmware is required.
- **Timing settings**: Configure when the screensaver starts and its startup
  duration in the local web app. You can optionally turn the panel fully off
  after a second delay; this is disabled by default, so the current
  always-visible screensaver behaviour is preserved after updating.
- **Device settings**: Brightness, the **Startup** and **Idle** screensaver
  toggles and the **Turn off** toggle remain available under
  **Menu → Display** and are synchronized with the web settings. The web app
  controls the additional off delay.
- **Wake behaviour**: Touching the dark panel, changing the scale load or
  starting a grind wakes it. The first wake touch is consumed so it cannot
  accidentally press the control underneath. Lift your finger after waking
  the panel, then use the next touch normally.
- **Black screen (V2 boards)**: A sagging supply can leave the AMOLED panel
  black while the grinder itself keeps running. Switching the grinder off
  and on brings it back. The firmware follows the panel's frame signal and
  shows under **Settings → System & updates** on the web page when the panel
  has stopped. It resends the panel's setup while the display is on, and
  with **Menu → Display → Turn off** enabled it sets the panel up from scratch each
  time the display wakes. See
  [Troubleshooting](TROUBLESHOOTING.md#display-goes-black-while-the-grinder-keeps-working)
  for the cause and what to do about it.
- **Startup behavior**: On normal Ready boots, the image is drawn early while the full UI initializes, then the regular timed screensaver overlay takes over.
- **Wi-Fi and the power supply**: Wi-Fi is the board's largest load, and the
  grinder's own supply has little to spare for it. **Menu → Wi-Fi → Pause**
  switches Wi-Fi off from the start of a grind until about 15 seconds after
  it; the web page and Home Assistant lose the grinder for that time and
  find it again by themselves. It is off as shipped and switches itself on
  after a grinder has restarted on a brownout. **Enabled** switches Wi-Fi off
  altogether until you switch it on again for the web page, Home Assistant
  or an update. See
  [Troubleshooting](TROUBLESHOOTING.md#display-goes-black-while-the-grinder-keeps-working).
- **Firmware updates**: While firmware is uploaded, from the web page, over Bluetooth or from the grinder's own menu, the display shows the update's progress at about a third of its normal brightness, to spare the grinder's power supply. During a Bluetooth upload Wi-Fi is paused and the processor runs at a third of its speed while the data arrives. The grinder restarts when the upload is done. An update failure warning keeps the display on.

---

**Previous:** [Firmware and first setup](FIRMWARE_SETUP.md) ·
**Next:** [Diagnostics and data →](DIAGNOSTICS_AND_DATA.md)
