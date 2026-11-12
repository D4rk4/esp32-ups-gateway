#pragma once
#include "ups_driver.hpp"

namespace ups {
class MegaTecQ1Driver final : public UpsDriver {
 public:
  const DriverInfo& info() const override;
  bool buildPoll(PollKind kind, std::string& bytes) const override;
  std::size_t maxReplyLength() const override;
  bool frameComplete(PollKind kind, const std::string& buffer, uint32_t idle_ms) const override;
  ParseResult parseReply(PollKind kind, const std::string& line, Snapshot& state) const override;
  bool encodeCommand(Command command, const CommandArgs& args, EncodedCommand& out,
                     std::string& error) const override;
};
}
