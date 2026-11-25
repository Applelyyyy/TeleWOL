#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <cctype>
#include <cstring>
#include <esp_task_wdt.h>
#include <time.h>

#include "AppTypes.h"
#include "BoundedPing.h"
#include "TelegramFastClient.h"
#include "TelegramUi.h"
#include "WolSender.h"

Target targets[Limits::kTargetCount] = {
    {{"Windows", "🪟", WINDOWS_MAC, WINDOWS_IP, AUTO_WAKE_WINDOWS,
      Callback::kWakeWindows, Callback::kStatusWindows,
      Callback::kRefreshWindows, Command::kWakeWindows,
      Command::kStatusWindows},
     {}},
    {{"Linux", "🐧", LINUX_MAC, LINUX_IP, AUTO_WAKE_LINUX,
      Callback::kWakeLinux, Callback::kStatusLinux,
      Callback::kRefreshLinux, Command::kWakeLinux,
      Command::kStatusLinux},
     {}},
};

WiFiUDP udp;
Preferences preferences;
TelegramFastClient telegramFast;

NetworkRuntime network;
UiCache ui;
ActionRuntime actions;
OtaRuntime ota;
HealthRuntime health;
PerformanceRuntime performance;
ClockRuntime clockRuntime;

struct DashboardSyncState {
  int32_t messageId = 0;
  int32_t lastSavedUpdateId = 0;
  bool sendInFlight = false;
  bool commandsRegistered = false;
  char lastCallbackQueryId[64]{};
  String chatId;
  String lastText;
  String lastKeyboard;
} dashboardState;

namespace {

Target* findTarget(TargetId id) {
  const uint8_t index = static_cast<uint8_t>(id);
  return index < Limits::kTargetCount ? &targets[index] : nullptr;
}

TargetId targetIdAt(size_t index) {
  return index < Limits::kTargetCount ? static_cast<TargetId>(index)
                                      : TargetId::kNone;
}

bool sameAction(const ParsedAction& left, const ParsedAction& right) {
  return left.action == right.action && left.target == right.target;
}

void feedWatchdog() {
  if (health.watchdogEnabled) {
    esp_task_wdt_reset();
  }
}

void finishLoopTiming(uint64_t startedMs) {
  performance.lastLoopMs = static_cast<uint32_t>(monotonicMs() - startedMs);
  if (performance.lastLoopMs > performance.maximumLoopMs) {
    performance.maximumLoopMs = performance.lastLoopMs;
  }
  if (Diagnostics::kPerformance &&
      performance.lastLoopMs >= Timing::kSlowLoopMs) {
    Serial.printf("[WARN] Core1 slow loop: %lu ms\n",
                  static_cast<unsigned long>(performance.lastLoopMs));
  }
}

uint8_t hexValue(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  value = static_cast<char>(tolower(static_cast<unsigned char>(value)));
  return static_cast<uint8_t>(value - 'a' + 10);
}

bool parseMacAddress(const char* text, uint8_t (&mac)[6]) {
  if (text == nullptr || strlen(text) != 17) {
    return false;
  }
  for (uint8_t index = 0; index < 17; ++index) {
    if (index % 3 == 2) {
      if (text[index] != ':') return false;
    } else if (!isxdigit(static_cast<unsigned char>(text[index]))) {
      return false;
    }
  }
  for (uint8_t index = 0; index < 6; ++index) {
    mac[index] = static_cast<uint8_t>((hexValue(text[index * 3]) << 4U) |
                                      hexValue(text[index * 3 + 1]));
  }
  return true;
}

void initializeTargets() {
  for (Target& target : targets) {
    const bool hasPair =
        target.config.macText[0] != '\0' && target.config.ipText[0] != '\0';
    target.runtime.configured =
        hasPair && target.runtime.ip.fromString(target.config.ipText) &&
        parseMacAddress(target.config.macText, target.runtime.mac);
    target.runtime.state = target.runtime.configured
                               ? TargetState::kUnknown
                               : TargetState::kNotConfigured;
    target.runtime.autoWakeResult = !target.runtime.configured
                                        ? AutoWakeResult::kNotConfigured
                                        : target.config.autoWake
                                              ? AutoWakeResult::kPending
                                              : AutoWakeResult::kDisabled;
    if (hasPair && !target.runtime.configured) {
      Serial.printf("Invalid %s target configuration\n", target.config.name);
    }
  }
}

void queueScreen(Screen screen, const char* banner = nullptr,
                 bool allowFallback = true) {
  ui.screen = screen;
  if (banner != nullptr) {
    ui.statusBanner = banner;
  }
  ui.updatePending = true;
  ui.fallbackAllowed = allowFallback;
}

void saveDashboardIdentity() {
  preferences.putInt("dashId", dashboardState.messageId);
  preferences.putString("dashChat", dashboardState.chatId);
}

void adoptDashboardMessage(const TelegramInboundEvent& event) {
  if (event.messageId <= 0) {
    return;
  }
  if (dashboardState.messageId == event.messageId &&
      dashboardState.chatId == event.chatId) {
    return;
  }
  dashboardState.messageId = event.messageId;
  dashboardState.chatId = event.chatId;
  dashboardState.sendInFlight = false;
  dashboardState.lastText = "";
  dashboardState.lastKeyboard = "";
  saveDashboardIdentity();
}

void runParallelStatusPing(TargetId onlyTarget = TargetId::kNone) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  PingTargetRequest reqs[Limits::kTargetCount]{};
  PingTargetOutcome outs[Limits::kTargetCount]{};
  bool anyEnabled = false;

