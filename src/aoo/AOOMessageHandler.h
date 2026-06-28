#pragma once
#include <vector>
#include "aoo/AOOClockSync.h"
#include "aoo/AOOStream.h"
#include "aoo/AOOProtocol.h"
#include "AudioTools/Communication/OSCData.h"

namespace arduino_aoo {

/**
 * @brief Virtual listener interface for AOO message events.
 *
 * Ping/pong and clock sync are handled internally by AOOMessageHandler.
 * Subclass and override only the methods that differ between sender and
 * receiver.
 *
 * @ingroup aoo
 * @author Phil Schatzmann
 * @copyright GPLv3
 */
class AOOMessageListener {
 public:
  virtual ~AOOMessageListener() = default;
  virtual bool onData(AOOData& data) { return false; }
  virtual bool onBinaryData(const uint8_t* data, size_t len) { return false; }
  virtual bool onStart(OSCData& osc) { return false; }
  virtual bool onStop(OSCData& osc) { return false; }
  virtual bool onInvite(OSCData& osc) { return false; }
  virtual bool onUninvite(OSCData& osc) { return false; }
  virtual bool onDecline(OSCData& osc) { return false; }
  virtual bool onResendRequest(OSCData& osc) { return false; }
  virtual bool onRequestStart(OSCData& osc) { return false; }
};

/**
 * @brief Common message parsing, routing, and ping/pong handling for AOO.
 *
 * Reads messages from an AOOStream transport, determines whether they are
 * binary or OSC, parses them, and either handles them internally (ping/pong)
 * or dispatches to an AOOMessageListener.
 *
 * Ping/pong and clock synchronization are handled directly — both sender
 * and receiver do the same thing: parse timestamps, reply, update clock.
 *
 * @ingroup aoo
 * @author Phil Schatzmann
 * @copyright GPLv3
 */
class AOOMessageHandler {
 public:
  void setStream(AOOStream& io) { p_io = &io; }
  void setListener(AOOMessageListener& listener) { p_listener = &listener; }
  void setId(int id_value) { aoo_id = id_value; }
  int getId() const { return aoo_id; }
  void setAddressPrefix(const char* prefix) { address_prefix = prefix; }
  void setLogOsc(bool flag) { log_osc = flag; }

  /// Access the clock synchronization state (updated from pong messages)
  AOOClockSync& clockSync() { return clock_sync; }

  /// Process all available messages. Returns number processed.
  int processMessages() {
    int count = 0;
    while (p_io != nullptr && p_io->available() > 0) {
      if (!processOneMessage()) break;
      count++;
    }
    return count;
  }

  /// Process a single message from the stream. Returns true if processed.
  bool processOneMessage() {
    if (p_io == nullptr) return false;

    size_t avail = p_io->available();
    if (avail == 0) return false;

    if (in_buffer.size() < avail) in_buffer.resize(avail);
    p_io->setTimeout(1);
    size_t read = p_io->readBytes(in_buffer.data(), avail);
    if (read == 0) return false;

    if (aoo_is_binary(in_buffer.data(), read)) {
      return processBinaryMessage(in_buffer.data(), read);
    }
    return processOSCMessage(in_buffer.data(), read);
  }

  struct Stats {
    uint32_t binary_data = 0;
    uint32_t osc_data = 0;
    uint32_t osc_start = 0;
    uint32_t osc_stop = 0;
    uint32_t osc_ping = 0;
    uint32_t osc_pong = 0;
    uint32_t osc_invite = 0;
    uint32_t osc_uninvite = 0;
    uint32_t osc_decline = 0;
    uint32_t osc_other = 0;
    uint32_t parse_errors = 0;
    uint32_t bin_parse_fail = 0;
    uint32_t bin_wrong_id = 0;
  };

  const Stats& stats() const { return stats_; }
  void resetStats() { stats_ = {}; }
  AOOStream* stream() { return p_io; }

 protected:
  AOOStream* p_io = nullptr;
  AOOMessageListener* p_listener = nullptr;
  int aoo_id = 0;
  const char* address_prefix = "/aoo/sink/";
  bool log_osc = false;
  bool is_sink = true;
  std::vector<uint8_t> in_buffer;
  Stats stats_;
  AOOClockSync clock_sync;

  int getIdFromAddress(const char* address) {
    if (StrView(address).startsWith(address_prefix)) {
      return StrView(address + strlen(address_prefix)).toInt();
    }
    return -1;
  }

  /// Determine direction from address prefix
  bool isSinkAddress() const {
    return strstr(address_prefix, "/sink/") != nullptr;
  }

  // --- Ping/pong handled internally ---

