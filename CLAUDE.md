## Project Overview

ESP32-S3 intelligent coffee scale with grind-by-weight functionality. Features predictive grinding system, LVGL touch UI, and BLE OTA updates. Automatically grinds coffee beans to precise target weights using flow prediction and pulse correction algorithms.

## Essential Commands

The canonical checkout is `/home/cmossom/src/smart-grind-by-weight` on the
native WSL2 ext4 filesystem in `Ubuntu-24.04`. Do not build this project from a
Windows checkout, OneDrive, `/mnt/c`, or another Windows-mounted path. From
PowerShell, enter the canonical checkout with:

```powershell
wsl.exe -d Ubuntu-24.04 --cd /home/cmossom/src/smart-grind-by-weight
```

All development tasks use the unified cross-platform Python tool:

```bash
# Build and upload
python3 tools/grinder.py build-upload

# Data analysis (exports data and launches Streamlit report)  
python3 tools/grinder.py analyze
```

**Common Commands:**
- `tools/venv/bin/python3 tools/grinder.py build --hardware v1 --jobs 8` - Build V1 firmware
- `tools/venv/bin/python3 tools/grinder.py build --hardware v2 --jobs 8` - Build V2 firmware
- `python3 tools/grinder.py upload` - Upload latest firmware via BLE
- `python3 tools/grinder.py export` - Export grind data to database
- `python3 tools/grinder.py report` - Launch Streamlit report from existing data
- `python3 tools/grinder.py scan` - Scan for BLE devices
- `python3 tools/grinder.py info` - Get device system information
- `python3 tools/grinder.py clean` - Clean build artifacts

## Architecture

**4-Layer Architecture:**
1. **Hardware Layer** (`src/hardware/`): ESP32-S3 peripheral abstraction
2. **Control Layer** (`src/controllers/`): Business logic and algorithms  
3. **System Layer** (`src/system/`): State management
4. **UI Layer** (`src/ui/`): LVGL touchscreen interface

**Key Components:**
- **HardwareManager**: Central hardware coordinator
- **GrindController**: Multi-phase state machine with predictive flow control, 10 pulse corrections, mechanical instability detection, time mode additional pulses, and target-free manual grinding
- **LoadCell (HX711)**: Multi-mode precision weight measurement (instant, smoothed, filtered), calibration flag, noise diagnostics
- **DiagnosticsController**: System health monitoring (calibration status, sustained noise, mechanical instability), state persistence, hysteresis, priority-based warnings
- **UIManager**: 7 screens with LVGL integration; menu page surfaces quick Tools (Scale view, Calibrate, Tune Pulses, Motor Test) followed by Settings (Bluetooth, Display, Grind Settings) and Info sections (Diagnostics, System Info, Logs & Data, Lifetime Stats), warning icon indicator, split-button layout for time mode pulses
- **StateMachine**: Central state coordination (READY → GRINDING → GRIND_COMPLETE)

**Update Intervals:** 20ms grind control, 25ms load cell (active), 50ms UI/hardware

**Grind Phases:**
- Standard phases: IDLE, INITIALIZING, SETUP, TARING, TARE_CONFIRM, PRIME, PRIME_SETTLING, PREDICTIVE, PULSE_DECISION, PULSE_EXECUTE, PULSE_SETTLING, FINAL_SETTLING, TIME_GRINDING, MANUAL_GRINDING, COMPLETED, TIMEOUT
- `TIME_ADDITIONAL_PULSE` - Dedicated phase for post-completion additional grinding pulses in time mode
- `PURGE_CONFIRM` - Pauses after chute operation (in Purge mode) to allow user to discard grinds before continuing to main grind
- **Timeouts**: Targeted grinds stop after 60 seconds; target-free Manual mode has its own 30-second safety cutoff. The error screen wording comes from `src/controllers/grind_timeout_text.h`
- **Waits for a still scale**: by default every one is for a still reading, however long that takes (only the wait after the prime goes on after 10 s). Shaky-bench handling (`GRIND_SHAKY_BENCH_DEFAULT`, 0 as shipped; preference `shaky_bench`, read at `start_grind`) gives them limits for a grinder on a vibrating bench: rules in `src/controllers/settle_wait_policy.h`, values in `src/config/grind_control.h`. Use `GrindController::wait_for_still_scale()` for a new wait, never `check_settling_complete()` on its own. A wait that needs no weight passes `stillness_wait_past_limit()`. A pulse decision and the final weight pass `weight_wait_past_limit()`: with the handling on, a grind that found its bench shaking (`bench_is_shaking()`: the scale came to rest neither after taring nor after the prime) may go by a 2 s average, and only one `WeightSensor::get_shaken_weight()` accepts. A hand on the portafilter does not average out, which is why the handling is not on by default
- **Removal guard**: the vessel's weight is `vessel_weight_estimate()` (`src/controllers/vessel_weight_tracker.h`): the reading before the tare where that can be a vessel, else that reading plus what stood on the scale when a grind last zeroed it, else the `VesselWeightTracker` value. The guard judges each scale sample once, not each control tick