  for (size_t i = 0; i < Limits::kTargetCount; ++i) {
    reqs[i].ip = targets[i].runtime.ip;
    const bool match = (onlyTarget == TargetId::kNone) ||
                       (targetIdAt(i) == onlyTarget);
    reqs[i].enabled = targets[i].runtime.configured && match;
    anyEnabled |= reqs[i].enabled;
  }
  if (!anyEnabled) {
    return;
  }

  const uint64_t startedMs = monotonicMs();
  boundedPingMultiple(reqs, Limits::kTargetCount, Timing::kPingTimeoutMs,
                      Timing::kPingRetryAfterMs, outs);
  const uint64_t finishedMs = monotonicMs();
  performance.lastPingMs = static_cast<uint32_t>(finishedMs - startedMs);

  if (Diagnostics::kPerformance) {
    Serial.printf("[PERF] Parallel Ping: %lu ms\n",
                  static_cast<unsigned long>(performance.lastPingMs));
  }

  for (size_t i = 0; i < Limits::kTargetCount; ++i) {
    if (!reqs[i].enabled) {
      continue;
    }
    Target& target = targets[i];
    target.runtime.lastCheckMs = finishedMs;
    target.runtime.responseTimeMs = outs[i].responseTimeMs;
    if (outs[i].result == BoundedPingResult::kReply) {
      target.runtime.state = TargetState::kOnline;
      target.runtime.lastSuccessfulCheckMs = finishedMs;
      target.runtime.trackingWake = false;
      target.runtime.wakeTimedOut = false;
    } else if (outs[i].result == BoundedPingResult::kTimeout) {
      target.runtime.state = TargetState::kOffline;
    } else {
      target.runtime.state = TargetState::kUnknown;
    }
  }
}

String normalizeCommand(String command) {
  command.trim();
  const int suffix = command.indexOf('@');
  if (suffix >= 0) {
    command.remove(suffix);
  }
  return command;
}

ParsedAction parseAction(const char* rawValue, bool callback) {
  String value = callback ? String(rawValue) : normalizeCommand(String(rawValue));
  if (value == (callback ? Callback::kMain : Command::kStart) ||
      (!callback && value == Command::kHelp)) {
    return {Action::kMain, TargetId::kNone};
  }
  if (value == (callback ? Callback::kEspStatus : Command::kEspStatus) ||
      (!callback && value == Command::kLegacyStatus)) {
    return {Action::kEspStatus, TargetId::kNone};
  }
  if (value == (callback ? Callback::kAutoWake : Command::kAutoWake)) {
    return {Action::kAutoWake, TargetId::kNone};
  }
  if (value == (callback ? Callback::kWakeAll : Command::kWakeAll)) {
    return {Action::kWakeAll, TargetId::kNone};
  }
  if (value == (callback ? Callback::kStatusAll : Command::kStatusAll) ||
      (callback && value == Callback::kRefreshAll) ||
      (!callback && value == Command::kPing)) {
    return {Action::kAllStatus, TargetId::kNone};
  }
  for (size_t index = 0; index < Limits::kTargetCount; ++index) {
    const TargetConfig& config = targets[index].config;
    if (value == (callback ? config.wakeCallback : config.wakeCommand)) {
      return {Action::kWakeTarget, targetIdAt(index)};
    }
    if (value == (callback ? config.statusCallback : config.statusCommand) ||
        (callback && value == config.refreshCallback)) {
      return {Action::kTargetStatus, targetIdAt(index)};
    }
  }
  return {Action::kUnknown, TargetId::kNone};
}

