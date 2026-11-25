#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <cstddef>

enum class BoundedPingResult : uint8_t {
  kReply,
  kTimeout,
  kError,
};

struct PingTargetRequest {
  IPAddress ip;
  bool enabled = false;
};

struct PingTargetOutcome {
  BoundedPingResult result = BoundedPingResult::kError;
  float responseTimeMs = 0.0F;
};

// Sends one ICMP echo request. The receive wait is strictly bounded by timeoutMs.
BoundedPingResult boundedPing(const IPAddress& target, uint32_t timeoutMs,
                              float& responseTimeMs);

// Sends ICMP echo requests concurrently to multiple targets on a single raw
// socket, returning as soon as all enabled targets reply or timeoutMs elapses.
void boundedPingMultiple(const PingTargetRequest* targets, size_t count,
                         uint32_t timeoutMs, uint32_t retryAfterMs,
                         PingTargetOutcome* outcomes);
