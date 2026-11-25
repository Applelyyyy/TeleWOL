#include "TelegramUi.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <cstdio>

namespace {

void addButton(JsonArray row, const char* text, const char* callback) {
  JsonObject button = row.createNestedObject();
  button["text"] = text;
  button["callback_data"] = callback;
}

void addButton(JsonArray row, const String& text, const char* callback) {
  JsonObject button = row.createNestedObject();
  button["text"] = text;
  button["callback_data"] = callback;
}

const __FlashStringHelper* signalQuality(int rssi) {
  if (rssi >= -50) return F("Excellent");
  if (rssi >= -60) return F("Good");
  if (rssi >= -70) return F("Fair");
  return F("Weak");
}

const __FlashStringHelper* autoWakeResultText(AutoWakeResult result) {
  switch (result) {
    case AutoWakeResult::kDisabled: return F("Disabled");
    case AutoWakeResult::kNotConfigured: return F("Not configured");
    case AutoWakeResult::kSkippedReset: return F("Skipped (non-power reset)");
    case AutoWakeResult::kAlreadyOnline: return F("Already online (skipped)");
    case AutoWakeResult::kWakeSent: return F("Wake packets sent");
    case AutoWakeResult::kWakeFailed: return F("Wake failed");
    default: return F("Waiting");
  }
}

bool formatThailandTime(uint64_t timestampMs, uint64_t nowMs, bool timeOnly,
                        char* output, size_t outputSize) {
  if (timestampMs == 0 || output == nullptr || outputSize == 0) {
    return false;
  }
  const time_t currentEpoch = time(nullptr);
  if (currentEpoch < 1704067200) {
    return false;
  }
  const uint64_t deltaSec =
      (nowMs >= timestampMs) ? ((nowMs - timestampMs) / 1000ULL) : 0ULL;
  const time_t eventEpoch = currentEpoch - static_cast<time_t>(deltaSec);
  struct tm thailandTime {};
  if (localtime_r(&eventEpoch, &thailandTime) == nullptr) {
    return false;
  }
  const char* fmt = timeOnly ? "%H:%M:%S" : "%d/%m/%Y %H:%M:%S ICT";
  return strftime(output, outputSize, fmt, &thailandTime) > 0;
}

uint64_t latestCheckTime(const Target* targets, size_t targetCount) {
  uint64_t latest = 0;
  for (size_t i = 0; i < targetCount; ++i) {
    if (targets[i].runtime.lastCheckMs > latest) {
      latest = targets[i].runtime.lastCheckMs;
    }
  }
  return latest;
}

}  // namespace

String escapeHtml(const String& raw) {
  String out;
  out.reserve(raw.length() + 8);
  for (size_t i = 0; i < raw.length(); ++i) {
    const char ch = raw.charAt(i);
    if (ch == '&') out += F("&amp;");
    else if (ch == '<') out += F("&lt;");
    else if (ch == '>') out += F("&gt;");
    else out += ch;
  }
  return out;
}

String resetReasonText(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return F("Power-on");
    case ESP_RST_EXT: return F("External reset");
    case ESP_RST_SW: return F("Software reset");
    case ESP_RST_PANIC: return F("Crash / panic");
    case ESP_RST_INT_WDT: return F("Interrupt Watchdog");
    case ESP_RST_TASK_WDT: return F("Task Watchdog");
    case ESP_RST_WDT: return F("Watchdog");
    case ESP_RST_DEEPSLEEP: return F("Deep sleep");
    case ESP_RST_BROWNOUT: return F("Brownout");
    case ESP_RST_SDIO: return F("SDIO reset");
    default: return F("Unknown");
  }
}

String applicationStateText(ApplicationState state) {
  switch (state) {
    case ApplicationState::kBooting: return F("BOOTING");
    case ApplicationState::kWaitingForWiFi: return F("WAITING_FOR_WIFI");
    case ApplicationState::kOnline: return F("ONLINE");
    case ApplicationState::kOtaUpdating: return F("OTA_UPDATING");
    case ApplicationState::kSafeMode: return F("SAFE_MODE");
    default: return F("UNKNOWN");
  }
}