void handleWakeRequest(TargetId targetId, const char* queryId, uint64_t now) {
  if (targetId == TargetId::kNone) {
    bool anySent = false;
    bool anyCooldown = false;
    uint32_t totalWolMs = 0;
    for (Target& target : targets) {
      if (!target.runtime.configured) continue;
      uint32_t durMs = 0;
      bool cooldown = false;
      if (triggerManualWake(udp, target, network.broadcast, now, durMs,
                            cooldown)) {
        anySent = true;
      }
      anyCooldown |= cooldown;
      totalWolMs += durMs;
    }
    performance.lastWolMs = totalWolMs;
    if (anySent) {
      if (queryId != nullptr) {
        telegramFast.enqueueCallbackAck(
            queryId, "⚡ WOL burst sent to all targets! Tracking boot...");
      }
      queueScreen(Screen::kMain,
                  "⚡ Sent WOL burst (Port 9 & 7) to all targets.");
    } else if (anyCooldown) {
      if (queryId != nullptr) {
        telegramFast.enqueueCallbackAck(queryId, "⏳ Wake requested recently.");
      }
      queueScreen(Screen::kMain, "⏳ Wake cooldown active; please wait.");
    } else {
      if (queryId != nullptr) {
        telegramFast.enqueueCallbackAck(queryId, "❌ Failed to send WOL.");
      }
      queueScreen(Screen::kMain, "❌ Wake-on-LAN failed (check Wi-Fi/targets).");
    }
    return;
  }

  Target* target = findTarget(targetId);
  if (target == nullptr || !target->runtime.configured) {
    if (queryId != nullptr) {
      telegramFast.enqueueCallbackAck(queryId, "Target is not configured.");
    }
    queueScreen(Screen::kMain, "⚠️ Target is not configured.");
    return;
  }

  uint32_t wolMs = 0;
  bool cooldown = false;
  const bool sent = triggerManualWake(udp, *target, network.broadcast, now,
                                      wolMs, cooldown);
  performance.lastWolMs = wolMs;

  char toast[96]{};
  char banner[128]{};
  if (sent) {
    snprintf(toast, sizeof(toast), "⚡ Waking %s! Monitoring boot...",
             target->config.name);
    snprintf(banner, sizeof(banner),
             "⚡ Sent WOL burst to %s (%lu ms) — monitoring boot...",
             target->config.name, static_cast<unsigned long>(wolMs));
  } else if (cooldown) {
    snprintf(toast, sizeof(toast), "⏳ %s wake cooldown active.",
             target->config.name);
    snprintf(banner, sizeof(banner),
             "⏳ Wake for %s requested recently; please wait.",
             target->config.name);
  } else {
    snprintf(toast, sizeof(toast), "❌ Failed to wake %s.", target->config.name);
    snprintf(banner, sizeof(banner), "❌ Failed to send WOL packet to %s.",
             target->config.name);
  }

  if (queryId != nullptr) {
    telegramFast.enqueueCallbackAck(queryId, toast);
  }
  queueScreen(Screen::kMain, banner);
}

void handleStatusRefresh(TargetId targetId, const char* queryId, uint64_t now) {
  if (actions.lastStatusRequestMs != 0 &&
      now - actions.lastStatusRequestMs < Timing::kStatusRefreshCooldownMs) {
    if (queryId != nullptr) {
      telegramFast.enqueueCallbackAck(queryId, "⏳ Refreshing too fast.");
    }
    return;
  }
  actions.lastStatusRequestMs = now;
  runParallelStatusPing(targetId);

  if (queryId != nullptr) {
    char toast[64]{};
    snprintf(toast, sizeof(toast), "✅ Refreshed (%lu ms)",
             static_cast<unsigned long>(performance.lastPingMs));
    telegramFast.enqueueCallbackAck(queryId, toast);
  }
  queueScreen(Screen::kMain, "");
}

