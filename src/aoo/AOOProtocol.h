#pragma once
#include <vector>
#include "AOOConfig.h"
#include "AudioTools.h"
#include "AudioTools/Communication/OSCData.h"
#include "Print.h"
#include "stdint.h"
#include <algorithm>

namespace arduino_aoo {

using std::min;
using std::max;
  
/// Extract the numeric ID from an OSC address after a known prefix
/// e.g. aoo_parse_id("/aoo/sink/42/data", "/aoo/sink/") returns 42
inline int32_t aoo_parse_id(const char *address, const char *prefix) {
  return atoi(address + strlen(prefix));
}

/**
 * @brief  abstract aoo protocol message
 * @ingroup aoo-protocol
 * @author Phil Schatzmann
 */
struct AOOMessage {
 public:
  /// Send the message to the stream
  virtual bool send(Print &stream) = 0;
  /// Parse the osc data into variables
  virtual bool parse(uint8_t *data, size_t len) = 0;
  /// Logs the content of the message
  virtual void logData() = 0;
};

/**
 * @brief  Start a new stream.
 * @ingroup aoo-protocol
 * @author Phil Schatzmann
 */
struct AOOStart : public AOOMessage {
  int32_t source_id = 0;
  int32_t sink_id = 0;
  const char *version = nullptr;
  int32_t stream_id = 0;
  int32_t sequence_number_start = 0;
  int32_t format_id = 0;
  int32_t channels = 0;
  int32_t sample_rate = 0;
  int32_t block_size = 0;
  const char *codec = nullptr;
  OSCBinaryData codec_extension;
  uint64_t start_time = 0;
  int32_t reblock_resample_latency_samples = 0;
  int32_t codec_delay_samples = 0;
  int32_t sample_offset = 0;

  void logData() override {
    LOGI("AOOStart: source_id=%d, sink_id=%d, version=%s", source_id, sink_id,
         version);
    LOGI("- stream_id=%d, sequence_number_start=%d", stream_id,
         sequence_number_start);
    LOGI("- format_id=%d, channels=%d", format_id, channels);
    LOGI("- sample_rate=%d, block_size=%d", sample_rate, block_size);
    LOGI("- codec=%s", codec);
    LOGI("- start_time=%lu", start_time);
    LOGI("- reblock_resample_latency_samples=%d",
         reblock_resample_latency_samples);
    LOGI("- codec_delay_samples=%d", codec_delay_samples);
    LOGI("- sample_offset=%d", sample_offset);
  }

  bool send(Print &stream) override {
    char address[AOO_MAX_ADDRESS_LEN];
    snprintf(address, sizeof(address), "/aoo/sink/%d/start", sink_id);
    uint32_t msg_size = size(address, format);

    uint8_t aao_send_data[msg_size + 1];
    memset(aao_send_data, 0, msg_size + 1);
    OSCData data{aao_send_data, msg_size};

    data.setAddress(address);
    data.setFormat(format);
    data.write(source_id);
    data.write(version);
    data.write(stream_id);
    data.write(sequence_number_start);
    data.write(format_id);
    data.write(channels);
    data.write(sample_rate);
    data.write(block_size);
    data.write(codec);
    data.write(codec_extension);
    data.write(start_time);
    data.write(reblock_resample_latency_samples);
    data.write(codec_delay_samples);
    data.write(sample_offset);

    // check that we did not overwrite the trailing byte
    assert(aao_send_data[msg_size] == 0);
    assert(data.size() == msg_size);

    size_t written = stream.write((const uint8_t *)data.data(), data.size());
    if (written != data.size()) {
      return false;
    }
    return true;
  }
  /// Parse the osc data into variables
  bool parse(uint8_t *bin, size_t len) override {
    TRACED();
    OSCData data;
    if (!data.parse(bin, len)) {
      return false;
    }
    // Check the format
    const char *in_format = data.getFormat();
    if (in_format == nullptr) {
      LOGE("format is null");
      return false;
    }
    if (strcmp(in_format, format) != 0) {
      LOGE("Invalid start message format: %s (expected %s)", in_format, format);
      return false;
    }

    // Parse format data
    sink_id = aoo_parse_id(data.getAddress(), "/aoo/sink/");
    source_id = data.readInt32();
    version = data.readString();
    stream_id = data.readInt32();
    sequence_number_start = data.readInt32();
    format_id = data.readInt32();
    channels = data.readInt32();
    sample_rate = data.readInt32();
    block_size = data.readInt32();
    codec = data.readString();
    codec_extension = data.readData();
    start_time = data.readTime();
    reblock_resample_latency_samples = data.readInt32();
    codec_delay_samples = data.readInt32();
    // skip NN (metadata nil markers) — no data bytes to consume
    sample_offset = data.readInt32();

    // Log info
    LOGI("AOOStart: source_id=%d, sink_id=%d, version=%s", source_id, sink_id,
         version);
    return true;
  }

