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

constexpr uint32_t WIFI_STARTUP_TIMEOUT_MS = 18000;
constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 5000;
constexpr uint32_t BOT_POLL_INTERVAL_MS = 700;
constexpr uint32_t SAFE_MODE_BOT_POLL_INTERVAL_MS = 5000;
constexpr uint32_t TELEGRAM_FIRST_FAILURE_BACKOFF_MS = 2000;
constexpr uint32_t TELEGRAM_CONTINUED_FAILURE_BACKOFF_MS = 5000;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 300;
constexpr uint32_t WOL_COOLDOWN_MS = 3000;
constexpr uint32_t WOL_REPEAT_INTERVAL_MS = 100;
constexpr uint8_t WOL_REPEAT_COUNT = 5;
constexpr uint16_t WOL_PORT = 9;
constexpr uint32_t PING_TIMEOUT_MS = 600;
constexpr uint32_t BOOT_HEALTHY_AFTER_MS = 60000;
constexpr uint8_t SAFE_MODE_FAILURE_THRESHOLD = 3;
constexpr uint32_t TASK_WATCHDOG_TIMEOUT_SECONDS = 15;
constexpr uint32_t SLOW_LOOP_WARNING_MS = 250;
constexpr uint32_t SLOW_OPERATION_WARNING_MS = 100;
constexpr bool DEBUG_PERFORMANCE = true;

const char BACK_KEYBOARD[] =
    R"json([[{"text":"Back","callback_data":"refresh_menu"}]])json";
const char WINDOWS_STATUS_KEYBOARD[] =
    R"json([[{"text":"Refresh","callback_data":"refresh_status_win"}],[{"text":"Back","callback_data":"refresh_menu"}]])json";
const char LINUX_STATUS_KEYBOARD[] =
    R"json([[{"text":"Refresh","callback_data":"refresh_status_linux"}],[{"text":"Back","callback_data":"refresh_menu"}]])json";
const char ALL_STATUS_KEYBOARD[] =
    R"json([[{"text":"Refresh","callback_data":"refresh_status_all"}],[{"text":"Back","callback_data":"refresh_menu"}]])json";
const char SAFE_MODE_KEYBOARD[] =
    R"json([[{"text":"Refresh","callback_data":"esp_status"}]])json";

enum class TargetState : uint8_t { kNotConfigured, kUnknown, kOnline, kOffline };
enum class ApplicationState : uint8_t {
  kBooting,
  kWaitingForWiFi,
  kOnline,
  kOtaUpdating,
  kSafeMode,
};
enum class UiScreen : uint8_t {
  kMain,
  kWindowsStatus,
  kLinuxStatus,
  kAllStatus,
  kEspStatus,
  kAutoWakeStatus,
  kActionResult,
  kSafeMode,
};
enum class PendingAction : uint8_t {
  kNone,
  kCheckWindows,
  kCheckLinux,
  kCheckAllWindows,
  kCheckAllLinux,
  kWakeWindows,
  kWakeLinux,
  kAutoCheckWindows,
  kAutoCheckLinux,
  kAutoWakeWindows,
  kAutoWakeLinux,
};
enum class AutoWakeResult : uint8_t {
  kPending,
  kDisabled,
  kNotConfigured,
  kSkippedReset,
  kAlreadyOnline,
  kWakeSent,
  kWakeFailed,
};
enum class TelegramResultKind : uint8_t {
  kSuccess,
  kNotModified,
  kApiError,
  kNetworkError,
  kParseError,
};