void handleInboundEvent(const TelegramInboundEvent& event, uint64_t now) {
  if (event.updateId > 0 && event.updateId != dashboardState.lastSavedUpdateId) {
    dashboardState.lastSavedUpdateId = event.updateId;
    preferences.putLong("tgUpdate", event.updateId);
  }

  if (strcmp(event.chatId, TELEGRAM_CHAT_ID) != 0) {
    Serial.printf("Unauthorized Telegram access from chat ID %s\n",
                  event.chatId);
    if (event.isCallback) {
      telegramFast.enqueueCallbackAck(event.queryId, "Access denied.", true);
    } else {
      telegramFast.enqueueOneShotText(
          event.chatId, "⛔ <b>Access denied.</b> This chat is not authorized.");
    }
    return;
  }

  if (dashboardState.chatId.length() == 0) {
    dashboardState.chatId = event.chatId;
  }

  if (event.isCallback) {
    if (event.queryId[0] != '\0' &&
        strcmp(event.queryId, dashboardState.lastCallbackQueryId) == 0) {
      telegramFast.enqueueCallbackAck(event.queryId, "Already handled.");
      return;
    }
    strlcpy(dashboardState.lastCallbackQueryId, event.queryId,
            sizeof(dashboardState.lastCallbackQueryId));

    const ParsedAction parsed = parseAction(event.text, true);
    if (sameAction(parsed, actions.lastButton) && actions.lastButtonMs != 0 &&
        now - actions.lastButtonMs < Timing::kButtonDebounceMs) {
      telegramFast.enqueueCallbackAck(event.queryId, "Please wait.");
      return;
    }
    actions.lastButton = parsed;
    actions.lastButtonMs = now;
    adoptDashboardMessage(event);

    if (ota.inProgress) {
      telegramFast.enqueueCallbackAck(event.queryId,
                                      "OTA update is in progress.");
      return;
    }
    if (health.safeMode) {
      telegramFast.enqueueCallbackAck(event.queryId, "Safe Mode active.");
      queueScreen(Screen::kSafeMode, "");
      return;
    }

    switch (parsed.action) {
      case Action::kMain:
        runParallelStatusPing();
        telegramFast.enqueueCallbackAck(event.queryId, "✅ Control Center");
        queueScreen(Screen::kMain, "");
        break;
      case Action::kEspStatus:
        telegramFast.enqueueCallbackAck(event.queryId, "⚙️ ESP32 Health");
        queueScreen(Screen::kEspStatus, "");
        break;
      case Action::kAutoWake:
        telegramFast.enqueueCallbackAck(event.queryId, "⏱ Auto-Wake");
        queueScreen(Screen::kAutoWake, "");
        break;
      case Action::kAllStatus:
      case Action::kTargetStatus:
        handleStatusRefresh(parsed.target, event.queryId, now);
        break;
      case Action::kWakeTarget:
        handleWakeRequest(parsed.target, event.queryId, now);
        break;
      case Action::kWakeAll:
        handleWakeRequest(TargetId::kNone, event.queryId, now);
        break;
      default:
        telegramFast.enqueueCallbackAck(event.queryId, "Unknown action.");
        break;
    }
    return;
  }

  if (event.text[0] == '\0') {
    return;
  }
  dashboardState.chatId = event.chatId;

  if (health.safeMode) {
    queueScreen(Screen::kSafeMode, "");
    return;
  }

  const ParsedAction parsed = parseAction(event.text, false);
  if (parsed.action == Action::kWakeTarget ||
      parsed.action == Action::kWakeAll) {
    const time_t currentEpoch = time(nullptr);
    if (currentEpoch < 1704067200 || event.messageDate <= 0 ||
        event.messageDate > currentEpoch ||
        currentEpoch - event.messageDate >
            static_cast<int64_t>(Timing::kWakeCommandMaxAgeSeconds)) {
      Serial.println(
          F("Ignored expired or unverifiable Telegram wake command"));
      queueScreen(Screen::kMain,
                  "⚠️ Wake command expired or clock not synced. Try again.");
      return;
    }
  }

  switch (parsed.action) {
    case Action::kMain:
      runParallelStatusPing();
      // Only /start intentionally spawns a fresh Control Center message.
      dashboardState.messageId = 0;
      dashboardState.sendInFlight = false;
      dashboardState.lastText = "";
      dashboardState.lastKeyboard = "";
      queueScreen(Screen::kMain, "");
      break;
    case Action::kEspStatus:
      queueScreen(Screen::kEspStatus, "");
      break;
    case Action::kAutoWake:
      queueScreen(Screen::kAutoWake, "");
      break;
    case Action::kAllStatus:
    case Action::kTargetStatus:
      handleStatusRefresh(parsed.target, nullptr, now);
      break;
    case Action::kWakeTarget:
      handleWakeRequest(parsed.target, nullptr, now);
      break;
    case Action::kWakeAll:
      handleWakeRequest(TargetId::kNone, nullptr, now);
      break;
    default:
      queueScreen(Screen::kMain,
                  "ℹ️ Unknown command. Use /start or the buttons below.");
      break;
  }
}

