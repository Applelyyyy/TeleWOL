#pragma once

#include <Arduino.h>
#include "AppTypes.h"
#include "TelegramFastClient.h"

struct UiCache {
  Screen screen = Screen::kMain;
  bool updatePending = false;
  bool fallbackAllowed = false;
  String statusBanner;
  String mainKeyboard;
  String espStatusKeyboard;
  String autoWakeKeyboard;
  String safeModeKeyboard;
};

// Pre-builds static inline keyboard JSON payloads once at startup.
void buildUiKeyboardCache(UiCache& cache, const Target* targets,
                          size_t targetCount);

// Escapes &, <, > for safe Telegram HTML parse_mode rendering.
String escapeHtml(const String& raw);

// Human-readable helpers
String resetReasonText(esp_reset_reason_t reason);
String applicationStateText(ApplicationState state);
String formattedUptime(uint64_t nowMs);
String thailandTimeText(uint64_t timestampMs, uint64_t nowMs,
                        bool timeOnly = false);

// Renders the current active screen in clean modern HTML.
String renderScreenHtml(const UiCache& ui, const Target* targets,
                        size_t targetCount, const NetworkRuntime& network,
                        const HealthRuntime& health,
                        const PerformanceRuntime& perf,
                        const TelegramStats& tgStats, bool otaInitialized,
                        uint64_t nowMs);

// Selects the cached inline keyboard JSON for the active screen.
const String& keyboardForScreen(const UiCache& ui);
