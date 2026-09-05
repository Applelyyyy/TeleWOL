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

}  // namespace

BoundedPingResult boundedPing(const IPAddress& target, uint32_t timeoutMs,
                              float& responseTimeMs) {
  responseTimeMs = 0.0F;
  if (timeoutMs == 0) {
    return BoundedPingResult::kError;
  }

  const int socketFd = lwip_socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
  if (socketFd < 0) {
    return BoundedPingResult::kError;
  }

  if (!setReceiveTimeout(socketFd, timeoutMs)) {
    lwip_close(socketFd);
    return BoundedPingResult::kError;
  }

  static uint16_t sequence = 0;
  ++sequence;
  const uint16_t identifier = static_cast<uint16_t>(0xA500U | (millis() & 0xFFU));

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

  const uint32_t startedUs = micros();
  const int sent = lwip_sendto(socketFd, &packet, sizeof(packet), 0,
                               reinterpret_cast<sockaddr*>(&destination),
                               sizeof(destination));
  if (sent != static_cast<int>(sizeof(packet))) {
    lwip_close(socketFd);
    return BoundedPingResult::kError;
  }

  uint8_t response[96];
  BoundedPingResult result = BoundedPingResult::kTimeout;
  const uint32_t startedMs = millis();

  while (millis() - startedMs < timeoutMs) {
    const uint32_t elapsedMs = millis() - startedMs;
    if (elapsedMs >= timeoutMs) break;
    const uint32_t calculatedRemaining = timeoutMs - elapsedMs;
    const uint32_t remainingMs = calculatedRemaining == 0 ? 1U : calculatedRemaining;
    if (!setReceiveTimeout(socketFd, remainingMs)) {
      result = BoundedPingResult::kError;
      break;
    }

    sockaddr_in source{};
    socklen_t sourceLength = sizeof(source);
    const int received = lwip_recvfrom(socketFd, response, sizeof(response), 0,
                                       reinterpret_cast<sockaddr*>(&source),
                                       &sourceLength);
    if (received < 0) {
      break;
    }
    if (received < static_cast<int>(sizeof(ip_hdr) + sizeof(icmp_echo_hdr))) {
      continue;
    }

    const auto* ipHeader = reinterpret_cast<const ip_hdr*>(response);
    const size_t ipHeaderLength = IPH_HL(ipHeader) * 4U;
    if (ipHeaderLength + sizeof(icmp_echo_hdr) > static_cast<size_t>(received)) {
      continue;
    }

    const auto* echo = reinterpret_cast<const icmp_echo_hdr*>(response + ipHeaderLength);
    if (ICMPH_TYPE(echo) == ICMP_ER && echo->id == htons(identifier) &&
        echo->seqno == htons(sequence)) {
      responseTimeMs = static_cast<float>(micros() - startedUs) / 1000.0F;
      result = BoundedPingResult::kReply;
      break;
    }
  }

  lwip_close(socketFd);
  return result;
}