void serviceDashboardDispatch(uint64_t now) {
  TelegramDashboardOutcome outcome;
  if (telegramFast.consumeDashboardOutcome(outcome)) {
    if (!outcome.wasEdit) {
      dashboardState.sendInFlight = false;
    }
    if (outcome.kind == TelegramResultKind::kSuccess ||
        outcome.kind == TelegramResultKind::kNotModified) {
      if (!outcome.wasEdit && outcome.messageId > 0) {
        dashboardState.messageId = outcome.messageId;
        saveDashboardIdentity();
      }
      if (!dashboardState.commandsRegistered) {
        dashboardState.commandsRegistered = true;
        telegramFast.enqueueRegisterCommands();
      }
    } else if (outcome.wasEdit &&
               outcome.kind == TelegramResultKind::kMessageMissing &&
               ui.fallbackAllowed) {
      // Only send a replacement message if Telegram says the old one is gone.
      dashboardState.messageId = 0;
      dashboardState.lastText = "";
      dashboardState.lastKeyboard = "";
      ui.fallbackAllowed = false;
      ui.updatePending = true;
    } else if (outcome.kind == TelegramResultKind::kNetworkError ||
               outcome.kind == TelegramResultKind::kParseError) {
      dashboardState.lastText = "";
      ui.updatePending = true;
    }
  }

  if (!ui.updatePending || ota.inProgress || WiFi.status() != WL_CONNECTED) {
    return;
  }
  // Never spawn a second sendMessage while the first one is still in flight.
  if (dashboardState.messageId <= 0 && dashboardState.sendInFlight) {
    return;
  }
  if (dashboardState.chatId.length() == 0) {
    dashboardState.chatId = TELEGRAM_CHAT_ID;
  }

  const TelegramStats tgStats = telegramFast.getStats();
  const String htmlText = renderScreenHtml(
      ui, targets, Limits::kTargetCount, network, health, performance, tgStats,
      ota.initialized, now);
  const String& keyboard = keyboardForScreen(ui);
  const bool editExisting = dashboardState.messageId > 0;

  if (editExisting && htmlText == dashboardState.lastText &&
      keyboard == dashboardState.lastKeyboard) {
    ui.updatePending = false;
    return;
  }

  if (!editExisting) {
    dashboardState.sendInFlight = true;
  }
  telegramFast.enqueueDashboardUpdate(dashboardState.chatId.c_str(),
                                      dashboardState.messageId, htmlText,
                                      keyboard, editExisting);
  dashboardState.lastText = htmlText;
  dashboardState.lastKeyboard = keyboard;
  ui.updatePending = false;
}

