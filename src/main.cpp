#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <UniversalTelegramBot.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <cctype>
#include <cstring>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include "BoundedPing.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "include/secrets.h not found. Run tools/generate_secrets.py first."
#endif

#ifndef DEBUG_PERFORMANCE
#define DEBUG_PERFORMANCE 0
#endif

namespace Timing {
constexpr uint32_t kWifiStartupTimeoutMs = 18000;
constexpr uint32_t kWifiRetryMs = 5000;
constexpr uint32_t kTelegramPollMs = 700;
constexpr uint32_t kSafeModeTelegramPollMs = 5000;
constexpr uint32_t kTelegramFirstBackoffMs = 2000;
constexpr uint32_t kTelegramContinuedBackoffMs = 5000;
constexpr uint32_t kButtonDebounceMs = 300;
constexpr uint32_t kStatusRefreshCooldownMs = 1000;
constexpr uint32_t kWolCooldownMs = 3000;
constexpr uint32_t kWolRepeatMs = 100;
constexpr uint32_t kPingTimeoutMs = 600;
constexpr uint32_t kBootHealthyMs = 60000;
constexpr uint32_t kSlowLoopMs = 250;
constexpr uint32_t kSlowOperationMs = 100;
constexpr uint32_t kWatchdogSeconds = 15;
constexpr uint32_t kTelegramClientTimeoutSeconds = 2;
constexpr uint32_t kTelegramHandshakeTimeoutSeconds = 3;
constexpr uint32_t kTelegramResponseWaitMs = 1200;
constexpr uint32_t kStartupPollMs = 100;
}  // namespace Timing

namespace Limits {
constexpr size_t kTargetCount = 2;
constexpr uint8_t kWolPacketCount = 5;
constexpr uint16_t kWolPort = 9;
constexpr uint8_t kSafeModeBootCount = 3;
constexpr size_t kTelegramPayloadBytes = 2304;
constexpr size_t kTelegramResponseBytes = 4096;
constexpr int kTelegramMessageBytes = 3000;
}  // namespace Limits

namespace Callback {
constexpr char kMain[] = "refresh_menu";
constexpr char kEspStatus[] = "esp_status";
constexpr char kAutoWake[] = "auto_wake_status";
constexpr char kStatusAll[] = "status_all";
constexpr char kRefreshAll[] = "refresh_status_all";
constexpr char kWakeWindows[] = "wake_win";
constexpr char kWakeLinux[] = "wake_linux";
constexpr char kStatusWindows[] = "status_win";
constexpr char kStatusLinux[] = "status_linux";
constexpr char kRefreshWindows[] = "refresh_status_win";
constexpr char kRefreshLinux[] = "refresh_status_linux";
}  // namespace Callback

namespace Command {
constexpr char kStart[] = "/start";
constexpr char kHelp[] = "/help";
constexpr char kEspStatus[] = "/status";
constexpr char kAutoWake[] = "/auto_wake";
constexpr char kStatusAll[] = "/status_all";
constexpr char kWakeWindows[] = "/wake_win";
constexpr char kWakeLinux[] = "/wake_linux";
constexpr char kStatusWindows[] = "/status_win";
constexpr char kStatusLinux[] = "/status_linux";
}  // namespace Command

namespace Diagnostics {
constexpr bool kPerformance = DEBUG_PERFORMANCE != 0;
}  // namespace Diagnostics

enum class TargetId : uint8_t { kWindows, kLinux, kNone = UINT8_MAX };
enum class TargetState : uint8_t { kNotConfigured, kUnknown, kOnline, kOffline };
enum class AutoWakeResult : uint8_t {
  kPending,
  kDisabled,
  kNotConfigured,
  kSkippedReset,
  kAlreadyOnline,
  kWakeSent,
  kWakeFailed,
};
enum class ApplicationState : uint8_t {
  kBooting,
  kWaitingForWiFi,
  kOnline,
  kOtaUpdating,
  kSafeMode,
};
enum class Screen : uint8_t {
  kMain,
  kTargetStatus,
  kAllStatus,
  kEspStatus,
  kAutoWake,
  kActionResult,
  kSafeMode,
};
enum class Action : uint8_t {
  kNone,
  kMain,
  kEspStatus,
  kAutoWake,
  kTargetStatus,
  kAllStatus,
  kWakeTarget,
  kUnknown,
};
enum class StatusJobType : uint8_t { kNone, kTarget, kAll };
enum class TelegramResultKind : uint8_t {
  kSuccess,
  kNotModified,
  kApiError,
  kNetworkError,
  kParseError,
};

struct TargetConfig {
  const char* name;
  const char* macText;
  const char* ipText;
  bool autoWake;
  const char* wakeCallback;
  const char* statusCallback;
  const char* refreshCallback;
  const char* wakeCommand;
  const char* statusCommand;
};

struct TargetRuntime {
  bool configured = false;
  IPAddress ip;
  uint8_t mac[6]{};
  TargetState state = TargetState::kNotConfigured;
  AutoWakeResult autoWakeResult = AutoWakeResult::kNotConfigured;
  float responseTimeMs = 0.0F;
  uint32_t lastCheckMs = 0;
  uint32_t lastSuccessfulCheckMs = 0;
  uint32_t lastWakeMs = 0;
};

struct Target {
  TargetConfig config;
  TargetRuntime runtime;
};

struct ParsedAction {
  Action action;
  TargetId target;

  ParsedAction(Action actionValue = Action::kNone,
               TargetId targetValue = TargetId::kNone)
      : action(actionValue), target(targetValue) {}
};

struct TelegramUpdate {
  bool isCallback = false;
  int32_t updateId = 0;
  int messageId = 0;
  String chatId;
  String text;
  String queryId;
};

struct TelegramResult {
  TelegramResultKind kind = TelegramResultKind::kNetworkError;
  int messageId = 0;
};

struct NetworkRuntime {
  bool wasConnected = false;
  bool connectedBefore = false;
  uint32_t lastRetryMs = 0;
  IPAddress broadcast;
};