struct TargetRuntimeStatus {
  TargetState state = TargetState::kUnknown;
  float responseTimeMs = 0.0F;
  uint32_t lastCheckMs = 0;
  uint32_t lastSuccessfulCheckMs = 0;
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

WiFiClientSecure client;
UniversalTelegramBot bot(TELEGRAM_BOT_TOKEN, client);
WiFiUDP udp;
Preferences recoveryPreferences;

TargetRuntimeStatus windowsStatus;
TargetRuntimeStatus linuxStatus;
ApplicationState applicationState = ApplicationState::kBooting;
UiScreen currentScreen = UiScreen::kMain;
PendingAction pendingAction = PendingAction::kNone;
AutoWakeResult autoWindowsResult = AutoWakeResult::kPending;
AutoWakeResult autoLinuxResult = AutoWakeResult::kPending;

bool otaInitialized = false;
bool telegramInitialized = false;
bool telegramReachable = false;
bool wifiWasConnected = false;
bool hasConnectedBefore = false;
bool autoWakePending = false;
bool autoWakeHandled = false;
bool autoWakeWorkflowActive = false;
bool otaInProgress = false;
bool safeMode = false;
bool bootMarkedHealthy = false;
bool watchdogEnabled = false;
bool dashboardUpdatePending = false;
bool dashboardFallbackAllowed = false;
bool wolAnyPacketSent = false;

uint8_t telegramFailureCount = 0;
uint8_t wolPacketsSent = 0;
uint8_t lastOtaProgress = 255;
uint32_t autoWakeStartedAt = 0;
uint32_t bootStartedAt = 0;
uint32_t lastWiFiReconnectAttempt = 0;
uint32_t nextTelegramPollAt = 0;
uint32_t telegramRequestNotBefore = 0;
uint32_t lastWindowsWakeMs = 0;
uint32_t lastLinuxWakeMs = 0;
uint32_t nextWolPacketAt = 0;
uint32_t lastButtonAt = 0;
uint32_t lastLoopDurationMs = 0;
uint32_t maximumLoopDurationMs = 0;
int32_t lastTelegramUpdateId = 0;
int dashboardMessageId = 0;
esp_reset_reason_t lastResetReason = ESP_RST_UNKNOWN;
IPAddress wolBroadcastAddress;

String dashboardChatId;
String lastDashboardText;
String lastDashboardKeyboard;
String actionResultText;
String lastCallbackQueryId;
String lastButtonAction;
String mainKeyboard;
String backKeyboard;
String windowsStatusKeyboard;
String linuxStatusKeyboard;
String allStatusKeyboard;
String safeModeKeyboard;
DynamicJsonDocument telegramPayloadDocument(2304);
DynamicJsonDocument telegramResponseDocument(4096);
DynamicJsonDocument telegramUpdateDocument(4096);

bool timeReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

void logOperationDuration(const char* operation, uint32_t startedAt) {
  if (!DEBUG_PERFORMANCE) return;
  const uint32_t duration = millis() - startedAt;
  if (duration >= SLOW_OPERATION_WARNING_MS) {
    Serial.printf("[PERF] %s: %lu ms\n", operation,
                  static_cast<unsigned long>(duration));
  }
}

void finishLoopTiming(uint32_t startedAt) {
  lastLoopDurationMs = millis() - startedAt;
  if (lastLoopDurationMs > maximumLoopDurationMs) {
    maximumLoopDurationMs = lastLoopDurationMs;
  }
  if (DEBUG_PERFORMANCE && lastLoopDurationMs >= SLOW_LOOP_WARNING_MS) {
    Serial.printf("[WARN] Slow loop: %lu ms\n",
                  static_cast<unsigned long>(lastLoopDurationMs));
  }
}

bool isWindowsConfigured() {
  return WINDOWS_MAC[0] != '\0' && WINDOWS_IP[0] != '\0';
}

bool isLinuxConfigured() {
  return LINUX_MAC[0] != '\0' && LINUX_IP[0] != '\0';
}

bool isAuthorized(const String& chatId) {
  return chatId == TELEGRAM_CHAT_ID;
}

bool isHeavyAction(const String& action) {
  return action == "wake_win" || action == "wake_linux" ||
         action == "status_win" || action == "refresh_status_win" ||
         action == "status_linux" || action == "refresh_status_linux" ||
         action == "status_all" || action == "refresh_status_all";
}

bool actionBusy() {
  return pendingAction != PendingAction::kNone;
}

void feedTaskWatchdog() {
  if (watchdogEnabled) esp_task_wdt_reset();
}

String getResetReasonText() {
  switch (lastResetReason) {
    case ESP_RST_POWERON: return "Power-on";
    case ESP_RST_EXT: return "External reset";
    case ESP_RST_SW: return "Software reset";
    case ESP_RST_PANIC: return "Crash / panic";
    case ESP_RST_INT_WDT: return "Interrupt Watchdog";
    case ESP_RST_TASK_WDT: return "Task Watchdog";
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep";
    case ESP_RST_BROWNOUT: return "Brownout";
    case ESP_RST_SDIO: return "SDIO reset";
    default: return "Unknown";
  }
}

String getApplicationStateText() {
  switch (applicationState) {
    case ApplicationState::kBooting: return "BOOTING";
    case ApplicationState::kWaitingForWiFi: return "WAITING_FOR_WIFI";
    case ApplicationState::kOnline: return "ONLINE";
    case ApplicationState::kOtaUpdating: return "OTA_UPDATING";
    case ApplicationState::kSafeMode: return "SAFE_MODE";
    default: return "UNKNOWN";
  }
}

String getTargetStateText(TargetState state) {
  switch (state) {
    case TargetState::kOnline: return "Online";
    case TargetState::kOffline: return "Offline";
    case TargetState::kNotConfigured: return "Not configured";
    default: return "Unknown";
  }
}

String getAutoWakeResultText(AutoWakeResult result) {
  switch (result) {
    case AutoWakeResult::kDisabled: return "Disabled";
    case AutoWakeResult::kNotConfigured: return "Not configured";
    case AutoWakeResult::kSkippedReset: return "Skipped for this reset";
    case AutoWakeResult::kAlreadyOnline: return "Already online; WOL skipped";
    case AutoWakeResult::kWakeSent: return "Wake packets sent";
    case AutoWakeResult::kWakeFailed: return "Wake packet failed";
    default: return "Waiting";
  }
}

String getFormattedUptime() {
  uint32_t totalSeconds = millis() / 1000U;
  const uint32_t days = totalSeconds / 86400U;
  totalSeconds %= 86400U;
  const uint32_t hours = totalSeconds / 3600U;
  totalSeconds %= 3600U;
  const uint32_t minutes = totalSeconds / 60U;
  const uint32_t seconds = totalSeconds % 60U;
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%lud %luh %lum %lus",
           static_cast<unsigned long>(days), static_cast<unsigned long>(hours),
           static_cast<unsigned long>(minutes), static_cast<unsigned long>(seconds));
  return String(buffer);
}

String getAgeText(uint32_t timestamp) {
  if (timestamp == 0) return "never";
  const uint32_t ageSeconds = (millis() - timestamp) / 1000U;
  if (ageSeconds < 2) return "just now";
  if (ageSeconds < 60) return String(ageSeconds) + "s ago";
  if (ageSeconds < 3600) return String(ageSeconds / 60U) + "m ago";
  return String(ageSeconds / 3600U) + "h ago";
}

String getSignalQuality(int rssi) {
  if (rssi >= -50) return "Excellent";
  if (rssi >= -60) return "Good";
  if (rssi >= -70) return "Fair";
  return "Weak";
}

void buildKeyboardCache() {
  backKeyboard = BACK_KEYBOARD;
  windowsStatusKeyboard = WINDOWS_STATUS_KEYBOARD;
  linuxStatusKeyboard = LINUX_STATUS_KEYBOARD;
  allStatusKeyboard = ALL_STATUS_KEYBOARD;
  safeModeKeyboard = SAFE_MODE_KEYBOARD;

  DynamicJsonDocument document(768);
  JsonArray keyboard = document.to<JsonArray>();
  auto addButton = [](JsonArray row, const char* text, const char* data) {
    JsonObject button = row.createNestedObject();
    button["text"] = text;
    button["callback_data"] = data;
  };

  if (isWindowsConfigured() || isLinuxConfigured()) {
    JsonArray wakeRow = keyboard.createNestedArray();
    if (isWindowsConfigured()) addButton(wakeRow, "Wake Windows", "wake_win");
    if (isLinuxConfigured()) addButton(wakeRow, "Wake Linux", "wake_linux");
    JsonArray statusRow = keyboard.createNestedArray();
    if (isWindowsConfigured()) addButton(statusRow, "Status Windows", "status_win");
    if (isLinuxConfigured()) addButton(statusRow, "Status Linux", "status_linux");
    if (isWindowsConfigured() && isLinuxConfigured()) {
      JsonArray allRow = keyboard.createNestedArray();
      addButton(allRow, "Status All", "status_all");
    }
  }
  JsonArray autoRow = keyboard.createNestedArray();
  addButton(autoRow, "Auto Wake", "auto_wake_status");
  JsonArray espRow = keyboard.createNestedArray();
  addButton(espRow, "ESP32 Status", "esp_status");

  mainKeyboard.reserve(700);
  serializeJson(document, mainKeyboard);
}