void serviceAutomaticStatus(uint64_t now) {
  if (health.safeMode || ota.inProgress || WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (actions.automaticStatusStarted &&
      now - actions.lastAutomaticStatusMs <
          Timing::kAutomaticStatusIntervalMs) {
    return;
  }

  bool hasConfiguredTarget = false;
  for (const Target& target : targets) {
    hasConfiguredTarget |= target.runtime.configured;
  }
  if (!hasConfiguredTarget) {
    return;
  }

  actions.automaticStatusStarted = true;
  actions.lastAutomaticStatusMs = now;
  runParallelStatusPing();
  if (ui.screen == Screen::kMain) {
    queueScreen(Screen::kMain, "");
  }
  Serial.println(F("Automatic 10-minute parallel status check completed"));
}

bool autoWakeAllowedForReset() {
  return health.resetReason == ESP_RST_POWERON && !health.safeMode;
}

void scheduleAutoWake(uint64_t now) {
  if (actions.autoWake.handled || !autoWakeAllowedForReset()) {
    actions.autoWake.handled = true;
    for (Target& target : targets) {
      if (target.runtime.configured && target.config.autoWake) {
        target.runtime.autoWakeResult = AutoWakeResult::kSkippedReset;
      }
    }
    Serial.println(F("Auto Wake skipped for this reset reason"));
    return;
  }
  bool hasEnabledTarget = false;
  for (const Target& target : targets) {
    hasEnabledTarget |= target.runtime.configured && target.config.autoWake;
  }
  if (!hasEnabledTarget) {
    actions.autoWake.handled = true;
    return;
  }
  actions.autoWake.pending = true;
  actions.autoWake.startedMs = now;
}

void serviceAutoWake(uint64_t now) {
  if (!actions.autoWake.pending || actions.autoWake.handled || ota.inProgress ||
      WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (now - actions.autoWake.startedMs <
      static_cast<uint64_t>(AUTO_WAKE_DELAY_SECONDS) * 1000ULL) {
    return;
  }
  actions.autoWake.pending = false;
  actions.autoWake.handled = true;

  runParallelStatusPing();
  for (Target& target : targets) {
    if (!target.runtime.configured || !target.config.autoWake) {
      continue;
    }
    if (target.runtime.state == TargetState::kOnline) {
      target.runtime.autoWakeResult = AutoWakeResult::kAlreadyOnline;
    } else if (target.runtime.state == TargetState::kOffline) {
      uint32_t wolMs = 0;
      const bool sent = sendWolBurst(udp, target, network.broadcast,
                                     Limits::kWolInitialBurstCount, wolMs);
      target.runtime.autoWakeResult =
          sent ? AutoWakeResult::kWakeSent : AutoWakeResult::kWakeFailed;
      if (sent) {
        target.runtime.trackingWake = true;
        target.runtime.wakeStartedMs = now;
        target.runtime.lastTrackPingMs = now;
      }
    } else {
      target.runtime.autoWakeResult = AutoWakeResult::kWakeFailed;
    }
  }
  queueScreen(ui.screen, "⏱ Startup Auto-Wake check completed.");
}

void configureBroadcastAddress() {
  const IPAddress local = WiFi.localIP();
  const IPAddress mask = WiFi.subnetMask();
  for (uint8_t index = 0; index < 4; ++index) {
    network.broadcast[index] =
        local[index] | static_cast<uint8_t>(~mask[index]);
  }
  Serial.printf("WOL broadcast address: %s\n",
                network.broadcast.toString().c_str());
}

void setupOTA() {
  if (ota.initialized) {
    return;
  }
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.onStart([]() {
    ota.lastProgress = UINT8_MAX;
    ota.inProgress = true;
    telegramFast.setPaused(true);
    health.applicationState = ApplicationState::kOtaUpdating;
    feedWatchdog();
    Serial.println(F("OTA update starting; Telegram worker paused"));
  });
  ArduinoOTA.onEnd([]() {
    preferences.putUChar("failed", 0);
    feedWatchdog();
    Serial.println(F("OTA update complete"));
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (total == 0) return;
    const uint8_t percent = static_cast<uint8_t>((progress * 100U) / total);
    if (percent == 100 || ota.lastProgress == UINT8_MAX ||
        percent >= ota.lastProgress + 10) {
      Serial.printf("OTA Progress: %u%%\n", percent);
      ota.lastProgress = percent;
    }
    feedWatchdog();
  });
  ArduinoOTA.onError([](ota_error_t error) {
    ota.inProgress = false;
    telegramFast.setPaused(false);
    health.applicationState = health.safeMode ? ApplicationState::kSafeMode
                                              : ApplicationState::kOnline;
    Serial.printf("OTA Error: %u; Telegram worker resumed\n", error);
  });
  ArduinoOTA.begin();
  ota.initialized = true;
  Serial.println(F("ArduinoOTA ready"));
}

void setupThailandClock() {
  if (clockRuntime.configured) {
    return;
  }
  configTzTime(Timing::kThailandTimezone, Timing::kPrimaryTimeServer,
               Timing::kSecondaryTimeServer);
  clockRuntime.configured = true;
  Serial.println(F("Time sync configured: Thailand ICT (UTC+7)"));
}

void onWiFiConnected(uint64_t now) {
  WiFi.setSleep(false);
  Serial.println(F("Wi-Fi connected (Modem Sleep disabled for low latency)"));
  Serial.printf("SSID: %s | IP: %s | RSSI: %d dBm\n", WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str(), WiFi.RSSI());

  configureBroadcastAddress();
  setupThailandClock();
  setupOTA();

  telegramFast.begin(TELEGRAM_BOT_TOKEN, dashboardState.lastSavedUpdateId);
  telegramFast.setPollIntervalSafeMode(health.safeMode);
  telegramFast.setPaused(false);

  if (!network.connectedBefore) {
    network.connectedBefore = true;
    scheduleAutoWake(now);
    if (!health.safeMode) {
      runParallelStatusPing();
    }
    queueScreen(health.safeMode ? Screen::kSafeMode : Screen::kMain);
  } else {
    if (!health.safeMode) {
      runParallelStatusPing();
    }
    queueScreen(ui.screen, "🔄 Wi-Fi reconnected; dashboard synced.");
  }
}

void onWiFiDisconnected() {
  Serial.println(F("Wi-Fi disconnected"));
  ++network.wifiDrops;
  telegramFast.setPaused(true);
  telegramFast.requestUpdateSync();
  actions.autoWake.pending = false;
  actions.autoWake.handled = true;
  for (Target& target : targets) {
    target.runtime.trackingWake = false;
    target.runtime.followupPacketsRemaining = 0;
  }
}

void configureWiFiAddress() {
  if (ESP_STATIC_IP[0] == '\0') {
    Serial.println(F("Wi-Fi address mode: DHCP"));
    return;
  }

  const bool valid = network.staticIp.fromString(ESP_STATIC_IP) &&
                     network.gateway.fromString(ESP_GATEWAY) &&
                     network.subnet.fromString(ESP_SUBNET) &&
                     network.primaryDns.fromString(ESP_DNS_PRIMARY) &&
                     (ESP_DNS_SECONDARY[0] == '\0' ||
                      network.secondaryDns.fromString(ESP_DNS_SECONDARY));
  if (!valid) {
    Serial.println(F("Invalid static Wi-Fi configuration; using DHCP"));
    return;
  }

  network.staticIpEnabled =
      WiFi.config(network.staticIp, network.gateway, network.subnet,
                  network.primaryDns, network.secondaryDns);
  Serial.printf("Wi-Fi address mode: %s%s\n",
                network.staticIpEnabled ? "static " : "DHCP fallback",
                network.staticIpEnabled ? ESP_STATIC_IP : "");
}

void serviceWiFi(uint64_t now) {
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != network.wasConnected) {
    network.wasConnected = connected;
    if (connected) onWiFiConnected(now);
    else onWiFiDisconnected();
  }
  if (connected) {
    health.applicationState = ota.inProgress
                                  ? ApplicationState::kOtaUpdating
                                  : health.safeMode
                                        ? ApplicationState::kSafeMode
                                        : ApplicationState::kOnline;
    return;
  }

  health.applicationState = health.safeMode ? ApplicationState::kSafeMode
                                            : ApplicationState::kWaitingForWiFi;
  if (now - network.lastRetryMs < Timing::kWifiRetryMs) {
    return;
  }
  Serial.println(F("Retrying Wi-Fi connection..."));
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  network.lastRetryMs = now;
}

bool isUnhealthyReset(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
      return true;
    default:
      return false;
  }
}

