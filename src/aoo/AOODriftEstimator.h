#pragma once
#include "AudioTools.h"
#include "stdint.h"

namespace arduino_aoo {

/**
 * @brief Estimates the effective source sample rate as seen by the
 * local clock, using a DLL (Delay-Locked Loop) on the timestamps
 * carried in AOOData messages.
 *
 * Each call to update() provides the source-side timestamp (from the
 * AOOData message) and the number of samples in the block. The DLL
 * tracks the relationship between elapsed source time and elapsed
 * local time, producing a smoothed estimate of how fast the source
 * is really running relative to our clock.
 *
 * The estimated rate can be fed directly to a resampler to
 * compensate for crystal-oscillator drift between the two devices.
 *
 * @ingroup aoo-utils
 * @author Phil Schatzmann
 */
class AOODriftEstimator {
 public:
  AOODriftEstimator() = default;

  /// Initialize with the nominal sample rate
  void begin(int nominal_rate) {
    nominal_rate_ = nominal_rate;
    estimated_rate_ = nominal_rate;
    reset();
  }

  /// Feed a received block. source_timestamp_us is the timestamp from
  /// the AOOData message, samples_in_block is the number of audio
  /// samples (per channel) in this block.
  void update(uint64_t source_timestamp_us, int samples_in_block) {
    uint64_t local_now = micros();

    if (!has_prev_) {
      prev_source_us_ = source_timestamp_us;
      prev_local_us_ = local_now;
      has_prev_ = true;
      sample_count_++;
      return;
    }

    int64_t dt_source = (int64_t)(source_timestamp_us - prev_source_us_);
    int64_t dt_local = (int64_t)(local_now - prev_local_us_);

    // skip bogus intervals (negative, zero, or unreasonably large)
    if (dt_source <= 0 || dt_local <= 0 || dt_local > 2000000) {
      prev_source_us_ = source_timestamp_us;
      prev_local_us_ = local_now;
      return;
    }

    // instantaneous source rate as measured by our clock:
    // source claims dt_source us elapsed for samples_in_block samples,
    // but our clock measured dt_local us for the same interval.
    // effective_rate = nominal_rate * (dt_source / dt_local)
    double ratio = (double)dt_source / (double)dt_local;
    double instant_rate = nominal_rate_ * ratio;

    // EMA smoothing
    if (sample_count_ < warmup_blocks_) {
      // during warmup, use a faster alpha to converge quickly
      float a = 0.3f;
      estimated_rate_ = a * instant_rate + (1.0f - a) * estimated_rate_;
    } else {
      estimated_rate_ =
          bandwidth_ * instant_rate + (1.0f - bandwidth_) * estimated_rate_;
    }

    prev_source_us_ = source_timestamp_us;
    prev_local_us_ = local_now;
    sample_count_++;
  }

  /// Estimated source sample rate as seen by our local clock
  float estimatedRate() { return estimated_rate_; }

  /// Ratio of estimated source rate to nominal rate (> 1.0 means source is faster)
  float driftRatio() {
    if (nominal_rate_ <= 0) return 1.0f;
    return estimated_rate_ / nominal_rate_;
  }

  /// True once at least 2 blocks have been received
  bool isValid() { return sample_count_ >= 2; }

  /// Number of blocks processed
  uint32_t sampleCount() { return sample_count_; }

  /// Set DLL bandwidth (0..1, lower = smoother, default 0.01)
  void setBandwidth(float bw) { bandwidth_ = bw; }

  /// Set number of warmup blocks with faster convergence (default 10)
  void setWarmupBlocks(int n) { warmup_blocks_ = n; }

  void reset() {
    has_prev_ = false;
    prev_source_us_ = 0;
    prev_local_us_ = 0;
    sample_count_ = 0;
    estimated_rate_ = nominal_rate_;
  }

 protected:
  int nominal_rate_ = 0;
  float estimated_rate_ = 0;
  float bandwidth_ = 0.01f;
  int warmup_blocks_ = 10;
  bool has_prev_ = false;
  uint64_t prev_source_us_ = 0;
  uint64_t prev_local_us_ = 0;
  uint32_t sample_count_ = 0;
};

}  // namespace arduino_aoo
