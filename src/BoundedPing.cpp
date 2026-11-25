#include "BoundedPing.h"

#include <cstring>

extern "C" {
#include <lwip/icmp.h>
#include <lwip/inet_chksum.h>
#include <lwip/ip.h>
#include <lwip/sockets.h>
}

namespace {

constexpr size_t kPayloadSize = 16;
constexpr size_t kMaxParallelTargets = 8;

struct EchoPacket {
  icmp_echo_hdr header;
  uint8_t payload[kPayloadSize];
};

bool setReceiveTimeout(int socketFd, uint32_t timeoutMs) {
  timeval timeout{};
  timeout.tv_sec = timeoutMs / 1000U;
  timeout.tv_usec = (timeoutMs % 1000U) * 1000U;
  return lwip_setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout)) == 0;
}

bool sendEchoRequest(int socketFd, const IPAddress& target, uint16_t identifier,
                     uint16_t sequence) {
  EchoPacket packet{};
  ICMPH_TYPE_SET(&packet.header, ICMP_ECHO);
  ICMPH_CODE_SET(&packet.header, 0);
  packet.header.id = htons(identifier);
  packet.header.seqno = htons(sequence);
  for (size_t i = 0; i < kPayloadSize; ++i) {
    packet.payload[i] = static_cast<uint8_t>(i);
  }
  packet.header.chksum = 0;
  packet.header.chksum = inet_chksum(&packet, sizeof(packet));

  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_addr.s_addr = static_cast<uint32_t>(target);

  const int sent = lwip_sendto(socketFd, &packet, sizeof(packet), 0,
                               reinterpret_cast<sockaddr*>(&destination),
                               sizeof(destination));
  return sent == static_cast<int>(sizeof(packet));
}

}  // namespace

BoundedPingResult boundedPing(const IPAddress& target, uint32_t timeoutMs,
                              float& responseTimeMs) {
  PingTargetRequest req;
  req.ip = target;
  req.enabled = true;
  PingTargetOutcome out{};
  boundedPingMultiple(&req, 1, timeoutMs, 0, &out);
  responseTimeMs = out.responseTimeMs;
  return out.result;
}

void boundedPingMultiple(const PingTargetRequest* targets, size_t count,
                         uint32_t timeoutMs, uint32_t retryAfterMs,
                         PingTargetOutcome* outcomes) {
  if (targets == nullptr || outcomes == nullptr || count == 0) {
    return;
  }
  const size_t boundedCount =
      count > kMaxParallelTargets ? kMaxParallelTargets : count;

  for (size_t i = 0; i < count; ++i) {
    outcomes[i].result = BoundedPingResult::kError;
    outcomes[i].responseTimeMs = 0.0F;
  }
  if (timeoutMs == 0) {
    return;
  }

  size_t enabledCount = 0;
  for (size_t i = 0; i < boundedCount; ++i) {
    if (targets[i].enabled) {
      ++enabledCount;
    }
  }
  if (enabledCount == 0) {
    return;
  }

  const int socketFd = lwip_socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
  if (socketFd < 0) {
    return;
  }

  static uint16_t baseSequence = 0;
  baseSequence = static_cast<uint16_t>(baseSequence + kMaxParallelTargets);
  const uint16_t identifier =
      static_cast<uint16_t>(0xA500U | (millis() & 0xFFU));

  bool completed[kMaxParallelTargets]{};
  uint32_t sentAtUs[kMaxParallelTargets]{};
  size_t remaining = 0;

  for (size_t i = 0; i < boundedCount; ++i) {
    if (!targets[i].enabled) {
      completed[i] = true;
      continue;
    }
    sentAtUs[i] = micros();
    if (sendEchoRequest(socketFd, targets[i].ip, identifier,
                        static_cast<uint16_t>(baseSequence + i))) {
      outcomes[i].result = BoundedPingResult::kTimeout;
      ++remaining;
    } else {
      outcomes[i].result = BoundedPingResult::kError;
      completed[i] = true;
    }
  }

  if (remaining == 0) {
    lwip_close(socketFd);
    return;
  }

  uint8_t response[96];
  bool retryDispatched = (retryAfterMs == 0 || retryAfterMs >= timeoutMs);
  const uint32_t startedMs = millis();

  while (remaining > 0) {
    const uint32_t elapsedMs = millis() - startedMs;
    if (elapsedMs >= timeoutMs) {
      break;
    }

    if (!retryDispatched && elapsedMs >= retryAfterMs) {
      retryDispatched = true;
      for (size_t i = 0; i < boundedCount; ++i) {
        if (!completed[i]) {
          sentAtUs[i] = micros();
          sendEchoRequest(socketFd, targets[i].ip, identifier,
                          static_cast<uint16_t>(baseSequence + i));
        }
      }
    }

    uint32_t sliceMs = timeoutMs - elapsedMs;
    if (!retryDispatched && retryAfterMs > elapsedMs) {
      const uint32_t untilRetryMs = retryAfterMs - elapsedMs;
      if (untilRetryMs < sliceMs) {
        sliceMs = untilRetryMs;
      }
    }
    if (sliceMs == 0) {
      sliceMs = 1U;
    }

    if (!setReceiveTimeout(socketFd, sliceMs)) {
      for (size_t i = 0; i < boundedCount; ++i) {
        if (!completed[i]) {
          outcomes[i].result = BoundedPingResult::kError;
        }
      }
      break;
    }

    sockaddr_in source{};
    socklen_t sourceLength = sizeof(source);
    const int received = lwip_recvfrom(socketFd, response, sizeof(response), 0,
                                       reinterpret_cast<sockaddr*>(&source),
                                       &sourceLength);
    if (received < 0) {
      continue;
    }
    if (received < static_cast<int>(sizeof(ip_hdr) + sizeof(icmp_echo_hdr))) {
      continue;
    }

    const auto* ipHeader = reinterpret_cast<const ip_hdr*>(response);
    const size_t ipHeaderLength = IPH_HL(ipHeader) * 4U;
    if (ipHeaderLength + sizeof(icmp_echo_hdr) >
        static_cast<size_t>(received)) {
      continue;
    }

    const auto* echo =
        reinterpret_cast<const icmp_echo_hdr*>(response + ipHeaderLength);
    if (ICMPH_TYPE(echo) != ICMP_ER || echo->id != htons(identifier)) {
      continue;
    }

    const uint16_t replySeq = ntohs(echo->seqno);
    for (size_t i = 0; i < boundedCount; ++i) {
      if (completed[i]) {
        continue;
      }
      if (replySeq == static_cast<uint16_t>(baseSequence + i) &&
          ipHeader->src.addr == static_cast<uint32_t>(targets[i].ip)) {
        outcomes[i].responseTimeMs =
            static_cast<float>(micros() - sentAtUs[i]) / 1000.0F;
        outcomes[i].result = BoundedPingResult::kReply;
        completed[i] = true;
        --remaining;
        break;
      }
    }
  }

  lwip_close(socketFd);
}
