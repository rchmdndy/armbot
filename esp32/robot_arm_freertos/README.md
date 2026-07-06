# Robot Arm — ESP32 FreeRTOS Firmware

FreeRTOS refactor of the original single-loop MQTT robot-arm firmware
(`../robot_arm/robot_arm.ino`). Same MQTT protocol, same pins, same home
angles — **drop-in compatible** with the existing web UI at
`../../web/`. Original firmware is untouched.

## What changed and why

The original firmware ran everything in one `loop()`: WiFi connect,
MQTT keepalive, message parsing, and servo writes. Three structural
problems came from that:

| Problem (original) | Consequence | Fix (this firmware) |
|---|---|---|
| `delay()` in `setup()`/callback blocks the whole loop | WiFi/MQTT go unresponsive during servo init | All blocking work moved into FreeRTOS tasks that yield |
| MQTT callback executes servo work inline | Broker keepalive can time out while servos move | Callback only parses + enqueues a `Command`; a separate task dispatches |
| 180° servos jump instantly to a target angle | Current spike → brownout reset, jitter | `servoTask` ramps 1° per 4 ms (~250°/s peak) toward each target |
| No serialization of servo writes | ESP32Servo LEDC timer allocation races between tasks | One `xSemaphoreCreateMutex()` guards every physical `write()`/`attach()` |

## Architecture

```
            ┌──────────── core 0 (WiFi/BT stack) ────────────┐
            │  networkTask  prio 3                            │
            │    WiFi STA  +  PubSubClient.loop()             │
            │    callback ──► parseAndEnqueue ──► cmdQueue    │
            │    LWT "offline", heartbeat "online"/JSON       │
            └───────────────────────┬─────────────────────────┘
                                    │ cmdQueue (FreeRTOS queue, len 16)
            ┌──────────── core 1 (app) ────────┼──────────────┐
            │  commandTask  prio 2              ▼              │
            │    EMERGENCY STOP/RESUME gate                    │
            │    routes SET_SERVO / BASE_STOP                  │
            │         │                                       │
            │         ▼                                       │
            │  ServoManager  (mutex-guarded)                  │
            │   setTarget()  ──────►  servoTask prio 2        │
            │   writeImmediate()      ramps 180° servos       │
            │   detachAll()           360° base set instantly │
            │                                                  │
            │  statusTask  prio 1  ─► publishes status JSON   │
            └──────────────────────────────────────────────────┘
```

### File layout

| File | Role |
|---|---|
| `config.h` | All tunables: pins, WiFi, MQTT topics, task stacks/priorities/cores, calibration, ramp tuning, log macros |
| `types.h` | `Command` struct, `ServoId` enum, `ServoManager` class declaration, `servoName()` |
| `ServoManager.cpp` | Thread-safe servo actuator: staggered attach, ramp tick, emergency detach/reattach |
| `network.h` / `network.cpp` | WiFi + PubSubClient task (core 0), MQTT callback, LWT, non-blocking reconnects |
| `command.cpp` | `parseAndEnqueue()` parser + `commandTask()` dispatcher |
| `tasks.h` / `tasks.cpp` | `servoTask()` (ramp) + `statusTask()` (heartbeat JSON) |
| `robot_arm_freertos.ino` | `setup()` spawns the four tasks; `loop()` idles |
| `platformio.ini` | PlatformIO build config |

## Task table

| Task | Core | Prio | Stack | Job |
|---|---|---|---|---|
| `networkTask` | 0 | 3 | 8 KB | WiFi + MQTT, callback enqueues commands |
| `commandTask` | 1 | 2 | 3 KB | Dequeues + routes commands, emergency gating |
| `servoTask` | 1 | 2 | 2 KB | Ramps 180° servos toward targets every 4 ms |
| `statusTask` | 1 | 1 | 2 KB | Publishes heartbeat/status JSON every 5 s |

**Core assignment rationale:** ESP32's WiFi/Bluetooth protocol stack runs
on core 0. Putting the network task there lets WiFi and PubSubClient share
the same core as the RF stack — no cross-core synchronization on hot
paths. Servo + command + status run on core 1 (the application core) so
actuation never contends with WiFi time-critical work.

## MQTT protocol (unchanged)

- **Subscribe:** `robot/control` — `name:value` commands
- **Publish:** `robot/status` — `"online"` on connect, JSON heartbeat every
  5 s (`{"state":"online","heap":...,"uptime":...,"qdepth":...}`),
  `"offline"` as LWT on ungraceful drop
- **Commands:**
  - `base:<0-180>` — 360° speed (`<90` CCW, `90` stop, `>90` CW)
  - `base:stop` — explicit stop
  - `updown:<0-180>`, `arm:<0-180>`, `gripper:<0-180>` — 180° angles (ramped)
  - `emergency:STOP` — detach all servos
  - `emergency:RESUME` — re-attach and home