uint32_t latestCheckTime() {
  return windowsStatus.lastCheckMs > linuxStatus.lastCheckMs
             ? windowsStatus.lastCheckMs
             : linuxStatus.lastCheckMs;
}

String buildMainDashboard() {
  String message;
  message.reserve(320);
  message += "TeleWOL\n\n";
  if (isWindowsConfigured()) {
    message += "Windows: ";
    message += getTargetStateText(windowsStatus.state);
    message += '\n';
  }
  if (isLinuxConfigured()) {
    message += "Linux: ";
    message += getTargetStateText(linuxStatus.state);
    message += '\n';
  }
  message += "\nLast status check: ";
  message += getAgeText(latestCheckTime());
  message += "\n\nESP32: Online\nWi-Fi: ";
  message += String(WiFi.RSSI());
  message += " dBm";
  return message;
}

String buildTargetStatusScreen(const char* label, const char* ipAddress,
                               const TargetRuntimeStatus& status) {
  String message;
  message.reserve(280);
  message += label;
  message += " Status\n\nStatus: ";
  message += getTargetStateText(status.state);
  if (status.state != TargetState::kNotConfigured) {
    message += "\nIP: ";
    message += ipAddress;
  }
  if (status.state == TargetState::kOnline) {
    message += "\nPing: ";
    message += String(status.responseTimeMs, 1);
    message += " ms";
  } else if (status.state == TargetState::kOffline) {
    message += " / Unreachable";
  } else if (status.state == TargetState::kUnknown) {
    message += "\nThe check could not be completed.";
  }
  message += "\n\nLast check: ";
  message += getAgeText(status.lastCheckMs);
  message += "\nLast successful reply: ";
  message += getAgeText(status.lastSuccessfulCheckMs);
  return message;
}

String buildWindowsStatusScreen() {
  return buildTargetStatusScreen("Windows", WINDOWS_IP, windowsStatus);
}

String buildLinuxStatusScreen() {
  return buildTargetStatusScreen("Linux", LINUX_IP, linuxStatus);
}

String buildAllStatusScreen() {
  String message;
  message.reserve(320);
  message += "PC Status\n\n";
  if (isWindowsConfigured()) {
    message += "Windows: ";
    message += getTargetStateText(windowsStatus.state);
    message += '\n';
  }
  if (isLinuxConfigured()) {
    message += "Linux: ";
    message += getTargetStateText(linuxStatus.state);
    message += '\n';
  }
  message += "\nLast check: ";
  message += getAgeText(latestCheckTime());
  return message;
}

String buildEspStatusScreen() {
  String message;
  message.reserve(700);
  message += "ESP32 Status\n\nState: ";
  message += getApplicationStateText();
  message += "\nLast Reset: ";
  message += getResetReasonText();
  message += "\nWatchdog: ";
  message += watchdogEnabled ? "Enabled" : "Unavailable";
  message += "\nSafe Mode: ";
  message += safeMode ? "Yes" : "No";
  message += "\nWi-Fi: ";
  message += WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected";
  message += "\nTelegram: ";
  message += telegramReachable ? "Reachable" : "Temporarily unavailable";
  if (WiFi.status() == WL_CONNECTED) {
    message += "\nSSID: ";
    message += WiFi.SSID();
    message += "\nIP: ";
    message += WiFi.localIP().toString();
    const int rssi = WiFi.RSSI();
    message += "\nRSSI: ";
    message += String(rssi);
    message += " dBm (";
    message += getSignalQuality(rssi);
    message += ")\nGateway: ";
    message += WiFi.gatewayIP().toString();
    message += "\nSubnet: ";
    message += WiFi.subnetMask().toString();
  }
  message += "\nUptime: ";
  message += getFormattedUptime();
  message += "\n\nMemory\nFree Heap: ";
  message += String(ESP.getFreeHeap() / 1024U);
  message += " KB\nMin Free Heap: ";
  message += String(ESP.getMinFreeHeap() / 1024U);
  message += " KB\nLargest Block: ";
  message += String(ESP.getMaxAllocHeap() / 1024U);
  message += " KB\n\nPerformance\nLast loop: ";
  message += String(lastLoopDurationMs);
  message += " ms\nMaximum loop: ";
  message += String(maximumLoopDurationMs);
  message += " ms";
  return message;
}

String buildAutoWakeScreen() {
  String message;
  message.reserve(420);
  message += "Auto Wake\n\nWindows: ";
  if (!isWindowsConfigured()) message += "Not configured";
  else message += AUTO_WAKE_WINDOWS ? "Enabled" : "Disabled";
  message += "\nLinux: ";
  if (!isLinuxConfigured()) message += "Not configured";
  else message += AUTO_WAKE_LINUX ? "Enabled" : "Disabled";
  message += "\nDelay: ";
  message += String(AUTO_WAKE_DELAY_SECONDS);
  message += " seconds\n\nLast startup workflow\nWindows: ";
  message += getAutoWakeResultText(autoWindowsResult);
  message += "\nLinux: ";
  message += getAutoWakeResultText(autoLinuxResult);
  return message;
}

String buildSafeModeScreen() {
  String message;
  message.reserve(350);
  message += "TeleWOL SAFE MODE\n\nRepeated unhealthy boots were detected.\n\nWi-Fi: ";
  message += WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected";
  message += "\nOTA: ";
  message += otaInitialized ? "Ready" : "Waiting for Wi-Fi";
  message += "\nReset reason: ";
  message += getResetReasonText();
  message += "\n\nUpload corrected firmware using ArduinoOTA or USB.";
  return message;
}