String formattedUptime(uint64_t nowMs) {
  uint64_t totalSeconds = nowMs / 1000ULL;
  const uint64_t days = totalSeconds / 86400ULL;
  totalSeconds %= 86400ULL;
  const uint64_t hours = totalSeconds / 3600ULL;
  totalSeconds %= 3600ULL;
  const uint64_t minutes = totalSeconds / 60ULL;
  const uint64_t seconds = totalSeconds % 60ULL;
  char buffer[36];
  if (days > 0) {
    snprintf(buffer, sizeof(buffer), "%llud %lluh %llum",
             static_cast<unsigned long long>(days),
             static_cast<unsigned long long>(hours),
             static_cast<unsigned long long>(minutes));
  } else if (hours > 0) {
    snprintf(buffer, sizeof(buffer), "%lluh %llum %llus",
             static_cast<unsigned long long>(hours),
             static_cast<unsigned long long>(minutes),
             static_cast<unsigned long long>(seconds));
  } else {
    snprintf(buffer, sizeof(buffer), "%llum %llus",
             static_cast<unsigned long long>(minutes),
             static_cast<unsigned long long>(seconds));
  }
  return String(buffer);
}

String thailandTimeText(uint64_t timestampMs, uint64_t nowMs, bool timeOnly) {
  if (timestampMs == 0) {
    return F("Never");
  }
  char buffer[32]{};
  if (!formatThailandTime(timestampMs, nowMs, timeOnly, buffer,
                          sizeof(buffer))) {
    return F("Syncing...");
  }
  return String(buffer);
}

void buildUiKeyboardCache(UiCache& cache, const Target* targets,
                          size_t targetCount) {
  auto buildSubTab = [](String& out, const char* refreshCallback) {
    DynamicJsonDocument doc(256);
    JsonArray kb = doc.to<JsonArray>();
    JsonArray row = kb.createNestedArray();
    addButton(row, "🔄 Refresh", refreshCallback);
    addButton(row, "⬅️ Control Center", Callback::kMain);
    out.reserve(192);
    out = "";
    serializeJson(doc, out);
  };
  buildSubTab(cache.espStatusKeyboard, Callback::kEspStatus);
  buildSubTab(cache.autoWakeKeyboard, Callback::kAutoWake);
  {
    DynamicJsonDocument doc(128);
    JsonArray kb = doc.to<JsonArray>();
    JsonArray row = kb.createNestedArray();
    addButton(row, "🔄 Refresh", Callback::kEspStatus);
    cache.safeModeKeyboard.reserve(96);
    cache.safeModeKeyboard = "";
    serializeJson(doc, cache.safeModeKeyboard);
  }

  DynamicJsonDocument doc(1024);
  JsonArray kb = doc.to<JsonArray>();
  size_t configuredCount = 0;
  for (size_t i = 0; i < targetCount; ++i) {
    if (targets[i].runtime.configured) {
      ++configuredCount;
    }
  }

  if (configuredCount > 0) {
    JsonArray wakeRow = kb.createNestedArray();
    for (size_t i = 0; i < targetCount; ++i) {
      if (!targets[i].runtime.configured) {
        continue;
      }
      String label;
      label.reserve(24);
      label = F("⚡ Wake ");
      label += targets[i].config.name;
      addButton(wakeRow, label, targets[i].config.wakeCallback);
    }

    JsonArray actionRow = kb.createNestedArray();
    if (configuredCount > 1) {
      addButton(actionRow, "⚡ Wake All", Callback::kWakeAll);
      addButton(actionRow, "🔄 Refresh Status", Callback::kRefreshAll);
    } else {
      addButton(actionRow, "🔄 Refresh Status", Callback::kRefreshAll);
    }
  }

  JsonArray utilRow = kb.createNestedArray();
  addButton(utilRow, "⏱ Auto-Wake", Callback::kAutoWake);
  addButton(utilRow, "⚙️ ESP32 Health", Callback::kEspStatus);

  cache.mainKeyboard.reserve(768);
  serializeJson(doc, cache.mainKeyboard);
}