## Hardware (unchanged)

| Servo | Type | Pin | Home |
|---|---|---|---|
| Base | SG90 360° continuous | GPIO 13 | 90 (stop) |
| Up/Down | SG90 180° | GPIO 12 | 100° |
| Arm | SG90 180° | GPIO 14 | 150° |
| Gripper | SG90 180° | GPIO 27 | 99° |

## Build & flash

### PlatformIO (recommended)

```bash
cd robot-control-mqtt/esp32/robot_arm_freertos
pio run -t upload          # build + flash
pio device monitor         # serial monitor @ 115200
```

### Arduino IDE

1. Open `robot_arm_freertos.ino` (the other files in the folder are picked
   up automatically as part of the sketch).
2. Install libraries via Library Manager:
   - **PubSubClient** by Nick O'Leary
   - **ESP32Servo** by Kevin Harrington / John K. Bennett
3. Board: **ESP32 Dev Module**. Upload.

> The `.cpp` files share the sketch's global scope, so `network.cpp`,
> `command.cpp`, and `tasks.cpp` compile and link with the `.ino` as one
> translation unit set. No extra setup needed.

## Tuning

- **Ramp speed:** `RAMP_STEP_MS` / `RAMP_STEP_DEG` in `config.h`. Slower =
  smoother but laggy; faster = snappier but more current draw.
- **Heartbeat:** `STATUS_PERIOD_MS`.
- **Calibration:** `BASE_STOP_VALUE` and `HOME_*` angles.
- **Task stacks:** if you see `Stack canary watchpoint` crashes, bump
  `STACK_*` in `config.h`.

## Verifying the refactor

Quick manual checklist after flashing:

1. Serial shows `===== Robot Arm ESP32 (FreeRTOS) =====` then
   `Tasks spawned. Free heap=...`.
2. Web UI ESP32 badge goes ✅ Online and **stays** online (no flapping).
3. Slide `arm` from 90 to 150 — servo should ramp smoothly over ~250 ms,
   not jump.
4. Press-and-hold a base button — base spins; release — stops.
5. `emergency:STOP` — all servos go limp, badge shows offline/emergency;
   `emergency:RESUME` — they re-attach and home, badge back to online.
6. Pull the ESP32's WiFi — web UI badge flips to ❌ Offline (retained LWT
   fires) within a few seconds; restore WiFi — badge returns to ✅ Online.

## Troubleshooting: badge flapping online/offline

A previous revision flapped the web UI ESP32 badge between online and
offline roughly every second. Root causes (all fixed):

1. **Two payload formats on `robot/status`.** `statusTask` published JSON
   `{"state":"online",...}` while `networkTask` published bare `"online"`.
   The web UI does strict `msg === 'online'`, so every JSON message flipped
   the badge to offline. **Fix:** the topic now carries only the bare
   strings `online` | `offline` | `emergency`. No JSON on `robot/status`.

2. **PubSubClient data race across cores.** `statusTask` (core 1) called
   `mqttClient.publish()` while `networkTask` (core 0) called
   `mqttClient.loop()`/`connected()` on the same instance. PubSubClient is
   not thread-safe; the race corrupted outgoing packets, the broker closed
   the socket, LWT fired, and the badge cycled offline→online. **Fix:**
   single-owner pattern — only `networkTask` touches `mqttClient`.
   `statusTask` hands its payload to `networkTask` via `statusQueue`.

3. **No retain on status.** LWT and the `online` announcement used
   `retain=false`, so a web UI that subscribed while the ESP32 was offline
   saw nothing until the next 5 s heartbeat. **Fix:** LWT and `online` are
   now `retain=true`, qos 1 — a late subscriber learns the real state
   immediately.

4. **WiFi radio blips passed through undebounced.** A single dropped
   `WiFi.status()` poll yanked `mqttConnected` false and called
   `connectWiFi()`, passing a ~1 s radio flap straight to the badge.
   **Fix:** WiFi loss is debounced (5 consecutive failed polls ≈ 1 s of
   sustained loss) before MQTT is torn down.

5. **Blocking `connectWiFi()`.** The old version spun up to 20 s blocking
   the network task, so `mqttClient.loop()` keepalive was missed and the
   broker fired LWT mid-retry. **Fix:** bounded 8 s connect attempt that
   yields back to the task loop; keepalive stays alive.

6. **`volatile bool` across cores.** A plain `volatile bool` does not
   guarantee multi-core visibility on Xtensa. **Fix:** replaced with
   `std::atomic<bool>`.

If the badge still flaps after this revision, check (a) the broker is not
itself restarting, (b) WiFi signal strength at the ESP32, and (c) the
serial log for `MQTT connect failed rc=X` (rc=4 = refused by broker,
rc=-2 = network timeout).