 protected:
  const char *format = "isiiiiiisbtiiNNi";

  uint32_t size(const char *addr, const char *fmt) {
    return OSCData::oscSize(addr) + OSCData::oscFormatSize(fmt) + (10 * 4) +
           OSCData::oscSize(version) + OSCData::oscSize(codec) +
           OSCData::oscSize(codec_extension) + 8;
  }
};

/**
 * @brief  Request a start message
 * @ingroup aoo-protocol
 * @author Phil Schatzmann
 */

struct AOORequestStart : public AOOMessage {
  const char *version = nullptr;
  int32_t source_id = 0;
  int32_t sink_id = 0;

  void logData() override {
    LOGI("AOORequestStart: source_id=%d, sink_id=%d, version=%s", source_id,
         sink_id, version);
  }

  bool send(Print &stream) override {
    char address[AOO_MAX_ADDRESS_LEN];
    snprintf(address, sizeof(address), "/aoo/src/%d/start", source_id);

    uint32_t msg_size = size(address, format);
    uint8_t aao_send_data[msg_size + 1];
    memset(aao_send_data, 0, msg_size + 1);
    OSCData data{aao_send_data, msg_size};

    data.setAddress(address);
    data.setFormat(format);
    data.write(sink_id);
    data.write(version);

    // check that we did not overwrite the trailing byte
    assert(aao_send_data[msg_size] == 0);
    assert(data.size() == msg_size);

    size_t written = stream.write((const uint8_t *)data.data(), data.size());
    if (written != data.size()) {
      return false;
    }
    return true;
  }
  /// Parse the osc data into variables
  bool parse(uint8_t *bin, size_t len) override {
    TRACED();
    OSCData data;
    if (!data.parse(bin, len)) {
      return false;
    }
    // Check the format
    const char *format = data.getFormat();
    if (format == nullptr) {
      LOGE("format is null");
      return false;
    }

    if (strcmp(format, this->format) != 0) {
      LOGE("Invalid start message format: %s", format);
      return false;
    }

    // Parse format data
    source_id = aoo_parse_id(data.getAddress(), "/aoo/src/");
    sink_id = data.readInt32();
    version = data.readString();

    // Log info
    LOGI("Received start: source_id=%d, sink_id=%d, version=%s", source_id,
         sink_id, version);
    return true;
  }

 protected:
  const char *format = "is";
  uint32_t size(const char *addr, const char *fmt) {
    return OSCData::oscSize(addr) + OSCData::oscFormatSize(fmt) + 4 +
           OSCData::oscSize(version);
  }
};

/**
 * @brief  Stop a stream.
 * @ingroup aoo-protocol
 * @author Phil Schatzmann
 */
struct AOOStopSink : public AOOMessage {
  int32_t source_id = 0;
  int32_t sink_id = 0;
  int32_t stream_id = 0;
  int32_t last_seq = 0;
  int32_t sample_offset = 0;

  void logData() override {
    LOGI("AOOStopSink: source_id=%d, sink_id=%d, stream_id=%d", source_id,
         sink_id, stream_id);
  }

  bool send(Print &stream) override {
    char address[AOO_MAX_ADDRESS_LEN];
    snprintf(address, sizeof(address), "/aoo/sink/%d/stop", sink_id);

    uint32_t msg_size = size(address, format);
    uint8_t aao_send_data[msg_size + 1];
    memset(aao_send_data, 0, msg_size + 1);
    OSCData data{aao_send_data, msg_size};

    data.setAddress(address);
    data.setFormat(format);
    data.write(source_id);
    data.write(stream_id);
    data.write(last_seq);
    data.write(sample_offset);

    // check that we did not overwrite the trailing byte
    assert(aao_send_data[msg_size] == 0);
    assert(data.size() == msg_size);

    size_t written = stream.write((const uint8_t *)data.data(), data.size());
    if (written != data.size()) {
      return false;
    }
    return true;
  }

  bool parse(uint8_t *bin, size_t len) override {
    TRACED();
    OSCData data;
    if (!data.parse(bin, len)) {
      return false;
    }
    // Check the format
    const char *format = data.getFormat();
    if (format == nullptr) {
      LOGE("format is null");
      return false;
    }
    if (strcmp(format, this->format) != 0) {
      LOGE("Invalid stop message format: %s", format);
      return false;
    }

    // Parse format data
    sink_id = aoo_parse_id(data.getAddress(), "/aoo/sink/");
    source_id = data.readInt32();
    stream_id = data.readInt32();
    last_seq = data.readInt32();
    sample_offset = data.readInt32();

    // Log info
    LOGI("AOOStopSink: source_id=%d, sink_id=%d", source_id, sink_id);
    return true;
  }

