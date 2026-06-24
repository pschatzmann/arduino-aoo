#pragma once
#include <vector>
#include "AudioTools.h"
#include "AudioTools/CoreAudio/Buffers.h"
#include "stdint.h"

namespace arduino_aoo {

/**
 * @brief AOO Write Buffer which caches the written data for some time to be
 * able to resend lost data
 * @ingroup aoo-utils
 * @author Phil Schatzmann
 */

class AOOSourceBuffer {
 public:
  AOOSourceBuffer() = default;

  AOOSourceBuffer(uint32_t timeout) { setTimeout(timeout); }

  /// adds the data to the buffer
  size_t writeArray(int id, const uint8_t *data, size_t len) {
    if (buffer_size == 0) setBufferSize(len);
    if (timeout_ms == 0) return 0;
    if (len > buffer_size) {
      LOGE("Buffer overflow %d > %d", (int)len, (int)buffer_size);
      return 0;
    }
    SingleBuffer<uint8_t> *p_buffer = newBuffer();
    if (p_buffer == nullptr) {
      TRACEE()
      return 0;
    }
    p_buffer->timestamp = millis() + timeout_ms;
    p_buffer->id = id;
    return p_buffer->writeArray(data, len);
  }

  /// finds the data from the buffer
  size_t readArray(int id, uint8_t *data, size_t len) {
    if (len > buffer_size) {
      LOGE("Buffer underflow %d > %d", (int)len, (int)buffer_size);
      return 0;
    }
    for (auto &buffer : buffers) {
      if (buffer.id == id) {
        return buffer.readArray(data, len);
      }
    }
    return 0;
  }

  SingleBuffer<uint8_t> *getBuffer(int id) {
    for (auto &buffer : buffers) {
      if (buffer.id == id) {
        return &buffer;
      }
    }
    return nullptr;
  }

  void clear() { buffers.clear(); }

  /// Defines the validity time for a buffer entry: If 0, do not buffer!
  void setTimeout(uint32_t ms) { timeout_ms = ms; }

  /// Defines the size of an individual buffer entry. If 0 it will be determined
  /// automatically based on the first write call
  void setBufferSize(size_t len) {
    clear();
    buffer_size = len;
  }

 protected:
  uint32_t timeout_ms = 1000;
  size_t buffer_size = 0;
  std::vector<SingleBuffer<uint8_t>> buffers;

  /// Returns an expired buffer or creates a new one
  SingleBuffer<uint8_t> *newBuffer() {
    for (auto &buffer : buffers) {
      if (buffer.timestamp < millis()) {
        return &buffer;
      }
    }
    buffers.push_back(SingleBuffer<uint8_t>(buffer_size));
    return &buffers.back();
  }
};

/**
 * @brief AOO Buffer is a NBuffer which uses a sequence numer to identify each
 * buffer entry and allows to add an empty buffer entry for gaps that can be
 * refilled later. The BaseBuffer entries are resized dynamically.
 * @ingroup aoo-utils
 * @author Phil Schatzmann
 */

class AOOSinkBuffer : public BaseBuffer<uint8_t> {
 public:
  AOOSinkBuffer() = default;

  /// Must be sized at runtime to support psram
  void resize(int count) {
    if (count == 0) return;
    nbuffer.resize(0, count);
  }

  /// Defines the actual buffer id that will be used in the next entry
  void setActualId(int id) { actual_id = id; }

  /// reads multiple values
  int readArray(uint8_t data[], int len) override {
    return nbuffer.readArray(data, len);
  }

  /// Standard write function used by the mixer
  int writeArray(const uint8_t data[], int len) override {
    // provide individual SingleBuffer
    SingleBuffer<uint8_t> *rec = nbuffer.writeEnd();
    if (rec == nullptr) {
      LOGE("insufficient Buffers");
      return 0;
    }
    // resize if needed
    if (len > rec->size()) {
      rec->resize(len);
    }
    rec->active = len > 0;
    rec->id = actual_id;
    rec->timestamp = millis();
    return rec->writeArray(data, len);
  }

  /// Fill the buffer for a specific gap id
  int updateArray(int id, const uint8_t data[], int len) {
    SingleBuffer<uint8_t> *rec =
        (SingleBuffer<uint8_t> *)nbuffer.getBuffer(id);
    if (rec == nullptr) {
      return 0;
    }
    if (len > rec->size()) {
      rec->resize(len);
    }
    rec->active = true;
    rec->timestamp = millis();
    return rec->writeArray(data, len);
  }

