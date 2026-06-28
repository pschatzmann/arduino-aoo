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
    return const_cast<Entry*>(
        static_cast<const IndexedRingBuffer*>(this)->get(id));        
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


  size_t write(int32_t id, const uint8_t* data, size_t len) {
    Entry* e = reserve(id, len);

    if (!e) return 0;

    e->active = true;

    return e->writeArray(data, len);
  }

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


}  // namespace arduino_aoo
