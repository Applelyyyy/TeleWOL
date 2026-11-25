#include "TelegramFastClient.h"

#include <WiFi.h>
#include <cstring>

#include "AppTypes.h"

namespace {

constexpr char kTelegramHost[] = "api.telegram.org";

void configureClientSocket(WiFiClientSecure& client, uint32_t timeoutMs) {
  client.setInsecure();
  client.setTimeout((timeoutMs + 500U) / 1000U);
  client.setHandshakeTimeout(Timing::kTelegramHandshakeTimeoutSeconds);
}

void configureHttpClient(HTTPClient& http, uint32_t timeoutMs) {
  http.setReuse(true);
  http.setConnectTimeout(
      static_cast<int32_t>(Timing::kTelegramHandshakeTimeoutSeconds * 1000U));
  http.setTimeout(static_cast<uint16_t>(timeoutMs));
  http.useHTTP10(false);
}

bool isMessageMissingError(const char* desc) {
  if (desc == nullptr || desc[0] == '\0') {
    return false;
  }
  return strstr(desc, "message to edit not found") != nullptr ||
         strstr(desc, "message can't be edited") != nullptr ||
         strstr(desc, "MESSAGE_ID_INVALID") != nullptr;
}

}  // namespace

void TelegramFastClient::begin(const char* botToken, int32_t initialUpdateId) {
  if (started_) {
    return;
  }
  strlcpy(botToken_, botToken ? botToken : "", sizeof(botToken_));
  baseUrl_.reserve(112);
  baseUrl_ = F("https://");
  baseUrl_ += kTelegramHost;
  baseUrl_ += F("/bot");
  baseUrl_ += botToken_;
  baseUrl_ += '/';

  lastUpdateId_ = initialUpdateId;
  needsSync_ = true;
  lastHeartbeatMs_ = monotonicMs();

  dashboardSlot_.text.reserve(Limits::kTelegramPayloadBytes);
  dashboardSlot_.keyboard.reserve(1024);

  configureClientSocket(pollClient_,
                        (Timing::kTelegramPollTimeoutSeconds + 3U) * 1000U);
  configureClientSocket(apiClient_, Timing::kTelegramApiTimeoutMs);
  configureHttpClient(pollHttp_,
                      (Timing::kTelegramPollTimeoutSeconds + 3U) * 1000U);
  configureHttpClient(apiHttp_, Timing::kTelegramApiTimeoutMs);

  inboundQueue_ = xQueueCreate(kInboundQueueLen, sizeof(TelegramInboundEvent));
  actionQueue_ = xQueueCreate(kActionQueueLen, sizeof(OutboundActionItem));
  stateMutex_ = xSemaphoreCreateMutex();

  started_ = true;
  xTaskCreatePinnedToCore(workerTaskEntry, "tg_worker", 10240, this, 1,
                          &workerHandle_, 0);
}

void TelegramFastClient::setPaused(bool paused) { paused_ = paused; }

void TelegramFastClient::requestUpdateSync() {
  needsSync_ = true;
  hadFailureBeforeSync_ = true;
}

void TelegramFastClient::setPollIntervalSafeMode(bool safeMode) {
  safeMode_ = safeMode;
}

bool TelegramFastClient::pollInboundEvent(TelegramInboundEvent& outEvent) {
  if (inboundQueue_ == nullptr) {
    return false;
  }
  return xQueueReceive(inboundQueue_, &outEvent, 0) == pdTRUE;
}

void TelegramFastClient::enqueueCallbackAck(const char* queryId,
                                            const char* text, bool showAlert) {
  if (actionQueue_ == nullptr || queryId == nullptr || queryId[0] == '\0') {
    return;
  }
  OutboundActionItem item{};
  item.type = OutboundActionItem::Type::kCallbackAck;
  item.showAlert = showAlert;
  strlcpy(item.targetId, queryId, sizeof(item.targetId));
  if (text != nullptr) {
    strlcpy(item.text, text, sizeof(item.text));
  }
  xQueueSend(actionQueue_, &item, 0);
}

void TelegramFastClient::enqueueOneShotText(const char* chatId,
                                            const char* text) {
  if (actionQueue_ == nullptr || chatId == nullptr || text == nullptr) {
    return;
  }
  OutboundActionItem item{};
  item.type = OutboundActionItem::Type::kOneShotText;
  strlcpy(item.targetId, chatId, sizeof(item.targetId));
  strlcpy(item.text, text, sizeof(item.text));
  xQueueSend(actionQueue_, &item, 0);
}

