#pragma once
#include <vector>
#include "AudioTools.h"

/// Maximum address size for AOO messages
#ifndef AOO_MAX_ADDRESS_LEN
#define AOO_MAX_ADDRESS_LEN 80
#endif

/// AOO protocol version string
#define AOO_VERSION "2.0"

/// Default ping interval in ms
#ifndef AOO_PING_INTERVAL_MS
#define AOO_PING_INTERVAL_MS 1000
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
  /// Number of encoded segments to buffer per source. Data is only provided
  /// once the buffer is half full, giving time for packet recovery.
  /// The priming delay = (buffer_depth / 2) * (block_size / sample_rate).
  /// With the default of 10 and typical 10ms blocks: 5 * 10ms = 50ms delay,
  /// which is enough for WiFi resend round-trips (10-30ms).
  /// Increase to 20 for lossy networks or small block sizes (<5ms).
  /// Memory per source: buffer_depth * encoded_segment_size
  /// (e.g. 10 * ~100 bytes for Opus, 10 * ~960 bytes for PCM mono).
  int buffer_depth = 10;
  /// Enable adaptive resampling to compensate for source/sink clock drift:
  /// the playback rate is adjusted so that the buffer stays half full
  bool adaptive_resampling = true;
  /// Adaptive resampling: interval in ms in which the playback rate is
  /// adjusted
  int resample_update_ms = 1000;
  /// Adaptive resampling: maximum deviation from the nominal sample rate in
  /// percent (clock drift is usually far below 0.1%)
  float max_resample_percent = 0.5f;
  /// The output format (sample rate, channels) is defined by this config.
  /// Set to true to use the format of the first source instead.
  bool output_follows_source = false;
  /// Wait time in ms before requesting a resend for a missing packet (once
  /// a later packet has arrived): it gives reordered packets time to arrive.
  /// The resent packet must arrive before the read position reaches the
  /// gap, which is about (buffer_depth / 2) blocks behind the newest
  /// packet: e.g. 5 * 5.3 ms = 27 ms for 256 samples at 48 kHz, or
  /// 5 * 20 ms = 100 ms for 20 ms Opus frames. So the wait plus the round
  /// trip time must be clearly below that delay.
  int recovery_wait_ms = 10;
  /// Maximum resend attempts per missing packet. Keep at 1 to
  /// avoid flooding the network — the min_quality_percent gate
  /// already prevents resends during bad periods.
  int recovery_max_requests = 1;
  /// Only send resend requests when the recent quality (last 100 blocks) is
  /// above this threshold: below it resends are suppressed to avoid making
  /// congestion worse. Requests for packets the reader has already passed
  /// are discarded.
  float min_quality_percent = 80.0f;
  /// Remove sources whose recent quality (last 100 blocks) is below this
  /// percentage. At 50%: a source losing more than half its packets is
  /// dropped from the mixer.
  float min_source_quality = 50.0f;
  /// Auto-remove sources that haven't sent data for this many ms; 0 = disabled
  int stream_timeout_ms = 5000;
  /// Log raw OSC message headers for debugging
  bool log_osc = false;
};

}  // namespace arduino_aoo
