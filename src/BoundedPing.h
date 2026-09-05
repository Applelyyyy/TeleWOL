#pragma once

#include <Arduino.h>
#include <IPAddress.h>

enum class BoundedPingResult : uint8_t {
  kReply,
  kTimeout,
  kError,
};

// Sends one ICMP echo request. The receive wait is strictly bounded by timeoutMs.
BoundedPingResult boundedPing(const IPAddress& target, uint32_t timeoutMs,
                              float& responseTimeMs);