void TelegramFastClient::enqueueRegisterCommands() {
  if (actionQueue_ == nullptr) {
    return;
  }
  OutboundActionItem item{};
  item.type = OutboundActionItem::Type::kSetCommands;
  xQueueSend(actionQueue_, &item, 0);
}

uint32_t TelegramFastClient::enqueueDashboardUpdate(
    const char* chatId, int32_t messageId, const String& htmlText,
    const String& inlineKeyboardJson, bool editExisting) {
  if (stateMutex_ == nullptr) {
    return 0;
  }
  uint32_t assignedSeq = 0;
  if (xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
    ++dashboardSeqCounter_;
    if (dashboardSeqCounter_ == 0) {
      ++dashboardSeqCounter_;
    }
    assignedSeq = dashboardSeqCounter_;
    dashboardSlot_.pending = true;
    dashboardSlot_.seq = assignedSeq;
    dashboardSlot_.editExisting = editExisting;
    dashboardSlot_.messageId = messageId;
    strlcpy(dashboardSlot_.chatId, chatId ? chatId : "",
            sizeof(dashboardSlot_.chatId));
    dashboardSlot_.text = htmlText;
    dashboardSlot_.keyboard = inlineKeyboardJson;
    xSemaphoreGive(stateMutex_);
  }
  return assignedSeq;
}

bool TelegramFastClient::consumeDashboardOutcome(
    TelegramDashboardOutcome& outOutcome) {
  if (stateMutex_ == nullptr) {
    return false;
  }
  bool hasOutcome = false;
  if (xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
    if (latestOutcome_.valid) {
      outOutcome = latestOutcome_;
      latestOutcome_.valid = false;
      hasOutcome = true;
    }
    xSemaphoreGive(stateMutex_);
  }
  return hasOutcome;
}

bool TelegramFastClient::consumeOutageRecovery() {
  if (stateMutex_ == nullptr) {
    return false;
  }
  bool recovered = false;
  if (xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
    if (recoveredFromOutage_) {
      recoveredFromOutage_ = false;
      recovered = true;
    }
    xSemaphoreGive(stateMutex_);
  }
  return recovered;
}

TelegramStats TelegramFastClient::getStats() {
  TelegramStats snapshot;
  if (stateMutex_ != nullptr &&
      xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
    snapshot = stats_;
    snapshot.workerHealthy =
        (monotonicMs() - lastHeartbeatMs_) < 15000ULL;
    xSemaphoreGive(stateMutex_);
  }
  return snapshot;
}

void TelegramFastClient::workerTaskEntry(void* arg) {
  auto* self = static_cast<TelegramFastClient*>(arg);
  if (self != nullptr) {
    self->runWorker();
  }
  vTaskDelete(nullptr);
}