 protected:
  const char *format = "iiii";
  uint32_t size(const char *addr, const char *fmt) {
    return OSCData::oscSize(addr) + OSCData::oscFormatSize(fmt) + (4 * 4);
  }
};

/**
 * @brief  Stop a stream.
 * @ingroup aoo-protocol
 * @author Phil Schatzmann
 */

struct AOOStopSource : public AOOMessage {
  int32_t source_id = 0;
  int32_t sink_id = 0;
  int32_t stream_id = 0;

  void logData() override {
    LOGI("AOOStopSource: source_id=%d, sink_id=%d, stream_id=%d", source_id,
         sink_id, stream_id);
  }
  bool send(Print &stream) override {
    char address[AOO_MAX_ADDRESS_LEN];

    snprintf(address, sizeof(address), "/aoo/src/%d/stop", source_id);

    uint32_t msg_size = size(address, format);
    uint8_t aao_send_data[msg_size + 1];
    memset(aao_send_data, 0, msg_size + 1);
    OSCData data{aao_send_data, msg_size};

    data.setAddress(address);
    data.setFormat(format);
    data.write(sink_id);
    data.write(stream_id);

    // check that we did not overwrite the trailing byte
    assert(aao_send_data[msg_size] == 0);
    assert(data.size() == msg_size);

    size_t written = stream.write((const uint8_t *)data.data(), data.size());
    if (written != data.size()) {
      return false;
    }
    return true;
  }

  bool parse(uint8_t *bin, size_t len) override {
    TRACED();
    OSCData data;
    if (!data.parse(bin, len)) {
      return false;
    }
    // Check the format
    const char *format = data.getFormat();
    if (format == nullptr) {
      LOGE("format is null");
      return false;
    }
    if (strcmp(format, this->format) != 0) {
      LOGE("Invalid stop message format: %s", format);
      return false;
    }

    // Parse format data
    source_id = aoo_parse_id(data.getAddress(), "/aoo/src/");
    sink_id = data.readInt32();
    stream_id = data.readInt32();

    // Log info
    LOGI("AOOStopSource: source_id=%d, sink_id=%d, stream=%d", source_id,
         sink_id, stream_id);
    return true;
  }

 protected:
  const char *format = "ii";  // sink_id, stream_id
  uint32_t size(const char *addr, const char *fmt) {
    return OSCData::oscSize(addr) + OSCData::oscFormatSize(fmt) + (2 * 4);
  }
};

/**
 * @brief  deliver audio data, large blocks are split across several frames:
 *  ``/aoo/sink/<sink>/data ,iiidiiiib <src> <stream_id> <seq> <samplerate>
 *  <channel_onset> <totalsize> <total_number_of_frames> <frame> <data>``
 * @ingroup aoo-protocol
 * @author Phil Schatzmann
 */
struct AOOData : public AOOMessage {
  int32_t source_id = 0;
  int32_t sink_id = 0;
  int32_t stream_id = 0;
  int32_t seq_no = 0;
  uint64_t timestamp = 0;
  double real_sample_rate = 0.0;
  int32_t channel_onset = 0;
  int32_t total_size = 0;
  int32_t message_data_size = 0;
  int32_t total_number_of_frames = 0;
  int32_t frame_idx = 0;
  OSCBinaryData audio_data;

  void logData() override {
    LOGI("AOOData: source_id=%d, sink_id=%d, stream_id=%d", source_id, sink_id,
         stream_id);
    LOGI("- seq_no=%d, timestamp=%lu", seq_no, timestamp);
    LOGI("- real_sample_rate=%f, channel_onset=%d", real_sample_rate,
         channel_onset);
    LOGI("- total_size=%d, message_data_size=%d", total_size,
         message_data_size);
    LOGI("- total_number_of_frames=%d, frame_idx=%d", total_number_of_frames,
         frame_idx);
  }

