#pragma once
#include "ups_model.hpp"
#include <cstddef>
#include <string>
namespace ups {
enum class PollKind { Status, Rating, Identity };
enum class ParseResult { Accepted, Ignored, Invalid };
struct EncodedCommand { std::string bytes; uint32_t settle_ms{500}; bool changes_output{false}; };
class UpsDriver {
 public:
  virtual ~UpsDriver() = default;
  virtual const DriverInfo& info() const = 0;
  virtual bool buildPoll(PollKind kind, std::string& bytes) const = 0;
  // Framing belongs to the protocol. Buffers contain unmodified wire bytes;
  // idle_ms is elapsed time since the last received byte, for RTU-style frames.
  virtual std::size_t maxReplyLength() const = 0;
  virtual bool frameComplete(PollKind kind, const std::string& buffer, uint32_t idle_ms) const = 0;
  virtual ParseResult parseReply(PollKind kind, const std::string& line, Snapshot& state) const = 0;
  virtual bool encodeCommand(Command command, const CommandArgs& args, EncodedCommand& out, std::string& error) const = 0;
};
}

