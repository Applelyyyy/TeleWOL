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
5. Connect ESP32 by USB.
6. Select correct serial port in PlatformIO.
7. Build and upload through USB. PlatformIO auto-detects the serial port or uses the port selected in its UI. It validates `.env` and generates the private
   `include/secrets.h` automatically before compiling:

   ```powershell
   pio run -e esp32dev -t upload
   ```

8. Open Serial Monitor at `115200` baud and send `/start` to your bot.

Enable Wake-on-LAN in target PC BIOS/UEFI and Ethernet adapter settings. Use Ethernet adapter MAC address, not Wi-Fi MAC address.

## Configuration

| Variable | Purpose |
| --- | --- |
| `WIFI_SSID` | Wi-Fi SSID |
| `WIFI_PASSWORD` | Wi-Fi password |
| `ESP_STATIC_IP` | Optional fixed ESP32 IPv4 address; blank uses DHCP |
| `ESP_GATEWAY` / `ESP_SUBNET` | Gateway and subnet for static addressing |
| `ESP_DNS_PRIMARY` / `ESP_DNS_SECONDARY` | DNS servers for static addressing |
| `TELEGRAM_BOT_TOKEN` | BotFather token |
| `TELEGRAM_CHAT_ID` | Authorized Telegram chat |
| `WINDOWS_MAC` / `WINDOWS_IP` | Windows Ethernet MAC and local IPv4 address |
| `LINUX_MAC` / `LINUX_IP` | Linux Ethernet MAC and local IPv4 address |
| `OTA_HOSTNAME` | OTA device name |
| `OTA_PASSWORD` | OTA upload password |
| `OTA_PORT` | ArduinoOTA UDP port; normally `3232` |
| `AUTO_WAKE_WINDOWS` | Auto-wake Windows after power-on |
| `AUTO_WAKE_LINUX` | Auto-wake Linux after power-on |
| `AUTO_WAKE_DELAY_SECONDS` | Delay before auto-wake, from 0 to 300 seconds |

Only one target is required. Leave both MAC and IP empty for any unused target. Every build regenerates `include/secrets.h` from `.env`.

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

The default Upload button uses the selected USB serial port. Wi-Fi OTA reads target, port, and password from `.env`:

```powershell
pio run -e esp32dev-ota -t upload
```

When `ESP_STATIC_IP` is blank, OTA uses `OTA_HOSTNAME.local`. Reserve a configured static IP outside the router DHCP pool. If OTA fails, allow Python/PlatformIO through the firewall and allow UDP `OTA_PORT`; ESP32 also connects back to a temporary TCP port on the computer.

## Troubleshooting

| Problem | Check |
| --- | --- |
| Build fails | Check the `.env` validation error printed before compilation |
| Bot not responding | Wi-Fi, Internet, bot token, Chat ID, Serial Monitor |
| PC does not wake | Ethernet MAC, BIOS WOL, adapter WOL, standby power, VLAN/broadcast |
| Status check fails | Target IP, firewall, ICMP reachability |
| OTA target missing | Wi-Fi, mDNS, firewall, VLAN |
| ESP32 stuck | Press RESET once; use USB recovery if OTA remains unavailable |