  bool send(Print &stream) override {
    char address[AOO_MAX_ADDRESS_LEN];
    LOGD("AOOData: %d", (int)audio_data.len);
    snprintf(address, sizeof(address), "/aoo/sink/%d/data", sink_id);
    // This might get big, so we allocate the data on the heap
    uint32_t msg_size = size(address, format);
    // std::vector<uint8_t> aao_send_data;
    //  aao_send_data.resize(msg_size + 1);
    uint8_t aao_send_data[msg_size + 1];
    memset(aao_send_data, 0, msg_size + 1);
    OSCData data{aao_send_data, msg_size};

    // use data on the heap
    data.setAddress(address);
    data.setFormat(format);
    data.write(source_id);
    data.write(stream_id);
    data.write(seq_no);  // seq
    data.write(timestamp);
    data.write(real_sample_rate);
    data.write(channel_onset);
    data.write(total_size);
    // N = no stream message
    data.write(total_number_of_frames);
    data.write(frame_idx);
    data.write(audio_data);

    // check that we did not overwrite the trailing byte
    assert(data.size() == msg_size);
    assert(aao_send_data[msg_size] == 0);

    size_t written = stream.write((const uint8_t *)data.data(), data.size());
    if (written != data.size()) {
      return false;
    }
    return true;
  }

  bool parse(uint8_t *bin, size_t len) override {
    TRACED();
    OSCData data;
    if (!data.parse(bin, len)) {
      return false;
    }
    const char *format = data.getFormat();
    if (format == nullptr) {
      LOGE("format is null");
      return false;
    }
    if (strcmp(format, this->format) != 0) {
      LOGE("Invalid data message format: %s", format);
      return false;
    }
    // Read data message info
    sink_id = aoo_parse_id(data.getAddress(), "/aoo/sink/");
    source_id = data.readInt32();
    stream_id = data.readInt32();
    seq_no = data.readInt32();
    timestamp = data.readTime();
    real_sample_rate = data.readDouble();
    channel_onset = data.readInt32();
    total_size = data.readInt32();
    // skip N (stream message nil marker)
    total_number_of_frames = data.readInt32();
    frame_idx = data.readInt32();
    audio_data = data.readData();
    return true;
  }

 protected:
  const char *format = "iiitdiiNiib";  // src, stream_id, seq, timestamp,
                                       // sample_rate, channel_onset, total_size,
                                       // N(msg_size), num_frames, frame, data
  uint32_t size(const char *addr, const char *fmt) {
    return OSCData::oscSize(addr) + OSCData::oscFormatSize(fmt) + (7 * 4) +
           (2 * 8) + OSCData::oscSize(audio_data);
  }
};

/**
 * @brief request dropped packets: /aoo/src/<src>/data ,ii[ii]* <sink>
 * <stream_id>
 * [<seq> <frame>]*
 * @ingroup aoo-protocol
 * @author Phil Schatzmann
 */
struct AOOResendData : public AOOMessage {
  int32_t source_id = 0;
  int32_t sink_id = 0;
  int32_t stream_id = 0;

  struct ResendItem {
    int32_t seq = 0;
    int32_t frame = 0;
  };

  std::vector<ResendItem> resend_items;

  void logData() override {
    LOGI("AOOResendData: source_id=%d, sink_id=%d, stream_id=%d", source_id,
         sink_id, stream_id);
    for (auto &item : resend_items) {
      LOGI("- seq=%d, frame=%d", item.seq, item.frame);
    }
  }

  /// Send request for missing data
  bool send(Print &stream) override {
    // create format string
    int format_len = 2 + resend_items.size() * 2 + 1;
    char format[format_len];
    memset(format, 0, format_len);
    memset(format, 'i', format_len - 1);

    // address
    char address[AOO_MAX_ADDRESS_LEN];
    snprintf(address, sizeof(address), "/aoo/src/%d/data", source_id);

    uint32_t msg_size = size(address, format);
    uint8_t aao_send_data[msg_size + 1];
    memset(aao_send_data, 0, msg_size + 1);
    OSCData data{aao_send_data, msg_size};

    data.setAddress(address);
    data.setFormat(format);
    data.write(sink_id);
    data.write(stream_id);
    for (auto &item : resend_items) {
      data.write(item.seq);
      data.write(item.frame);
    }

    // check that we did not overwrite the trailing byte
    assert(aao_send_data[msg_size] == 0);
    assert(data.size() == msg_size);

    size_t written = stream.write((const uint8_t *)data.data(), data.size());
    if (written != data.size()) {
      return false;
    }
    return true;
  }

  /// Parse the resend request
  bool parse(uint8_t *bin, size_t len) override {
    TRACED();
    OSCData data;
    if (!data.parse(bin, len)) {
      return false;
    }
    const char *format = data.getFormat();
    if (format == nullptr) {
      LOGE("format is null");
      return false;
    }
    int format_len = strlen(format);

    source_id = aoo_parse_id(data.getAddress(), "/aoo/src/");
    sink_id = data.readInt32();
    stream_id = data.readInt32();

    int n = (format_len - 2) / 2;
    for (int j = 0; j < n; j++) {
      ResendItem item;
      item.seq = data.readInt32();
      item.frame = data.readInt32();
      resend_items.push_back(item);
    }
    return true;
  }