struct TelegramRuntime {
  bool initialized = false;
  bool reachable = false;
  bool backoffActive = false;
  uint8_t failureCount = 0;
  uint32_t lastPollMs = 0;
  uint32_t lastFailureMs = 0;
  uint32_t retryDelayMs = 0;
  int32_t lastUpdateId = 0;
  int dashboardMessageId = 0;
  String dashboardChatId;
  String lastDashboardText;
  String lastDashboardKeyboard;
  String lastCallbackQueryId;
  String pollCommand;
  TelegramUpdate update;
};

struct UiRuntime {
  Screen screen = Screen::kMain;
  TargetId target = TargetId::kNone;
  bool updatePending = false;
  bool fallbackAllowed = false;
  String actionResult;
  String mainKeyboard;
  String backKeyboard;
  String allStatusKeyboard;
  String safeModeKeyboard;
  String targetStatusKeyboards[Limits::kTargetCount];
};

struct StatusJob {
  StatusJobType type = StatusJobType::kNone;
  TargetId target = TargetId::kNone;
  uint8_t nextTargetIndex = 0;
};

struct WolJob {
  bool active = false;
  bool automatic = false;
  bool anyPacketSent = false;
  TargetId target = TargetId::kNone;
  uint8_t packetsSent = 0;
  uint32_t lastSendMs = 0;
};

struct AutoWakeJob {
  bool pending = false;
  bool active = false;
  bool handled = false;
  uint8_t nextTargetIndex = 0;
  uint32_t startedMs = 0;
};

struct ActionRuntime {
  StatusJob status;
  WolJob wol;
  AutoWakeJob autoWake;
  ParsedAction lastButton;
  uint32_t lastButtonMs = 0;
  uint32_t lastStatusRequestMs = 0;
};

struct OtaRuntime {
  bool initialized = false;
  bool inProgress = false;
  uint8_t lastProgress = UINT8_MAX;
};

struct HealthRuntime {
  bool safeMode = false;
  bool bootMarkedHealthy = false;
  bool watchdogEnabled = false;
  uint32_t bootStartedMs = 0;
  esp_reset_reason_t resetReason = ESP_RST_UNKNOWN;
  ApplicationState applicationState = ApplicationState::kBooting;
};

struct PerformanceRuntime {
  uint32_t lastLoopMs = 0;
  uint32_t maximumLoopMs = 0;
};

Target targets[Limits::kTargetCount] = {
    {{"Windows", WINDOWS_MAC, WINDOWS_IP, AUTO_WAKE_WINDOWS,
      Callback::kWakeWindows, Callback::kStatusWindows,
      Callback::kRefreshWindows, Command::kWakeWindows,
      Command::kStatusWindows}, {}},
    {{"Linux", LINUX_MAC, LINUX_IP, AUTO_WAKE_LINUX,
      Callback::kWakeLinux, Callback::kStatusLinux,
      Callback::kRefreshLinux, Command::kWakeLinux,
      Command::kStatusLinux}, {}},
};

WiFiClientSecure telegramClient;
UniversalTelegramBot bot(TELEGRAM_BOT_TOKEN, telegramClient);
WiFiUDP udp;
Preferences preferences;
DynamicJsonDocument telegramPayload(Limits::kTelegramPayloadBytes);
DynamicJsonDocument telegramResponse(Limits::kTelegramResponseBytes);
DynamicJsonDocument telegramUpdateDocument(Limits::kTelegramResponseBytes);

NetworkRuntime network;
TelegramRuntime telegram;
UiRuntime ui;
ActionRuntime actions;
OtaRuntime ota;
HealthRuntime health;
PerformanceRuntime performance;

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

bool isStatusAction(Action action) {
  return action == Action::kTargetStatus || action == Action::kAllStatus;
}

bool isNetworkAction(Action action) {
  return isStatusAction(action) || action == Action::kWakeTarget;
}

bool actionBusy() {
  return actions.status.type != StatusJobType::kNone || actions.wol.active ||
         actions.autoWake.active;
}

bool telegramRequestAllowed(uint32_t now) {
  return !telegram.backoffActive ||
         now - telegram.lastFailureMs >= telegram.retryDelayMs;
}

void feedWatchdog() {
  if (health.watchdogEnabled) esp_task_wdt_reset();
}

void logOperationDuration(const char* operation, uint32_t startedMs) {
  if (!Diagnostics::kPerformance) return;
  const uint32_t durationMs = millis() - startedMs;
  if (durationMs >= Timing::kSlowOperationMs) {
    Serial.printf("[PERF] %s: %lu ms\n", operation,
                  static_cast<unsigned long>(durationMs));
  }
}

void finishLoopTiming(uint32_t startedMs) {
  performance.lastLoopMs = millis() - startedMs;
  if (performance.lastLoopMs > performance.maximumLoopMs) {
    performance.maximumLoopMs = performance.lastLoopMs;
  }
  if (Diagnostics::kPerformance &&
      performance.lastLoopMs >= Timing::kSlowLoopMs) {
    Serial.printf("[WARN] Slow loop: %lu ms\n",
                  static_cast<unsigned long>(performance.lastLoopMs));
  }
}