static String buildMainCardHtml(const UiCache& ui, const Target* targets,
                                size_t targetCount, const PerformanceRuntime&,
                                uint64_t nowMs) {
  String msg;
  msg.reserve(1024);
  msg += F("⚡ <b>TeleWOL Control Center</b>\n\n");

  for (size_t i = 0; i < targetCount; ++i) {
    const Target& target = targets[i];
    if (!target.runtime.configured) {
      continue;
    }

    msg += target.config.icon;
    msg += F(" <b>");
    msg += target.config.name;
    msg += F("</b>  ·  ");

    if (target.runtime.trackingWake) {
      const uint32_t elapsedSec =
          static_cast<uint32_t>((nowMs - target.runtime.wakeStartedMs) / 1000ULL);
      msg += F("🚀 <code>WAKING UP</code> · <code>");
      msg += String(elapsedSec);
      msg += F("s</code>\n");
    } else if (target.runtime.state == TargetState::kOnline) {
      msg += F("🟢 <code>ONLINE</code> · <code>");
      msg += String(target.runtime.responseTimeMs, 1);
      msg += F(" ms</code>");
      if (target.runtime.bootedJustNow) {
        msg += F(" <i>(⚡ up in ");
        msg += String(target.runtime.lastBootSeconds, 1);
        msg += F("s)</i>");
      }
      msg += '\n';
    } else if (target.runtime.state == TargetState::kOffline) {
      msg += F("🔴 <code>OFFLINE</code>");
      if (target.runtime.wakeTimedOut) {
        msg += F(" <i>(no reply after 60s)</i>");
      }
      msg += '\n';
    } else {
      msg += F("🟡 <code>UNCHECKED</code>\n");
    }

    msg += F("└ <code>");
    msg += target.runtime.ip.toString();
    msg += F("</code> · ");
    if (target.runtime.trackingWake) {
      msg += F("<i>Monitoring boot via ICMP...</i>\n\n");
    } else if (target.runtime.state == TargetState::kOffline &&
               target.runtime.lastSuccessfulCheckMs > 0) {
      msg += F("Last seen <code>");
      msg += thailandTimeText(target.runtime.lastSuccessfulCheckMs, nowMs, true);
      msg += F("</code>\n\n");
    } else if (target.runtime.lastCheckMs > 0) {
      msg += F("Checked <code>");
      msg += thailandTimeText(target.runtime.lastCheckMs, nowMs, true);
      msg += F("</code>\n\n");
    } else {
      msg += F("<i>Waiting for initial check</i>\n\n");
    }
  }

  if (ui.statusBanner.length() > 0) {
    msg += F("<blockquote>");
    msg += escapeHtml(ui.statusBanner);
    msg += F("</blockquote>\n");
  }

  const int rssi = WiFi.RSSI();
  msg += F("<blockquote>📡 <b>Wi-Fi:</b> <code>");
  msg += String(rssi);
  msg += F(" dBm</code> (");
  msg += signalQuality(rssi);
  msg += F(") · ⏱ <b>Up:</b> <code>");
  msg += formattedUptime(nowMs);
  msg += F("</code>\n🕒 <b>Updated:</b> <code>");
  msg += thailandTimeText(nowMs, nowMs, false);
  msg += F("</code>");
  const uint64_t lastCheck = latestCheckTime(targets, targetCount);
  if (lastCheck > 0) {
    msg += F(" · Ping <code>");
    msg += thailandTimeText(lastCheck, nowMs, true);
    msg += F("</code>");
  }
  msg += F("</blockquote>");
  return msg;
}

static String buildEspStatusHtml(const NetworkRuntime& network,
                                 const HealthRuntime& health,
                                 const PerformanceRuntime& perf,
                                 const TelegramStats& tgStats, uint64_t nowMs) {
  String msg;
  msg.reserve(1152);
  msg += F("⚙️ <b>ESP32 Health &amp; Telemetry</b>\n\n");

  msg += F("<b>System</b>\n");
  msg += F("• State: <code>");
  msg += applicationStateText(health.applicationState);
  msg += F("</code> · WDT: <code>");
  msg += health.watchdogEnabled ? F("ON") : F("OFF");
  msg += F("</code> · Core0: <code>");
  msg += tgStats.workerHealthy ? F("Healthy") : F("Stalled");
  msg += F("</code>\n• Reset: <code>");
  msg += resetReasonText(health.resetReason);
  msg += F("</code> · Uptime: <code>");
  msg += formattedUptime(nowMs);
  msg += F("</code>\n\n");

  msg += F("<b>Network &amp; Telegram Keep-Alive</b>\n");
  if (WiFi.status() == WL_CONNECTED) {
    const int rssi = WiFi.RSSI();
    msg += F("• SSID: <code>");
    msg += escapeHtml(WiFi.SSID());
    msg += F("</code> (");
    msg += network.staticIpEnabled ? F("Static") : F("DHCP");
    msg += F(")\n• IP: <code>");
    msg += WiFi.localIP().toString();
    msg += F("</code> · RSSI: <code>");
    msg += String(rssi);
    msg += F(" dBm</code> (");
    msg += signalQuality(rssi);
    msg += F(")\n");
  } else {
    msg += F("• Wi-Fi: <code>Disconnected</code>\n");
  }
  msg += F("• Telegram: ");
  msg += tgStats.reachable ? F("🟢 <code>Connected</code>")
                           : F("🟡 <code>Reconnecting</code>");
  msg += F(" (Poll: ");
  msg += tgStats.pollConnected ? F("TLS Reuse") : F("Idle");
  msg += F(", API: ");
  msg += tgStats.apiConnected ? F("TLS Reuse") : F("Idle");
  msg += F(")\n• Last API RTT: <code>");
  msg += String(tgStats.lastApiRttMs);
  msg += F(" ms</code> · Wi-Fi Drops: <code>");
  msg += String(network.wifiDrops);
  msg += F("</code> · WAN Recoveries: <code>");
  msg += String(tgStats.outageRecoveries);
  msg += F("</code>\n\n");

  msg += F("<b>Memory &amp; Speed</b>\n");
  msg += F("• Heap Free: <code>");
  msg += String(ESP.getFreeHeap() / 1024U);
  msg += F(" KB</code> (Min <code>");
  msg += String(ESP.getMinFreeHeap() / 1024U);
  msg += F(" KB</code> · MaxBlk <code>");
  msg += String(ESP.getMaxAllocHeap() / 1024U);
  msg += F(" KB</code>)\n• Core 1 Loop: <code>");
  msg += String(perf.lastLoopMs);
  msg += F(" ms</code> (Max <code>");
  msg += String(perf.maximumLoopMs);
  msg += F(" ms</code>)\n• Last Ping: <code>");
  msg += String(perf.lastPingMs);
  msg += F(" ms</code> · Last WOL: <code>");
  msg += String(perf.lastWolMs);
  msg += F(" ms</code>");
  return msg;
}

