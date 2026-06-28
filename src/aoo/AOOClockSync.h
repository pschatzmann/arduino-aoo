#pragma once
#include "AudioTools.h"
#include "stdint.h"

namespace arduino_aoo {

/**
 * @brief NTP-style clock synchronization using AOO ping/pong round-trips.
 *
 * Estimates the clock offset between source and sink from the three
 * timestamps carried in every pong message (t1=source send, t2=sink
 * receive, t3=sink send) plus the local receive time t4.
 *
 * The offset is smoothed with an exponential moving average and can be
 * queried to convert remote timestamps to local time.
 *
 * This is used for latency monitoring only and not for the clock
 * synchronization of the audio stream itself
 *
 * @ingroup aoo-utils
 * @author Phil Schatzmann
 */
class AOOClockSync {
 public:
  /// Default constructor
  AOOClockSync() = default;

  /// Process a completed ping/pong round-trip
  void update(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4) {
    int64_t rtt = (int64_t)(t4 - t1) - (int64_t)(t3 - t2);
    if (rtt < 0) rtt = 0;

    int64_t offset = ((int64_t)(t2 - t1) + (int64_t)(t3 - t4)) / 2;

    rtt_us = rtt;
    if (!has_estimate) {
      offset_us = offset;
      has_estimate = true;
    } else {
      offset_us = (int64_t)(alpha * offset + (1.0f - alpha) * offset_us);
    }
    sample_count++;
  }

  /// Convert a remote timestamp to local time
  uint64_t toLocal(uint64_t remote_us) const {
    return (uint64_t)((int64_t)remote_us - offset_us);
  }

  /// Convert a local timestamp to remote time
  uint64_t toRemote(uint64_t local_us) const {
    return (uint64_t)((int64_t)local_us + offset_us);
  }

  /// Current estimated clock offset in microseconds (remote - local)
  int64_t offsetMicros() const { return offset_us; }

  /// Last measured round-trip time in microseconds
  uint64_t rttMicros() const { return rtt_us; }

  /// True once at least one round-trip has been processed
  bool isValid() const { return has_estimate; }

  /// Number of round-trip samples processed
  uint32_t sampleCount() const { return sample_count; }

  /// Set EMA smoothing factor (0..1, lower = smoother)
  void setAlpha(float a) { alpha = a; }

  /// Resets all state to initial values
  void reset() {
    offset_us = 0;
    rtt_us = 0;
    has_estimate = false;
    sample_count = 0;
  }

 protected:
  int64_t offset_us = 0;
  uint64_t rtt_us = 0;
  float alpha = 0.125f;
  bool has_estimate = false;
  uint32_t sample_count = 0;
};

}  // namespace arduino_aoo
