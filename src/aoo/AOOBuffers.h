#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

#include "AudioTools.h"
#include "AudioTools/CoreAudio/Buffers.h"

namespace arduino_aoo {
/**
 * @brief Ring buffer that allows indexed access to slots by sequence number.
 * @ingroup aoo
 * @author Phil Schatzmann
 * @copyright GPLv3
 */

template <typename Entry = SingleBuffer<uint8_t>> 
class IndexedRingBuffer {
 public:
  IndexedRingBuffer() = default;
  explicit IndexedRingBuffer(size_t slots) { resize(slots); }

  void resize(size_t slots) {
    entries_.resize(slots);
    reset();
  }

  size_t size() const { return entries_.size(); }

  bool empty() const { return entries_.empty(); }

  Entry* get(int32_t id) {
    if (entries_.empty()) return nullptr;

    Entry& e = entries_[index(id)];
    if (e.id != id) return nullptr;
    return &e;
  }

  const Entry* get(int32_t id) const {
    if (entries_.empty()) return nullptr;
    const Entry& e = entries_[index(id)];
    if (e.id != id) return nullptr;
    return &e;
  }

  Entry* reserve(int32_t id, size_t capacity = 0) {
    if (entries_.empty()) return nullptr;

    Entry& e = entries_[index(id)];
    if (capacity > e.size()) e.resize(capacity);

    e.reset();
    e.id = id;
    e.active = false;

    return &e;
  }

  //----------------------------------------------------------
  // write by id
  //----------------------------------------------------------

  size_t write(int32_t id, const uint8_t* data, size_t len) {
    Entry* e = reserve(id, len);

    if (!e) return 0;

    e->active = true;

    return e->writeArray(data, len);
  }

  //----------------------------------------------------------
  // FIFO interface
  //----------------------------------------------------------

  Entry* writeEnd() {
    if (entries_.empty()) return nullptr;

    Entry& e = entries_[write_pos_];

    write_pos_ = (write_pos_ + 1) % entries_.size();

    if (count_ < entries_.size())
      ++count_;
    else
      read_pos_ = (read_pos_ + 1) % entries_.size();

    e.clear();

    return &e;
  }

  Entry* readFront() {
    if (count_ == 0) return nullptr;

    Entry& e = entries_[read_pos_];

    read_pos_ = (read_pos_ + 1) % entries_.size();
    --count_;

    return &e;
  }

  bool isEmpty() const { return count_ == 0; }

  bool isFull() const { return !entries_.empty() && count_ == entries_.size(); }

  size_t available() const { return count_; }

  size_t availableForWrite() const { return entries_.size() - count_; }

  void reset() {
    for (auto& e : entries_) e.clear();

    read_pos_ = 0;
    write_pos_ = 0;
    count_ = 0;
  }

  int activeCount() const {
    int n = 0;

    for (auto& e : entries_) {
      if (e.active) n++;
    }

    return n;
  }

 protected:
  size_t index(int32_t id) const {
    return static_cast<size_t>(id) % entries_.size();
  }

  std::vector<Entry> entries_;
  size_t read_pos_ = 0;
  size_t write_pos_ = 0;
  size_t count_ = 0;
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
  using Entry = SingleBuffer<uint8_t>;
  using Buffer = IndexedRingBuffer<Entry>;

  AOOJitterBuffer() = default;

  bool begin(int depth) {
    if (depth <= 0) return false;
    depth_ = depth;
    buffer_.resize(depth_);
    read_seq_ = -1;
    write_count_ = 0;
    active_ = true;
    return true;
  }

  void end() {
    reset();
    buffer_.resize(0);
    active_ = false;
  }

  bool write(int32_t seq, const uint8_t* data, size_t len) {
    if (!active_) return false;

    // drop late packets
    if (read_seq_ >= 0 && seq <= read_seq_) {
      return false;
    }

    Entry* e = buffer_.reserve(seq, len);
    if (!e) return false;

    // update data
    std::memcpy(e->address(), data, len);
    e->setAvailable(len);
    e->active = true;
    e->timestamp = millis();

    write_count_++;

    // initialize read pointer once buffer is "primed"
    if (read_seq_ < 0 && write_count_ >= depth_) {
      read_seq_ = seq - depth_ + 1;
    }

    return true;
  }

  size_t read(std::vector<uint8_t>& out) {
    out.clear();

    if (!active_ || read_seq_ < 0) return 0;
    int32_t next_seq = read_seq_ + 1;
    Entry* e = buffer_.get(next_seq);
    size_t len = 0;

    if (e && e->active) {
      len = e->size();
      out.resize(len);
      std::memcpy(out.data(), e->address(), len);
      e->active = false;
      e->clear();
    }

    read_seq_ = next_seq;
    return len;
  }

  bool isReady() const { return read_seq_ >= 0; }

  int32_t readSeq() const { return read_seq_; }

  int depth() const { return depth_; }

  void reset() {
    buffer_.reset();
    read_seq_ = -1;
    write_count_ = 0;
  }

  int filledSlots() const { return buffer_.activeCount(); }

 private:
  Buffer buffer_;

  int depth_ = 0;
  int32_t read_seq_ = -1;
  int write_count_ = 0;
  bool active_ = false;
};

}  // namespace arduino_aoo