static String buildAutoWakeHtml(const Target* targets, size_t targetCount) {
  String msg;
  msg.reserve(600);
  msg += F("⏱ <b>Auto-Wake Configuration</b>\n\n");
  for (size_t i = 0; i < targetCount; ++i) {
    const Target& target = targets[i];
    msg += target.config.icon;
    msg += F(" <b>");
    msg += target.config.name;
    msg += F("</b>\n");
    if (!target.runtime.configured) {
      msg += F("└ <code>Not configured</code>\n\n");
    } else {
      msg += F("└ Policy: <code>");
      msg += target.config.autoWake ? F("Enabled") : F("Disabled");
      msg += F("</code> · Startup: <i>");
      msg += autoWakeResultText(target.runtime.autoWakeResult);
      msg += F("</i>\n\n");
    }
  }
  msg += F("<blockquote>⏳ Startup Delay: <code>");
  msg += String(AUTO_WAKE_DELAY_SECONDS);
  msg += F(" s</code> (Power-on reset only)</blockquote>");
  return msg;
}

static String buildSafeModeHtml(const HealthRuntime& health,
                                bool otaInitialized) {
  String msg;
  msg.reserve(420);
  msg += F("⚠️ <b>TeleWOL SAFE MODE</b>\n\n");
  msg += F("Repeated unhealthy resets were detected.\n");
  msg += F("• Wi-Fi: <code>");
  msg += WiFi.status() == WL_CONNECTED ? F("Connected") : F("Disconnected");
  msg += F("</code>\n• ArduinoOTA: <code>");
  msg += otaInitialized ? F("Ready") : F("Waiting for Wi-Fi");
  msg += F("</code>\n• Reset reason: <code>");
  msg += resetReasonText(health.resetReason);
  msg += F("</code>\n\n<blockquote>Upload patched firmware via ArduinoOTA or USB.</blockquote>");
  return msg;
}

String renderScreenHtml(const UiCache& ui, const Target* targets,
                        size_t targetCount, const NetworkRuntime& network,
                        const HealthRuntime& health,
                        const PerformanceRuntime& perf,
                        const TelegramStats& tgStats, bool otaInitialized,
                        uint64_t nowMs) {
  switch (ui.screen) {
    case Screen::kEspStatus:
      return buildEspStatusHtml(network, health, perf, tgStats, nowMs);
    case Screen::kAutoWake:
      return buildAutoWakeHtml(targets, targetCount);
    case Screen::kSafeMode:
      return buildSafeModeHtml(health, otaInitialized);
    default:
      return buildMainCardHtml(ui, targets, targetCount, perf, nowMs);
  }
}

const String& keyboardForScreen(const UiCache& ui) {
  if (ui.screen == Screen::kMain) {
    return ui.mainKeyboard;
  }
  if (ui.screen == Screen::kEspStatus) {
    return ui.espStatusKeyboard;
  }
  if (ui.screen == Screen::kAutoWake) {
    return ui.autoWakeKeyboard;
  }
  return ui.safeModeKeyboard;
}
