#include "megatec_q1.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

namespace ups {
namespace {
constexpr std::size_t kMaxLine = 160;
const DriverInfo kInfo{"megatec_q1", "MegaTec Q1 (Sinus EVO: verify compatibility)",
    2400, CapBeeper | CapTest | CapTestCancel | CapShutdownCancel | CapOutputOff | CapOutputRestart};

std::vector<std::string> fields(const std::string& line) {
  std::vector<std::string> result;
  std::size_t pos = 1;
  while (pos < line.size()) {
    pos = line.find_first_not_of(' ', pos);
    if (pos == std::string::npos) break;
    const auto end = line.find(' ', pos);
    result.emplace_back(line.substr(pos, end == std::string::npos ? end : end-pos));
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return result;
}

// Only decimal wire representations are accepted, avoiding locale-dependent strtof,
// exponent/hex extensions, NaN/Inf, and partial numeric conversions.
bool parseMetric(const std::string& field, float min, float max, Metric& target) {
  bool missing = false;
  bool placeholder = !field.empty();
  for (const char c : field) {
    missing = missing || c == '@';
    placeholder = placeholder && (c == '@' || c == '.');
  }
  if (missing && placeholder) { target = {}; return true; }
  if (field.empty() || missing) return false;
  std::size_t i = 0;
  const bool negative = field[0] == '-';
  if (negative) ++i;
  bool dot = false, digit = false;
  double value = 0, fraction = 0.1;
  for (; i < field.size(); ++i) {
    const char c = field[i];
    if (c == '.' && !dot) { dot = true; continue; }
    if (c < '0' || c > '9') return false;
    digit = true;
    if (!dot) value = value * 10 + (c-'0');
    else { value += (c-'0') * fraction; fraction *= 0.1; }
  }
  if (negative) value = -value;
  if (!digit || !std::isfinite(value) || value < min || value > max) return false;
  target = {static_cast<float>(value), true};
  return true;
}

std::string trim(const std::string& s) {
  const auto first = s.find_first_not_of(' ');
  if (first == std::string::npos) return {};
  return s.substr(first, s.find_last_not_of(' ')-first+1);
}

bool parseStatus(const std::string& line, Snapshot& next) {
  if (line.empty() || line.front() != '(') return false;
  const auto f = fields(line);
  if (f.size() != 8 || f[7].size() != 8) return false;
  for (const char bit : f[7]) if (bit != '0' && bit != '1') return false;
  // Broad protocol field limits, not model-specific safe operating thresholds.
  const std::array<float,7> minimum{{0,0,0,0,0,0,-99.9f}};
  const std::array<float,7> maximum{{999.9f,999.9f,999.9f,999,99.9f,999.9f,199.9f}};
  const std::array<Metric*,7> metrics{{&next.input_voltage,&next.input_fault_voltage,
      &next.output_voltage,&next.load_percent,&next.input_frequency,
      &next.battery_voltage_raw,&next.ups_temperature}};
  for (std::size_t i=0; i<metrics.size(); ++i)
    if (!parseMetric(f[i],minimum[i],maximum[i],*metrics[i])) return false;
  const std::array<bool*,8> bits{{&next.mains_failed,&next.low_battery,&next.bypass_or_avr,
      &next.ups_fault,&next.standby_topology,&next.test_running,&next.shutdown_active,&next.beeper_on}};
  for (std::size_t i=0; i<bits.size(); ++i) *bits[i] = f[7][i] == '1';
  next.status_valid = true;
  next.output_mode = next.mains_failed ? OutputMode::OnBattery :
      (next.bypass_or_avr && !next.standby_topology ? OutputMode::Bypass : OutputMode::Online);
  next.raw_status = f[7];
  next.last_status_line = line;
  return true;
}

bool parseRating(const std::string& line, Snapshot& next) {
  if (line.empty() || line.front() != '#') return false;
  const auto f = fields(line);
  if (f.size() != 4) return false;
  return parseMetric(f[0],0,999.9f,next.nominal_voltage)
      && parseMetric(f[1],0,999,next.nominal_current)
      && parseMetric(f[2],0,999.9f,next.nominal_battery_voltage)
      && parseMetric(f[3],0,99.9f,next.nominal_frequency);
}

bool parseIdentity(const std::string& line, Snapshot& next) {
  if (line.size() != 38 || line[0] != '#' || line[16] != ' ' || line[27] != ' ') return false;
  next.manufacturer = trim(line.substr(1,15));
  next.model = trim(line.substr(17,10));
  next.firmware = trim(line.substr(28,10));
  return true;
}
}

const DriverInfo& MegaTecQ1Driver::info() const { return kInfo; }
std::size_t MegaTecQ1Driver::maxReplyLength() const { return kMaxLine; }
bool MegaTecQ1Driver::frameComplete(PollKind, const std::string& buffer, uint32_t) const {
  return !buffer.empty() && (buffer.back() == '\r' || buffer.back() == '\n');
}

bool MegaTecQ1Driver::buildPoll(PollKind kind, std::string& bytes) const {
  bytes.clear();
  switch (kind) {
    case PollKind::Status: bytes = "Q1\r"; return true;
    case PollKind::Rating: bytes = "F\r"; return true;
    case PollKind::Identity: bytes = "I\r"; return true;
  }
  return false;
}

ParseResult MegaTecQ1Driver::parseReply(PollKind kind, const std::string& input, Snapshot& state) const {
  if (input.size() > kMaxLine) return ParseResult::Invalid;
  std::string line = input;
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
  if (line.empty() && !input.empty()) return ParseResult::Ignored;
  if (line == "Q1" || line == "F" || line == "I" || line == "NAK" || line == "(NAK")
    return ParseResult::Ignored;
  for (const unsigned char c : line) if (c < 32 || c > 126) return ParseResult::Invalid;
  Snapshot next = state;
  bool valid = false;
  switch (kind) {
    case PollKind::Status: valid = parseStatus(line,next); break;
    case PollKind::Rating: valid = parseRating(line,next); break;
    case PollKind::Identity: valid = parseIdentity(line,next); break;
  }
  if (!valid) return ParseResult::Invalid;
  state = std::move(next);
  return ParseResult::Accepted;
}

bool MegaTecQ1Driver::encodeCommand(Command command, const CommandArgs& args,
                                  EncodedCommand& out, std::string& error) const {
  out = {};
  error.clear();
  if ((command == Command::OutputOff || command == Command::OutputRestart)
      && (args.delay_minutes < 1 || args.delay_minutes > 10)) {
    error = "Shutdown delay must be 1..10 minutes";
    return false;
  }
  if (command == Command::OutputRestart && (args.restore_minutes < 3 || args.restore_minutes > 9999)) {
    error = "Restore delay must be 3..9999 minutes";
    return false;
  }
  char wire[16]{};
  switch (command) {
    case Command::BeeperToggle: out.bytes = "Q\r"; break;
    case Command::TestShort: out.bytes = "T\r"; break;
    case Command::TestCancel: out.bytes = "CT\r"; break;
    case Command::ShutdownCancel: out.bytes = "C\r"; out.changes_output = true; break;
    case Command::OutputOff:
      std::snprintf(wire,sizeof(wire),"S%02u\r",static_cast<unsigned>(args.delay_minutes));
      out.bytes = wire; out.changes_output = true; break;
    case Command::OutputRestart:
      std::snprintf(wire,sizeof(wire),"S%02uR%04u\r",static_cast<unsigned>(args.delay_minutes),
          static_cast<unsigned>(args.restore_minutes));
      out.bytes = wire; out.changes_output = true; break;
    default: error = "Unsupported command"; return false;
  }
  return true;
}
}