 protected:
  uint32_t size(const char *addr, const char *fmt) {
    int fmt_len = strlen(fmt);
    return OSCData::oscSize(addr) + OSCData::oscFormatSize(fmt) + (fmt_len * 4);
  }
};

/**
 * @brief ping message from source to sink (usually sent once per second):
 *  ``/aoo/sink/<sink>/ping ,it <sink> <t1>`
 * @ingroup aoo-protocol
 * @author Phil Schatzmann
 */
struct AOOPingSink : public AOOMessage {
  int32_t source_id = 0;
  int32_t sink_id = 0;
  uint64_t send_time = 0;

  void logData() override {
    LOGI("AOOPongSource: source_id=%d, sink_id=%d, t1=%lu", source_id, sink_id,
         send_time);
  }

  bool send(Print &stream) override {
    char address[AOO_MAX_ADDRESS_LEN];
    snprintf(address, sizeof(address), "/aoo/sink/%d/ping", sink_id);

    uint32_t msg_size = size(address, format);
    uint8_t aao_send_data[msg_size + 1];
    memset(aao_send_data, 0, msg_size + 1);
    OSCData data{aao_send_data, msg_size};

    data.setAddress(address);
    data.setFormat(format);
    data.write(source_id);
    data.write(send_time);

    // check that we did not overwrite the trailing byte
    assert(aao_send_data[msg_size] == 0);
    assert(data.size() == msg_size);

    size_t written = stream.write((const uint8_t *)data.data(), data.size());
    if (written != data.size()) {
      return false;
    }
    return true;
  }

  /// Process ping message
  bool parse(uint8_t *bin, size_t len) override {
    TRACED();
    OSCData data;
    if (!data.parse(bin, len)) {
      return false;
    }
    const char *format = data.getFormat();
    if (format == nullptr) {
      LOGE("format is null");
      return false;
    }
    if (strcmp(format, this->format) != 0) {
      LOGE("Invalid ping message format: %s", format);
      return false;
    }

    // Read ping data
    sink_id = aoo_parse_id(data.getAddress(), "/aoo/sink/");
    source_id = data.readInt32();
    send_time = data.readTime();

    return true;
  }

 protected:
  const char *format = "it";  // sink_id, t1
  uint32_t size(const char *addr, const char *fmt) {
    return OSCData::oscSize(addr) + OSCData::oscFormatSize(fmt) + (4 + 8);
  }
};

/**
 * @brief ping message from sink to source (usually sent once per second):
 *  ``/aoo/src/<source>/ping ,itt <sink> <t1> ``
 * @ingroup aoo-protocol
 * @author Phil Schatzmann
 */
struct AOOPingSource : public AOOMessage {
  int32_t source_id = 0;
  int32_t sink_id = 0;
  uint64_t send_time = 0;

  void logData() override {
    LOGI("AOOPongSource: source_id=%d, sink_id=%d, t1=%lu", source_id, sink_id,
         send_time);
  }

  bool send(Print &stream) override {
    char address[AOO_MAX_ADDRESS_LEN];
    snprintf(address, sizeof(address), "/aoo/src/%d/ping", source_id);

    uint32_t msg_size = size(address, format);
    uint8_t aao_send_data[msg_size + 1];
    memset(aao_send_data, 0, msg_size + 1);
    OSCData data{aao_send_data, msg_size};

    data.setAddress(address);
    data.setFormat(format);
    data.write(sink_id);
    data.write(send_time);

    // check that we did not overwrite the trailing byte
    assert(aao_send_data[msg_size] == 0);
    assert(data.size() == msg_size);

    size_t written = stream.write((const uint8_t *)data.data(), data.size());
    if (written != data.size()) {
      return false;
    }
    return true;
  }

  /// Process ping message
  bool parse(uint8_t *bin, size_t len) override {
    TRACED();
    OSCData data;
    if (!data.parse(bin, len)) {
      return false;
    }
    const char *format = data.getFormat();
    if (format == nullptr) {
      LOGE("format is null");
      return false;
    }
    if (strcmp(format, this->format) != 0) {
      LOGE("Invalid ping message format: %s", format);
      return false;
    }

    // Read ping data
    source_id = aoo_parse_id(data.getAddress(), "/aoo/src/");
    sink_id = data.readInt32();
    send_time = data.readTime();

    return true;
  }

 protected:
  const char *format = "it";  // sink_id, t1
  uint32_t size(const char *addr, const char *fmt) {
    return OSCData::oscSize(addr) + OSCData::oscFormatSize(fmt) + (4 + 8);
  }
};

/**
 * @brief pong message:
 *  ``/aoo/sink/<sink>/pong ,ittt <sink> <t1> <t2> <t3>``
 * @ingroup aoo-protocol
 * @author Phil Schatzmann
 */
struct AOOPongSink : public AOOMessage {
  int32_t source_id = 0;
  int32_t sink_id = 0;
  uint64_t t1 = 0;  // send time
  uint64_t t2 = 0;  // receive time
  uint64_t t3 = 0;  // send time