void setupRecovery() {
  health.resetReason = esp_reset_reason();
  Serial.printf("Reset reason: %s\n",
                resetReasonText(health.resetReason).c_str());
  preferences.begin("recovery", false);
  uint8_t failedBoots = preferences.getUChar("failed", 0);
  if (isUnhealthyReset(health.resetReason)) {
    if (failedBoots < UINT8_MAX) ++failedBoots;
  } else {
    failedBoots = 0;
  }
  preferences.putUChar("failed", failedBoots);
  health.safeMode = failedBoots >= Limits::kSafeModeBootCount;
  dashboardState.messageId = preferences.getInt("dashId", 0);
  dashboardState.chatId = preferences.getString("dashChat", TELEGRAM_CHAT_ID);
  dashboardState.lastSavedUpdateId = preferences.getLong("tgUpdate", 0);
  if (health.safeMode) {
    health.applicationState = ApplicationState::kSafeMode;
    ui.screen = Screen::kSafeMode;
    Serial.println(F("SAFE MODE: repeated unhealthy boots detected"));
  }
}

void setupWatchdog() {
  const esp_err_t initResult =
      esp_task_wdt_init(Timing::kWatchdogSeconds, true);
  if (initResult != ESP_OK && initResult != ESP_ERR_INVALID_STATE) {
    Serial.printf("Task Watchdog initialization failed: %d\n", initResult);
    return;
  }
  if (esp_task_wdt_status(nullptr) != ESP_OK) {
    const esp_err_t addResult = esp_task_wdt_add(nullptr);
    if (addResult != ESP_OK) {
      Serial.printf("Task Watchdog subscription failed: %d\n", addResult);
      return;
    }
  }
  health.watchdogEnabled = true;
  Serial.printf("Task Watchdog enabled: %lu seconds\n",
                static_cast<unsigned long>(Timing::kWatchdogSeconds));
}

