#pragma once
#include <stdint.h>
#include <string.h>
#include "AOOProtocol.h"

namespace arduino_aoo {

/// Check if a message is in binary format (as opposed to OSC).
/// Binary messages have the high bit set in the first byte;
/// OSC messages start with '/' (0x2F).
inline bool aoo_is_binary(const uint8_t* data, size_t len) {
  return len >= 4 && (data[0] & 0x80);
}

/// Read a uint8 from a byte pointer, advancing the pointer
inline uint8_t read_uint8(const uint8_t*& p) {
  return *p++;
}

/// Read a big-endian int32 from a byte pointer, advancing the pointer
inline int32_t read_int32(const uint8_t*& p) {
  uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | (uint32_t)p[3];
  p += 4;
  return (int32_t)v;
}

/// Read a big-endian uint16 from a byte pointer, advancing the pointer
inline uint16_t read_uint16(const uint8_t*& p) {
  uint16_t v = ((uint16_t)p[0] << 8) | (uint16_t)p[1];
  p += 2;
  return v;
}

/// Read a big-endian uint64 from a byte pointer, advancing the pointer
inline uint64_t read_uint64(const uint8_t*& p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) {
    v = (v << 8) | p[i];
  }
  p += 8;
  return v;
}

/// Read a big-endian double (IEEE 754) from a byte pointer, advancing the pointer
inline double read_double(const uint8_t*& p) {
  uint64_t bits = read_uint64(p);
  double result;
  memcpy(&result, &bits, sizeof(result));
  return result;
}

/// Binary data message flag bits
enum AOOBinDataFlags : uint8_t {
  kAOOBinFlag_SampleRate = 0x01,
  kAOOBinFlag_Frames = 0x02,
  kAOOBinFlag_StreamMessage = 0x04,
  kAOOBinFlag_XRun = 0x08,
  kAOOBinFlag_TimeStamp = 0x10,
};

/// Binary message command values
enum AOOBinCmd : uint8_t {
  kAOOBinCmd_Data = 0,
};

/**
 * @brief Parse a binary-format data message into an AOOData struct.
 *
 * The binary format is used by the official aoo library for data messages.
 * The header is 4 bytes for IDs < 256, or 12 bytes for larger IDs.
 * After the header come the body fields (stream_id, sequence, channel,
 * flags, data_size, and conditional fields based on flags).
 *
 * @param data  raw message bytes
 * @param len   total message length
 * @param out   AOOData struct to populate
 * @return true if parsing succeeded, false on error
 */
inline bool aoo_parse_bin_data(const uint8_t* data, size_t len, AOOData& out) {
  if (len < 4) return false;

  const uint8_t* p = data;

  // -- Header --
  uint8_t type_byte = read_uint8(p);  // type | 0x80
  if (!(type_byte & 0x80)) return false;

  uint8_t cmd_byte = read_uint8(p);
  bool large_ids = (cmd_byte & 0x80) != 0;
  uint8_t cmd = cmd_byte & 0x7F;

  // Only data command is supported
  if (cmd != kAOOBinCmd_Data) return false;

  if (!large_ids) {
    // Small IDs: 4-byte header
    if (len < 4) return false;
    out.sink_id = read_uint8(p);   // to
    out.source_id = read_uint8(p); // from
  } else {
    // Large IDs: 12-byte header
    if (len < 12) return false;
    // skip the 2 small ID bytes (already read type and cmd)
    p += 2;  // skip small to/from
    out.sink_id = read_int32(p);   // to (int32 big-endian)
    out.source_id = read_int32(p); // from (int32 big-endian)
  }

  // -- Body --
  // Minimum body: stream_id(4) + seq(4) + channel(1) + flags(1) + data_size(2) = 12
  size_t header_size = p - data;
  if (len < header_size + 12) return false;

  out.stream_id = read_int32(p);
  out.seq_no = read_int32(p);
  uint8_t channel = read_uint8(p);
  out.channel_onset = channel;
  uint8_t flags = read_uint8(p);
  uint16_t data_size = read_uint16(p);

  // Defaults
  out.total_size = data_size;
  out.total_number_of_frames = 1;
  out.frame_idx = 0;
  out.message_data_size = 0;
  out.real_sample_rate = 0.0;
  out.timestamp = 0;

  // Conditional fields based on flags
  if (flags & kAOOBinFlag_Frames) {
    size_t remaining = len - (p - data);
    if (remaining < 8) return false;  // total_size(4) + num_frames(2) + frame_index(2)
    out.total_size = read_int32(p);
    out.total_number_of_frames = (int32_t)read_uint16(p);
    out.frame_idx = (int32_t)read_uint16(p);
  }

  if (flags & kAOOBinFlag_StreamMessage) {
    size_t remaining = len - (p - data);
    if (remaining < 4) return false;
    out.message_data_size = read_int32(p);
    // Skip the stream message data
    size_t remaining2 = len - (p - data);
    if (remaining2 < (size_t)out.message_data_size) return false;
    p += out.message_data_size;
  }

  if (flags & kAOOBinFlag_SampleRate) {
    size_t remaining = len - (p - data);
    if (remaining < 8) return false;
    out.real_sample_rate = read_double(p);
  }

  if (flags & kAOOBinFlag_TimeStamp) {
    size_t remaining = len - (p - data);
    if (remaining < 8) return false;
    out.timestamp = read_uint64(p);
  }

  // XRun flag means empty block (data_size should be 0)
  if (flags & kAOOBinFlag_XRun) {
    out.audio_data.data = nullptr;
    out.audio_data.len = 0;
    return true;
  }

  // Audio data
  size_t remaining = len - (p - data);
  if (remaining < data_size) return false;
  // Cast away const: the pointer refers into the caller's receive buffer
  // which is non-const (aao_in_buffer); OSCBinaryData stores uint8_t*.
  out.audio_data.data = const_cast<uint8_t*>(p);
  out.audio_data.len = data_size;

  return true;
}

}  // namespace arduino_aoo