String renderCurrentScreen() {
  switch (currentScreen) {
    case UiScreen::kWindowsStatus: return buildWindowsStatusScreen();
    case UiScreen::kLinuxStatus: return buildLinuxStatusScreen();
    case UiScreen::kAllStatus: return buildAllStatusScreen();
    case UiScreen::kEspStatus: return buildEspStatusScreen();
    case UiScreen::kAutoWakeStatus: return buildAutoWakeScreen();
    case UiScreen::kActionResult: return actionResultText;
    case UiScreen::kSafeMode: return buildSafeModeScreen();
    default: return buildMainDashboard();
  }
}

const String& keyboardForCurrentScreen() {
  switch (currentScreen) {
    case UiScreen::kWindowsStatus: return windowsStatusKeyboard;
    case UiScreen::kLinuxStatus: return linuxStatusKeyboard;
    case UiScreen::kAllStatus: return allStatusKeyboard;
    case UiScreen::kSafeMode: return safeModeKeyboard;
    case UiScreen::kMain: return mainKeyboard;
    default: return backKeyboard;
  }
}

void queueScreen(UiScreen screen, bool allowFallback = true) {
  currentScreen = screen;
  dashboardUpdatePending = true;
  dashboardFallbackAllowed = allowFallback;
}

void recordTelegramSuccess() {
  telegramReachable = true;
  telegramFailureCount = 0;
  telegramRequestNotBefore = millis();
}

void recordTelegramFailure() {
  telegramReachable = false;
  if (telegramFailureCount < UINT8_MAX) ++telegramFailureCount;
  const uint32_t waitMs = telegramFailureCount == 1
                              ? TELEGRAM_FIRST_FAILURE_BACKOFF_MS
                              : TELEGRAM_CONTINUED_FAILURE_BACKOFF_MS;
  telegramRequestNotBefore = millis() + waitMs;
  nextTelegramPollAt = telegramRequestNotBefore;
  Serial.printf("Telegram unavailable; retrying in %lu ms\n",
                static_cast<unsigned long>(waitMs));
}

TelegramResult telegramPost(const char* method, JsonObject payload,
                            const char* timingLabel) {
  const uint32_t startedAt = millis();
  const String response = bot.sendPostToTelegram(bot.buildCommand(method), payload);
  client.stop();
  logOperationDuration(timingLabel, startedAt);

  TelegramResult result;
  if (response.length() == 0) {
    result.kind = TelegramResultKind::kNetworkError;
    recordTelegramFailure();
    return result;
  }

  telegramResponseDocument.clear();
  const DeserializationError error = deserializeJson(telegramResponseDocument, response);
  if (error) {
    result.kind = TelegramResultKind::kParseError;
    recordTelegramFailure();
    return result;
  }

  recordTelegramSuccess();
  if (telegramResponseDocument["ok"] | false) {
    result.kind = TelegramResultKind::kSuccess;
    result.messageId = telegramResponseDocument["result"]["message_id"] | 0;
    return result;
  }

  const String description = telegramResponseDocument["description"].as<String>();
  result.kind = description.indexOf("message is not modified") >= 0
                    ? TelegramResultKind::kNotModified
                    : TelegramResultKind::kApiError;
  return result;
}

TelegramResult sendOrEditDashboard(const String& text, const String& keyboard,
                                   bool editExisting) {
  telegramPayloadDocument.clear();
  telegramPayloadDocument["chat_id"] = dashboardChatId;
  telegramPayloadDocument["text"] = text;
  if (editExisting) telegramPayloadDocument["message_id"] = dashboardMessageId;
  JsonObject replyMarkup = telegramPayloadDocument.createNestedObject("reply_markup");
  replyMarkup["inline_keyboard"] = serialized(keyboard);
  return telegramPost(editExisting ? "editMessageText" : "sendMessage",
                      telegramPayloadDocument.as<JsonObject>(),
                      editExisting ? "Edit dashboard" : "Send dashboard");
}

void saveDashboardIdentity() {
  recoveryPreferences.putInt("dashId", dashboardMessageId);
  recoveryPreferences.putString("dashChat", dashboardChatId);
}

bool handleDashboardUpdate() {
  if (!dashboardUpdatePending || otaInProgress || !telegramInitialized ||
      !timeReached(millis(), telegramRequestNotBefore)) {
    return false;
  }
  if (dashboardChatId.length() == 0) dashboardChatId = TELEGRAM_CHAT_ID;

  const String text = renderCurrentScreen();
  const String& keyboard = keyboardForCurrentScreen();
  const bool editExisting = dashboardMessageId > 0;
  if (editExisting && text == lastDashboardText && keyboard == lastDashboardKeyboard) {
    dashboardUpdatePending = false;
    return false;
  }

  const TelegramResult result = sendOrEditDashboard(text, keyboard, editExisting);
  if (result.kind == TelegramResultKind::kSuccess ||
      result.kind == TelegramResultKind::kNotModified) {
    if (!editExisting && result.messageId > 0) {
      dashboardMessageId = result.messageId;
      saveDashboardIdentity();
    }
    lastDashboardText = text;
    lastDashboardKeyboard = keyboard;
    dashboardUpdatePending = false;
  } else if (editExisting && result.kind == TelegramResultKind::kApiError &&
             dashboardFallbackAllowed) {
    // The old message was deleted/invalid. Send exactly one replacement next loop.
    dashboardMessageId = 0;
    dashboardFallbackAllowed = false;
    lastDashboardText = "";
    lastDashboardKeyboard = "";
  } else {
    dashboardUpdatePending = false;
  }
  return true;
}

