#pragma once
#include "AudioTools.h"
#include "stdint.h"
// the system time is only used on platforms which support it (and can set it
// e.g. via SNTP): on some others (e.g. Renesas) sys/time.h is incomplete
#if (defined(ESP32) || defined(ESP8266) || defined(IS_DESKTOP) ||   \
     (defined(ARDUINO_ARCH_RP2040) && !defined(ARDUINO_ARCH_MBED))) && \
    __has_include(<sys/time.h>)
#include <sys/time.h>
#define AOO_HAS_GETTIMEOFDAY 1
#endif

namespace arduino_aoo {

/// Seconds between the NTP epoch (1900) and the Unix epoch (1970)
constexpr uint64_t kAOONtpUnixOffset = 2208988800ULL;

/// Converts microseconds to an NTP timetag (32.32 fixed point seconds)
inline uint64_t aoo_ntp_from_micros(uint64_t us) {
  uint64_t sec = us / 1000000ULL;
  uint64_t frac = ((us % 1000000ULL) << 32) / 1000000ULL;
  return (sec << 32) | frac;
}

/// Converts a difference of two NTP timetags to microseconds
inline int64_t aoo_ntp_diff_micros(int64_t ntp_diff) {
  int64_t sec = ntp_diff >> 32;  // floor, also for negative values
  uint64_t frac = (uint64_t)ntp_diff & 0xFFFFFFFFULL;
  return sec * 1000000LL + (int64_t)((frac * 1000000ULL + 0x80000000ULL) >> 32);
}

/// Current time as NTP timetag, as used by OSC and the official aoo
/// library. Uses the system time when available: it is only absolute if the
/// clock was set (e.g. via SNTP), but differences are always correct.
inline uint64_t aoo_ntp_now() {
#if defined(AOO_HAS_GETTIMEOFDAY)
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (((uint64_t)tv.tv_sec + kAOONtpUnixOffset) << 32) |
         (((uint64_t)tv.tv_usec << 32) / 1000000ULL);
#else
  return aoo_ntp_from_micros(micros());
#endif
}

/**
 * @brief NTP-style clock synchronization using AOO ping/pong round-trips.
 *
 * Estimates the clock offset between source and sink from the three
 * timestamps carried in every pong message (t1=source send, t2=sink
 * receive, t3=sink send) plus the local receive time t4. All timestamps are
 * NTP timetags (see aoo_ntp_now()), like in the official aoo library.
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

  /// Process a completed ping/pong round-trip (NTP timetags)
  void update(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4) {
    int64_t rtt = (int64_t)(t4 - t1) - (int64_t)(t3 - t2);
    if (rtt < 0) rtt = 0;
    // halve the summands separately to avoid an overflow
    int64_t offset = (int64_t)(t2 - t1) / 2 + (int64_t)(t3 - t4) / 2;

    rtt_us = aoo_ntp_diff_micros(rtt);
    if (!has_estimate) {
      offset_ntp = offset;
      has_estimate = true;
    } else {
      offset_ntp = (int64_t)(alpha * offset + (1.0f - alpha) * offset_ntp);
    }
    sample_count++;
  }

  /// Convert a remote NTP timetag to local time
  uint64_t toLocal(uint64_t remote_ntp) const {
    return remote_ntp - (uint64_t)offset_ntp;
  }

  /// Convert a local NTP timetag to remote time
  uint64_t toRemote(uint64_t local_ntp) const {
    return local_ntp + (uint64_t)offset_ntp;
  }

  /// Current estimated clock offset in microseconds (remote - local)
  int64_t offsetMicros() const { return aoo_ntp_diff_micros(offset_ntp); }

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
    offset_ntp = 0;
    rtt_us = 0;
    has_estimate = false;
    sample_count = 0;
  }

 protected:
  int64_t offset_ntp = 0;
  uint64_t rtt_us = 0;
  float alpha = 0.125f;
  bool has_estimate = false;
  uint32_t sample_count = 0;
};

}  // namespace arduino_aoo