String resetReasonText() {
  switch (health.resetReason) {
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

String applicationStateText() {
  switch (health.applicationState) {
    case ApplicationState::kBooting: return F("BOOTING");
    case ApplicationState::kWaitingForWiFi: return F("WAITING_FOR_WIFI");
    case ApplicationState::kOnline: return F("ONLINE");
    case ApplicationState::kOtaUpdating: return F("OTA_UPDATING");
    case ApplicationState::kSafeMode: return F("SAFE_MODE");
    default: return F("UNKNOWN");
  }
}

const __FlashStringHelper* targetStateText(TargetState state) {
  switch (state) {
    case TargetState::kOnline: return F("Online");
    case TargetState::kOffline: return F("Offline");
    case TargetState::kNotConfigured: return F("Not configured");
    default: return F("Unknown");
  }
}

const __FlashStringHelper* autoWakeResultText(AutoWakeResult result) {
  switch (result) {
    case AutoWakeResult::kDisabled: return F("Disabled");
    case AutoWakeResult::kNotConfigured: return F("Not configured");
    case AutoWakeResult::kSkippedReset: return F("Skipped for this reset");
    case AutoWakeResult::kAlreadyOnline: return F("Already online; WOL skipped");
    case AutoWakeResult::kWakeSent: return F("Wake packets sent");
    case AutoWakeResult::kWakeFailed: return F("Wake packet failed");
    default: return F("Waiting");
  }
}

String formattedUptime(uint32_t now) {
  uint32_t totalSeconds = now / 1000U;
  const uint32_t days = totalSeconds / 86400U;
  totalSeconds %= 86400U;
  const uint32_t hours = totalSeconds / 3600U;
  totalSeconds %= 3600U;
  const uint32_t minutes = totalSeconds / 60U;
  const uint32_t seconds = totalSeconds % 60U;
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%lud %luh %lum %lus",
           static_cast<unsigned long>(days), static_cast<unsigned long>(hours),
           static_cast<unsigned long>(minutes),
           static_cast<unsigned long>(seconds));
  return String(buffer);
}

String ageText(uint32_t timestamp, uint32_t now) {
  if (timestamp == 0) return F("never");
  const uint32_t ageSeconds = (now - timestamp) / 1000U;
  if (ageSeconds < 2) return F("just now");
  if (ageSeconds < 60) return String(ageSeconds) + F("s ago");
  if (ageSeconds < 3600) return String(ageSeconds / 60U) + F("m ago");
  return String(ageSeconds / 3600U) + F("h ago");
}

const __FlashStringHelper* signalQuality(int rssi) {
  if (rssi >= -50) return F("Excellent");
  if (rssi >= -60) return F("Good");
  if (rssi >= -70) return F("Fair");
  return F("Weak");
}

uint8_t hexValue(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  value = static_cast<char>(tolower(static_cast<unsigned char>(value)));
  return static_cast<uint8_t>(value - 'a' + 10);
}

bool parseMacAddress(const char* text, uint8_t (&mac)[6]) {
  if (text == nullptr || strlen(text) != 17) return false;
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
    const bool hasPair = target.config.macText[0] != '\0' &&
                         target.config.ipText[0] != '\0';
    target.runtime.configured = hasPair &&
        target.runtime.ip.fromString(target.config.ipText) &&
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

void addKeyboardButton(JsonArray row, const char* text, const char* callback) {
  JsonObject button = row.createNestedObject();
  button["text"] = text;
  button["callback_data"] = callback;
}

void buildKeyboardCache() {
  auto buildNavigationKeyboard = [](String& output, const char* primaryLabel,
                                    const char* primaryCallback, bool addBack) {
    DynamicJsonDocument document(256);
    JsonArray keyboard = document.to<JsonArray>();
    JsonArray primaryRow = keyboard.createNestedArray();
    addKeyboardButton(primaryRow, primaryLabel, primaryCallback);
    if (addBack) {
      JsonArray backRow = keyboard.createNestedArray();
      addKeyboardButton(backRow, "Back", Callback::kMain);
    }
    output.reserve(192);
    serializeJson(document, output);
  };
  buildNavigationKeyboard(ui.backKeyboard, "Back", Callback::kMain, false);
  buildNavigationKeyboard(ui.allStatusKeyboard, "Refresh", Callback::kRefreshAll,
                          true);
  buildNavigationKeyboard(ui.safeModeKeyboard, "Refresh", Callback::kEspStatus,
                          false);

  DynamicJsonDocument keyboardDocument(768);
  JsonArray keyboard = keyboardDocument.to<JsonArray>();
  size_t configuredTargetCount = 0;
  for (const Target& target : targets) {
    if (target.runtime.configured) ++configuredTargetCount;
  }
  if (configuredTargetCount > 0) {
    JsonArray wakeRow = keyboard.createNestedArray();
    JsonArray statusRow = keyboard.createNestedArray();
    for (const Target& target : targets) {
      if (!target.runtime.configured) continue;
      String wakeLabel = F("Wake ");
      wakeLabel += target.config.name;
      String statusLabel = F("Status ");
      statusLabel += target.config.name;
      addKeyboardButton(wakeRow, wakeLabel.c_str(), target.config.wakeCallback);
      addKeyboardButton(statusRow, statusLabel.c_str(), target.config.statusCallback);
    }
  }
  if (configuredTargetCount > 1) {
    JsonArray allRow = keyboard.createNestedArray();
    addKeyboardButton(allRow, "Status All", Callback::kStatusAll);
  }
  JsonArray autoRow = keyboard.createNestedArray();
  addKeyboardButton(autoRow, "Auto Wake", Callback::kAutoWake);
  JsonArray espRow = keyboard.createNestedArray();
  addKeyboardButton(espRow, "ESP32 Status", Callback::kEspStatus);
  ui.mainKeyboard.reserve(700);
  serializeJson(keyboardDocument, ui.mainKeyboard);

  for (size_t index = 0; index < Limits::kTargetCount; ++index) {
    buildNavigationKeyboard(ui.targetStatusKeyboards[index], "Refresh",
                            targets[index].config.refreshCallback, true);
  }
}

uint32_t latestCheckTime() {
  uint32_t latest = 0;
  for (const Target& target : targets) {
    if (target.runtime.lastCheckMs > latest) latest = target.runtime.lastCheckMs;
  }
  return latest;
}

String buildMainDashboard(uint32_t now) {
  String message;
  message.reserve(320);
  message += F("TeleWOL\n\n");
  for (const Target& target : targets) {
    if (!target.runtime.configured) continue;
    message += target.config.name;
    message += F(": ");
    message += targetStateText(target.runtime.state);
    message += '\n';
  }
  message += F("\nLast status check: ");
  message += ageText(latestCheckTime(), now);
  message += F("\n\nESP32: Online\nWi-Fi: ");
  message += String(WiFi.RSSI());
  message += F(" dBm");
  return message;
}

String buildTargetStatusScreen(const Target& target, uint32_t now) {
  String message;
  message.reserve(280);
  message += target.config.name;
  message += F(" Status\n\nStatus: ");
  message += targetStateText(target.runtime.state);
  if (target.runtime.configured) {
    message += F("\nIP: ");
    message += target.runtime.ip.toString();
  }
  if (target.runtime.state == TargetState::kOnline) {
    message += F("\nPing: ");
    message += String(target.runtime.responseTimeMs, 1);
    message += F(" ms");
  } else if (target.runtime.state == TargetState::kOffline) {
    message += F(" / Unreachable");
  } else if (target.runtime.state == TargetState::kUnknown) {
    message += F("\nCheck could not complete.");
  }
  message += F("\n\nLast check: ");
  message += ageText(target.runtime.lastCheckMs, now);
  message += F("\nLast successful reply: ");
  message += ageText(target.runtime.lastSuccessfulCheckMs, now);
  return message;
}

String buildAllStatusScreen(uint32_t now) {
  String message;
  message.reserve(320);
  message += F("PC Status\n\n");
  for (const Target& target : targets) {
    if (!target.runtime.configured) continue;
    message += target.config.name;
    message += F(": ");
    message += targetStateText(target.runtime.state);
    message += '\n';
  }
  message += F("\nLast check: ");
  message += ageText(latestCheckTime(), now);
  return message;
}

String buildEspStatusScreen(uint32_t now) {
  String message;
  message.reserve(700);
  message += F("ESP32 Status\n\nState: ");
  message += applicationStateText();
  message += F("\nLast Reset: ");
  message += resetReasonText();
  message += F("\nWatchdog: ");
  message += health.watchdogEnabled ? F("Enabled") : F("Unavailable");
  message += F("\nSafe Mode: ");
  message += health.safeMode ? F("Yes") : F("No");
  message += F("\nWi-Fi: ");
  message += WiFi.status() == WL_CONNECTED ? F("Connected") : F("Disconnected");
  message += F("\nTelegram: ");
  message += telegram.reachable ? F("Reachable") : F("Temporarily unavailable");
  if (WiFi.status() == WL_CONNECTED) {
    const int rssi = WiFi.RSSI();
    message += F("\nSSID: ");
    message += WiFi.SSID();
    message += F("\nIP: ");
    message += WiFi.localIP().toString();
    message += F("\nRSSI: ");
    message += String(rssi);
    message += F(" dBm (");
    message += signalQuality(rssi);
    message += F(")\nGateway: ");
    message += WiFi.gatewayIP().toString();
    message += F("\nSubnet: ");
    message += WiFi.subnetMask().toString();
  }
  message += F("\nUptime: ");
  message += formattedUptime(now);
  message += F("\n\nMemory\nFree Heap: ");
  message += String(ESP.getFreeHeap() / 1024U);
  message += F(" KB\nMin Free Heap: ");
  message += String(ESP.getMinFreeHeap() / 1024U);
  message += F(" KB\nLargest Block: ");
  message += String(ESP.getMaxAllocHeap() / 1024U);
  message += F(" KB\n\nPerformance\nLast loop: ");
  message += String(performance.lastLoopMs);
  message += F(" ms\nMaximum loop: ");
  message += String(performance.maximumLoopMs);
  message += F(" ms");
  return message;
}

String buildAutoWakeScreen() {
  String message;
  message.reserve(420);
  message += F("Auto Wake\n\n");
  for (const Target& target : targets) {
    message += target.config.name;
    message += F(": ");
    if (!target.runtime.configured) message += F("Not configured");
    else message += target.config.autoWake ? F("Enabled") : F("Disabled");
    message += '\n';
  }
  message += F("Delay: ");
  message += String(AUTO_WAKE_DELAY_SECONDS);
  message += F(" seconds\n\nLast startup workflow\n");
  for (const Target& target : targets) {
    message += target.config.name;
    message += F(": ");
    message += autoWakeResultText(target.runtime.autoWakeResult);
    message += '\n';
  }
  return message;
}

String buildSafeModeScreen() {
  String message;
  message.reserve(350);
  message += F("TeleWOL SAFE MODE\n\nRepeated unhealthy boots were detected.\n\nWi-Fi: ");
  message += WiFi.status() == WL_CONNECTED ? F("Connected") : F("Disconnected");
  message += F("\nOTA: ");
  message += ota.initialized ? F("Ready") : F("Waiting for Wi-Fi");
  message += F("\nReset reason: ");
  message += resetReasonText();
  message += F("\n\nUpload corrected firmware using ArduinoOTA or USB.");
  return message;
}

String renderCurrentScreen(uint32_t now) {
  switch (ui.screen) {
    case Screen::kTargetStatus: {
      const Target* target = findTarget(ui.target);
      return target == nullptr ? String(F("Target not configured."))
                               : buildTargetStatusScreen(*target, now);
    }
    case Screen::kAllStatus: return buildAllStatusScreen(now);
    case Screen::kEspStatus: return buildEspStatusScreen(now);
    case Screen::kAutoWake: return buildAutoWakeScreen();
    case Screen::kActionResult: return ui.actionResult;
    case Screen::kSafeMode: return buildSafeModeScreen();
    default: return buildMainDashboard(now);
  }
}

const String& keyboardForCurrentScreen() {
  if (ui.screen == Screen::kMain) return ui.mainKeyboard;
  if (ui.screen == Screen::kAllStatus) return ui.allStatusKeyboard;
  if (ui.screen == Screen::kSafeMode) return ui.safeModeKeyboard;
  if (ui.screen == Screen::kTargetStatus) {
    const uint8_t index = static_cast<uint8_t>(ui.target);
    if (index < Limits::kTargetCount) return ui.targetStatusKeyboards[index];
  }
  return ui.backKeyboard;
}

void queueScreen(Screen screen, TargetId target = TargetId::kNone,
                 bool allowFallback = true) {
  ui.screen = screen;
  ui.target = target;
  ui.updatePending = true;
  ui.fallbackAllowed = allowFallback;
}

void showActionResult(const String& text) {
  ui.actionResult = text;
  queueScreen(Screen::kActionResult);
}

void recordTelegramSuccess() {
  telegram.reachable = true;
  telegram.backoffActive = false;
  telegram.failureCount = 0;
}

void recordTelegramFailure(uint32_t now) {
  telegram.reachable = false;
  telegram.backoffActive = true;
  telegram.lastFailureMs = now;
  if (telegram.failureCount < UINT8_MAX) ++telegram.failureCount;
  telegram.retryDelayMs = telegram.failureCount == 1
                              ? Timing::kTelegramFirstBackoffMs
                              : Timing::kTelegramContinuedBackoffMs;
  Serial.printf("Telegram unavailable; retrying in %lu ms\n",
                static_cast<unsigned long>(telegram.retryDelayMs));
}

TelegramResult telegramPost(const char* method, JsonObject payload,
                            const char* timingLabel) {
  const uint32_t startedMs = millis();
  const String response = bot.sendPostToTelegram(bot.buildCommand(method), payload);
  telegramClient.stop();
  logOperationDuration(timingLabel, startedMs);

  TelegramResult result;
  if (response.length() == 0) {
    result.kind = TelegramResultKind::kNetworkError;
    recordTelegramFailure(millis());
    return result;
  }

  telegramResponse.clear();
  if (deserializeJson(telegramResponse, response)) {
    result.kind = TelegramResultKind::kParseError;
    recordTelegramFailure(millis());
    return result;
  }

  recordTelegramSuccess();
  if (telegramResponse["ok"] | false) {
    result.kind = TelegramResultKind::kSuccess;
    result.messageId = telegramResponse["result"]["message_id"] | 0;
    return result;
  }
  const String description = telegramResponse["description"].as<String>();
  result.kind = description.indexOf("message is not modified") >= 0
                    ? TelegramResultKind::kNotModified
                    : TelegramResultKind::kApiError;
  return result;
}

TelegramResult sendOrEditDashboard(const String& text, const String& keyboard,
                                   bool editExisting) {
  telegramPayload.clear();
  telegramPayload["chat_id"] = telegram.dashboardChatId;
  telegramPayload["text"] = text;
  if (editExisting) telegramPayload["message_id"] = telegram.dashboardMessageId;
  JsonObject replyMarkup = telegramPayload.createNestedObject("reply_markup");
  replyMarkup["inline_keyboard"] = serialized(keyboard);
  return telegramPost(editExisting ? "editMessageText" : "sendMessage",
                      telegramPayload.as<JsonObject>(),
                      editExisting ? "Edit dashboard" : "Send dashboard");
}

void saveDashboardIdentity() {
  preferences.putInt("dashId", telegram.dashboardMessageId);
  preferences.putString("dashChat", telegram.dashboardChatId);
}

bool serviceDashboard(uint32_t now) {
  if (!ui.updatePending || ota.inProgress || !telegram.initialized ||
      !telegramRequestAllowed(now)) {
    return false;
  }
  if (telegram.dashboardChatId.length() == 0) {
    telegram.dashboardChatId = TELEGRAM_CHAT_ID;
  }

  const String text = renderCurrentScreen(now);
  const String& keyboard = keyboardForCurrentScreen();
  const bool editExisting = telegram.dashboardMessageId > 0;
  if (editExisting && text == telegram.lastDashboardText &&
      keyboard == telegram.lastDashboardKeyboard) {
    ui.updatePending = false;
    return false;
  }

  const TelegramResult result = sendOrEditDashboard(text, keyboard, editExisting);
  if (result.kind == TelegramResultKind::kSuccess ||
      result.kind == TelegramResultKind::kNotModified) {
    if (!editExisting && result.messageId > 0) {
      telegram.dashboardMessageId = result.messageId;
      saveDashboardIdentity();
    }
    telegram.lastDashboardText = text;
    telegram.lastDashboardKeyboard = keyboard;
    ui.updatePending = false;
  } else if (editExisting && result.kind == TelegramResultKind::kApiError &&
             ui.fallbackAllowed) {
    telegram.dashboardMessageId = 0;
    telegram.lastDashboardText = "";
    telegram.lastDashboardKeyboard = "";
    ui.fallbackAllowed = false;
  } else {
    ui.updatePending = false;
  }
  return true;
}

void answerCallback(const String& queryId, const char* text = nullptr,
                    bool showAlert = false) {
  telegramPayload.clear();
  telegramPayload["callback_query_id"] = queryId;
  telegramPayload["show_alert"] = showAlert;
  telegramPayload["cache_time"] = 0;
  if (text != nullptr && text[0] != '\0') telegramPayload["text"] = text;
  telegramPost("answerCallbackQuery", telegramPayload.as<JsonObject>(),
               "Callback ACK");
}

void sendOneShotText(const String& chatId, const __FlashStringHelper* text) {
  telegramPayload.clear();
  telegramPayload["chat_id"] = chatId;
  telegramPayload["text"] = text;
  telegramPost("sendMessage", telegramPayload.as<JsonObject>(), "Telegram send");
}

String normalizeCommand(String command) {
  command.trim();
  const int suffix = command.indexOf('@');
  if (suffix >= 0) command.remove(suffix);
  return command;
}

ParsedAction parseAction(String value, bool callback) {
  if (!callback) value = normalizeCommand(value);
  if (value == (callback ? Callback::kMain : Command::kStart) ||
      (!callback && value == Command::kHelp)) {
    return {Action::kMain, TargetId::kNone};
  }
  if (value == (callback ? Callback::kEspStatus : Command::kEspStatus)) {
    return {Action::kEspStatus, TargetId::kNone};
  }
  if (value == (callback ? Callback::kAutoWake : Command::kAutoWake)) {
    return {Action::kAutoWake, TargetId::kNone};
  }
  if (value == (callback ? Callback::kStatusAll : Command::kStatusAll) ||
      (callback && value == Callback::kRefreshAll)) {
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

void adoptDashboardMessage(const TelegramUpdate& update) {
  if (update.messageId <= 0) return;
  if (telegram.dashboardMessageId == update.messageId &&
      telegram.dashboardChatId == update.chatId) {
    return;
  }
  telegram.dashboardMessageId = update.messageId;
  telegram.dashboardChatId = update.chatId;
  telegram.lastDashboardText = "";
  telegram.lastDashboardKeyboard = "";
  saveDashboardIdentity();
}

bool statusRequestAllowed(uint32_t now) {
  return actions.lastStatusRequestMs == 0 ||
         now - actions.lastStatusRequestMs >= Timing::kStatusRefreshCooldownMs;
}

void startStatusJob(const ParsedAction& parsed, uint32_t now) {
  if (!statusRequestAllowed(now)) {
    showActionResult(F("Status refresh requested too quickly. Please wait."));
    return;
  }
  actions.lastStatusRequestMs = now;
  if (parsed.action == Action::kAllStatus) {
    actions.status.type = StatusJobType::kAll;
    actions.status.target = TargetId::kNone;
    actions.status.nextTargetIndex = 0;
    return;
  }
  Target* target = findTarget(parsed.target);
  if (target == nullptr || !target->runtime.configured) {
    showActionResult(F("Target is not configured."));
    return;
  }
  actions.status.type = StatusJobType::kTarget;
  actions.status.target = parsed.target;
  actions.status.nextTargetIndex = 0;
}

void startWolJob(TargetId targetId, bool automatic, uint32_t now) {
  Target* target = findTarget(targetId);
  if (target == nullptr || !target->runtime.configured) {
    showActionResult(F("Target is not configured."));
    return;
  }
  if (!automatic && target->runtime.lastWakeMs != 0 &&
      now - target->runtime.lastWakeMs < Timing::kWolCooldownMs) {
    showActionResult(F("Wake requested recently. Please wait a moment."));
    return;
  }
  if (!automatic) target->runtime.lastWakeMs = now;
  actions.wol.active = true;
  actions.wol.automatic = automatic;
  actions.wol.anyPacketSent = false;
  actions.wol.target = targetId;
  actions.wol.packetsSent = 0;
  actions.wol.lastSendMs = now;
}

void dispatchAction(const ParsedAction& parsed, uint32_t now) {
  switch (parsed.action) {
    case Action::kMain:
      queueScreen(Screen::kMain);
      break;
    case Action::kEspStatus:
      queueScreen(health.safeMode ? Screen::kSafeMode : Screen::kEspStatus);
      break;
    case Action::kAutoWake:
      queueScreen(Screen::kAutoWake);
      break;
    case Action::kTargetStatus:
    case Action::kAllStatus:
      startStatusJob(parsed, now);
      break;
    case Action::kWakeTarget:
      startWolJob(parsed.target, false, now);
      break;
    default:
      showActionResult(F("Unknown command. Use /help to open control panel."));
      break;
  }
}

void handleCallbackUpdate(const TelegramUpdate& update, uint32_t now) {
  if (update.chatId != TELEGRAM_CHAT_ID) {
    Serial.printf("Unauthorized Telegram callback from chat ID %s\n",
                  update.chatId.c_str());
    answerCallback(update.queryId, "Access denied.", true);
    return;
  }
  if (update.queryId == telegram.lastCallbackQueryId) {
    answerCallback(update.queryId, "Already handled.");
    return;
  }
  telegram.lastCallbackQueryId = update.queryId;
  const ParsedAction parsed = parseAction(update.text, true);
  if (sameAction(parsed, actions.lastButton) && actions.lastButtonMs != 0 &&
      now - actions.lastButtonMs < Timing::kButtonDebounceMs) {
    answerCallback(update.queryId, "Please wait.");
    return;
  }
  actions.lastButton = parsed;
  actions.lastButtonMs = now;
  if (ota.inProgress) {
    answerCallback(update.queryId, "OTA update is in progress.");
    return;
  }
  if (isNetworkAction(parsed.action) && actionBusy()) {
    answerCallback(update.queryId, "Another action is running.");
    return;
  }

  // Acknowledge before any ping or WOL work.
  answerCallback(update.queryId);
  adoptDashboardMessage(update);
  if (health.safeMode) queueScreen(Screen::kSafeMode);
  else dispatchAction(parsed, now);
}

void handleCommandUpdate(const TelegramUpdate& update, uint32_t now) {
  if (update.chatId != TELEGRAM_CHAT_ID) {
    Serial.printf("Unauthorized Telegram access attempt from chat ID %s\n",
                  update.chatId.c_str());
    sendOneShotText(update.chatId, F("Access denied. This chat is not authorized."));
    return;
  }
  telegram.dashboardChatId = update.chatId;
  if (health.safeMode) {
    queueScreen(Screen::kSafeMode);
    return;
  }
  const ParsedAction parsed = parseAction(update.text, false);
  if (isNetworkAction(parsed.action) && actionBusy()) {
    showActionResult(F("Another network action is already running. Please wait."));
    return;
  }
  dispatchAction(parsed, now);
}

bool parseTelegramUpdate(const String& response, TelegramUpdate& update) {
  update.isCallback = false;
  update.updateId = 0;
  update.messageId = 0;
  update.chatId.remove(0);
  update.text.remove(0);
  update.queryId.remove(0);
  telegramUpdateDocument.clear();
  if (deserializeJson(telegramUpdateDocument, response) ||
      !(telegramUpdateDocument["ok"] | false)) {
    return false;
  }
  JsonArray results = telegramUpdateDocument["result"].as<JsonArray>();
  if (results.isNull() || results.size() == 0) return true;

  JsonObject result = results[0];
  update.updateId = result["update_id"] | 0;
  if (result.containsKey("callback_query")) {
    JsonObject callback = result["callback_query"];
    update.isCallback = true;
    update.queryId = callback["id"].as<String>();
    update.text = callback["data"].as<String>();
    update.chatId = callback["message"]["chat"]["id"].as<String>();
    update.messageId = callback["message"]["message_id"] | 0;
  } else if (result.containsKey("message")) {
    JsonObject message = result["message"];
    update.chatId = message["chat"]["id"].as<String>();
    update.text = message["text"].as<String>();
    update.messageId = message["message_id"] | 0;
  }
  return true;
}

bool serviceTelegram(uint32_t now) {
  if (!telegram.initialized || ota.inProgress ||
      !telegramRequestAllowed(now)) {
    return false;
  }
  const uint32_t pollInterval = health.safeMode
                                    ? Timing::kSafeModeTelegramPollMs
                                    : Timing::kTelegramPollMs;
  if (telegram.lastPollMs != 0 && now - telegram.lastPollMs < pollInterval) {
    return false;
  }
  telegram.lastPollMs = now;
  telegram.pollCommand = F("getUpdates?offset=");
  telegram.pollCommand += String(telegram.lastUpdateId + 1);
  telegram.pollCommand += F("&limit=1&timeout=0");

  const uint32_t startedMs = millis();
  const String response = bot.sendGetToTelegram(bot.buildCommand(telegram.pollCommand));
  telegramClient.stop();
  logOperationDuration("Telegram getUpdates", startedMs);
  if (response.length() == 0) {
    recordTelegramFailure(millis());
    return true;
  }

  if (!parseTelegramUpdate(response, telegram.update)) {
    recordTelegramFailure(millis());
    return true;
  }
  recordTelegramSuccess();
  if (telegram.update.updateId <= telegram.lastUpdateId) return true;
  telegram.lastUpdateId = telegram.update.updateId;
  preferences.putLong("tgUpdate", telegram.lastUpdateId);
  const uint32_t dispatchTime = millis();
  if (telegram.update.isCallback) {
    handleCallbackUpdate(telegram.update, dispatchTime);
  } else if (telegram.update.text.length() > 0) {
    handleCommandUpdate(telegram.update, dispatchTime);
  }
  return true;
}

void performTargetCheck(Target& target, uint32_t now) {
  if (!target.runtime.configured || WiFi.status() != WL_CONNECTED) {
    target.runtime.state = target.runtime.configured
                               ? TargetState::kUnknown
                               : TargetState::kNotConfigured;
    target.runtime.responseTimeMs = 0.0F;
    target.runtime.lastCheckMs = now;
    return;
  }

  const uint32_t startedMs = millis();
  float responseTimeMs = 0.0F;
  const BoundedPingResult result = boundedPing(
      target.runtime.ip, Timing::kPingTimeoutMs, responseTimeMs);
  char timingLabel[32];
  snprintf(timingLabel, sizeof(timingLabel), "Ping %s", target.config.name);
  logOperationDuration(timingLabel, startedMs);

  target.runtime.lastCheckMs = millis();
  target.runtime.responseTimeMs = responseTimeMs;
  if (result == BoundedPingResult::kReply) {
    target.runtime.state = TargetState::kOnline;
    target.runtime.lastSuccessfulCheckMs = target.runtime.lastCheckMs;
  } else if (result == BoundedPingResult::kTimeout) {
    target.runtime.state = TargetState::kOffline;
  } else {
    target.runtime.state = TargetState::kUnknown;
  }
}

bool sendMagicPacket(const Target& target) {
  if (!target.runtime.configured || WiFi.status() != WL_CONNECTED) return false;
  uint8_t packet[102];
  memset(packet, 0xFF, 6);
  for (uint8_t repeat = 0; repeat < 16; ++repeat) {
    memcpy(packet + 6 + repeat * 6, target.runtime.mac, 6);
  }
  if (!udp.beginPacket(network.broadcast, Limits::kWolPort)) return false;
  const size_t written = udp.write(packet, sizeof(packet));
  return written == sizeof(packet) && udp.endPacket() == 1;
}

void showManualWolResult(const Target& target, bool sent) {
  String message;
  message.reserve(220);
  message += F("Wake ");
  message += target.config.name;
  message += F("\n\n");
  message += sent ? F("Five Wake-on-LAN packets were scheduled and sent.")
                  : F("Wake-on-LAN failed: no packet could be sent.");
  message += F("\nUse Status ");
  message += target.config.name;
  message += F(" to check whether it comes online.");
  showActionResult(message);
}

bool serviceWol(uint32_t now) {
  if (!actions.wol.active) return false;
  if (actions.wol.packetsSent > 0 &&
      now - actions.wol.lastSendMs < Timing::kWolRepeatMs) {
    return false;
  }
  Target* target = findTarget(actions.wol.target);
  if (target == nullptr) {
    actions.wol.active = false;
    return false;
  }

  const bool packetSent = sendMagicPacket(*target);
  actions.wol.anyPacketSent |= packetSent;
  actions.wol.lastSendMs = now;
  ++actions.wol.packetsSent;
  Serial.printf("WOL %s packet %u/%u: %s\n", target->config.name,
                actions.wol.packetsSent, Limits::kWolPacketCount,
                packetSent ? "sent" : "failed");
  if (actions.wol.packetsSent < Limits::kWolPacketCount) return false;

  const bool automatic = actions.wol.automatic;
  const bool anyPacketSent = actions.wol.anyPacketSent;
  actions.wol.active = false;
  if (automatic) {
    target->runtime.autoWakeResult = anyPacketSent
                                         ? AutoWakeResult::kWakeSent
                                         : AutoWakeResult::kWakeFailed;
    ++actions.autoWake.nextTargetIndex;
  } else {
    showManualWolResult(*target, anyPacketSent);
  }
  return false;
}

bool serviceStatusJob(uint32_t now) {
  if (actions.status.type == StatusJobType::kNone) return false;
  if (actions.status.type == StatusJobType::kTarget) {
    const TargetId targetId = actions.status.target;
    Target* target = findTarget(targetId);
    actions.status.type = StatusJobType::kNone;
    if (target == nullptr) return false;
    performTargetCheck(*target, now);
    queueScreen(Screen::kTargetStatus, targetId);
    return true;
  }

  while (actions.status.nextTargetIndex < Limits::kTargetCount &&
         !targets[actions.status.nextTargetIndex].runtime.configured) {
    ++actions.status.nextTargetIndex;
  }
  if (actions.status.nextTargetIndex >= Limits::kTargetCount) {
    actions.status.type = StatusJobType::kNone;
    queueScreen(Screen::kAllStatus);
    return false;
  }
  Target& target = targets[actions.status.nextTargetIndex++];
  performTargetCheck(target, now);
  if (actions.status.nextTargetIndex >= Limits::kTargetCount) {
    actions.status.type = StatusJobType::kNone;
    queueScreen(Screen::kAllStatus);
  }
  return true;
}

bool serviceAutoWake(uint32_t now) {
  if (!actions.autoWake.active || actions.wol.active) return false;
  while (actions.autoWake.nextTargetIndex < Limits::kTargetCount) {
    Target& target = targets[actions.autoWake.nextTargetIndex];
    if (!target.runtime.configured || !target.config.autoWake) {
      ++actions.autoWake.nextTargetIndex;
      continue;
    }
    performTargetCheck(target, now);
    if (target.runtime.state == TargetState::kOnline) {
      target.runtime.autoWakeResult = AutoWakeResult::kAlreadyOnline;
      ++actions.autoWake.nextTargetIndex;
    } else {
      startWolJob(targetIdAt(actions.autoWake.nextTargetIndex), true, now);
    }
    return true;
  }
  actions.autoWake.active = false;
  queueScreen(Screen::kAutoWake);
  return false;
}

bool servicePendingAction(uint32_t now) {
  if (ota.inProgress || WiFi.status() != WL_CONNECTED) return false;
  if (actions.status.type != StatusJobType::kNone) return serviceStatusJob(now);
  if (actions.wol.active) return serviceWol(now);
  if (actions.autoWake.active) return serviceAutoWake(now);
  return false;
}

bool autoWakeAllowedForReset() {
  return health.resetReason == ESP_RST_POWERON && !health.safeMode;
}

void scheduleAutoWake(uint32_t now) {
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

void serviceAutoWakeStart(uint32_t now) {
  if (!actions.autoWake.pending || actions.autoWake.handled || ota.inProgress ||
      actionBusy()) {
    return;
  }
  if (now - actions.autoWake.startedMs < AUTO_WAKE_DELAY_SECONDS * 1000UL) {
    return;
  }
  actions.autoWake.pending = false;
  actions.autoWake.handled = true;
  actions.autoWake.active = true;
  actions.autoWake.nextTargetIndex = 0;
}

void configureBroadcastAddress() {
  const IPAddress local = WiFi.localIP();
  const IPAddress mask = WiFi.subnetMask();
  for (uint8_t index = 0; index < 4; ++index) {
    network.broadcast[index] = local[index] | static_cast<uint8_t>(~mask[index]);
  }
  Serial.printf("WOL broadcast address: %s\n",
                network.broadcast.toString().c_str());
}

void setupTelegram() {
  if (telegram.initialized) return;
  telegramClient.setInsecure();
  // Arduino-ESP32 2.x setTimeout() uses seconds, not milliseconds.
  telegramClient.setTimeout(Timing::kTelegramClientTimeoutSeconds);
  telegramClient.setHandshakeTimeout(Timing::kTelegramHandshakeTimeoutSeconds);
  bot.longPoll = 0;
  bot.waitForResponse = Timing::kTelegramResponseWaitMs;
  bot.maxMessageLength = Limits::kTelegramMessageBytes;
  telegram.pollCommand.reserve(80);
  telegram.initialized = true;
  Serial.println(F("Telegram client ready (bounded TLS timeouts)"));
}

void setupOTA() {
  if (ota.initialized) return;
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    ota.lastProgress = UINT8_MAX;
    ota.inProgress = true;
    health.applicationState = ApplicationState::kOtaUpdating;
    feedWatchdog();
    Serial.println(F("OTA update starting; network actions paused"));
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
    health.applicationState = health.safeMode ? ApplicationState::kSafeMode
                                               : ApplicationState::kOnline;
    Serial.printf("OTA Error: %u; paused actions may resume\n", error);
  });
  ArduinoOTA.begin();
  ota.initialized = true;
  Serial.println(F("ArduinoOTA ready"));
}

void printWiFiDetails() {
  Serial.println(F("Wi-Fi connected"));
  Serial.printf("SSID: %s\n", WiFi.SSID().c_str());
  Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
}

void onWiFiConnected(uint32_t now) {
  printWiFiDetails();
  configureBroadcastAddress();
  setupTelegram();
  setupOTA();
  if (!network.connectedBefore) {
    network.connectedBefore = true;
    scheduleAutoWake(now);
    queueScreen(health.safeMode ? Screen::kSafeMode : Screen::kMain);
  } else {
    queueScreen(ui.screen, ui.target);
  }
}

void onWiFiDisconnected() {
  Serial.println(F("Wi-Fi disconnected"));
  telegram.reachable = false;
}

void connectWiFi(uint32_t now) {
  Serial.println(F("Connecting to Wi-Fi..."));
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  network.lastRetryMs = now;
}

void serviceWiFi(uint32_t now) {
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
  if (now - network.lastRetryMs < Timing::kWifiRetryMs) return;
  Serial.println(F("Retrying Wi-Fi connection..."));
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  network.lastRetryMs = now;
}

void setupRecovery() {
  health.resetReason = esp_reset_reason();
  Serial.printf("Reset reason: %s\n", resetReasonText().c_str());
  preferences.begin("recovery", false);
  uint8_t failedBoots = preferences.getUChar("failed", 0);
  if (failedBoots < UINT8_MAX) ++failedBoots;
  preferences.putUChar("failed", failedBoots);
  health.safeMode = failedBoots >= Limits::kSafeModeBootCount;
  telegram.dashboardMessageId = preferences.getInt("dashId", 0);
  telegram.dashboardChatId = preferences.getString("dashChat", TELEGRAM_CHAT_ID);
  telegram.lastUpdateId = preferences.getLong("tgUpdate", 0);
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

void serviceBootHealth(uint32_t now) {
  if (health.bootMarkedHealthy || health.safeMode || ota.inProgress ||
      now - health.bootStartedMs < Timing::kBootHealthyMs) {
    return;
  }
  preferences.putUChar("failed", 0);
  health.bootMarkedHealthy = true;
  Serial.println(F("Boot marked healthy"));
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("ESP32 Wake-on-LAN Bot starting"));
  health.bootStartedMs = millis();
  setupRecovery();
  setupWatchdog();
  initializeTargets();
  buildKeyboardCache();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  connectWiFi(millis());
  const uint32_t startupStartedMs = millis();
  // Bounded startup wait gives serial USB uploads immediate connection feedback.
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startupStartedMs < Timing::kWifiStartupTimeoutMs) {
    delay(Timing::kStartupPollMs);
    feedWatchdog();
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("Wi-Fi startup timed out; background recovery remains active."));
  }
}

void loop() {
  const uint32_t now = millis();
  serviceWiFi(now);
  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
    if (!ota.inProgress) {
      serviceAutoWakeStart(now);
      const bool actionUsedNetwork = servicePendingAction(now);
      const bool dashboardUsedNetwork =
          !actionUsedNetwork && serviceDashboard(now);
      if (!actionUsedNetwork && !dashboardUsedNetwork) serviceTelegram(now);
      serviceBootHealth(now);
    }
  }
  feedWatchdog();
  finishLoopTiming(now);
}
