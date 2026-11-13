#include "snmp_service.hpp"
#include "config_model.hpp"
#include "event_bus.hpp"
#include "powernet_mib.hpp"
#include "snmp.hpp"
#include "ups_service.hpp"
#include <Arduino.h>
#include <ETH.h>
#include <WiFiUdp.h>
#include <array>
#include <string>
#include <utility>
#include <vector>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "unknown"
#endif

namespace gateway {
namespace {
constexpr uint16_t kAgentPort = 161;
constexpr size_t kMaxPacket = 1400;
constexpr unsigned kWorkPerIteration = 4;
constexpr uint32_t kBindRetryMs = 1000;
constexpr uint64_t kTrapMaxAgeMs = 30000;
constexpr TickType_t kTaskDelay = pdMS_TO_TICKS(5) ? pdMS_TO_TICKS(5) : 1;

class PowerNetWriter final : public snmp::Writer {
 public:
  PowerNetWriter(UpsService& ups,const ups::Snapshot& snapshot,bool fresh):ups_(ups),snapshot_(snapshot),fresh_(fresh){}
  int32_t set(const std::vector<snmp::VarBind>& bindings,size_t& errorIndex) override {
    invoked=true;
    PowerNetControlAction action;
    const int32_t error=mapPowerNetSet(bindings,snapshot_,fresh_,action,errorIndex);
    if(error||!action.pending)return error;
    uint32_t id=0;std::string reason;
    if(!ups_.enqueue(action.command,action.args,id,reason)){errorIndex=action.index;return 12;}
    return 0;
  }
  bool invoked{false};
 private:
  UpsService& ups_;const ups::Snapshot& snapshot_;bool fresh_;
};

bool ipv4(const std::string& text, IPAddress& address) {
  uint32_t parsed = 0;
  if (!parseIPv4(text, parsed)) return false;
  address = IPAddress(uint8_t(parsed >> 24), uint8_t(parsed >> 16),
                      uint8_t(parsed >> 8), uint8_t(parsed));
  return true;
}

bool sendDatagram(WiFiUDP& socket, IPAddress address, uint16_t port,
                  const std::vector<uint8_t>& bytes) {
  // A packet is kept below the Arduino UDP implementation's 1460-byte transmit
  // buffer, so WiFiUDP::write cannot split it into separate datagrams.
  return !bytes.empty() && bytes.size() <= kMaxPacket &&
         socket.beginPacket(address, port) == 1 &&
         socket.write(bytes.data(), bytes.size()) == bytes.size() &&
         socket.endPacket() == 1;
}

snmp::Oid trapOid(EventType type) {
  const uint32_t number = powerNetTrapNumber(type);
  if (number) return {1,3,6,1,4,1,318,0,number};
  if (type == EventType::Startup || type == EventType::TestTrap)
    return {1,3,6,1,6,3,1,1,5,1}; // coldStart; test notification is labelled below.
  return {};
}

std::vector<snmp::VarBind> trapArguments(const Event& event, const Config& config,
                                       uint32_t bootId) {
  snmp::Value argument;
  argument.type = snmp::Type::OctetString;
  if (event.type == EventType::TestTrap) argument.text = "TEST TRAP: ";
  // EventBus caps stored messages to 180 bytes. Keep this bound explicit even
  // if another event source is added later.
  argument.text += event.message.substr(0, 180);
  argument.text += "; boot_id=" + std::to_string(bootId);
  argument.text += "; seq=" + std::to_string(event.seq);
  argument.text += "; event_uptime_ms=" + std::to_string(event.uptime_ms);
  snmp::Value name;
  name.type = snmp::Type::OctetString;
  name.text = config.hostname;
  // APC notifications in use require mtrapargsString; sysName identifies the
  // gateway alongside the event's boot/sequence/time information.
  return {{{1,3,6,1,4,1,318,2,3,3,0}, std::move(argument)},
          {{1,3,6,1,2,1,1,5,0}, std::move(name)}};
}

struct Recipient {
  bool enabled{false};
  IPAddress address;
  uint16_t port{162};
  std::string community;
};
}  // namespace

SnmpService::SnmpService(const Config& config, UpsService& ups, EventBus& events,
                         uint32_t bootId)
    : config_(config), ups_(ups), events_(events), boot_id_(bootId) {}

bool SnmpService::begin() {
  if (task_) return true;
  if (xTaskCreate(taskEntry, "snmp", 12288, this, 1, &task_) != pdPASS) {
    task_ = nullptr;
    return false;
  }
  return true;
}

SnmpStats SnmpService::stats() const {
  return {sent_.load(), failed_.load(), events_.dropped(), events_.queued()};
}

void SnmpService::taskEntry(void* context) {
  static_cast<SnmpService*>(context)->run();
  vTaskDelete(nullptr);
}

void SnmpService::run() {
  // These sockets are created and accessed only by this task. No Arduino
  // network callback mutates them or shares a pending packet with another task.
  WiFiUDP listener, sender;
  bool listening = false, senderReady = false, wasReady = false;
  bool attemptedBind = false;
  uint32_t lastBindAttempt = 0;
  IPAddress activeAddress;
  IPAddress allowedManager;
  const bool restrictManager = !config_.snmp_manager_ip.empty();
  const bool validManager = !restrictManager || ipv4(config_.snmp_manager_ip, allowedManager);

  std::array<Recipient, 2> recipients;
  bool hasRecipients = false;
  for (size_t i = 0; i < recipients.size(); ++i) {
    const auto& target = config_.traps[i];
    auto& recipient = recipients[i];
    recipient.enabled = target.enabled && target.port != 0 &&
                        !target.community.empty() && target.community.size() <= 64 &&
                        ipv4(target.ip, recipient.address);
    recipient.port = target.port;
    recipient.community = target.community;
    hasRecipients = hasRecipients || recipient.enabled;
  }

  std::array<uint8_t, kMaxPacket> input{};
  std::vector<uint8_t> output;
  output.reserve(kMaxPacket);
  for (;;) {
    const IPAddress localAddress = ETH.localIP();
    const bool ready = ETH.linkUp() && uint32_t(localAddress) != 0;
    if (!ready || (wasReady && localAddress != activeAddress)) {
      if (wasReady || listening || senderReady) {
        listener.stop();
        sender.stop();
      }
      listening = false;
      senderReady = false;
      attemptedBind = false;
    }
    if (ready && (!wasReady || localAddress != activeAddress)) activeAddress = localAddress;
    wasReady = ready;

    if (ready && config_.snmp_enabled && validManager && !listening) {
      const uint32_t now = millis();
      if (!attemptedBind || uint32_t(now - lastBindAttempt) >= kBindRetryMs) {
        attemptedBind = true;
        lastBindAttempt = now;
        listening = listener.begin(activeAddress, kAgentPort) == 1;
      }
    }

    if (listening) {
      for (unsigned request = 0; request < kWorkPerIteration; ++request) {
        const int length = listener.parsePacket();
        if (length <= 0) break;
        const IPAddress manager = listener.remoteIP();
        const uint16_t managerPort = listener.remotePort();
        if (size_t(length) > input.size() || managerPort == 0 ||
            (restrictManager && manager != allowedManager)) {
          // In Arduino-ESP32 2.0.17, parsePacket receives one datagram into a
          // buffer capped at 1460; flush frees its remainder directly. An
          // oversized datagram therefore cannot cause an unbounded drain loop.
          listener.flush();
          continue;
        }
        const int received = listener.read(input.data(), size_t(length));
        listener.flush();
        if (received != length) continue;

        const uint64_t now=uptimeMs();
        if (!replay_.respond(uint32_t(manager),managerPort,input.data(),size_t(length),now,
            [&](std::vector<uint8_t>& response,bool& invoked) {
              // Each new packet sees one consistent snapshot. Replays reuse
              // the original response without sampling or invoking the writer.
              const auto snapshot = ups_.snapshot();
              const uint32_t ticks = uint32_t(now / 10);
              const PowerNetMib mib(config_, snapshot, ticks, FIRMWARE_VERSION);
              const bool fresh=snapshot.connected&&snapshot.status_valid&&uint32_t(uint32_t(now)-snapshot.last_success_ms)<config_.stale_ms;
              PowerNetWriter writer(ups_,snapshot,fresh);
              const bool ok=snmp::processRequest(input.data(),size_t(length),config_.snmp_community,
                                                 config_.snmp_write_community,mib,&writer,response);
              invoked=writer.invoked;return ok;
            },output)) continue;
        if (!sendDatagram(listener, manager, managerPort, output)) {
          // Reset any partially prepared transmission and rebind later. This
          // request is not retried and cannot send an incomplete BER payload.
          listener.stop();
          listening = false;
          break;
        }
      }
    }

    // A disabled listener does not disable independently configured traps.
    // With no recipients, discard queued notifications without send/failure
    // accounting; the EventBus history already retains their log entries.
    if (!hasRecipients || ready) {
      for (unsigned count = 0; count < kWorkPerIteration; ++count) {
        Event event;
        if (!events_.popTrap(event)) break;
        if (!hasRecipients) continue;
        const uint64_t now = uptimeMs();
        if (now >= event.uptime_ms && now - event.uptime_ms > kTrapMaxAgeMs) {
          events_.countDropped();
          continue;
        }
        const auto oid = trapOid(event.type);
        if (oid.empty()) continue;
        const auto arguments = trapArguments(event, config_, boot_id_);
        for (const auto& recipient : recipients) {
          if (!recipient.enabled) continue;
          if (!snmp::encodeTrap(recipient.community, uint32_t(event.uptime_ms / 10),
                                oid, arguments, output)) {
            ++failed_;
            continue;
          }
          if (!senderReady) senderReady = sender.begin(activeAddress, 0) == 1;
          if (senderReady && sendDatagram(sender, recipient.address, recipient.port, output)) {
            ++sent_;
          } else {
            ++failed_;
            sender.stop();
            senderReady = false;
          }
        }
      }
    }
    vTaskDelay(kTaskDelay);
  }
}
}  // namespace gateway
