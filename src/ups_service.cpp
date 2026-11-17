#include "ups_service.hpp"
#include "driver_registry.hpp"
#include "ups_extended_probe.hpp"
#include <Arduino.h>
#include <HardwareSerial.h>
#include <cmath>
#include <type_traits>
#include <utility>

namespace gateway {
namespace {
constexpr uint32_t kOptionalPeriodMs = 600000;
constexpr uint32_t kResyncMs = 20;
constexpr std::size_t kDrainLimit = 1024;

const char* commandName(ups::Command command) {
  const auto* info=ups::findCommand(command);
  return info?info->name:"unknown";
}
uint32_t serialFormat(const ups::DriverInfo& info) {
  if (info.stop_bits == 1) {
    switch (info.parity) {
      case ups::Parity::None: return SERIAL_8N1;
      case ups::Parity::Even: return SERIAL_8E1;
      case ups::Parity::Odd: return SERIAL_8O1;
    }
  } else if (info.stop_bits == 2) {
    switch (info.parity) {
      case ups::Parity::None: return SERIAL_8N2;
      case ups::Parity::Even: return SERIAL_8E2;
      case ups::Parity::Odd: return SERIAL_8O2;
    }
  }
  return 0;
}
void drainInput() {
  for (std::size_t n=0; n<kDrainLimit && Serial2.available()>0; ++n) Serial2.read();
}
// Keep only printable ASCII, capped at 32 chars, for event-logged raw replies.
std::string sanitizeReply(const std::string& input) {
  std::string out;
  for (const unsigned char c : input) {
    if (c >= 32 && c <= 126) out.push_back(static_cast<char>(c));
  }
  if (out.size() > 32) out.resize(32);
  return out;
}
void publishObservation(EventBus& bus, EventEngine& engine, const ups::Snapshot& state) {
  for (const auto& event : engine.observe(state,uptimeMs()))
    bus.publish(event.type,event.severity,event.message,event.uptime_ms);
}
struct OptionalPoll {
  uint8_t attempts{0};
  uint32_t first_attempt_ms{0};
  bool due(uint32_t now) {
    if (attempts >= 2 && uint32_t(now-first_attempt_ms) >= kOptionalPeriodMs) attempts=0;
    return attempts < 2;
  }
  void started(uint32_t now) {
    if (!attempts) first_attempt_ms=now;
    ++attempts;
  }
};
}

UpsService::UpsService(const Config& config, EventBus& events) : config_(config), events_(events) {}

bool UpsService::begin() {
  if (task_) return true;
  driver_=ups::findDriver(config_.driver);
  if (!driver_ || !driver_->maxReplyLength() || !serialFormat(driver_->info())) return false;
  if (!mutex_) mutex_=xSemaphoreCreateMutex();
  if (!mutex_) return false;
  static_assert(std::is_trivially_copyable<Request>::value,"FreeRTOS queue requests must be POD");
  if (!queue_) queue_=xQueueCreate(4,sizeof(Request));
  if (!queue_) return false;
  return xTaskCreate(taskEntry,"ups-uart",8192,this,1,&task_) == pdPASS;
}

ups::Snapshot UpsService::snapshot() const {
  if (!mutex_) return {};
  xSemaphoreTake(mutex_,portMAX_DELAY);
  ups::Snapshot copy=state_;
  xSemaphoreGive(mutex_);
  return copy;
}
ControlStatus UpsService::controlStatus() const {
  if (!mutex_) return {};
  xSemaphoreTake(mutex_,portMAX_DELAY);
  ControlStatus copy=control_;
  xSemaphoreGive(mutex_);
  return copy;
}

bool UpsService::validateCommandLocked(ups::Command command, const ups::CommandArgs& args,
                                     uint32_t now, ups::EncodedCommand& encoded,
                                     std::string& error) const {
  if (!config_.controls_enabled) { error="UPS controls are disabled"; return false; }
  const auto* info=ups::findCommand(command);
  if (!driver_ || !info || !(driver_->info().capabilities & info->capability)) {
    error="Command is not supported by the selected driver"; return false;
  }
  if (!driver_->encodeCommand(command,args,encoded,error) || encoded.bytes.empty()) {
    if (error.empty()) error="Driver rejected the command";
    return false;
  }
  if (encoded.changes_output && !config_.power_controls_enabled) {
    error="Power controls are disabled"; return false;
  }
  if (!state_.connected || !state_.status_valid || uint32_t(now-state_.last_success_ms)>=config_.stale_ms) {
    error="A fresh connected UPS status is required"; return false;
  }
  return true;
}

bool UpsService::enqueue(ups::Command command, const ups::CommandArgs& args,
                         uint32_t& id, std::string& error) {
  id=0; error.clear();
  if (!queue_ || !mutex_ || !task_) { error="UPS service is unavailable"; return false; }
  ups::EncodedCommand encoded;
  xSemaphoreTake(mutex_,portMAX_DELAY);
  if (command_pending_) {
    xSemaphoreGive(mutex_); error="Another UPS command is pending"; return false;
  }
  if (!validateCommandLocked(command,args,millis(),encoded,error)) {
    xSemaphoreGive(mutex_); return false;
  }
  // Reserve the pending slot before the nonblocking queue insertion. This lock
  // prevents concurrent HTTP callers from admitting duplicate shutdowns.
  command_pending_=true;
  if (++next_id_ == 0) ++next_id_;
  const Request request{command,args,next_id_};
  control_={request.id,commandName(command),"queued","Command queued"};
  if (xQueueSend(queue_,&request,0) != pdTRUE) {
    command_pending_=false;
    control_.last_state="failed"; control_.last_message="UPS command queue is full";
    xSemaphoreGive(mutex_);
    error="UPS command queue is full";
    events_.publish(EventType::CommandFailed,2,std::string(commandName(command))+": "+error);
    return false;
  }
  xSemaphoreGive(mutex_);
  events_.publish(EventType::CommandQueued,1,std::string(commandName(command))+": Command queued");
  // The worker may peek the request immediately, but cannot consume it until
  // the queued event is published. EventBus publication never holds state mutex.
  xSemaphoreTake(mutex_,portMAX_DELAY);
  ready_command_id_=request.id;
  xSemaphoreGive(mutex_);
  id=request.id;
  return true;
}

void UpsService::finishCommand(const Request& request, bool sent, const std::string& message) {
  xSemaphoreTake(mutex_,portMAX_DELAY);
  control_={request.id,commandName(request.command),sent?"sent_unconfirmed":"failed",message};
  xSemaphoreGive(mutex_);
  events_.publish(sent?EventType::CommandSent:EventType::CommandFailed,sent?1:2,
                  std::string(commandName(request.command))+": "+message);
  xSemaphoreTake(mutex_,portMAX_DELAY);
  command_pending_=false;
  xSemaphoreGive(mutex_);
}

void UpsService::taskEntry(void* context) { static_cast<UpsService*>(context)->run(); }

void UpsService::run() {
  Serial2.begin(config_.baud,serialFormat(driver_->info()),UPS_UART_RX,UPS_UART_TX);
  const uint32_t boot_ms=millis();
  EventEngine engine;
  OptionalPoll rating,identity;
  // Self-disabling extended probes (BT runtime / BP charge): unsupported after
  // 3 consecutive failures until reboot, never sent more than once per period,
  // and never allowed to touch the Q1 error counter or disconnect state.
  ups::ExtendedProbe runtime_probe(config_.response_timeout_ms, 3), charge_probe(config_.response_timeout_ms, 3);
  bool runtime_logged=false, charge_logged=false;
  bool initial_observation=false, status_requested=false, active=false;
  bool overflow=false, transaction_error=false, settling=false, resync=false;
  uint32_t last_status_request=0, started=0, last_byte=0, settle_started=0, settle_ms=0, resync_started=0;
  ups::PollKind kind=ups::PollKind::Status;
  std::string buffer;
  buffer.reserve(driver_->maxReplyLength());

  auto countError=[&]() {
    if (transaction_error) return;
    xSemaphoreTake(mutex_,portMAX_DELAY); ++state_.errors; xSemaphoreGive(mutex_);
    transaction_error=true;
  };
  auto resynchronize=[&](uint32_t now) {
    active=false; buffer.clear(); overflow=false;
    resync=true; resync_started=now;
    drainInput();
  };
  auto acceptFrame=[&](uint32_t now) {
    if (overflow) {
      // The rolling bounded buffer is used only to find a driver-defined end;
      // a suffix of an overlong frame is never offered to the parser.
      buffer.clear(); overflow=false; return;
    }
    ups::Snapshot next=snapshot();
    const auto result=driver_->parseReply(kind,buffer,next);
    const std::string reply=sanitizeReply(buffer);
    buffer.clear();
    if (result==ups::ParseResult::Ignored) {
      if (kind==ups::PollKind::ExtendedRuntime || kind==ups::PollKind::ExtendedCharge) {
        (kind==ups::PollKind::ExtendedRuntime ? runtime_probe : charge_probe).recordFailure();
        events_.publish(EventType::ExtendedQuery,1,
            std::string("UPS extended query ")
            +(kind==ups::PollKind::ExtendedRuntime?"BT":"BP")
            +" unsupported (NAK/echo)");
      }
      return;
    }
    if (result==ups::ParseResult::Invalid) {
      if (kind==ups::PollKind::ExtendedRuntime || kind==ups::PollKind::ExtendedCharge) {
        (kind==ups::PollKind::ExtendedRuntime ? runtime_probe : charge_probe).recordFailure();
        events_.publish(EventType::ExtendedQuery,1,
            std::string("UPS extended query ")
            +(kind==ups::PollKind::ExtendedRuntime?"BT":"BP")
            +" unsupported ("+reply+")");
      } else countError();
      return;
    }
    if (kind==ups::PollKind::Status) {
      const bool reconnect=!next.connected;
      next.connected=true; next.last_success_ms=now; ++next.successful_polls;
      const float pack=next.battery_voltage_raw.value*config_.battery_multiplier;
      next.battery_voltage={pack,next.battery_voltage_raw.valid && config_.battery_multiplier>0 && std::isfinite(pack)};
      if (!next.battery_voltage.valid) next.battery_voltage.value=0;
      if (reconnect) { rating={}; identity={}; }
      xSemaphoreTake(mutex_,portMAX_DELAY); state_=next; xSemaphoreGive(mutex_);
      initial_observation=true;
      publishObservation(events_,engine,next);
    } else if (kind==ups::PollKind::ExtendedRuntime || kind==ups::PollKind::ExtendedCharge) {
      // Successful extended reply: update snapshot and log the capability once.
      xSemaphoreTake(mutex_,portMAX_DELAY); state_=std::move(next); xSemaphoreGive(mutex_);
      bool& logged = kind==ups::PollKind::ExtendedRuntime ? runtime_logged : charge_logged;
      if (!logged) {
        logged=true;
        events_.publish(EventType::ExtendedQuery,1,
            std::string("UPS extended query ")
            +(kind==ups::PollKind::ExtendedRuntime?"BT":"BP")
            +" supported");
      }
      (kind==ups::PollKind::ExtendedRuntime ? runtime_probe : charge_probe).recordSuccess();
    } else {
      // F/I mutate only their fields; successful optional replies do not count
      // as status success and never extend connection freshness.
      xSemaphoreTake(mutex_,portMAX_DELAY); state_=std::move(next); xSemaphoreGive(mutex_);
      (kind==ups::PollKind::Rating?rating:identity).attempts=2;
    }
    active=false;
    drainInput();
  };

  for (;;) {
    uint32_t now=millis();
    if (settling || resync) {
      drainInput();
      if (settling && uint32_t(now-settle_started)>=settle_ms) {
        settling=false;
        resync=true; resync_started=now;
        status_requested=false; // Refresh actual UPS status after every control.
      }
      if (resync && uint32_t(now-resync_started)>=kResyncMs) resync=false;
    } else if (active) {
      // Read available bytes only. Embedded NULs and high bytes are preserved;
      // both byte-count and idle-based completion belong to the driver.
      for (std::size_t n=0; active && n<kDrainLimit && Serial2.available()>0; ++n) {
        const int raw=Serial2.read();
        if (raw<0) break;
        now=millis(); last_byte=now;
        if (buffer.size()>=driver_->maxReplyLength()) {
          overflow=true; countError();
          buffer.erase(0,1);
        }
        buffer.push_back(static_cast<char>(static_cast<uint8_t>(raw)));
        if (driver_->frameComplete(kind,buffer,0)) acceptFrame(now);
      }
      now=millis();
      if (active && !buffer.empty() && driver_->frameComplete(kind,buffer,uint32_t(now-last_byte))) acceptFrame(now);
      if (active && uint32_t(now-started)>=config_.response_timeout_ms) {
        if (kind==ups::PollKind::ExtendedRuntime || kind==ups::PollKind::ExtendedCharge) {
          // Extended probe timeout is not a UPS comm error: never mark
          // disconnected, never increment errors, never delay Q1 beyond one
          // extra response timeout. Just record the probe failure.
          (kind==ups::PollKind::ExtendedRuntime ? runtime_probe : charge_probe).recordFailure();
        } else {
          countError(); resynchronize(now);
        }
      }
    } else {
      drainInput();
    }

    now=millis();
    ups::Snapshot current=snapshot();
    if ((current.connected && uint32_t(now-current.last_success_ms)>=config_.stale_ms)
        || (!initial_observation && uint32_t(now-boot_ms)>=config_.stale_ms)) {
      current.connected=false;
      xSemaphoreTake(mutex_,portMAX_DELAY); state_.connected=false; xSemaphoreGive(mutex_);
      initial_observation=true;
      publishObservation(events_,engine,current);
    }

    if (!active && !settling && !resync) {
      Request request{};
      bool have_command=false;
      if (xQueuePeek(queue_,&request,0)==pdTRUE) {
        xSemaphoreTake(mutex_,portMAX_DELAY);
        if (ready_command_id_==request.id) {
          have_command=xQueueReceive(queue_,&request,0)==pdTRUE;
          if (have_command) ready_command_id_=0;
        }
        xSemaphoreGive(mutex_);
      }
      if (have_command) {
        ups::EncodedCommand encoded;
        std::string error;
        xSemaphoreTake(mutex_,portMAX_DELAY);
        const bool allowed=validateCommandLocked(request.command,request.args,millis(),encoded,error);
        xSemaphoreGive(mutex_);
        if (!allowed) finishCommand(request,false,error);
        else {
          drainInput();
          const std::size_t written=Serial2.write(reinterpret_cast<const uint8_t*>(encoded.bytes.data()),encoded.bytes.size());
          settle_started=millis(); settle_ms=encoded.settle_ms; settling=true;
          finishCommand(request,written==encoded.bytes.size(),written==encoded.bytes.size()
              ?"Command transmitted; execution is unconfirmed":"UART could not transmit the complete command");
        }
      } else {
        bool should_poll=false;
        if (!status_requested || uint32_t(now-last_status_request)>=config_.poll_ms) {
          kind=ups::PollKind::Status; should_poll=true;
        } else if (current.connected && current.status_valid && uint32_t(now-current.last_success_ms)<config_.stale_ms) {
          if (rating.due(now)) { kind=ups::PollKind::Rating; should_poll=true; rating.started(now); }
          else if (identity.due(now)) { kind=ups::PollKind::Identity; should_poll=true; identity.started(now); }
          else if (runtime_probe.due(now)) { kind=ups::PollKind::ExtendedRuntime; should_poll=true; runtime_probe.markSent(now); }
          else if (charge_probe.due(now)) { kind=ups::PollKind::ExtendedCharge; should_poll=true; charge_probe.markSent(now); }
        }
        if (should_poll) {
          std::string query;
          if (kind==ups::PollKind::Status) { status_requested=true; last_status_request=now; }
          if (driver_->buildPoll(kind,query) && !query.empty()) {
            drainInput(); buffer.clear(); overflow=false; transaction_error=false;
            started=millis(); last_byte=started;
            active=Serial2.write(reinterpret_cast<const uint8_t*>(query.data()),query.size())==query.size();
            if (!active) { countError(); resynchronize(millis()); }
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
}
