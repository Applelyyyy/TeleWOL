#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <cstdint>
#include <esp_system.h>
#include <esp_timer.h>
#include <time.h>

inline uint64_t monotonicMs() {
  return static_cast<uint64_t>(esp_timer_get_time() / 1000LL);
}

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "include/secrets.h" not found. Run tools/generate_secrets.py first.
#endif

#ifndef DEBUG_PERFORMANCE
#define DEBUG_PERFORMANCE 0
#endif

namespace Timing {
constexpr uint32_t kWifiStartupTimeoutMs = 18000;
constexpr uint32_t kWifiRetryMs = 5000;
constexpr uint32_t kButtonDebounceMs = 250;
constexpr uint32_t kStatusRefreshCooldownMs = 600;
constexpr uint32_t kAutomaticStatusIntervalMs = 10UL * 60UL * 1000UL;
constexpr uint32_t kWolCooldownMs = 2000;
constexpr uint32_t kWolFollowupSpacingMs = 60;
constexpr uint32_t kWakeTrackStartDelayMs = 3000;
constexpr uint32_t kWakeTrackIntervalMs = 3000;
constexpr uint32_t kWakeTrackUiUpdateMs = 6000;
constexpr uint32_t kWakeTrackTimeoutMs = 60000;
constexpr uint32_t kPingTimeoutMs = 350;
constexpr uint32_t kPingRetryAfterMs = 150;
constexpr uint32_t kBootHealthyMs = 60000;
constexpr uint32_t kSlowLoopMs = 150;
constexpr uint32_t kSlowOperationMs = 80;
constexpr uint32_t kWatchdogSeconds = 15;
constexpr uint32_t kTelegramPollTimeoutSeconds = 4;
constexpr uint32_t kTelegramApiTimeoutMs = 4500;
constexpr uint32_t kTelegramHandshakeTimeoutSeconds = 5;
constexpr uint32_t kTelegramFirstBackoffMs = 1500;
constexpr uint32_t kTelegramContinuedBackoffMs = 4000;
constexpr uint32_t kWakeCommandMaxAgeSeconds = 60;
constexpr uint32_t kStartupPollMs = 100;
constexpr char kThailandTimezone[] = "ICT-7";
constexpr char kPrimaryTimeServer[] = "pool.ntp.org";
constexpr char kSecondaryTimeServer[] = "time.google.com";
}  // namespace Timing

namespace Limits {
constexpr size_t kTargetCount = 2;
constexpr uint8_t kWolInitialBurstCount = 3;
constexpr uint8_t kWolFollowupCount = 2;
constexpr uint16_t kWolPortPrimary = 9;
constexpr uint16_t kWolPortSecondary = 7;
constexpr uint8_t kSafeModeBootCount = 3;
constexpr size_t kTelegramPayloadBytes = 3584;
constexpr size_t kTelegramResponseBytes = 4096;
}  // namespace Limits

namespace Callback {
constexpr char kMain[] = "refresh_menu";
constexpr char kEspStatus[] = "esp_status";
constexpr char kAutoWake[] = "auto_wake_status";
constexpr char kStatusAll[] = "status_all";
constexpr char kRefreshAll[] = "refresh_status_all";
constexpr char kWakeWindows[] = "wake_win";
constexpr char kWakeLinux[] = "wake_linux";
constexpr char kWakeAll[] = "wake_all";
constexpr char kStatusWindows[] = "status_win";
constexpr char kStatusLinux[] = "status_linux";
constexpr char kRefreshWindows[] = "refresh_status_win";
constexpr char kRefreshLinux[] = "refresh_status_linux";
}  // namespace Callback

namespace Command {
constexpr char kStart[] = "/start";
constexpr char kHelp[] = "/help";
constexpr char kEspStatus[] = "/esp";
constexpr char kLegacyStatus[] = "/status";
constexpr char kPing[] = "/ping";
constexpr char kAutoWake[] = "/auto_wake";
constexpr char kStatusAll[] = "/status_all";
constexpr char kWakeWindows[] = "/wake_win";
constexpr char kWakeLinux[] = "/wake_linux";
constexpr char kWakeAll[] = "/wake_all";
constexpr char kStatusWindows[] = "/status_win";
constexpr char kStatusLinux[] = "/status_linux";
}  // namespace Command

namespace Diagnostics {
constexpr bool kPerformance = DEBUG_PERFORMANCE != 0;
}  // namespace Diagnostics

enum class TargetId : uint8_t { kWindows = 0, kLinux = 1, kNone = UINT8_MAX };
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
  kEspStatus,
  kAutoWake,
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
  kWakeAll,
  kUnknown,
};

struct TargetConfig {
  const char* name;
  const char* icon;
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
  uint64_t lastCheckMs = 0;
  uint64_t lastSuccessfulCheckMs = 0;
  uint64_t lastWakeMs = 0;

  // Live boot-tracking state after WOL burst
  bool trackingWake = false;
  bool wakeTimedOut = false;
  bool bootedJustNow = false;
  float lastBootSeconds = 0.0F;
  uint64_t wakeStartedMs = 0;
  uint64_t lastTrackPingMs = 0;
  uint8_t followupPacketsRemaining = 0;
  uint64_t lastFollowupSendMs = 0;
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

struct NetworkRuntime {
  bool wasConnected = false;
  bool connectedBefore = false;
  bool staticIpEnabled = false;
  uint32_t wifiDrops = 0;
  uint64_t lastRetryMs = 0;
  IPAddress staticIp;
  IPAddress gateway;
  IPAddress subnet;
  IPAddress primaryDns;
  IPAddress secondaryDns;
  IPAddress broadcast;
};

struct AutoWakeJob {
  bool pending = false;
  bool active = false;
  bool handled = false;
  uint8_t nextTargetIndex = 0;
  uint64_t startedMs = 0;
};

struct ActionRuntime {
  AutoWakeJob autoWake;
  ParsedAction lastButton;
  uint64_t lastButtonMs = 0;
  uint64_t lastStatusRequestMs = 0;
  uint64_t lastAutomaticStatusMs = 0;
  bool automaticStatusStarted = false;
};

struct ClockRuntime {
  bool configured = false;
};

struct OtaRuntime {
  bool initialized = false;
  volatile bool inProgress = false;
  uint8_t lastProgress = UINT8_MAX;
};

struct HealthRuntime {
  bool safeMode = false;
  bool bootMarkedHealthy = false;
  bool watchdogEnabled = false;
  uint64_t bootStartedMs = 0;
  esp_reset_reason_t resetReason = ESP_RST_UNKNOWN;
  ApplicationState applicationState = ApplicationState::kBooting;
};

struct PerformanceRuntime {
  uint32_t lastLoopMs = 0;
  uint32_t maximumLoopMs = 0;
  uint32_t lastPingMs = 0;
  uint32_t lastWolMs = 0;
};
