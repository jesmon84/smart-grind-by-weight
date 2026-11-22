# Wi-Fi, Web and Home Assistant Architecture

This document records the Smart Grind network behaviour and public local API.
GaggiMate is used as mature behavioural prior art
because it runs a real-time coffee appliance on closely related ESP32 hardware.
Smart Grind's implementation is written independently and kept proportionate to
the grinder's smaller feature set.

## User journey

1. Try saved station credentials without blocking weight sampling, motor control,
   touch, or rendering.
2. If credentials are absent or cannot connect, start a secured setup access
   point with a generated, persisted password.
3. Show the setup SSID, password and Wi-Fi QR code on the grinder display.
4. Serve a captive setup page and support Improv Wi-Fi provisioning over USB
   serial as an alternative.
5. Reboot or transition cleanly into station mode after credentials are saved.
6. Resolve the grinder at its `.local` hostname and advertise versioned HTTP and
   Smart Grind services over mDNS.

## Reliability rules learned from mature ESP32 deployments

- Network connection and reconnection must be asynchronous. No network operation
  may delay the real-time grind-control tasks.
- Wi-Fi event callbacks must do minimal work. State changes are acted on from a
  normal application loop/task where stack and locking behaviour are controlled.
- Tear down an existing mDNS responder before restarting it and stop it on link
  loss, otherwise stale records can accumulate across reconnects.
- The HTTP listener should survive transient station reconnects. Repeatedly
  closing and immediately rebinding an asynchronous server can race socket
  teardown and leave port 80 unavailable.
- WebSocket clients that cannot consume updates must be disconnected rather than
  allowed to accumulate unbounded send queues and exhaust internal RAM.
- Keep LVGL's object heap in PSRAM on hardware so the Wi-Fi/lwIP stack retains
  enough internal RAM to accept TCP connections under a fully constructed UI.
- Captive portal probes used by Android, Apple, Windows and Firefox need explicit
  responses so setup opens reliably on common phones and laptops.
- OTA must be a controlled appliance state: require an explicit browser
  confirmation, refuse updates while grinding, stop the motor, validate
  size/image metadata and internal-heap headroom, incrementally write only the
  inactive application partition, reboot, and verify the expected version
  after boot. Update authentication and signed images are deliberately deferred
  until the end-to-end update experience is complete.
- Wi-Fi and Bluetooth may coexist during normal use. Before a web OTA upload,
  the main application loop temporarily shuts down idle Bluetooth and waits for
  internal memory to recover; active BLE transfers are never interrupted. If
  preparation expires or the upload fails after Bluetooth was stopped, the
  device cleanly restarts into its existing valid firmware. This restores the
  configured Bluetooth state without leaking heap by rebuilding the retained
  Arduino BLE server singleton in the same boot.
- Do not pre-erase the complete OTA partition or suspend a task while it can hold
  a flash lock. Both patterns can stall an upload or deadlock recovery; use the
  platform Update API's sector-at-a-time path and keep the network/main loop
  schedulable.
- Hardware tasks are removed from task-watchdog monitoring before OTA suspends
  them and registered again before they resume. A deliberately suspended task
  must never cause a watchdog reboot midway through an otherwise healthy upload.

## Wi-Fi and the power supply

The radio is the board's largest load, and a grind is when the grinder's
supply is weakest. With **Menu → Wi-Fi → Pause** on (preference `wifi_rest`),
`NetworkManager` leaves the joined network when a grind starts and joins it
again 15 seconds after the grind ends (`src/network/radio_rest_policy.h`);
its state reads `resting` meanwhile. A station that is not connected has its
radio powered down. Web and Home Assistant clients lose their connection for
that time and reconnect by themselves; the finished grind is in the history.
Until the user chooses, the pause is on for a grinder that has restarted on a
brownout before, and off otherwise (`NET_WIFI_REST_WHILE_GRINDING_DEFAULT`).
**Enabled** on the same page (preference `wifi_on`) switches Wi-Fi off
altogether.

## Service boundaries

- `NetworkManager`: credentials, station/AP state, reconnect policy and hostname.
- `ProvisioningService`: secured setup AP, captive DNS/HTTP, static setup/device
  pages and (when implemented) Improv serial.