void answerCallback(const String& queryId, const char* text = nullptr,
                    bool showAlert = false) {
  telegramPayloadDocument.clear();
  telegramPayloadDocument["callback_query_id"] = queryId;
  telegramPayloadDocument["show_alert"] = showAlert;
  telegramPayloadDocument["cache_time"] = 0;
  if (text != nullptr && text[0] != '\0') telegramPayloadDocument["text"] = text;
  telegramPost("answerCallbackQuery", telegramPayloadDocument.as<JsonObject>(),
               "Callback ACK");
}

void sendOneShotText(const String& chatId, const String& text) {
  telegramPayloadDocument.clear();
  telegramPayloadDocument["chat_id"] = chatId;
  telegramPayloadDocument["text"] = text;
  telegramPost("sendMessage", telegramPayloadDocument.as<JsonObject>(),
               "Telegram send");
}

String normalizeCommand(String command) {
  command.trim();
  const int atSign = command.indexOf('@');
  if (atSign >= 0) command.remove(atSign);
  return command;
}

bool isMacAddressValid(const char* macAddress) {
  if (macAddress == nullptr || strlen(macAddress) != 17) return false;
  for (uint8_t i = 0; i < 17; ++i) {
    if (i % 3 == 2) {
      if (macAddress[i] != ':') return false;
    } else if (!isxdigit(static_cast<unsigned char>(macAddress[i]))) {
      return false;
    }
  }
  return true;
}

uint8_t hexValue(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  value = static_cast<char>(tolower(static_cast<unsigned char>(value)));
  return static_cast<uint8_t>(value - 'a' + 10);
}

bool sendOneMagicPacket(const char* macAddress) {
  if (WiFi.status() != WL_CONNECTED || !isMacAddressValid(macAddress)) return false;

  uint8_t mac[6];
  for (uint8_t i = 0; i < 6; ++i) {
    mac[i] = static_cast<uint8_t>((hexValue(macAddress[i * 3]) << 4U) |
                                  hexValue(macAddress[i * 3 + 1]));
  }
  uint8_t packet[102];
  memset(packet, 0xFF, 6);
  for (uint8_t repeat = 0; repeat < 16; ++repeat) {
    memcpy(packet + 6 + repeat * 6, mac, 6);
  }

  if (!udp.beginPacket(wolBroadcastAddress, WOL_PORT)) return false;
  const size_t written = udp.write(packet, sizeof(packet));
  return written == sizeof(packet) && udp.endPacket() == 1;
}

void configureWolBroadcast() {
  const IPAddress local = WiFi.localIP();
  const IPAddress mask = WiFi.subnetMask();
  for (uint8_t i = 0; i < 4; ++i) {
    wolBroadcastAddress[i] = local[i] | static_cast<uint8_t>(~mask[i]);
  }
  Serial.printf("WOL broadcast address: %s\n", wolBroadcastAddress.toString().c_str());
}

bool isWakeOnCooldown(uint32_t lastWakeMs) {
  return lastWakeMs != 0 && millis() - lastWakeMs < WOL_COOLDOWN_MS;
}

void startWolSequence(PendingAction action) {
  pendingAction = action;
  wolPacketsSent = 0;
  wolAnyPacketSent = false;
  nextWolPacketAt = millis();
}

void setActionResult(const String& text) {
  actionResultText = text;
  queueScreen(UiScreen::kActionResult);
}

void scheduleManualAction(const String& action) {
  if (actionBusy()) {
    setActionResult("TeleWOL\n\nAnother network action is already running. Please wait.");
    return;
  }

  if (action == "status_win" || action == "refresh_status_win") {
    if (!isWindowsConfigured()) {
      setActionResult("Windows target is not configured.");
    } else {
      pendingAction = PendingAction::kCheckWindows;
    }
  } else if (action == "status_linux" || action == "refresh_status_linux") {
    if (!isLinuxConfigured()) {
      setActionResult("Linux target is not configured.");
    } else {
      pendingAction = PendingAction::kCheckLinux;
    }
  } else if (action == "status_all" || action == "refresh_status_all") {
    if (isWindowsConfigured()) pendingAction = PendingAction::kCheckAllWindows;
    else if (isLinuxConfigured()) pendingAction = PendingAction::kCheckAllLinux;
    else setActionResult("No target machines are configured.");
  } else if (action == "wake_win") {
    if (!isWindowsConfigured()) {
      setActionResult("Windows target is not configured.");
    } else if (isWakeOnCooldown(lastWindowsWakeMs)) {
      setActionResult("Wake Windows was requested recently. Please wait a moment.");
    } else if (!isMacAddressValid(WINDOWS_MAC)) {
      setActionResult("Wake-on-LAN failed: invalid Windows MAC address.");
    } else {
      lastWindowsWakeMs = millis();
      startWolSequence(PendingAction::kWakeWindows);
    }
  } else if (action == "wake_linux") {
    if (!isLinuxConfigured()) {
      setActionResult("Linux target is not configured.");
    } else if (isWakeOnCooldown(lastLinuxWakeMs)) {
      setActionResult("Wake Linux was requested recently. Please wait a moment.");
    } else if (!isMacAddressValid(LINUX_MAC)) {
      setActionResult("Wake-on-LAN failed: invalid Linux MAC address.");
    } else {
      lastLinuxWakeMs = millis();
      startWolSequence(PendingAction::kWakeLinux);
    }
  }
}

void routeLightAction(const String& action) {
  if (action == "refresh_menu") queueScreen(UiScreen::kMain);
  else if (action == "esp_status") queueScreen(safeMode ? UiScreen::kSafeMode : UiScreen::kEspStatus);
  else if (action == "auto_wake_status") queueScreen(UiScreen::kAutoWakeStatus);
  else queueScreen(UiScreen::kMain);
}

void adoptDashboardMessage(const TelegramUpdate& update) {
  if (update.messageId <= 0) return;
  if (dashboardMessageId != update.messageId || dashboardChatId != update.chatId) {
    dashboardMessageId = update.messageId;
    dashboardChatId = update.chatId;
    lastDashboardText = "";
    lastDashboardKeyboard = "";
    saveDashboardIdentity();
  }
}