  void logData() override {
    LOGI("AOOPongSource: source_id=%d, sink_id=%d, t1=%lu, t2=%lu, t3=%lu",
         source_id, sink_id, t1, t2, t3);
  }

  bool send(Print &stream) override {
    char address[AOO_MAX_ADDRESS_LEN];
    snprintf(address, sizeof(address), "/aoo/sink/%d/pong", sink_id);

    uint32_t msg_size = size(address, format);
    uint8_t aao_send_data[msg_size + 1];
    memset(aao_send_data, 0, msg_size + 1);
    OSCData data{aao_send_data, msg_size};

    data.setAddress(address);
    data.setFormat(format);
    data.write(source_id);
    data.write(t1);
    data.write(t2);
    data.write(t3);

    // check that we did not overwrite the trailing byte
    assert(aao_send_data[msg_size] == 0);
    assert(data.size() == msg_size);

    size_t written = stream.write((const uint8_t *)data.data(), data.size());
    if (written != data.size()) {
      return false;
    }
    return true;
  }

  bool parse(uint8_t *bin, size_t len) override {
    TRACED();
    OSCData data;
    if (!data.parse(bin, len)) {
      return false;
    }
    const char *format = data.getFormat();
    if (format == nullptr) {
      LOGE("format is null");
      return false;
    }
    if (strncmp(format, this->format, strlen(this->format)) != 0) {
      LOGE("Invalid pong message format: %s", format);
      return false;
    }

    // Read pong data (optional trailing fields like packet loss are ignored)
    sink_id = aoo_parse_id(data.getAddress(), "/aoo/sink/");
    source_id = data.readInt32();
    t1 = data.readTime();
    t2 = data.readTime();
    t3 = data.readTime();

    LOGI("AOOPongSink: %ld %ld %ld", t1, t2, t3);

    // Reply to ping
    return true;
  }

 protected:
  const char *format = "ittt";  // sink_id, t1, t2, t3
  uint32_t size(const char *addr, const char *fmt) {
    return OSCData::oscSize(addr) + OSCData::oscFormatSize(fmt) + 4 + (3 * 8);
  }
};

/**
 * @brief pong message:
 *  ``/aoo/src/<source>/pong ,ittt <sink> <t1> <t2> <t3>``
 * @ingroup aoo-protocol
 * @author Phil Schatzmann
 */
struct AOOPongSource : public AOOMessage {
  int32_t source_id = 0;
  int32_t sink_id = 0;
  uint64_t t1 = 0;
  uint64_t t2 = 0;
  uint64_t t3 = 0;

  void logData() override {
    LOGI("AOOPongSource: source_id=%d, sink_id=%d, t1=%lu, t2=%lu, t3=%lu",
         source_id, sink_id, t1, t2, t3);
  }

  bool send(Print &stream) override {
    char address[AOO_MAX_ADDRESS_LEN];
    memset(address, 0, sizeof(address));
    snprintf(address, sizeof(address), "/aoo/src/%d/pong", source_id);

    uint32_t msg_size = size(address, format);
    uint8_t aao_send_data[msg_size + 1];
    memset(aao_send_data, 0, msg_size + 1);
    OSCData data{aao_send_data, msg_size};

    data.setAddress(address);
    data.setFormat(format);
    data.write(sink_id);
    data.write(t1);
    data.write(t2);
    data.write(t3);

    // check that we did not overwrite the trailing byte
    assert(aao_send_data[msg_size] == 0);
    assert(data.size() == msg_size);

    size_t written = stream.write((const uint8_t *)data.data(), data.size());
    if (written != data.size()) {
      return false;
    }
    return true;
  }

  bool parse(uint8_t *bin, size_t len) override {
    TRACED();
    OSCData data;
    if (!data.parse(bin, len)) {
      return false;
    }
    const char *format = data.getFormat();
    if (format == nullptr) {
      LOGE("format is null");
      return false;
    }
    if (strncmp(format, this->format, strlen(this->format)) != 0) {
      LOGE("Invalid pong message format: %s", format);
      return false;
    }

    // Read pong data (optional trailing fields like packet loss are ignored)
    source_id = aoo_parse_id(data.getAddress(), "/aoo/src/");
    sink_id = data.readInt32();
    t1 = data.readTime();
    t2 = data.readTime();
    t3 = data.readTime();

    LOGI("AOOPongSource: %ld %ld %ld", t1, t2, t3);

    // Reply to ping
    return true;
  }