- `DeviceWebServer`: versioned status endpoints and safe full-image OTA.
- `DeviceApi`: one versioned state/control contract shared by the web UI and the
  native Home Assistant integration, plus queued persistent settings changes.
- `DiscoveryService`: `.local`, `_http._tcp` and `_smartgrind._tcp` records with
  firmware/API metadata.

## Delivery sequence

1. Buildable non-blocking station lifecycle and persisted settings.
2. Secured AP, captive provisioning, Improv serial, and on-device QR/details.
3. HTTP status surface and safe full-image OTA.
4. Versioned WebSocket state/control API with backpressure.
5. Live web UI and native Home Assistant integration on the same API.

## Improv serial provisioning

USB provisioning implements Improv Serial v1 using the pinned official C++
protocol library. Incoming data is consumed in small bounded batches from the
normal application loop, partial frames expire after 100 ms, and malformed
credential frames are rejected before the protocol parser sees them. Wi-Fi
connection attempts are asynchronous and report `Provisioned` plus the local
HTTP URL on success, or return to `Authorized` with `Unable to connect` after a
bounded attempt. Credentials are never written to the serial log.

The supported RPCs are Wi-Fi settings, current state, device information and
network state. Network scanning, hostname changes and device-name changes are
optional Improv extensions and currently return `Unknown RPC command`.

## WebSocket API v1

The device serves `/ws` and publishes one state message every 100 ms while a
grind is running. The start and the end of a grind are sent at once. While
idle it publishes a state message only when what a client shows has changed
(the weight by 0.05 g or more, or anything else), no closer together than
500 ms, and a complete state message as a keep-alive every 2.5 s. Every
message wakes the Wi-Fi radio, so an idle grinder stays quiet. No message is
built while no client is connected. The three intervals are reported under
`capabilities` by `GET /api/v1/status` as `state_interval_ms`,
`idle_state_interval_ms` and `idle_keepalive_interval_ms`. The rules are in
`src/network/state_stream_policy.h`.
It accepts no more than four clients and disconnects a client whose outbound
queue cannot keep up. Browser handshakes must have the same HTTP origin as the
device page; native clients without an `Origin` header remain supported.

State messages have this shape:

```json
{
  "api": "v1",
  "type": "state",
  "seq": 42,
  "timestamp_ms": 123456,
  "grind": {
    "active": true,
    "phase": "GRINDING",
    "mode": "weight",
    "profile": 1,
    "progress": 63,
    "target_weight": 18.0,
    "target_time_ms": 0
  },
  "scale": { "weight": 11.34, "flow": 2.17 },
  "motor": { "running": true },
  "system": { "free_heap": 118240 }
}
```

The public `phase` value is deliberately independent of internal controller
state names. API v1 publishes one of `IDLE`, `PREPARING`, `PRIMING`,
`GRINDING`, `PAUSED`, `COASTING`, `FINAL_SETTLING`, `COMPLETED`, or `TIMEOUT`,
so clients remain compatible if the firmware state machine is refined.

Commands may include a numeric `rid`. Firmware echoes it in the acknowledgement,
allowing a client to correlate concurrent requests without confusing a delayed
reply for a newer command. Omitting `rid` remains compatible with the browser
and older API v1 clients. Command objects accept ordinary JSON whitespace while
remaining limited to one bounded text frame.

The controller-backed command set is:

```json
{"type":"command","action":"start","rid":1}
{"type":"command","action":"start_manual","rid":2}
{"type":"command","action":"stop","rid":3}
{"type":"command","action":"dismiss","rid":4}
{"type":"command","action":"tare","rid":5}
{"type":"command","action":"select_profile","profile":1,"rid":6}
{"type":"command","action":"set_mode","mode":"weight","rid":7}
```

Each request receives a v1 acknowledgement containing `action`, `accepted` and
`reason`, plus the same `rid` when one was supplied. The network callback only
validates and queues requests; the normal control task applies the same idle,
load-cell, OTA, phase, safety-timeout and motor guards used by the touchscreen.
No network action drives the relay or a GPIO directly.

`start` runs the selected Single, Double or Custom profile in its configured
weight/time mode. `start_manual` uses the firmware's target-free manual mode and
its independent 30-second cutoff. Tare, profile and mode changes are refused
while the grinder is not idle.

