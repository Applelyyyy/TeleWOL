# TeleWOL Repository Reference

> **Purpose**: Single-source-of-truth architectural reference for the TeleWOL ESP32 firmware repository. Read this document first before making changes so you do not need to re-read every file in the codebase.

---

## 1. Project Overview

**TeleWOL** is an ESP32 (`esp32dev`, Arduino Framework on PlatformIO) Wake-on-LAN controller operated via Telegram. It supports waking up to two configured Ethernet targets (`Windows` and `Linux`), checking online/offline status via bounded parallel ICMP ping, optional power-on auto-wake, Wi-Fi auto-reconnect, crash-loop recovery (`SAFE_MODE`), and password-protected `ArduinoOTA` wireless firmware updates.

---

## 2. Repository File Map

| Path | Role |
| :--- | :--- |
| `platformio.ini` | PlatformIO environments (`esp32dev`, `esp32dev-ota`, `esp32dev-usb`), `ArduinoJson@^6.21.5`, pre/post Python hooks |
| `.env.example` / `.env` | Private configuration (Wi-Fi, optional static IP, Telegram token/chat ID, Windows/Linux MAC & IP, OTA, auto-wake) |
| `tools/generate_secrets.py` | Validates `.env` (IPv4, subnet, MAC `AA:BB:CC:DD:EE:FF`, ports, auto-wake delay 0-300s) and atomically generates `include/secrets.h` |
| `tools/platformio_pre.py` | Pre-build hook running `generate_secrets.py` and setting OTA upload host/port/auth flags |
| `tools/platformio_post.py` | Strips `--debug` from `espota` uploader flags to protect OTA password in logs |
| `src/AppTypes.h` | Shared enums, configuration structs, runtime state structs, timing/limit constants, and target declarations |
| `src/BoundedPing.h` / `src/BoundedPing.cpp` | Raw lwIP ICMP ping (`boundedPing` and parallel `boundedPingMultiple`) with bounded `SO_RCVTIMEO` |
| `src/TelegramFastClient.h` / `src/TelegramFastClient.cpp` | Persistent HTTP/1.1 Keep-Alive TLS client (`HTTPClient` + `WiFiClientSecure`) running on Core 0 FreeRTOS task |
| `src/WolSender.h` / `src/WolSender.cpp` | Instant dual-port (UDP 9 & 7) Wake-on-LAN burst sender and boot-tracking state machine |
| `src/TelegramUi.h` / `src/TelegramUi.cpp` | Modern HTML Control Card renderer, compact inline keyboards, and HTML escaping helpers |
| `src/main.cpp` | Core 1 coordinator: Wi-Fi (`WiFi.setSleep(false)`), ArduinoOTA, Watchdog (15s), NVS recovery, Safe Mode, and command dispatch |
| `.agents/skills/esp32/` | Local `ESP32-AI-Agent-Skill` (ESP32 hardware specs, GPIO safety, memory/RAII/FreeRTOS best practices) |

---

## 3. Dual-Core Architecture & Data Flow

```text
Core 0 (FreeRTOS Task: TelegramWorker, 8KB stack)
  ├─ pollClient_ (WiFiClientSecure + HTTPClient setReuse(true))
  │    └─ Short-bounded Long-Poll (timeout=4s) getUpdates -> pushes TelegramUpdate to FreeRTOS Queue
  └─ apiClient_  (WiFiClientSecure + HTTPClient setReuse(true))
       └─ Consumes outbound Queue (answerCallbackQuery, editMessageText, sendMessage, setMyCommands)

Core 1 (Arduino loop(), TWDT 15s)
  ├─ serviceWiFi()            -> WiFi.setSleep(false), DHCP/Static IP, subnet broadcast calculation
  ├─ ArduinoOTA.handle()      -> On OTA start: pauses Telegram Core 0 worker immediately
  ├─ Telegram command dequeue -> Auth check, anti-replay timestamp check, immediate callback toast
  ├─ WolSender                -> <5ms burst (UDP 9 & 7 to subnet broadcast + directed IP) + Auto-Ping tracker
  ├─ BoundedPing              -> Parallel ICMP echo to all configured targets in 1 raw socket
  └─ UiQueue                  -> Enqueues single-message HTML dashboard updates to Core 0
```

---

## 4. Configuration & Build Pipeline

- `.env` is **never** committed; `tools/generate_secrets.py` generates `include/secrets.h` on every build.
- PlatformIO CLI path on this Windows host: `$env:USERPROFILE\.platformio\penv\Scripts\pio.exe`
- Build command: `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32dev`
- OTA upload command: `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32dev-ota -t upload`
- Framework: `framework-arduinoespressif32` v2.0.17 (`esp32dev`, dual-core Xtensa LX6, 520 KB SRAM).

---

## 5. Safety Invariants (Must Never Be Regressed)

1. **Strict Access Control**: Every Telegram message and callback must verify `chatId == TELEGRAM_CHAT_ID`. Unauthorized chats receive access denied and are ignored.
2. **Anti-Replay Wake Protection**:
   - On startup or after Wi-Fi/Telegram disconnect, flush backlog once via `getUpdates?offset=-1`.
   - Text wake commands (`/wake_win`, `/wake_linux`, `/wake_all`) require synced NTP clock (`>= 1704067200`) and message age `<= 60` seconds (`kWakeCommandMaxAgeSeconds`).
   - Auto-Wake runs **only** on `ESP_RST_POWERON` and `!safeMode` after `AUTO_WAKE_DELAY_SECONDS`, and pings first to skip targets that are already online.
3. **OTA Priority**: When `ArduinoOTA.onStart` fires, pause Core 0 Telegram TLS tasks and halt Ping/WOL jobs immediately so OTA receives full Wi-Fi throughput.
4. **Watchdog & Safe Mode**:
   - Core 1 `loop()` subscribes to 15s Task Watchdog (`esp_task_wdt_init`).
   - NVS (`Preferences` namespace `"recovery"`) tracks `"failed"` unhealthy boots (`PANIC`, `WDT`, `BROWNOUT`), `"dashId"`, `"dashChat"`, and `"tgUpdate"`.
   - `>= 3` consecutive unhealthy boots before 60s uptime enters `SAFE_MODE` (Wi-Fi + OTA + Telegram Safe Mode notice only).
5. **ESP32 Hardware & Memory Discipline**:
   - No raw `new`/`delete` in application code; use bounded `DynamicJsonDocument` / `StaticJsonDocument` and pre-reserved `String` or stack buffers.
   - No blocking TLS handshakes on Core 1.
   - Always drain `http.getString()` completely before `http.end()` when `http.setReuse(true)` is active, and auto-retry once on stale keep-alive disconnect.
   - **64-bit Monotonic Clocks**: All uptime, cooldown, ping, and tracking timers use `monotonicMs()` (`esp_timer_get_time() / 1000LL`) so 49.7-day `millis()` rollover never breaks state.
   - **Zero Duplicate Messages & WAN Outage Sync**: `recordFailure()` arms `needsSync_ = true` (flushing backlog on WAN recovery too), `sendInFlight` prevents duplicate `sendMessage` calls while awaiting the initial `messageId`, and only `kMessageMissing` falls back to sending a replacement message.