  bool handlePing(OSCData& osc) {
    LOGD("Handling ping");
    if (isSinkAddress()) {
      // We are a sink, received ping from source → parse AOOPingSink, reply AOOPongSource
      AOOPingSink ping;
      if (!ping.parse(osc.data(), osc.size())) {
        LOGE("Failed to parse ping");
        return false;
      }
      AOOPongSource pong;
      pong.source_id = ping.source_id;
      pong.sink_id = aoo_id;
      pong.t1 = ping.send_time;
      pong.t2 = micros();
      pong.t3 = micros();
      retargetToSender();
      return pong.send(*p_io);
    } else {
      // We are a source, received ping from sink → parse AOOPingSource, reply AOOPongSink
      AOOPingSource ping;
      if (!ping.parse(osc.data(), osc.size())) {
        LOGE("Failed to parse ping");
        return false;
      }
      AOOPongSink pong;
      pong.source_id = aoo_id;
      pong.sink_id = ping.sink_id;
      pong.t1 = ping.send_time;
      pong.t2 = micros();
      pong.t3 = micros();
      return pong.send(*p_io);
    }
  }

  bool handlePong(OSCData& osc) {
    LOGD("Handling pong");
    if (isSinkAddress()) {
      // We are a sink, received pong from source
      AOOPongSink pong;
      if (!pong.parse(osc.data(), osc.size())) {
        LOGE("Failed to parse pong");
        return false;
      }
      uint64_t t4 = micros();
      clock_sync.update(pong.t1, pong.t2, pong.t3, t4);
    } else {
      // We are a source, received pong from sink
      AOOPongSource pong;
      if (!pong.parse(osc.data(), osc.size())) {
        LOGE("Failed to parse pong");
        return false;
      }
      uint64_t t4 = micros();
      clock_sync.update(pong.t1, pong.t2, pong.t3, t4);
    }
    LOGI("Pong: rtt=%lu us, offset=%ld us",
         (unsigned long)clock_sync.rttMicros(),
         (long)clock_sync.offsetMicros());
    return true;
  }

  void retargetToSender() {
    if (p_io != nullptr) {
      IPAddress ip = p_io->senderIP();
      uint16_t port = p_io->senderPort();
      if ((uint32_t)ip != 0) {
        p_io->setRemote(ip, port);
      }
    }
  }

  // --- Binary and OSC message processing ---

  bool processBinaryMessage(const uint8_t* data, size_t len) {
    TRACED();
    AOOData aoo_data;
    if (!aoo_parse_bin_data(data, len, aoo_data)) {
      stats_.bin_parse_fail++;
      return false;
    }

    int msg_id = aoo_data.sink_id;
    if (aoo_id == 0) {
      aoo_id = msg_id;
      LOGI("Auto-assigned ID from binary: %d", aoo_id);
    }
    if (msg_id != aoo_id) {
      stats_.bin_wrong_id++;
      return false;
    }

    stats_.binary_data++;
    if (p_listener == nullptr) return false;

    p_listener->onBinaryData(data, len);
    p_listener->onData(aoo_data);
    return true;
  }

  bool processOSCMessage(uint8_t* data, size_t len) {
    TRACED();
    OSCData osc;
    osc.setLogActive(log_osc);
    if (!osc.parse(data, len)) {
      stats_.parse_errors++;
      LOGE("Failed to parse OSC message: %d bytes", (int)len);
      return false;
    }

    const char* address = osc.getAddress();
    int id = getIdFromAddress(address);
    if (aoo_id == 0) {
      aoo_id = id;
      LOGI("Auto-assigned ID from OSC: %d", aoo_id);
    }
    if (id != aoo_id) {
      LOGD("OSC message for id %d ignored (our id %d)", id, aoo_id);
      return false;
    }

    // Ping/pong handled internally
    if (strstr(address, "/pong") != nullptr) {
      stats_.osc_pong++;
      return handlePong(osc);
    } else if (strstr(address, "/ping") != nullptr) {
      stats_.osc_ping++;
      return handlePing(osc);
    }

    // Everything else delegated to listener
    if (p_listener == nullptr) return false;

    if (strstr(address, "/start") != nullptr) {
      stats_.osc_start++;
      return p_listener->onStart(osc);
    } else if (strstr(address, "/uninvite") != nullptr) {
      stats_.osc_uninvite++;
      return p_listener->onUninvite(osc);
    } else if (strstr(address, "/invite") != nullptr) {
      stats_.osc_invite++;
      return p_listener->onInvite(osc);
    } else if (strstr(address, "/decline") != nullptr) {
      stats_.osc_decline++;
      return p_listener->onDecline(osc);
    } else if (strstr(address, "/stop") != nullptr) {
      stats_.osc_stop++;
      return p_listener->onStop(osc);
    } else if (strstr(address, "/data") != nullptr) {
      stats_.osc_data++;
      AOOData aoo_data;
      aoo_data.sink_id = aoo_id;
      if (aoo_data.parse(osc.data(), osc.size())) {
        return p_listener->onData(aoo_data);
      }
      return p_listener->onResendRequest(osc);
    } else {
      stats_.osc_other++;
      LOGW("Unsupported address: %s", address);
    }
    return false;
  }
};

}  // namespace arduino_aoo