**Grinder Purge/Prime:**
- **Always runs** before weight-mode grinding to saturate the grinder for accurate latency detection
- **Prime mode**: Keeps coffee, continues immediately to PREDICTIVE phase
- **Purge mode** (default): Shows confirmation popup, waits for user to discard stale grinds, then continues
- **Configurable amount**: 0.1g-5.0g (default 1.0g), replaces old hardcoded `GRIND_PRIME_TARGET_WEIGHT_G`
- **Purge popup**: "Keep purge grinds from now on" checkbox switches mode from Purge → Prime in preferences
- **Logging disabled** during PURGE_CONFIRM phase to avoid capturing data while paused
- **Preferences**: `chute_mode` (int: 0=Prime, 1=Purge, default=1), `chute_amount_g` (float: 0.1-5.0, default=1.0)

**Time Mode Pulses:** Split-button completion screen (OK + PULSE), `TIME_ADDITIONAL_PULSE` phase, 100ms duration

**Grind Settings:** Configurable through Menu → Grind Settings page
- **Mode Selection**: Radio buttons for Weight/Time mode selection
- **Swipe Gestures Toggle**: Enable/disable vertical swipe gestures for mode switching (default: disabled)
- **Automation**: Start on Cup and Return on Removal toggles
- **Purging**: Radio buttons (Prime/Purge) and Amount slider (0.1g-5.0g)
- **Preferences**: `swipe.enabled` (boolean), `grind_mode` (0=Weight, 1=Time), `chute_mode` (0=Prime, 1=Purge), `chute_amount_g` (float)
- **Behavior**: Swipe gestures only work when enabled; direct mode selection always works

**Color Scheme (RGB565):**
- `COLOR_PRIMARY`: 0xFF0000 (Red) - Primary theme color
- `COLOR_ACCENT`: 0x00AAFF (Blue) - Highlights and accents
- `COLOR_SUCCESS`: 0x00AA00 (Green) - Success states
- `COLOR_WARNING`: 0xCC8800 (Orange) - Warning states
- `COLOR_BACKGROUND`: 0x000000 (Black) - Background
- `COLOR_TEXT_PRIMARY`: 0xFFFFFF (White) - Primary text

**Font Usage Hierarchy:**
- `lv_font_montserrat_24`: Standard text and button labels
- `lv_font_montserrat_32`: Button symbols (OK, CLOSE, PLUS, MINUS)
- `lv_font_montserrat_36`: Screen titles (menu page titles use `lv_font_montserrat_28`, so the longest clears the back arrow)
- A menu switch row has about 140 px for its label beside the switch, and a slider caption 220 px: keep labels short and put the explanation in a description label above
- `lv_font_montserrat_56`: Large weight displays

## Development Notes