void handleCallbackQuery(const TelegramUpdate& update) {
  if (!isAuthorized(update.chatId)) {
    Serial.printf("Unauthorized Telegram callback from chat ID %s\n", update.chatId.c_str());
    answerCallback(update.queryId, "Access denied.", true);
    return;
  }

  if (update.queryId == lastCallbackQueryId) {
    answerCallback(update.queryId, "Already handled.");
    return;
  }
  lastCallbackQueryId = update.queryId;

  const uint32_t now = millis();
  if (update.text == lastButtonAction && now - lastButtonAt < BUTTON_DEBOUNCE_MS) {
    answerCallback(update.queryId, "Please wait.");
    return;
  }
  lastButtonAction = update.text;
  lastButtonAt = now;

  if (otaInProgress) {
    answerCallback(update.queryId, "OTA update is in progress.");
    return;
  }
  if (isHeavyAction(update.text) && actionBusy()) {
    answerCallback(update.queryId, "Another action is running.");
    return;
  }

  // ACK is deliberately the first network operation after authorization checks.
  answerCallback(update.queryId);
  adoptDashboardMessage(update);
  if (safeMode) {
    queueScreen(UiScreen::kSafeMode);
  } else if (isHeavyAction(update.text)) {
    scheduleManualAction(update.text);
  } else {
    routeLightAction(update.text);
  }
}

void handleTextCommand(const TelegramUpdate& update) {
  if (!isAuthorized(update.chatId)) {
    Serial.printf("Unauthorized Telegram access attempt from chat ID %s\n", update.chatId.c_str());
    sendOneShotText(update.chatId, "Access denied. This chat is not authorized.");
    return;
  }

  dashboardChatId = update.chatId;
  if (safeMode) {
    queueScreen(UiScreen::kSafeMode);
    return;
  }

  const String command = normalizeCommand(update.text);
  if (command == "/start" || command == "/help") {
    queueScreen(UiScreen::kMain);
  } else if (command == "/wake_win") {
    scheduleManualAction("wake_win");
  } else if (command == "/wake_linux") {
    scheduleManualAction("wake_linux");
  } else if (command == "/status_win") {
    scheduleManualAction("status_win");
  } else if (command == "/status_linux") {
    scheduleManualAction("status_linux");
  } else if (command == "/status_all") {
    scheduleManualAction("status_all");
  } else if (command == "/status") {
    queueScreen(UiScreen::kEspStatus);
  } else if (command == "/auto_wake") {
    queueScreen(UiScreen::kAutoWakeStatus);
  } else {
    setActionResult("Unknown command. Use /help to open the control panel.");
  }
}

bool parseTelegramUpdate(const String& response, TelegramUpdate& update) {
  telegramUpdateDocument.clear();
  const DeserializationError error = deserializeJson(telegramUpdateDocument, response);
  if (error || !(telegramUpdateDocument["ok"] | false)) return false;

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
  } else {
    update.updateId = result["update_id"] | 0;
  }
  return true;
}

bool handleTelegramPoll() {
  if (!telegramInitialized || otaInProgress) return false;
  const uint32_t now = millis();
  if (!timeReached(now, nextTelegramPollAt) ||
      !timeReached(now, telegramRequestNotBefore)) {
    return false;
  }

  String command;
  command.reserve(80);
  command += "getUpdates?offset=";
  command += String(lastTelegramUpdateId + 1);
  command += "&limit=1&timeout=0";
  const uint32_t startedAt = millis();
  const String response = bot.sendGetToTelegram(bot.buildCommand(command));
  client.stop();
  logOperationDuration("Telegram getUpdates", startedAt);

  if (response.length() == 0) {
    recordTelegramFailure();
    return true;
  }

  TelegramUpdate update;
  if (!parseTelegramUpdate(response, update)) {
    recordTelegramFailure();
    return true;
  }
  recordTelegramSuccess();
  nextTelegramPollAt = millis() +
      (safeMode ? SAFE_MODE_BOT_POLL_INTERVAL_MS : BOT_POLL_INTERVAL_MS);

  if (update.updateId > 0) {
    if (update.updateId <= lastTelegramUpdateId) return true;
    lastTelegramUpdateId = update.updateId;
    recoveryPreferences.putLong("tgUpdate", lastTelegramUpdateId);
    if (update.isCallback) handleCallbackQuery(update);
    else if (update.text.length() > 0) handleTextCommand(update);
  }
  return true;
}

void updateTargetStatus(const char* label, const char* ipAddress,
                        TargetRuntimeStatus& status) {
  IPAddress target;
  if (WiFi.status() != WL_CONNECTED || !target.fromString(ipAddress)) {
    status.state = TargetState::kUnknown;
    status.responseTimeMs = 0.0F;
    status.lastCheckMs = millis();
    return;
  }

  const uint32_t startedAt = millis();
  float responseTimeMs = 0.0F;
  const BoundedPingResult result = boundedPing(target, PING_TIMEOUT_MS, responseTimeMs);
  logOperationDuration(label, startedAt);
  status.lastCheckMs = millis();
  status.responseTimeMs = responseTimeMs;
  if (result == BoundedPingResult::kReply) {
    status.state = TargetState::kOnline;
    status.lastSuccessfulCheckMs = status.lastCheckMs;
  } else if (result == BoundedPingResult::kTimeout) {
    status.state = TargetState::kOffline;
  } else {
    status.state = TargetState::kUnknown;
  }
}

void finishManualWol(bool windowsTarget) {
  String message;
  message.reserve(220);
  message += windowsTarget ? "Wake Windows\n\n" : "Wake Linux\n\n";
  message += wolAnyPacketSent
                 ? "Five Wake-on-LAN packets were scheduled and sent."
                 : "Wake-on-LAN failed: no packet could be sent.";
  message += windowsTarget
                 ? "\nUse Status Windows to check whether it comes online."
                 : "\nUse Status Linux to check whether it comes online.";
  setActionResult(message);
}

void startNextAutoWakeStep();

void finishAutoWol(bool windowsTarget) {
  if (windowsTarget) {
    autoWindowsResult = wolAnyPacketSent ? AutoWakeResult::kWakeSent
                                         : AutoWakeResult::kWakeFailed;
  } else {
    autoLinuxResult = wolAnyPacketSent ? AutoWakeResult::kWakeSent
                                       : AutoWakeResult::kWakeFailed;
  }
  pendingAction = PendingAction::kNone;
  startNextAutoWakeStep();
}

