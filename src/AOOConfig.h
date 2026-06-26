#pragma once
#include <vector>
#include "AudioTools.h"

/// Maximum receive buffer size for source
#ifndef AAO_MAX_SOURCE_BUFFER
#define AAO_MAX_SOURCE_BUFFER 70
#endif

/// Maximum receive buffer size for sink: we need to be able to hold
/// the audio data message
#ifndef AAO_MAX_SINK_BUFFER
#define AAO_MAX_SINK_BUFFER (1024 * 2)
#endif

/// Maximum address size for AOO messages
#ifndef AOO_MAX_ADDRESS_LEN
#define AOO_MAX_ADDRESS_LEN 80
#endif

/// AOO protocol version string
#define AOO_VERSION "2.0"

/// Default jitter buffer depth (number of blocks)
#ifndef AOO_JITTER_BUFFER_DEPTH
#define AOO_JITTER_BUFFER_DEPTH 5
#endif

/// Default ping interval in ms
#ifndef AOO_PING_INTERVAL_MS
#define AOO_PING_INTERVAL_MS 1000
#endif

/// Default packet recovery wait time in ms before requesting resend
#ifndef AOO_RECOVERY_WAIT_MS
#define AOO_RECOVERY_WAIT_MS 20
#endif

/// Default max resend attempts before abandoning a packet
#ifndef AOO_RECOVERY_MAX_REQUESTS
#define AOO_RECOVERY_MAX_REQUESTS 3
#endif

namespace arduino_aoo {

/**
 * @brief Identifies a remote sink with its AOO id and network address.
 * When ip is 0.0.0.0, no per-sink retargeting is performed (broadcast).
 * @ingroup aoo
 */
struct AOOSinkTarget {
  int id = 0;
  uint32_t ip = 0;
  uint16_t port = 0;

  AOOSinkTarget() = default;
  AOOSinkTarget(int id) : id(id) {}
  AOOSinkTarget(int id, uint32_t ip, uint16_t port)
      : id(id), ip(ip), port(port) {}
};

/**
 * @brief Configuration for AOOSender
 * @ingroup aoo
 * @author Phil Schatzmann
 */
struct AOOSenderConfig : public AudioInfo {
  AOOSenderConfig() = default;
  AOOSenderConfig(int sampleRate, int ch, int bits)
      : AudioInfo(sampleRate, ch, bits) {}

  /// Unique source identifier used in AOO addressing
  int id = 0;
  /// Target sinks with optional network addresses; empty = broadcast to all
  std::vector<AOOSinkTarget> sink_targets;
  /// How many sent blocks to keep in the resend buffer (0 = disabled)
  int buffer_size = 50;
  /// Maximum encoded frame size in bytes before multi-frame splitting
  int max_frame_size = 1400;
  /// Number of times each block is sent (1 = no redundancy, 2+ for loss tolerance)
  int redundancy = 1;
  /// Codec delay in samples (communicated to sinks for alignment)
  int codec_delay_samples = 0;
  /// Ping interval in ms (how often to send keep-alive pings to sinks)
  int ping_interval_ms = AOO_PING_INTERVAL_MS;
  /// Channel onset in samples (communicated to sinks for alignment)
  int32_t channel_onset = 0;
  /// Log raw OSC message headers for debugging
  bool log_osc = false;
};

/**
 * @brief Configuration for AOOReceiver
 * @ingroup aoo
 * @author Phil Schatzmann
 */
struct AOOReceiverConfig : public AudioInfo {
  AOOReceiverConfig() = default;
  AOOReceiverConfig(int sampleRate, int ch, int bits)
      : AudioInfo(sampleRate, ch, bits) {}

  /// Unique sink identifier used in AOO addressing; 0 = auto-assign from first message
  int id = 0;
  /// Jitter buffer depth (number of blocks); 0 = disabled
  int jitter_buffer_depth = 0;
  /// Enable adaptive resampling to compensate for source/sink clock drift
  bool adaptive_resampling = false;
  /// Wait time in ms before requesting a resend for missing packets; 0 = disabled
  int recovery_wait_ms = 0;
  /// Maximum resend attempts before abandoning a missing packet
  int recovery_max_requests = 0;
  /// Internal mixer buffer size;
  int mixer_size = 2 * 1024;
  /// Auto-remove sources that haven't sent data for this many ms; 0 = disabled
  int stream_timeout_ms = 0;
  /// Log raw OSC message headers for debugging
  bool log_osc = false;
};

}  // namespace arduino_aoo
