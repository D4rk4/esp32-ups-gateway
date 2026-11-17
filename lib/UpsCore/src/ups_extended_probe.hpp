#pragma once
#include <cstdint>
namespace ups {
// Tracks capability detection for one self-disabling extended query (BT / BP).
// A query is supported until it fails n_failures consecutive times; afterwards
// it is never retried until the device is rebooted (reset()). It is deliberately
// free of Arduino, HAL, or EventBus dependencies so host tests can drive it.
class ExtendedProbe {
 public:
  // timeout_ms is how long to wait for the reply before declaring the attempt
  // a failure (used by the scheduler only); n_failures is the disable threshold.
  ExtendedProbe(uint32_t timeout_ms, uint32_t n_failures)
      : timeout_ms_(timeout_ms), n_failures_(n_failures) {}

  // True when the query may still be sent. False only after the failure
  // threshold is reached; it never auto-recovers (only reset() re-enables).
  bool supports() const { return failures_ < n_failures_; }
  uint32_t failures() const { return failures_; }

  // Called once per failed attempt (NAK/echo/timeout/malformed).
  // Returns true exactly when this failure disables the probe
  // (supports() transitions true -> false); false otherwise.
  bool recordFailure() {
    const bool just_disabled = supports() && failures_ + 1u == n_failures_;
    ++failures_;
    return just_disabled;
  }
  // Called after a good reply: clears the run of failures only while still
  // supported. Once disabled the query stays disabled until reboot (reset()).
  // Returns true when it cleared failures, false when the probe is disabled
  // (no-op).
  bool recordSuccess() {
    if (!supports()) return false;
    failures_ = 0;
    return true;
  }
  // Re-enable the query (device rebooted).
  void reset() { failures_ = 0; ever_sent_ = false; }

  // True when supported and the probe period has elapsed since the last send.
  // Before the first send, always true (probe at startup).
  bool due(uint32_t now) const {
    if (!supports()) return false;
    return !ever_sent_ || uint32_t(now - last_sent_ms_) >= kProbePeriodMs;
  }
  // Mark that an attempt was sent at time `now`.
  void markSent(uint32_t now) { ever_sent_ = true; last_sent_ms_ = now; }
  uint32_t last_sent_ms() const { return last_sent_ms_; }
  uint32_t timeout_ms() const { return timeout_ms_; }

  static constexpr uint32_t kProbePeriodMs = 30000;  // ~30 s between probes

 private:
  uint32_t timeout_ms_;
  uint32_t n_failures_;
  uint32_t failures_{0};
  bool ever_sent_{false};
  uint32_t last_sent_ms_{0};
};
}