bool processWolStep(bool windowsTarget, bool automatic) {
  const uint32_t now = millis();
  if (!timeReached(now, nextWolPacketAt)) return false;
  const char* macAddress = windowsTarget ? WINDOWS_MAC : LINUX_MAC;
  if (sendOneMagicPacket(macAddress)) wolAnyPacketSent = true;
  ++wolPacketsSent;
  Serial.printf("WOL %s packet %u/%u: %s\n", windowsTarget ? "Windows" : "Linux",
                wolPacketsSent, WOL_REPEAT_COUNT,
                wolAnyPacketSent ? "sent" : "failed");

  if (wolPacketsSent < WOL_REPEAT_COUNT) {
    nextWolPacketAt = now + WOL_REPEAT_INTERVAL_MS;
  } else if (automatic) {
    finishAutoWol(windowsTarget);
  } else {
    pendingAction = PendingAction::kNone;
    finishManualWol(windowsTarget);
  }
  return false;
}

void startNextAutoWakeStep() {
  if (!autoWakeWorkflowActive) return;
  if (AUTO_WAKE_WINDOWS && isWindowsConfigured() &&
      autoWindowsResult == AutoWakeResult::kPending) {
    pendingAction = PendingAction::kAutoCheckWindows;
    return;
  }
  if (AUTO_WAKE_LINUX && isLinuxConfigured() &&
      autoLinuxResult == AutoWakeResult::kPending) {
    pendingAction = PendingAction::kAutoCheckLinux;
    return;
  }
  autoWakeWorkflowActive = false;
  pendingAction = PendingAction::kNone;
  queueScreen(UiScreen::kAutoWakeStatus);
}

bool handlePendingActionStep() {
  if (pendingAction == PendingAction::kNone || otaInProgress ||
      WiFi.status() != WL_CONNECTED) {
    return false;
  }

  switch (pendingAction) {
    case PendingAction::kCheckWindows:
      updateTargetStatus("Ping Windows", WINDOWS_IP, windowsStatus);
      pendingAction = PendingAction::kNone;
      queueScreen(UiScreen::kWindowsStatus);
      return true;
    case PendingAction::kCheckLinux:
      updateTargetStatus("Ping Linux", LINUX_IP, linuxStatus);
      pendingAction = PendingAction::kNone;
      queueScreen(UiScreen::kLinuxStatus);
      return true;
    case PendingAction::kCheckAllWindows:
      updateTargetStatus("Ping Windows", WINDOWS_IP, windowsStatus);
      pendingAction = isLinuxConfigured() ? PendingAction::kCheckAllLinux
                                          : PendingAction::kNone;
      if (pendingAction == PendingAction::kNone) queueScreen(UiScreen::kAllStatus);
      return true;
    case PendingAction::kCheckAllLinux:
      updateTargetStatus("Ping Linux", LINUX_IP, linuxStatus);
      pendingAction = PendingAction::kNone;
      queueScreen(UiScreen::kAllStatus);
      return true;
    case PendingAction::kWakeWindows:
      return processWolStep(true, false);
    case PendingAction::kWakeLinux:
      return processWolStep(false, false);
    case PendingAction::kAutoCheckWindows:
      updateTargetStatus("Ping Windows", WINDOWS_IP, windowsStatus);
      if (windowsStatus.state == TargetState::kOnline) {
        autoWindowsResult = AutoWakeResult::kAlreadyOnline;
        pendingAction = PendingAction::kNone;
        startNextAutoWakeStep();
      } else {
        startWolSequence(PendingAction::kAutoWakeWindows);
      }
      return true;
    case PendingAction::kAutoCheckLinux:
      updateTargetStatus("Ping Linux", LINUX_IP, linuxStatus);
      if (linuxStatus.state == TargetState::kOnline) {
        autoLinuxResult = AutoWakeResult::kAlreadyOnline;
        pendingAction = PendingAction::kNone;
        startNextAutoWakeStep();
      } else {
        startWolSequence(PendingAction::kAutoWakeLinux);
      }
      return true;
    case PendingAction::kAutoWakeWindows:
      return processWolStep(true, true);
    case PendingAction::kAutoWakeLinux:
      return processWolStep(false, true);
    default:
      pendingAction = PendingAction::kNone;
      return false;
  }
}

bool isAutoWakeAllowedForReset() {
  return lastResetReason == ESP_RST_POWERON && !safeMode;
}

void scheduleAutoWake() {
  autoWindowsResult = isWindowsConfigured()
                          ? (AUTO_WAKE_WINDOWS ? AutoWakeResult::kPending
                                               : AutoWakeResult::kDisabled)
                          : AutoWakeResult::kNotConfigured;
  autoLinuxResult = isLinuxConfigured()
                        ? (AUTO_WAKE_LINUX ? AutoWakeResult::kPending
                                           : AutoWakeResult::kDisabled)
                        : AutoWakeResult::kNotConfigured;
  if (autoWakeHandled || !isAutoWakeAllowedForReset()) {
    autoWakeHandled = true;
    if (AUTO_WAKE_WINDOWS && isWindowsConfigured()) {
      autoWindowsResult = AutoWakeResult::kSkippedReset;
    }
    if (AUTO_WAKE_LINUX && isLinuxConfigured()) {
      autoLinuxResult = AutoWakeResult::kSkippedReset;
    }
    Serial.println("Auto Wake skipped for this reset reason");
    return;
  }
  autoWakePending = true;
  autoWakeStartedAt = millis();
}

void handleAutoWakeTimer() {
  if (!autoWakePending || autoWakeHandled || otaInProgress || actionBusy()) return;
  if (millis() - autoWakeStartedAt < AUTO_WAKE_DELAY_SECONDS * 1000UL) return;

  autoWakePending = false;
  autoWakeHandled = true;
  if ((!AUTO_WAKE_WINDOWS || !isWindowsConfigured()) &&
      (!AUTO_WAKE_LINUX || !isLinuxConfigured())) {
    return;
  }
  autoWakeWorkflowActive = true;
  startNextAutoWakeStep();
}