* When modifying this codebase, follow the existing architectural patterns, maintain the clean separation between layers, and ensure any timing-critical code respects the established update intervals.
* use macos compatible commands. macos uses python3
* after making a test build let me know the build number
* Always read entire files. Otherwise, you don’t know what you don’t know, and will end up making mistakes, duplicating code that already exists, or misunderstanding the architecture.  
* Commit early and often. When working on large tasks, your task could be broken down into multiple logical milestones. After a certain milestone is completed and confirmed to be ok by the user, you should commit it. If you do not, if something goes wrong in further steps, we would need to end up throwing away all the code, which is expensive and time consuming.  
* Your internal knowledgebase of libraries might not be up to date. When working with any external library, unless you are 100% sure that the library has a super stable interface, you will look up the latest syntax and usage via either Perplexity (first preference) or web search (less preferred, only use if Perplexity is not available)  
* Do not say things like: “x library isn’t working so I will skip it”. Generally, it isn’t working because you are using the incorrect syntax or patterns. This applies doubly when the user has explicitly asked you to use a specific library, if the user wanted to use another library they wouldn’t have asked you to use a specific one in the first place.  
* Always run linting after making major changes. Otherwise, you won’t know if you’ve corrupted a file or made syntax errors, or are using the wrong methods, or using methods in the wrong way.   
* Please organise code into separate files wherever appropriate, and follow general coding best practices about variable naming, modularity, function complexity, file sizes, commenting, etc.  
* Code is read more often than it is written, make sure your code is always optimised for readability  
* Unless explicitly asked otherwise, the user never wants you to do a “dummy” implementation of any given task. Never do an implementation where you tell the user: “This is how it *would* look like”. Just implement the thing.  
* Whenever you are starting a new task, it is of utmost importance that you have clarity about the task. You should ask the user follow up questions if you do not, rather than making incorrect assumptions.  
* Do not carry out large refactors unless explicitly instructed to do so.  
* When starting on a new task, you should first understand the current architecture, identify the files you will need to modify, and come up with a Plan. In the Plan, you will think through architectural aspects related to the changes you will be making, consider edge cases, and identify the best approach for the given task. Get your Plan approved by the user before writing a single line of code.   
* If you are running into repeated issues with a given task, figure out the root cause instead of throwing random things at the wall and seeing what sticks, or throwing in the towel by saying “I’ll just use another library / do a dummy implementation”.   
* You are an incredibly talented and experienced polyglot with decades of experience in diverse areas such as software architecture, system design, development, UI & UX, copywriting, and more.  
* When doing UI & UX work, make sure your designs are both aesthetically pleasing, easy to use, and follow UI / UX best practices. You pay attention to interaction patterns, micro-interactions, and are proactive about creating smooth, engaging user interfaces that delight users.   
* When you receive a task that is very large in scope or too vague, you will first try to break it down into smaller subtasks. If that feels difficult or still leaves you with too many open questions, push back to the user and ask them to consider breaking down the task for you, or guide them through that process. This is important because the larger the task, the more likely it is that things go wrong, wasting time and energy for everyone involved.
- Touch polling now uses the IDF I2C master driver with ACK checking disabled so idle NACKs don't spam logs. Toggle `DEBUG_SUPPRESS_TOUCH_I2C_ERRORS` to 0 if you need the raw driver output for troubleshooting.
- Use the src/config/constants.h aggregation file to include constants / settings - dont refer to config files directly.
- On V2 the grinder's 5 V supply (a 78L05A with under 100 mA to spare) sags under this firmware's load with Wi-Fi on, most of all during a firmware upload, and the AMOLED panel then goes black while the ESP32 keeps running. Checked on hardware: a panel that has gone dark this way stays dark through a panel reset and through a software restart, and only removing power revives it, so firmware can detect and report the fault but not cure it. What the display code does: `DisplayManager::set_panel_power(false)` puts the panel to sleep and resets it (reset pin); `set_panel_power(true)` resets it again unless a finger is on the screen, sets it up as at power-on and shows the picture once LVGL has drawn it again (`PanelReveal`). While the display is on, `service_panel_refresh()` resends the setup every 3 s and on touch, and resets the panel (about 0.25 s of black screen) only when the frame signal on `HW_DISPLAY_TE_PIN` stays lost or on `POST /api/v1/display/reset`. Do not reset a panel that is showing a picture as a precaution: resets on waking and after each grind were tried and rejected for their black flash. Rules and timing are in `src/hardware/panel_refresh_policy.h`. The reset pin also resets the touch controller, so a reset while the display is on waits for an untouched screen; ask for one with `DisplayManager::request_panel_reset_from_any_task()`. Send panel commands from the UI task only. Reading the panel's registers back is not known to work on this controller (other projects get 0xFF), so a running frame signal is the only sign of life there is, and it does not prove a picture.
- `health_counters` (`src/system/health_counters.h`) keeps brownout restarts and panel faults in flash; the main loop writes them while no grind runs.
- The supply leaves little to spare, so the firmware saves power where it costs the user nothing (rules in `src/system/power_saving_policy.h`, applied by `power_saver`): the processor runs at 80 MHz while the display is dimmed or off and at 240 MHz while it is in use, a grind runs or Bluetooth moves data; `ScreenTimeoutController` keeps the display asleep during a firmware upload from the web page or over Bluetooth; the online update check waits 30 minutes after a start and for a dimmed display. An idle grinder sends web and Home Assistant clients a state message only for a change, with a keep-alive every 2.5 s (`src/network/state_stream_policy.h`). Do not add work that runs at full load soon after a start or during an upload, and do not add regular Wi-Fi traffic while idle.
- A grind is when the supply is weakest. With **Menu → Wi-Fi → Pause** on (preference `wifi_rest`; on by default once `health_counters` has counted a brownout restart) the main loop asks `network_manager.set_radio_rest()` to leave the network from the start of a grind until 15 s after it (`src/network/radio_rest_policy.h`, state `WIFI_RESTING`). **Enabled** (`wifi_on`) switches Wi-Fi off altogether; the UI asks through `network_manager.request_enabled()`, never `set_enabled()` directly. The main loop also keeps a `RunSnapshot` in RTC memory on every pass, and `health_counters` stores the one from before a brownout restart (`/api/v1/status` `system.last_brownout`).
- The profile restored after a restart is stored once a profile page has stayed in view for 2 s or a grind starts with it (`src/system/profile_settle_policy.h`), not on every tab change. Tapping the grind screen changes the layout for that grind only; `GrindingScreen::set_layout()` stores the Grind Settings choice.
- `tools/tests/grind_bench_test.py` runs the unmodified grind controller, strategies and scale filter on the desktop against a simulated grinder and load cell (`tools/tests/grind_bench/`), with scale scatter recorded on a real grinder (`tools/tests/fixtures/`). Add a scenario there when changing grind control; `GRIND_BENCH_VERBOSE=1` prints each grind phase by phase.
- When new features have been added and tested always update the docs as well
- when making a commit, only focus on the end result not the process we went through to get to the end result
