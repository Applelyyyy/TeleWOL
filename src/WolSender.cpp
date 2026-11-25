#include "WolSender.h"

#include <WiFi.h>
#include <cstring>

#include "BoundedPing.h"

namespace {

bool sendSingleMagicPacket(WiFiUDP& udp, const IPAddress& destinationIp,
                           uint16_t port, const uint8_t (&packet)[102]) {
  if (!udp.beginPacket(destinationIp, port)) {
    return false;
  }
  const size_t written = udp.write(packet, sizeof(packet));
  return written == sizeof(packet) && udp.endPacket() == 1;
}

void buildMagicPacket(const uint8_t (&mac)[6], uint8_t (&packet)[102]) {
  memset(packet, 0xFF, 6);
  for (uint8_t repeat = 0; repeat < 16; ++repeat) {
    memcpy(packet + 6 + repeat * 6, mac, 6);
  }
}

}  // namespace

bool sendWolBurst(WiFiUDP& udp, const Target& target,
                  const IPAddress& broadcastIp, uint8_t repeatCount,
                  uint32_t& outDurationMs) {
  outDurationMs = 0;
  if (!target.runtime.configured || WiFi.status() != WL_CONNECTED ||
      repeatCount == 0) {
    return false;
  }

  const uint64_t startedMs = monotonicMs();
  uint8_t packet[102];
  buildMagicPacket(target.runtime.mac, packet);

  bool anySent = false;
  for (uint8_t i = 0; i < repeatCount; ++i) {
    anySent |= sendSingleMagicPacket(udp, broadcastIp, Limits::kWolPortPrimary,
                                     packet);
    anySent |= sendSingleMagicPacket(udp, broadcastIp,
                                     Limits::kWolPortSecondary, packet);
  }
  // Directed unicast UDP packet helps on switches/VLANs with broadcast filtering.
  anySent |= sendSingleMagicPacket(udp, target.runtime.ip,
                                   Limits::kWolPortPrimary, packet);

  outDurationMs = static_cast<uint32_t>(monotonicMs() - startedMs);
  if (Diagnostics::kPerformance) {
    Serial.printf("[PERF] WOL burst %s (%u rounds): %lu ms (%s)\n",
                  target.config.name, static_cast<unsigned>(repeatCount),
                  static_cast<unsigned long>(outDurationMs),
                  anySent ? "sent" : "failed");
  }
  return anySent;
}

