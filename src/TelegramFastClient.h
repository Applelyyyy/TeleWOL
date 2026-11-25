#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

enum class TelegramResultKind : uint8_t {
  kSuccess,
  kNotModified,
  kMessageMissing,
  kApiError,
  kNetworkError,
  kParseError,
};

struct TelegramInboundEvent {
  bool isCallback = false;
  int32_t updateId = 0;
  int32_t messageId = 0;
  int64_t messageDate = 0;
  char chatId[24]{};
  char text[64]{};
  char queryId[64]{};
};

struct TelegramStats {
  bool reachable = false;
  bool synced = false;
  bool pollConnected = false;
  bool apiConnected = false;
  bool workerHealthy = false;
  uint32_t lastApiRttMs = 0;
  uint32_t totalApiRequests = 0;
  uint32_t staleReconnects = 0;
  uint32_t outageRecoveries = 0;
};

struct TelegramDashboardOutcome {
  bool valid = false;
  uint32_t seq = 0;
  TelegramResultKind kind = TelegramResultKind::kNetworkError;
  int32_t messageId = 0;
  bool wasEdit = false;
};

class TelegramFastClient {
 public:
  TelegramFastClient() = default;
  ~TelegramFastClient() = default;

  TelegramFastClient(const TelegramFastClient&) = delete;
  TelegramFastClient& operator=(const TelegramFastClient&) = delete;

  void begin(const char* botToken, int32_t initialUpdateId);
  void setPaused(bool paused);
  void requestUpdateSync();
  void setPollIntervalSafeMode(bool safeMode);

  // Non-blocking enqueue APIs called from Core 1.
  bool pollInboundEvent(TelegramInboundEvent& outEvent);
  void enqueueCallbackAck(const char* queryId, const char* text = nullptr,
                          bool showAlert = false);
  void enqueueOneShotText(const char* chatId, const char* text);
  uint32_t enqueueDashboardUpdate(const char* chatId, int32_t messageId,
                                  const String& htmlText,
                                  const String& inlineKeyboardJson,
                                  bool editExisting);
  void enqueueRegisterCommands();

  bool consumeDashboardOutcome(TelegramDashboardOutcome& outOutcome);
  bool consumeOutageRecovery();
  TelegramStats getStats();

 private:
  struct OutboundActionItem {
    enum class Type : uint8_t { kCallbackAck, kOneShotText, kSetCommands };
    Type type = Type::kCallbackAck;
    bool showAlert = false;
    char targetId[64]{};  // queryId or chatId
    char text[192]{};
  };

  struct DashboardSlot {
    bool pending = false;
    bool editExisting = false;
    uint32_t seq = 0;
    int32_t messageId = 0;
    char chatId[24]{};
    String text;
    String keyboard;
  };

  static void workerTaskEntry(void* arg);
  void runWorker();
  void serviceOutboundQueue();
  void serviceDashboardSlot();
  void serviceLongPoll();

  bool performPost(const char* method, const String& jsonBody,
                   TelegramResultKind& outKind, int32_t& outMessageId,
                   uint32_t& outRttMs);
  bool performGet(const String& methodWithQuery, String& outBody,
                  bool& outTransportError);
  void registerBotCommands();

  void recordFailure();
  void recordSuccess();
  bool backoffAllowsRequest(uint64_t now) const;

  static constexpr size_t kInboundQueueLen = 8;
  static constexpr size_t kActionQueueLen = 10;

  char botToken_[80]{};
  String baseUrl_;

  WiFiClientSecure pollClient_;
  HTTPClient pollHttp_;
  WiFiClientSecure apiClient_;
  HTTPClient apiHttp_;

  DynamicJsonDocument responseDoc_{4096};
  DynamicJsonDocument payloadDoc_{3584};

  QueueHandle_t inboundQueue_ = nullptr;
  QueueHandle_t actionQueue_ = nullptr;
  SemaphoreHandle_t stateMutex_ = nullptr;
  TaskHandle_t workerHandle_ = nullptr;

  DashboardSlot dashboardSlot_;
  TelegramDashboardOutcome latestOutcome_;

  volatile bool started_ = false;
  volatile bool paused_ = false;
  volatile bool safeMode_ = false;
  volatile bool needsSync_ = true;
  volatile int32_t lastUpdateId_ = 0;
  uint32_t dashboardSeqCounter_ = 0;

  bool backoffActive_ = false;
  bool hadFailureBeforeSync_ = false;
  bool recoveredFromOutage_ = false;
  uint8_t failureCount_ = 0;
  uint64_t lastFailureMs_ = 0;
  uint32_t retryDelayMs_ = 0;
  uint64_t lastPollAttemptMs_ = 0;
  uint64_t lastHeartbeatMs_ = 0;

  TelegramStats stats_;
};