  int available() override { return nbuffer.available(); }
  int availableForWrite() override { return nbuffer.availableForWrite(); }
  bool isFull() override { return nbuffer.isFull(); }
  bool isEmpty() { return nbuffer.isEmpty(); }
  size_t size() { return nbuffer.size(); }
  bool read(uint8_t &result) override { return nbuffer.read(result); }
  bool peek(uint8_t &result) override { return nbuffer.peek(result); }
  bool write(uint8_t out) { return nbuffer.write(out); }
  void reset() {
    nbuffer.reset();
    actual_id = 0;
  }
  uint8_t *address() { return nbuffer.address(); }

 protected:
  uint8_t *buffer = nullptr;
  NBufferExt<uint8_t> nbuffer{0, 0};
  int actual_id = 0;
};

/**
 * @brief Reorder buffer that absorbs network jitter.
 *
 * Incoming blocks are placed into slots indexed by sequence number.
 * Blocks are released in strict sequence order after a configurable
 * number of blocks have been buffered (the "depth").
 *
 * Slots are not pre-allocated; the data vector in each slot grows
 * dynamically on the first write that needs it. A slot is considered
 * filled when its data vector is non-empty.
 *
 * @ingroup aoo-utils
 * @author Phil Schatzmann
 */
class AOOJitterBuffer {
 public:
  AOOJitterBuffer() = default;

  /// Configure the buffer. depth = number of blocks to hold before releasing.
  bool begin(int depth) {
    if (depth <= 0) return false;
    depth_ = depth;
    slots_.resize(depth);
    for (auto& s : slots_) {
      s.data.clear();
      s.seq = -1;
    }
    read_seq_ = -1;
    write_count_ = 0;
    is_active_ = true;
    return true;
  }

  /// Insert a block. Returns false if the block is too late or duplicate.
  bool write(int32_t seq, const uint8_t* data, size_t len) {
    if (!is_active_) return false;

    if (read_seq_ >= 0 && seq <= read_seq_) {
      LOGW("JitterBuffer: late packet seq=%d (read_seq=%d)", seq, read_seq_);
      return false;
    }

    auto& slot = slots_[seq % depth_];
    if (!slot.data.empty() && slot.seq == seq) return false;

    slot.data.resize(len);
    memcpy(slot.data.data(), data, len);
    slot.seq = seq;
    write_count_++;

    if (read_seq_ < 0 && write_count_ >= depth_) {
      read_seq_ = seq - depth_ + 1;
    }
    return true;
  }

  /// Read the next block in sequence order. Moves the data into out
  /// (zero-copy swap). Returns the number of bytes, or 0 for a gap.
  size_t read(std::vector<uint8_t> &out) {
    out.clear();
    if (!is_active_ || read_seq_ < 0) return 0;

    int32_t seq = read_seq_ + 1;
    auto& slot = slots_[seq % depth_];

    size_t result = 0;
    if (!slot.data.empty() && slot.seq == seq) {
      result = slot.data.size();
      std::swap(out, slot.data);
    }
    slot.data.clear();
    slot.seq = -1;
    read_seq_ = seq;
    return result;
  }

  /// True once enough blocks have been buffered to start reading
  bool isReady() const { return read_seq_ >= 0; }

  /// Current read sequence number
  int32_t readSeq() const { return read_seq_; }

  /// Number of filled slots
  int filledSlots() {
    int n = 0;
    for (auto& s : slots_)
      if (!s.data.empty()) n++;
    return n;
  }

  int depth() { return depth_; }

  void reset() {
    for (auto& s : slots_) {
      s.data.clear();
      s.seq = -1;
    }
    read_seq_ = -1;
    write_count_ = 0;
  }

  void end() {
    reset();
    slots_.clear();
    is_active_ = false;
  }

 protected:
  struct Slot {
    std::vector<uint8_t> data;
    int32_t seq = -1;
  };

  std::vector<Slot> slots_;
  int depth_ = 0;
  int32_t read_seq_ = -1;
  int write_count_ = 0;
  bool is_active_ = false;
};

}  // namespace arduino_aoo
