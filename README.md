# TeleWOL

TeleWOL is an ESP32 Wake-on-LAN controller operated through Telegram. It can wake configured Windows or Linux PCs, check their status, reconnect to Wi-Fi, run an optional startup wake, and receive firmware updates through ArduinoOTA.

## Features

- Wake Windows and Linux PCs from Telegram
- Persistent inline-button dashboard
- On-demand online/offline checks
- Optional auto-wake after power recovery
- Automatic Wi-Fi reconnect
- Password-protected ArduinoOTA updates
- Telegram Chat ID access control

## Requirements

- ESP32 development board
- 2.4 GHz Wi-Fi network with Internet access
- Ethernet-connected PC with Wake-on-LAN enabled
- PlatformIO Core or PlatformIO extension for VS Code

PlatformIO installs these libraries from `platformio.ini`:

- `UniversalTelegramBot`
- `ArduinoJson`

Wi-Fi, UDP, ICMP, and ArduinoOTA support comes from ESP32 Arduino framework and project source.

## Setup

1. Create a Telegram bot with `@BotFather` and copy its token.
2. Get your Telegram Chat ID from `@userinfobot`.
3. Copy example configuration:

   ```powershell
   Copy-Item .env.example .env
   ```

4. Edit `.env` with your Wi-Fi, Telegram, target PC, OTA, and auto-wake settings.
5. Generate private firmware settings:

   ```powershell
   python tools\generate_secrets.py
   ```

6. Connect ESP32 by USB.
7. Select correct serial port in PlatformIO.
8. Build and upload:

   ```powershell
   pio run -e esp32dev -t upload
   ```

9. Open Serial Monitor at `115200` baud and send `/start` to your bot.

Enable Wake-on-LAN in target PC BIOS/UEFI and Ethernet adapter settings. Use Ethernet adapter MAC address, not Wi-Fi MAC address.

## Configuration

| Variable | Purpose |
| --- | --- |
| `WIFI_SSID` | Wi-Fi SSID |
| `WIFI_PASSWORD` | Wi-Fi password |
| `TELEGRAM_BOT_TOKEN` | BotFather token |
| `TELEGRAM_CHAT_ID` | Authorized Telegram chat |
| `WINDOWS_MAC` / `WINDOWS_IP` | Windows Ethernet MAC and local IPv4 address |
| `LINUX_MAC` / `LINUX_IP` | Linux Ethernet MAC and local IPv4 address |
| `OTA_HOSTNAME` | OTA device name |
| `OTA_PASSWORD` | OTA upload password |
| `AUTO_WAKE_WINDOWS` | Auto-wake Windows after power-on |
| `AUTO_WAKE_LINUX` | Auto-wake Linux after power-on |
| `AUTO_WAKE_DELAY_SECONDS` | Delay before auto-wake, from 0 to 300 seconds |

Only one target is required. Leave both MAC and IP empty for any unused target. Run `python tools\generate_secrets.py` after every `.env` change.

## Telegram Controls

Use `/start` to open the button dashboard.

| Action | Button / Command |
| --- | --- |
| Wake Windows | `Wake Windows` / `/wake_win` |
| Wake Linux | `Wake Linux` / `/wake_linux` |
| Windows status | `Status Windows` / `/status_win` |
| Linux status | `Status Linux` / `/status_linux` |
| All status | `Status All` / `/status_all` |
| ESP32 status | `ESP32 Status` / `/status` |
| Auto Wake status | `Auto Wake` / `/auto_wake` |

Unavailable targets do not appear in dashboard. Status uses ICMP ping, so firewall rules can make an online PC appear offline.

## OTA Update

First upload must use USB. For later Wi-Fi uploads, set OTA target and password in PowerShell:

```powershell
$env:ESP32_OTA_HOST = "ESP32-IP-OR-HOSTNAME"
$env:ESP32_OTA_PASSWORD = "YOUR_OTA_PASSWORD"
pio run -e esp32dev-ota -t upload
```

If OTA target is missing, check Wi-Fi, mDNS, firewall, and VLAN reachability.

## Troubleshooting

| Problem | Check |
| --- | --- |
| Build fails | Run generator, confirm `include/secrets.h`, then run `pio run` |
| Bot not responding | Wi-Fi, Internet, bot token, Chat ID, Serial Monitor |
| PC does not wake | Ethernet MAC, BIOS WOL, adapter WOL, standby power, VLAN/broadcast |
| Status check fails | Target IP, firewall, ICMP reachability |
| OTA target missing | Wi-Fi, mDNS, firewall, VLAN |
| ESP32 stuck | Press RESET once; use USB recovery if OTA remains unavailable |

## Security

- Only `TELEGRAM_CHAT_ID` can control bot.
- ArduinoOTA requires `OTA_PASSWORD`.
- `.env` and `include/secrets.h` are ignored by Git.
- `client.setInsecure()` disables Telegram TLS certificate verification.