void serviceBootHealth(uint64_t now) {
  if (health.bootMarkedHealthy || health.safeMode || ota.inProgress ||
      now - health.bootStartedMs < Timing::kBootHealthyMs) {
    return;
  }
  preferences.putUChar("failed", 0);
  health.bootMarkedHealthy = true;
  Serial.println(F("Boot marked healthy"));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("TeleWOL High-Speed Dual-Core Controller starting"));
  health.bootStartedMs = monotonicMs();
  setupRecovery();
  setupWatchdog();
  initializeTargets();
  buildUiKeyboardCache(ui, targets, Limits::kTargetCount);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  configureWiFiAddress();
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  network.lastRetryMs = monotonicMs();

  const uint64_t startupStartedMs = monotonicMs();
  while (WiFi.status() != WL_CONNECTED &&
         monotonicMs() - startupStartedMs < Timing::kWifiStartupTimeoutMs) {
    delay(Timing::kStartupPollMs);
    feedWatchdog();
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(
        F("Wi-Fi startup timed out; background recovery remains active."));
  }
}

void loop() {
  const uint64_t now = monotonicMs();
  serviceWiFi(now);

  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
    if (!ota.inProgress) {
      if (telegramFast.consumeOutageRecovery()) {
        if (!health.safeMode) {
          runParallelStatusPing();
        }
        queueScreen(ui.screen, "🔄 Internet recovered; stale backlog cleared.");
      }

      TelegramInboundEvent event;
      while (telegramFast.pollInboundEvent(event)) {
        handleInboundEvent(event, monotonicMs());
      }

      serviceAutoWake(now);
      serviceAutomaticStatus(now);

      bool bootUiUpdate = false;
      uint32_t trackPingMs = 0;
      serviceWolAndBootTracking(udp, targets, Limits::kTargetCount,
                                network.broadcast, now, bootUiUpdate,
                                trackPingMs);
      if (trackPingMs > 0) {
        performance.lastPingMs = trackPingMs;
      }
      if (bootUiUpdate && ui.screen == Screen::kMain) {
        queueScreen(Screen::kMain);
      }

      serviceDashboardDispatch(monotonicMs());
      serviceBootHealth(now);
    }
  }

  feedWatchdog();
  finishLoopTiming(now);
  delay(2);
}