bool triggerManualWake(WiFiUDP& udp, Target& target,
                       const IPAddress& broadcastIp, uint64_t now,
                       uint32_t& outDurationMs, bool& outCooldownBlocked) {
  outCooldownBlocked = false;
  outDurationMs = 0;
  if (!target.runtime.configured || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (target.runtime.lastWakeMs != 0 &&
      now - target.runtime.lastWakeMs < Timing::kWolCooldownMs) {
    outCooldownBlocked = true;
    return false;
  }

  const bool sent = sendWolBurst(udp, target, broadcastIp,
                                 Limits::kWolInitialBurstCount, outDurationMs);
  if (!sent) {
    return false;
  }

  target.runtime.lastWakeMs = now;
  target.runtime.followupPacketsRemaining = Limits::kWolFollowupCount;
  target.runtime.lastFollowupSendMs = now;

  // Arm the non-blocking boot tracker so the UI updates automatically on boot.
  target.runtime.trackingWake = true;
  target.runtime.wakeTimedOut = false;
  target.runtime.bootedJustNow = false;
  target.runtime.wakeStartedMs = now;
  target.runtime.lastTrackPingMs = now;

  Serial.printf("WOL burst sent to %s in %lu ms (monitoring boot)\n",
                target.config.name, static_cast<unsigned long>(outDurationMs));
  return true;
}

void serviceWolAndBootTracking(WiFiUDP& udp, Target* targets, size_t count,
                               const IPAddress& broadcastIp, uint64_t now,
                               bool& outUiUpdateRequested,
                               uint32_t& outLastPingMs) {
  outUiUpdateRequested = false;
  if (targets == nullptr || count == 0 || WiFi.status() != WL_CONNECTED) {
    return;
  }

  // 1. Dispatch any non-blocking follow-up WOL redundancy packets.
  for (size_t i = 0; i < count; ++i) {
    Target& target = targets[i];
    if (target.runtime.followupPacketsRemaining > 0 &&
        now - target.runtime.lastFollowupSendMs >=
            Timing::kWolFollowupSpacingMs) {
      uint32_t unusedMs = 0;
      sendWolBurst(udp, target, broadcastIp, 1, unusedMs);
      --target.runtime.followupPacketsRemaining;
      target.runtime.lastFollowupSendMs = now;
    }
  }

  // 2. Collect targets due for boot-verification ping.
  PingTargetRequest pingRequests[Limits::kTargetCount]{};
  PingTargetOutcome pingOutcomes[Limits::kTargetCount]{};
  bool anyTrackingActive = false;
  bool anyPingDue = false;

  const size_t boundedCount =
      count > Limits::kTargetCount ? Limits::kTargetCount : count;
  for (size_t i = 0; i < boundedCount; ++i) {
    Target& target = targets[i];
    pingRequests[i].ip = target.runtime.ip;
    if (!target.runtime.configured || !target.runtime.trackingWake) {
      continue;
    }
    anyTrackingActive = true;

    const uint64_t elapsedWakeMs = now - target.runtime.wakeStartedMs;
    if (elapsedWakeMs >= Timing::kWakeTrackTimeoutMs) {
      target.runtime.trackingWake = false;
      target.runtime.wakeTimedOut = true;
      target.runtime.state = TargetState::kOffline;
      outUiUpdateRequested = true;
      Serial.printf("Boot tracker timed out for %s after 60s\n",
                    target.config.name);
      continue;
    }

    if (elapsedWakeMs >= Timing::kWakeTrackStartDelayMs &&
        now - target.runtime.lastTrackPingMs >= Timing::kWakeTrackIntervalMs) {
      pingRequests[i].enabled = true;
      target.runtime.lastTrackPingMs = now;
      anyPingDue = true;
    }
  }

  if (anyPingDue) {
    const uint64_t pingStartMs = monotonicMs();
    boundedPingMultiple(pingRequests, boundedCount, Timing::kPingTimeoutMs,
                        Timing::kPingRetryAfterMs, pingOutcomes);
    const uint64_t finishedMs = monotonicMs();
    outLastPingMs = static_cast<uint32_t>(finishedMs - pingStartMs);

    for (size_t i = 0; i < boundedCount; ++i) {
      if (!pingRequests[i].enabled) {
        continue;
      }
      Target& target = targets[i];
      target.runtime.lastCheckMs = finishedMs;
      target.runtime.responseTimeMs = pingOutcomes[i].responseTimeMs;

      if (pingOutcomes[i].result == BoundedPingResult::kReply) {
        target.runtime.state = TargetState::kOnline;
        target.runtime.lastSuccessfulCheckMs = finishedMs;
        target.runtime.trackingWake = false;
        target.runtime.wakeTimedOut = false;
        target.runtime.bootedJustNow = true;
        target.runtime.lastBootSeconds =
            static_cast<float>(finishedMs - target.runtime.wakeStartedMs) /
            1000.0F;
        outUiUpdateRequested = true;
        Serial.printf("%s is ONLINE (booted in %.1fs, ping %.1fms)\n",
                      target.config.name, target.runtime.lastBootSeconds,
                      target.runtime.responseTimeMs);
      } else if (pingOutcomes[i].result == BoundedPingResult::kTimeout) {
        target.runtime.state = TargetState::kOffline;
      }
    }
  }

  static uint64_t lastProgressUiMs = 0;
  if (anyTrackingActive && !outUiUpdateRequested &&
      (lastProgressUiMs == 0 ||
       now - lastProgressUiMs >= Timing::kWakeTrackUiUpdateMs)) {
    lastProgressUiMs = now;
    outUiUpdateRequested = true;
  } else if (!anyTrackingActive) {
    lastProgressUiMs = 0;
  }
}