 protected:
  const char *format = "ittt";  // sink_id, t1, t2, t3
  uint32_t size(const char *addr, const char *fmt) {
    return OSCData::oscSize(addr) + OSCData::oscFormatSize(fmt) + 4 + (3 * 8);
  }
};

/**
 * @brief Invite a source to start streaming to this sink:
 *  ``/aoo/src/<source>/invite ,iiNN <sink> <stream_id> <nil> <nil>``
 * @ingroup aoo-protocol
 * @author Phil Schatzmann
 */
struct AOOInvite : public AOOMessage {
  int32_t source_id = 0;
  int32_t sink_id = 0;
  int32_t stream_id = 0;

  void logData() override {
    LOGI("AOOInvite: source_id=%d, sink_id=%d, stream_id=%d", source_id,
         sink_id, stream_id);
  }

  bool send(Print &stream) override {
    char address[AOO_MAX_ADDRESS_LEN];
    snprintf(address, sizeof(address), "/aoo/src/%d/invite", source_id);

    uint32_t msg_size = size(address, format);
    uint8_t aao_send_data[msg_size + 1];
    memset(aao_send_data, 0, msg_size + 1);
    OSCData data{aao_send_data, msg_size};

    data.setAddress(address);
    data.setFormat(format);
    data.write(sink_id);
    data.write(stream_id);

    assert(aao_send_data[msg_size] == 0);
    assert(data.size() == msg_size);

    size_t written = stream.write((const uint8_t *)data.data(), data.size());
    return written == data.size();
  }

  bool parse(uint8_t *bin, size_t len) override {
    TRACED();
    OSCData data;
    if (!data.parse(bin, len)) return false;
    const char *fmt = data.getFormat();
    if (fmt == nullptr || strcmp(fmt, format) != 0) {
      LOGE("Invalid invite format: %s", fmt ? fmt : "null");
      return false;
    }
    source_id = aoo_parse_id(data.getAddress(), "/aoo/src/");
    sink_id = data.readInt32();
    stream_id = data.readInt32();
    // skip NN (metadata nil markers)
    LOGI("AOOInvite: source=%d, sink=%d", source_id, sink_id);
    return true;
  }

 protected:
  const char *format = "iiNN";
  uint32_t size(const char *addr, const char *fmt) {
    return OSCData::oscSize(addr) + OSCData::oscFormatSize(fmt) + (2 * 4);
  }
};

/**
 * @brief Uninvite a source (ask it to stop streaming to this sink):
 *  ``/aoo/src/<source>/uninvite ,ii <sink> <stream_id>``
 * @ingroup aoo-protocol
 * @author Phil Schatzmann
 */
struct AOOUninvite : public AOOMessage {
  int32_t source_id = 0;
  int32_t sink_id = 0;
  int32_t stream_id = 0;

  void logData() override {
    LOGI("AOOUninvite: source_id=%d, sink_id=%d, stream_id=%d", source_id,
         sink_id, stream_id);
  }

  bool send(Print &stream) override {
    char address[AOO_MAX_ADDRESS_LEN];
    snprintf(address, sizeof(address), "/aoo/src/%d/uninvite", source_id);

    uint32_t msg_size = size(address, format);
    uint8_t aao_send_data[msg_size + 1];
    memset(aao_send_data, 0, msg_size + 1);
    OSCData data{aao_send_data, msg_size};

    data.setAddress(address);
    data.setFormat(format);
    data.write(sink_id);
    data.write(stream_id);

    assert(aao_send_data[msg_size] == 0);
    assert(data.size() == msg_size);

    size_t written = stream.write((const uint8_t *)data.data(), data.size());
    return written == data.size();
  }

  bool parse(uint8_t *bin, size_t len) override {
    TRACED();
    OSCData data;
    if (!data.parse(bin, len)) return false;
    const char *fmt = data.getFormat();
    if (fmt == nullptr || strcmp(fmt, format) != 0) {
      LOGE("Invalid uninvite format: %s", fmt ? fmt : "null");
      return false;
    }
    source_id = aoo_parse_id(data.getAddress(), "/aoo/src/");
    sink_id = data.readInt32();
    stream_id = data.readInt32();
    LOGI("AOOUninvite: source=%d, sink=%d", source_id, sink_id);
    return true;
  }

 protected:
  const char *format = "ii";
  uint32_t size(const char *addr, const char *fmt) {
    return OSCData::oscSize(addr) + OSCData::oscFormatSize(fmt) + (2 * 4);
  }
};

/**
 * @brief Source declines a sink's invitation:
 *  ``/aoo/sink/<sink>/decline ,ii <source> <stream_id>``
 * @ingroup aoo-protocol
 * @author Phil Schatzmann
 */
struct AOODecline : public AOOMessage {
  int32_t source_id = 0;
  int32_t sink_id = 0;
  int32_t stream_id = 0;

