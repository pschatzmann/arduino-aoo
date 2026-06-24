#pragma once
#include "AudioTools.h"
#include "AudioTools/CoreAudio/ResampleStream.h"
#include "aoo/AOODriftEstimator.h"
#include "stdint.h"

namespace arduino_aoo {

/**
 * @brief Adaptive resampler that compensates for clock drift between
 * AOO source and sink.
 *
 * The primary drift signal comes from AOODriftEstimator, which
 * measures the effective source sample rate from the timestamps in
 * each received data block. A secondary buffer-level correction
 * nudges the ratio to keep the jitter buffer near its target fill.
 *
 * @ingroup aoo-utils
 * @author Phil Schatzmann
 */
class AOOResampler {
 public:
  AOOResampler() = default;

  /// Set up the resampler for the given output info
  bool begin(AudioInfo info, Print& out) {
    info_ = info;
    nominal_rate_ = info.sample_rate;
    drift_.begin(info.sample_rate);
    resampler_.setOutput(out);
    return resampler_.begin(info);
  }

  /// Write decoded audio. The resampler adjusts playback speed.
  size_t write(const uint8_t* data, size_t len) {
    return resampler_.write(data, len);
  }

  /// Feed a received block's timestamp for drift estimation
  void updateDrift(uint64_t source_timestamp_us, int samples_in_block) {
    drift_.update(source_timestamp_us, samples_in_block);
  }

  /// Compute and apply the resampling ratio. Call once per block.
  /// buffer_fill and buffer_target are in number-of-blocks.
  void adjust(int buffer_fill, int buffer_target) {
    float ratio = 1.0f;

    // primary: drift from data timestamps
    if (drift_.isValid()) {
      ratio = drift_.driftRatio();
    }

    // secondary: buffer-level P-correction
    if (buffer_target > 0) {
      float err = (float)(buffer_fill - buffer_target) / buffer_target;
      ratio += err * buffer_gain_;
    }

    if (ratio < min_ratio_) ratio = min_ratio_;
    if (ratio > max_ratio_) ratio = max_ratio_;
    resampler_.setStepSize(ratio);
  }

  /// Access the drift estimator for monitoring
  AOODriftEstimator& driftEstimator() { return drift_; }

  /// Set proportional gain for buffer-level correction (default 0.005)
  void setBufferGain(float g) { buffer_gain_ = g; }

  /// Set the allowed range for the resampling ratio
  void setRatioLimits(float min_r, float max_r) {
    min_ratio_ = min_r;
    max_ratio_ = max_r;
  }

  float currentStepSize() { return resampler_.getStepSize(); }

  void end() {
    resampler_.end();
    drift_.reset();
  }

 protected:
  ResampleStream resampler_;
  AOODriftEstimator drift_;
  AudioInfo info_;
  int nominal_rate_ = 0;
  float buffer_gain_ = 0.005f;
  float min_ratio_ = 0.95f;
  float max_ratio_ = 1.05f;
};

}  // namespace arduino_aoo
