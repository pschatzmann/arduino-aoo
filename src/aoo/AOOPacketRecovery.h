#pragma once
#include <vector>
#include "AudioTools.h"
#include "stdint.h"

namespace arduino_aoo {

/**
 * @brief Tracks received sequence numbers and schedules resend
 * requests for gaps.
 *
 * When a gap is detected (seq jumps by more than 1), the missing
 * sequence numbers are recorded. After a configurable wait time
 * the caller can query for sequences that should be requested from
 * the source. Sequences that arrive late are removed from the
 * pending list. Sequences that have been requested too many times
 * are abandoned.
 *
 * @ingroup aoo-utils
 * @author Phil Schatzmann
 */
class AOOPacketRecovery {
 public:
  /// Default constructor
  AOOPacketRecovery() = default;

  /// Configure recovery. wait_ms: time before requesting a resend.
  /// max_requests: maximum resend attempts before giving up.
  void begin(uint32_t wait_ms = 20, int max_requests = 3) {
    wait_ms_ = wait_ms;
    max_requests_ = max_requests;
    pending_.clear();
    last_seq_ = -1;
  }

  /// Notify that a sequence number was received.
  /// Detects gaps and removes the seq from pending if present.
  void received(int32_t seq) {
    // remove from pending if it was a late arrival
    for (int i = pending_.size() - 1; i >= 0; i--) {
      if (pending_[i].seq == seq) {
        pending_.erase(pending_.begin() + i);
      }
    }

    if (last_seq_ >= 0 && seq > last_seq_ + 1) {
      uint32_t now = millis();
      for (int32_t s = last_seq_ + 1; s < seq; s++) {
        bool already = false;
        for (auto& p : pending_) {
          if (p.seq == s) {
            already = true;
            break;
          }
        }
        if (!already) {
          PendingSeq ps;
          ps.seq = s;
          ps.request_time = now + wait_ms_;
          ps.attempts = 0;
          pending_.push_back(ps);
        }
      }
    }
    if (seq > last_seq_) last_seq_ = seq;
  }

  /// Collect sequences that are due for a resend request.
  /// Returns the number of sequences written to out_seqs.
  int getResendRequests(int32_t* out_seqs, int max_count) {
    uint32_t now = millis();
    int count = 0;
    for (int i = pending_.size() - 1; i >= 0 && count < max_count; i--) {
      auto& p = pending_[i];
      if (now >= p.request_time) {
        p.attempts++;
        if (p.attempts > max_requests_) {
          LOGW("PacketRecovery: abandoning seq %d after %d attempts", p.seq,
               max_requests_);
          pending_.erase(pending_.begin() + i);
          continue;
        }
        out_seqs[count++] = p.seq;
        p.request_time = now + wait_ms_;
      }
    }
    return count;
  }

  /// Number of sequences currently pending recovery
  int pendingCount() { return pending_.size(); }

  /// Returns the last received sequence number
  int32_t lastSeq() { return last_seq_; }

  /// Resets all pending recovery state
  void reset() {
    pending_.clear();
    last_seq_ = -1;
  }

  /// Stops recovery and clears all state
  void end() { reset(); }

 protected:
  struct PendingSeq {
    int32_t seq = -1;
    uint32_t request_time = 0;
    int attempts = 0;
  };

  std::vector<PendingSeq> pending_;
  uint32_t wait_ms_ = 20;
  int max_requests_ = 3;
  int32_t last_seq_ = -1;
};

}  // namespace arduino_aoo