void TelegramFastClient::runWorker() {
  while (true) {
    if (stateMutex_ != nullptr &&
        xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(5)) == pdTRUE) {
      lastHeartbeatMs_ = monotonicMs();
      xSemaphoreGive(stateMutex_);
    }

    if (paused_ || WiFi.status() != WL_CONNECTED) {
      if (pollClient_.connected()) {
        pollHttp_.end();
        pollClient_.stop();
      }
      if (apiClient_.connected()) {
        apiHttp_.end();
        apiClient_.stop();
      }
      if (stateMutex_ != nullptr &&
          xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
        stats_.pollConnected = false;
        stats_.apiConnected = false;
        if (WiFi.status() != WL_CONNECTED) {
          stats_.reachable = false;
        }
        xSemaphoreGive(stateMutex_);
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    serviceOutboundQueue();
    serviceDashboardSlot();

    if (!paused_ && WiFi.status() == WL_CONNECTED &&
        backoffAllowsRequest(monotonicMs())) {
      serviceLongPoll();
    }

    serviceOutboundQueue();
    serviceDashboardSlot();

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

bool TelegramFastClient::backoffAllowsRequest(uint64_t now) const {
  return !backoffActive_ || (now - lastFailureMs_ >= retryDelayMs_);
}

void TelegramFastClient::recordSuccess() {
  backoffActive_ = false;
  failureCount_ = 0;
  if (stateMutex_ != nullptr &&
      xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
    stats_.reachable = true;
    stats_.synced = !needsSync_;
    stats_.pollConnected = pollClient_.connected();
    stats_.apiConnected = apiClient_.connected();
    xSemaphoreGive(stateMutex_);
  }
}

void TelegramFastClient::recordFailure() {
  backoffActive_ = true;
  // Flush any commands queued during WAN/ISP outage once Telegram returns.
  needsSync_ = true;
  hadFailureBeforeSync_ = true;
  lastFailureMs_ = monotonicMs();
  if (failureCount_ < UINT8_MAX) {
    ++failureCount_;
  }
  retryDelayMs_ = failureCount_ == 1 ? Timing::kTelegramFirstBackoffMs
                                     : Timing::kTelegramContinuedBackoffMs;
  if (stateMutex_ != nullptr &&
      xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
    stats_.reachable = false;
    stats_.synced = false;
    stats_.pollConnected = pollClient_.connected();
    stats_.apiConnected = apiClient_.connected();
    xSemaphoreGive(stateMutex_);
  }
}

bool TelegramFastClient::performPost(const char* method, const String& jsonBody,
                                     TelegramResultKind& outKind,
                                     int32_t& outMessageId,
                                     uint32_t& outRttMs) {
  outKind = TelegramResultKind::kNetworkError;
  outMessageId = 0;
  outRttMs = 0;

  if (paused_ || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  String url;
  url.reserve(baseUrl_.length() + 28);
  url = baseUrl_;
  url += method;

  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    if (paused_ || WiFi.status() != WL_CONNECTED) {
      return false;
    }

    const uint64_t startedMs = monotonicMs();
    if (!apiHttp_.begin(apiClient_, url)) {
      apiHttp_.end();
      apiClient_.stop();
      continue;
    }
    apiHttp_.setReuse(true);
    apiHttp_.addHeader(F("Content-Type"), F("application/json"));

    const int httpCode = apiHttp_.POST(jsonBody);
    String body;
    if (httpCode > 0) {
      body = apiHttp_.getString();
      apiHttp_.end();
      if (apiClient_.fd() >= 0) {
        apiClient_.setNoDelay(true);
      }
      outRttMs = static_cast<uint32_t>(monotonicMs() - startedMs);

      if (Diagnostics::kPerformance) {
        Serial.printf("[PERF] TG %s: %lu ms (HTTP %d, keepalive=%d)\n", method,
                      static_cast<unsigned long>(outRttMs), httpCode,
                      apiClient_.connected() ? 1 : 0);
      }

      responseDoc_.clear();
      const DeserializationError err = deserializeJson(responseDoc_, body);
      if (err) {
        outKind = TelegramResultKind::kParseError;
        recordFailure();
        return false;
      }

      recordSuccess();
      if (stateMutex_ != nullptr &&
          xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
        stats_.lastApiRttMs = outRttMs;
        ++stats_.totalApiRequests;
        xSemaphoreGive(stateMutex_);
      }

      if (responseDoc_["ok"] | false) {
        outKind = TelegramResultKind::kSuccess;
        outMessageId = responseDoc_["result"]["message_id"] | 0;
        return true;
      }

      const int errorCode = responseDoc_["error_code"] | 0;
      const char* desc = responseDoc_["description"] | "";
      if (strstr(desc, "message is not modified") != nullptr) {
        outKind = TelegramResultKind::kNotModified;
        return true;
      }
      if (isMessageMissingError(desc)) {
        Serial.printf("Telegram dashboard target missing (%s): %s\n", method,
                      desc);
        outKind = TelegramResultKind::kMessageMissing;
        return false;
      }
      if (errorCode == 429 || errorCode >= 500) {
        Serial.printf("Telegram transient error %d (%s): %s\n", errorCode,
                      method, desc);
        outKind = TelegramResultKind::kNetworkError;
        recordFailure();
        return false;
      }
      Serial.printf("Telegram API error (%s): %s\n", method, desc);
      outKind = TelegramResultKind::kApiError;
      return false;
    }

    // Stale keep-alive socket closed by server: drop and immediately retry once.
    apiHttp_.end();
    apiClient_.stop();
    if (attempt == 0) {
      if (stateMutex_ != nullptr &&
          xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
        ++stats_.staleReconnects;
        xSemaphoreGive(stateMutex_);
      }
      if (Diagnostics::kPerformance) {
        Serial.printf("[PERF] TG %s stale socket (%d), reconnecting...\n",
                      method, httpCode);
      }
      continue;
    }

    Serial.printf("Telegram POST %s failed: %s\n", method,
                  apiHttp_.errorToString(httpCode).c_str());
    recordFailure();
    return false;
  }

  recordFailure();
  return false;
}

bool TelegramFastClient::performGet(const String& methodWithQuery,
                                    String& outBody, bool& outTransportError) {
  outBody.remove(0);
  outTransportError = false;

  if (paused_ || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  String url;
  url.reserve(baseUrl_.length() + methodWithQuery.length() + 4);
  url = baseUrl_;
  url += methodWithQuery;

  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    if (paused_ || WiFi.status() != WL_CONNECTED) {
      return false;
    }

    const uint64_t startedMs = monotonicMs();
    if (!pollHttp_.begin(pollClient_, url)) {
      pollHttp_.end();
      pollClient_.stop();
      continue;
    }
    pollHttp_.setReuse(true);

    const int httpCode = pollHttp_.GET();
    if (httpCode > 0) {
      outBody = pollHttp_.getString();
      pollHttp_.end();
      if (pollClient_.fd() >= 0) {
        pollClient_.setNoDelay(true);
      }
      if (Diagnostics::kPerformance &&
          (monotonicMs() - startedMs) <
              Timing::kTelegramPollTimeoutSeconds * 900ULL) {
        Serial.printf("[PERF] TG getUpdates hit: %lu ms\n",
                      static_cast<unsigned long>(monotonicMs() - startedMs));
      }
      return true;
    }

    pollHttp_.end();
    pollClient_.stop();
    if (attempt == 0 && !paused_) {
      if (stateMutex_ != nullptr &&
          xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
        ++stats_.staleReconnects;
        xSemaphoreGive(stateMutex_);
      }
      continue;
    }
    outTransportError = true;
    Serial.printf("Telegram getUpdates failed: %s\n",
                  pollHttp_.errorToString(httpCode).c_str());
    return false;
  }

  outTransportError = true;
  return false;
}

void TelegramFastClient::serviceOutboundQueue() {
  if (actionQueue_ == nullptr || paused_ || WiFi.status() != WL_CONNECTED) {
    return;
  }

  OutboundActionItem item{};
  while (!paused_ && xQueueReceive(actionQueue_, &item, 0) == pdTRUE) {
    if (item.type == OutboundActionItem::Type::kSetCommands) {
      registerBotCommands();
      continue;
    }

    payloadDoc_.clear();
    const char* method = nullptr;
    if (item.type == OutboundActionItem::Type::kCallbackAck) {
      method = "answerCallbackQuery";
      payloadDoc_["callback_query_id"] = item.targetId;
      payloadDoc_["show_alert"] = item.showAlert;
      payloadDoc_["cache_time"] = 0;
      if (item.text[0] != '\0') {
        payloadDoc_["text"] = item.text;
      }
    } else {
      method = "sendMessage";
      payloadDoc_["chat_id"] = item.targetId;
      payloadDoc_["text"] = item.text;
      payloadDoc_["parse_mode"] = "HTML";
    }

    String jsonBody;
    jsonBody.reserve(320);
    serializeJson(payloadDoc_, jsonBody);

    TelegramResultKind kind;
    int32_t messageId = 0;
    uint32_t rttMs = 0;
    performPost(method, jsonBody, kind, messageId, rttMs);
  }
}

void TelegramFastClient::serviceDashboardSlot() {
  if (stateMutex_ == nullptr || paused_ || WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (!backoffAllowsRequest(monotonicMs())) {
    return;
  }

  bool hasJob = false;
  uint32_t seq = 0;
  bool editExisting = false;
  int32_t messageId = 0;
  char chatId[24]{};
  String text;
  String keyboard;

  if (xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
    if (dashboardSlot_.pending) {
      hasJob = true;
      seq = dashboardSlot_.seq;
      editExisting = dashboardSlot_.editExisting;
      messageId = dashboardSlot_.messageId;
      strlcpy(chatId, dashboardSlot_.chatId, sizeof(chatId));
      text = dashboardSlot_.text;
      keyboard = dashboardSlot_.keyboard;
      dashboardSlot_.pending = false;
    }
    xSemaphoreGive(stateMutex_);
  }

  if (!hasJob) {
    return;
  }

  payloadDoc_.clear();
  payloadDoc_["chat_id"] = chatId;
  payloadDoc_["text"] = text;
  payloadDoc_["parse_mode"] = "HTML";
  payloadDoc_["disable_web_page_preview"] = true;
  if (editExisting && messageId > 0) {
    payloadDoc_["message_id"] = messageId;
  }
  JsonObject replyMarkup = payloadDoc_.createNestedObject("reply_markup");
  replyMarkup["inline_keyboard"] = serialized(keyboard);

  String jsonBody;
  jsonBody.reserve(text.length() + keyboard.length() + 192);
  serializeJson(payloadDoc_, jsonBody);

  TelegramResultKind kind = TelegramResultKind::kNetworkError;
  int32_t resultMsgId = 0;
  uint32_t rttMs = 0;
  performPost(editExisting ? "editMessageText" : "sendMessage", jsonBody, kind,
              resultMsgId, rttMs);

  if (xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
    latestOutcome_.valid = true;
    latestOutcome_.seq = seq;
    latestOutcome_.kind = kind;
    latestOutcome_.messageId = resultMsgId;
    latestOutcome_.wasEdit = editExisting;
    xSemaphoreGive(stateMutex_);
  }
}

void TelegramFastClient::registerBotCommands() {
  payloadDoc_.clear();
  JsonArray commands = payloadDoc_.createNestedArray("commands");
  auto addCmd = [&](const char* cmd, const char* desc) {
    JsonObject obj = commands.createNestedObject();
    obj["command"] = cmd;
    obj["description"] = desc;
  };
  addCmd("start", "Open Control Center");
  addCmd("wake_win", "Wake Windows PC");
  addCmd("wake_linux", "Wake Linux PC");
  addCmd("wake_all", "Wake all configured PCs");
  addCmd("ping", "Ping devices & refresh status");
  addCmd("status", "Refresh device status");
  addCmd("esp", "ESP32 health & diagnostics");
  addCmd("auto_wake", "Auto-Wake settings");

  String jsonBody;
  jsonBody.reserve(576);
  serializeJson(payloadDoc_, jsonBody);

  TelegramResultKind kind;
  int32_t unusedId = 0;
  uint32_t rttMs = 0;
  performPost("setMyCommands", jsonBody, kind, unusedId, rttMs);
}

void TelegramFastClient::serviceLongPoll() {
  const uint64_t now = monotonicMs();
  if (safeMode_ && lastPollAttemptMs_ != 0 &&
      now - lastPollAttemptMs_ < 4000ULL) {
    return;
  }
  lastPollAttemptMs_ = now;

  const bool syncing = needsSync_;
  const uint32_t pollTimeout =
      (syncing || safeMode_) ? 0U : Timing::kTelegramPollTimeoutSeconds;

  String query;
  query.reserve(112);
  query = F("getUpdates?offset=");
  query += syncing ? -1 : (lastUpdateId_ + 1);
  query += F("&limit=1&timeout=");
  query += pollTimeout;
  query += F("&allowed_updates=%5B%22message%22%2C%22callback_query%22%5D");

  String body;
  bool transportError = false;
  if (!performGet(query, body, transportError)) {
    if (transportError && !paused_) {
      recordFailure();
    }
    return;
  }

  responseDoc_.clear();
  const DeserializationError err = deserializeJson(responseDoc_, body);
  if (err || !(responseDoc_["ok"] | false)) {
    recordFailure();
    return;
  }

  JsonArray results = responseDoc_["result"].as<JsonArray>();
  if (syncing) {
    if (!results.isNull() && results.size() > 0) {
      lastUpdateId_ = results[0]["update_id"] | lastUpdateId_;
    }
    needsSync_ = false;
    if (hadFailureBeforeSync_) {
      hadFailureBeforeSync_ = false;
      if (stateMutex_ != nullptr &&
          xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
        recoveredFromOutage_ = true;
        ++stats_.outageRecoveries;
        xSemaphoreGive(stateMutex_);
      }
      Serial.println(F("Outage recovered: stale backlog discarded"));
    } else {
      Serial.println(F("Telegram backlog cleared; persistent poll ready"));
    }
    recordSuccess();
    return;
  }

  recordSuccess();
  if (results.isNull() || results.size() == 0) {
    return;
  }

  JsonObject result = results[0];
  const int32_t updateId = result["update_id"] | 0;
  if (updateId <= lastUpdateId_) {
    return;
  }
  lastUpdateId_ = updateId;

  TelegramInboundEvent event{};
  event.updateId = updateId;
  if (result.containsKey("callback_query")) {
    JsonObject cb = result["callback_query"];
    event.isCallback = true;
    strlcpy(event.queryId, cb["id"] | "", sizeof(event.queryId));
    strlcpy(event.text, cb["data"] | "", sizeof(event.text));
    strlcpy(event.chatId, cb["message"]["chat"]["id"].as<String>().c_str(),
            sizeof(event.chatId));
    event.messageId = cb["message"]["message_id"] | 0;
  } else if (result.containsKey("message")) {
    JsonObject msg = result["message"];
    event.isCallback = false;
    strlcpy(event.chatId, msg["chat"]["id"].as<String>().c_str(),
            sizeof(event.chatId));
    strlcpy(event.text, msg["text"] | "", sizeof(event.text));
    event.messageId = msg["message_id"] | 0;
    event.messageDate = msg["date"] | 0;
  }

  if (inboundQueue_ != nullptr) {
    xQueueSend(inboundQueue_, &event, pdMS_TO_TICKS(20));
  }
}