void setupRecovery() {
  lastResetReason = esp_reset_reason();
  Serial.printf("Reset reason: %s\n", getResetReasonText().c_str());
  recoveryPreferences.begin("recovery", false);
  uint8_t failedBoots = recoveryPreferences.getUChar("failed", 0);
  if (failedBoots < UINT8_MAX) ++failedBoots;
  recoveryPreferences.putUChar("failed", failedBoots);
  safeMode = failedBoots >= SAFE_MODE_FAILURE_THRESHOLD;
  dashboardMessageId = recoveryPreferences.getInt("dashId", 0);
  dashboardChatId = recoveryPreferences.getString("dashChat", TELEGRAM_CHAT_ID);
  lastTelegramUpdateId = recoveryPreferences.getLong("tgUpdate", 0);
  if (safeMode) {
    applicationState = ApplicationState::kSafeMode;
    currentScreen = UiScreen::kSafeMode;
    Serial.println("SAFE MODE: repeated unhealthy boots detected");
  }
}

void setupTaskWatchdog() {
  const esp_err_t initResult = esp_task_wdt_init(TASK_WATCHDOG_TIMEOUT_SECONDS, true);
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
  watchdogEnabled = true;
  Serial.printf("Task Watchdog enabled: %u seconds\n", TASK_WATCHDOG_TIMEOUT_SECONDS);
}

void markBootHealthy() {
  if (bootMarkedHealthy || safeMode || otaInProgress ||
      millis() - bootStartedAt < BOOT_HEALTHY_AFTER_MS) return;
  recoveryPreferences.putUChar("failed", 0);
  bootMarkedHealthy = true;
  Serial.println("Boot marked healthy");
}

void connectWiFi() {
  Serial.println("Connecting to Wi-Fi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiReconnectAttempt = millis();
}

void setupTelegram() {
  if (telegramInitialized) return;
  client.setInsecure();
  // In Arduino-ESP32 2.x setTimeout() takes seconds, not milliseconds.
  client.setTimeout(2);
  client.setHandshakeTimeout(3);
  bot.longPoll = 0;
  bot.waitForResponse = 1200;
  bot.maxMessageLength = 3000;
  telegramInitialized = true;
  Serial.println("Telegram client ready (bounded TLS timeouts)");
}

void setupOTA() {
  if (otaInitialized) return;
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    lastOtaProgress = 255;
    otaInProgress = true;
    applicationState = ApplicationState::kOtaUpdating;
    feedTaskWatchdog();
    Serial.println("OTA update starting; network actions paused");
  });
  ArduinoOTA.onEnd([]() {
    recoveryPreferences.putUChar("failed", 0);
    feedTaskWatchdog();
    Serial.println("OTA update complete");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (total == 0) return;
    const uint8_t percent = static_cast<uint8_t>((progress * 100U) / total);
    if (percent == 100 || lastOtaProgress == 255 || percent >= lastOtaProgress + 10) {
      Serial.printf("OTA Progress: %u%%\n", percent);
      lastOtaProgress = percent;
    }
    feedTaskWatchdog();
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    applicationState = safeMode ? ApplicationState::kSafeMode : ApplicationState::kOnline;
    Serial.printf("OTA Error: %u; paused actions may resume\n", error);
  });
  ArduinoOTA.begin();
  otaInitialized = true;
  Serial.println("ArduinoOTA ready");
}

void printWiFiDetails() {
  Serial.println("Wi-Fi connected");
  Serial.printf("SSID: %s\n", WiFi.SSID().c_str());
  Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
}

void handleWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      printWiFiDetails();
      configureWolBroadcast();
      setupTelegram();
      setupOTA();
      if (!hasConnectedBefore) {
        hasConnectedBefore = true;
        scheduleAutoWake();
        queueScreen(safeMode ? UiScreen::kSafeMode : UiScreen::kMain);
      } else {
        // Reuse the dashboard instead of sending a reconnect notification.
        queueScreen(currentScreen);
      }
    }
    applicationState = safeMode ? ApplicationState::kSafeMode : ApplicationState::kOnline;
    wifiWasConnected = true;
    return;
  }

  if (wifiWasConnected) Serial.println("Wi-Fi disconnected");
  wifiWasConnected = false;
  telegramReachable = false;
  applicationState = safeMode ? ApplicationState::kSafeMode
                              : ApplicationState::kWaitingForWiFi;
  const uint32_t now = millis();
  if (now - lastWiFiReconnectAttempt >= WIFI_RECONNECT_INTERVAL_MS) {
    Serial.println("Retrying Wi-Fi connection...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastWiFiReconnectAttempt = now;
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("ESP32 Wake-on-LAN Bot starting");
  bootStartedAt = millis();
  setupRecovery();
  setupTaskWatchdog();
  buildKeyboardCache();
  windowsStatus.state = isWindowsConfigured() ? TargetState::kUnknown
                                               : TargetState::kNotConfigured;
  linuxStatus.state = isLinuxConfigured() ? TargetState::kUnknown
                                           : TargetState::kNotConfigured;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  connectWiFi();
  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startedAt < WIFI_STARTUP_TIMEOUT_MS) {
    delay(100);
    feedTaskWatchdog();
  }
  if (WiFi.status() == WL_CONNECTED) printWiFiDetails();
  else Serial.println("Wi-Fi startup timed out; background recovery remains active.");
}

void loop() {
  const uint32_t loopStartedAt = millis();
  handleWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
    if (!otaInProgress) {
      handleAutoWakeTimer();
      const bool actionUsedNetwork = handlePendingActionStep();
      if (!actionUsedNetwork) {
        const bool dashboardUsedNetwork = handleDashboardUpdate();
        if (!dashboardUsedNetwork) handleTelegramPoll();
      }
      if (!safeMode) markBootHealthy();
    }
  }

  feedTaskWatchdog();
  finishLoopTiming(loopStartedAt);
}