  void logData() override {
    LOGI("AOODecline: source_id=%d, sink_id=%d, stream_id=%d", source_id,
         sink_id, stream_id);
  }

  bool send(Print &stream) override {
    char address[AOO_MAX_ADDRESS_LEN];
    snprintf(address, sizeof(address), "/aoo/sink/%d/decline", sink_id);

    uint32_t msg_size = size(address, format);
    uint8_t aao_send_data[msg_size + 1];
    memset(aao_send_data, 0, msg_size + 1);
    OSCData data{aao_send_data, msg_size};

    data.setAddress(address);
    data.setFormat(format);
    data.write(source_id);
    data.write(stream_id);

    assert(aao_send_data[msg_size] == 0);
    assert(data.size() == msg_size);

    size_t written = stream.write((const uint8_t *)data.data(), data.size());
    return written == data.size();
  }

  bool parse(uint8_t *bin, size_t len) override {
    TRACED();
    OSCData data;
    if (!data.parse(bin, len)) return false;
    const char *fmt = data.getFormat();
    if (fmt == nullptr || strcmp(fmt, format) != 0) {
      LOGE("Invalid decline format: %s", fmt ? fmt : "null");
      return false;
    }
    sink_id = aoo_parse_id(data.getAddress(), "/aoo/sink/");
    source_id = data.readInt32();
    stream_id = data.readInt32();
    LOGI("AOODecline: source=%d, sink=%d", source_id, sink_id);
    return true;
  }

 protected:
  const char *format = "ii";
  uint32_t size(const char *addr, const char *fmt) {
    return OSCData::oscSize(addr) + OSCData::oscFormatSize(fmt) + (2 * 4);
  }
};

// ============================================================
// Binary message format (used by the official aoo library)
// ============================================================

/// Check if a message is in binary format (as opposed to OSC).
inline bool aoo_is_binary(const uint8_t* data, size_t len) {
  return len >= 4 && (data[0] & 0x80);
}

/// Read a uint8 from a byte pointer, advancing the pointer
inline uint8_t read_uint8(const uint8_t*& p) { return *p++; }

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
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
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
 *
 * @param data  raw message bytes
 * @param len   total message length
 * @param out   AOOData struct to populate
 * @return true if parsing succeeded, false on error
 * @ingroup aoo-protocol
 */
inline bool aoo_parse_bin_data(const uint8_t* data, size_t len, AOOData& out) {
  if (len < 4) return false;
  const uint8_t* p = data;

  uint8_t type_byte = read_uint8(p);
  if (!(type_byte & 0x80)) return false;

  uint8_t cmd_byte = read_uint8(p);
  bool large_ids = (cmd_byte & 0x80) != 0;
  uint8_t cmd = cmd_byte & 0x7F;
  if (cmd != kAOOBinCmd_Data) return false;

  if (!large_ids) {
    if (len < 4) return false;
    out.sink_id = read_uint8(p);
    out.source_id = read_uint8(p);
  } else {
    if (len < 12) return false;
    p += 2;
    out.sink_id = read_int32(p);
    out.source_id = read_int32(p);
  }

  size_t header_size = p - data;
  if (len < header_size + 12) return false;

  out.stream_id = read_int32(p);
  out.seq_no = read_int32(p);
  out.channel_onset = read_uint8(p);
  uint8_t flags = read_uint8(p);
  uint16_t data_size = read_uint16(p);

  out.total_size = data_size;
  out.total_number_of_frames = 1;
  out.frame_idx = 0;
  out.message_data_size = 0;
  out.real_sample_rate = 0.0;
  out.timestamp = 0;

  if (flags & kAOOBinFlag_Frames) {
    size_t remaining = len - (p - data);
    if (remaining < 8) return false;
    out.total_size = read_int32(p);
    out.total_number_of_frames = (int32_t)read_uint16(p);
    out.frame_idx = (int32_t)read_uint16(p);
  }
  if (flags & kAOOBinFlag_StreamMessage) {
    size_t remaining = len - (p - data);
    if (remaining < 4) return false;
    out.message_data_size = read_int32(p);
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
  if (flags & kAOOBinFlag_XRun) {
    out.audio_data.data = nullptr;
    out.audio_data.len = 0;
    return true;
  }

  size_t remaining = len - (p - data);
  if (remaining < data_size) return false;
  out.audio_data.data = const_cast<uint8_t*>(p);
  out.audio_data.len = data_size;
  return true;
}

}  // namespace arduino_aoo