The browser applies a display-only exponential filter and near-zero deadband to
the flow value. Grinder control and saved session samples retain their
original precision; completed graphs are replaced with the full recorded trace.

## HTTP API v1

- `GET /api/v1/status`: device, build, network, memory and OTA progress state,
  plus the WebSocket path, protocol level and advertised command capabilities.
  It also reports the Wi-Fi signal (`network.rssi`, dBm), the reason for the
  last restart (`system.reset_reason`), how many restarts so far followed a
  brownout (`system.brownout_restarts`, kept across power cycles), what was
  running just before the last of them (`system.last_brownout`: `uptime_s`,
  `grind_phase`, `motor_on`, `wifi`, `display_on`, `brightness`, `cpu_mhz`;
  `null` when none is on record), the
  processor clock (`system.cpu_mhz`: 240 in use, 80 while the display is
  dimmed or off), and health
  counters: `display` and `touch` (`bus_resets`, `read_errors`). `display`
  holds `panel_on`, `brightness`, `panel_refreshes`, `reset_supported`,
  `frame_signal` (`running`, `lost`, `absent`, or `unknown` just after a
  setup and while the display is off), `panel_resets` since the last start
  (`display_off`: made as the display switched off; `frame_lost` and
  `requested`: made while it was on), and `faults` and `recoveries`: how
  often the frame signal stopped
  while the display should have been on and how often it came back, kept
  across power cycles.
- `POST /api/v1/display/reset`: asks for a reset of the display panel, for a
  screen that is black while the grinder works. V2 boards only. It is refused
  while the display is switched off.
- `GET /api/v1/settings`: the three grinder profiles and the matching on-device
  automation, purge, display, screensaver, optional GaggiMate host, logging,
  swipe and Bluetooth values.
- `POST /api/v1/settings`: validates a complete form and queues its application
  on the normal UI task. It is refused while grinding.
- `POST /api/v1/profile`: selects Single, Double or Custom while idle, persists
  the selection and keeps the touchscreen and dashboard target in sync.
- `GET /api/v1/history`: validated summaries for the latest 10 stored sessions.
- `GET /api/v1/history/session?id=N`: the checksum-validated raw session used by
  the browser for graphs and CSV/JSON downloads.
- `GET /api/v1/logs`: the latest 4 KiB of boot and runtime messages retained in
  RAM, also viewable and downloadable from the web settings page.
- `GET`, `POST` and `DELETE /api/v1/screensaver/image`: read, transactionally
  replace or remove the fixed-size RGB565 custom image while idle.

Settings, screensaver and display-reset requests enforce same-origin checks. Local API and
motor commands are currently unauthenticated, so the grinder should be kept on
a trusted home network. Web OTA is
deliberately unauthenticated at this stage; browser confirmation plus the
motor, transfer, image and partition guards protect the update operation while
authentication and signed images remain roadmap work. History and image access
are refused while grind logging, OTA or another transfer could contend for the
filesystem. HTTP endpoints do not start the motor; starts use the bounded
WebSocket command queue and controller path described above.

The optional GaggiMate screensaver is a separate outbound local-network
client. It consumes GaggiMate's existing `/ws` status stream and uses
`/api/status` as a temperature-only compatibility fallback. It never sends
control commands to the espresso machine.

`GET /api/v1/status` also exposes a stable 12-character device identifier,
model and hardware revision. The same identifier is advertised as the `id` TXT
property on `_smartgrind._tcp`; integrations use it as the config-entry and
entity identity so DHCP address changes do not create duplicate devices.

## Native Home Assistant integration

The separately versioned
[Smart Grind Home Assistant integration](https://github.com/Clinteastman/smart-grind-home-assistant)
uses `_smartgrind._tcp.local.` discovery, verifies the stable hardware ID over
HTTP, and then keeps a reconnecting local-push WebSocket connection. It exposes
standard sensors, binary sensors, selects and buttons rather than requiring an
MQTT broker or YAML configuration.

The firmware publishes at 10 Hz during a grind; while idle it publishes
changes as they happen and a keep-alive every 2.5 s.
The Home Assistant coordinator publishes semantic transitions immediately but
rate-limits changing scale values before they reach Home Assistant's recorder,
preventing idle sensor jitter from creating unnecessary database growth.
