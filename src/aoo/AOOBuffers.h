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

  /// Clears all entries
  void reset() {
    for (auto& e : entries_) e.clear();
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
};


}  // namespace arduino_aoo
