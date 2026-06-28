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
  /// Default constructor
  IndexedRingBuffer() = default;
  /// @param slots number of buffer slots
  explicit IndexedRingBuffer(size_t slots) { resize(slots); }

  /// Resizes the buffer to the given number of slots
  void resize(size_t slots) {
    entries_.resize(slots);
    reset();
  }

  /// Returns the number of slots
  size_t size() const { return entries_.size(); }

  /// Returns true if the buffer has no slots
  bool empty() const { return entries_.empty(); }

  /// Returns the entry for the given sequence id, or nullptr
  Entry* get(int32_t id) {
    return const_cast<Entry*>(
        static_cast<const IndexedRingBuffer*>(this)->get(id));        
  }

  /// Returns the entry for the given sequence id, or nullptr (const)
  const Entry* get(int32_t id) const {
    if (entries_.empty()) return nullptr;
    const Entry& e = entries_[index(id)];
    if (e.id != id) return nullptr;
    return &e;
  }

  /// Reserves a slot for the given sequence id with optional capacity
  Entry* reserve(int32_t id, size_t capacity = 0) {
    if (entries_.empty()) return nullptr;

    Entry& e = entries_[index(id)];
    if (capacity > e.size()) e.resize(capacity);

    e.reset();
    e.id = id;
    e.active = false;

    return &e;
  }


  /// Writes data into the slot for the given sequence id
  size_t write(int32_t id, const uint8_t* data, size_t len) {
    Entry* e = reserve(id, len);

    if (!e) return 0;

    e->active = true;

    return e->writeArray(data, len);
  }

  /// Advances the write position and returns the next slot
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

  /// Returns the front entry and advances the read position
  Entry* readFront() {
    if (count_ == 0) return nullptr;

    Entry& e = entries_[read_pos_];

    read_pos_ = (read_pos_ + 1) % entries_.size();
    --count_;

    return &e;
  }

  /// Returns true if no entries are queued
  bool isEmpty() const { return count_ == 0; }

  /// Returns true if all slots are in use
  bool isFull() const { return !entries_.empty() && count_ == entries_.size(); }

  /// Returns the number of entries queued
  size_t available() const { return count_; }

  /// Returns the number of free slots
  size_t availableForWrite() const { return entries_.size() - count_; }

  /// Clears all entries and resets positions
  void reset() {
    for (auto& e : entries_) e.clear();

    read_pos_ = 0;
    write_pos_ = 0;
    count_ = 0;
  }

  /// Returns the number of entries with active data
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
