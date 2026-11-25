#pragma once

#include <WiFiUdp.h>
#include "AppTypes.h"

// Sends a rapid burst of Magic Packets (UDP Port 9 + Port 7, broadcast + directed).
bool sendWolBurst(WiFiUDP& udp, const Target& target,
                  const IPAddress& broadcastIp, uint8_t repeatCount,
                  uint32_t& outDurationMs);

// Triggers an immediate manual wake burst and activates the non-blocking
// background boot tracker for the specified target.
bool triggerManualWake(WiFiUDP& udp, Target& target,
                       const IPAddress& broadcastIp, uint64_t now,
                       uint32_t& outDurationMs, bool& outCooldownBlocked);

// Services follow-up WOL packets and periodic parallel ICMP ping checks for
// targets currently booting after a Wake-on-LAN trigger.
void serviceWolAndBootTracking(WiFiUDP& udp, Target* targets, size_t count,
                               const IPAddress& broadcastIp, uint64_t now,
                               bool& outUiUpdateRequested,
                               uint32_t& outLastPingMs);
