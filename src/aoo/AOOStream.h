#pragma once
#include "AudioTools.h"
#include "AudioTools/Communication/HDLCStream.h"
#include "WiFiUdp.h"

namespace arduino_aoo {

/**
 * @brief Abstract transport stream for AOO (Audio Over OSC).
 *
 * Provides a Stream interface with optional addressing (setRemote/remoteIP/
 * remotePort).  Concrete subclasses implement the actual transport: UDP
 * packets (AOOStreamUDP), HDLC-framed serial (AOOStreamSerial), or any
 * other link that can carry AOO/OSC messages.
 */
class AOOStream : public Stream {
 public:
  virtual bool begin() { return true; }
  virtual void end() {}
  virtual void setRemote(IPAddress ip, uint16_t port) {}
  virtual IPAddress remoteIP() { return IPAddress(); }
  virtual uint16_t remotePort() { return 0; }
  virtual IPAddress senderIP() { return IPAddress(); }
  virtual uint16_t senderPort() { return 0; }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  size_t write(uint8_t) override { return 0; }
};

/**
 * @brief AOO transport over UDP.
 *
 * Each write() sends one UDP packet and each available()/read cycle receives
 * one, so UDP's datagram boundaries provide natural OSC message framing.
 * The template parameter selects the underlying UDP implementation
 * (defaults to WiFiUDP).
 */
template <typename UDP_t = WiFiUDP>
class AOOStreamUDP : public AOOStream {
 public:
  AOOStreamUDP() = default;

  AOOStreamUDP(IPAddress address, uint16_t port, bool multicast = false) {
    setRemote(address, port);
    is_multicast = multicast;
  }
  AOOStreamUDP(uint16_t port) {
    remote_port_ext = port;
  }

  void setMulticast(bool flag) { is_multicast = flag; }

  bool begin() override {
    if (is_multicast) {
      return udp.beginMulticast(remote_address_ext, remote_port_ext);
    }
    return udp.begin(remote_port_ext);
  }

  void end() override { udp.stop(); }

  int availableForWrite() override { return udp.availableForWrite(); }

    int available() override {
    int size = udp.available();
    if (size == 0) {
      size = udp.parsePacket();
    }
    yield();
    return size;
  }

  uint16_t remotePort() override { return remote_port_ext; }

  IPAddress remoteIP() override { return remote_address_ext; }

  void setRemote(IPAddress ip, uint16_t port) override {
    remote_address_ext = ip;
    remote_port_ext = port;
  }

  IPAddress senderIP() override { return udp.remoteIP(); }

  uint16_t senderPort() override { return udp.remotePort(); }

  size_t write(const uint8_t* data, size_t len) override {
    TRACED();
    udp.beginPacket(remoteIP(), remotePort());
    size_t result = udp.write(data, len);
    udp.endPacket();
    yield();
    return result;
  }

  size_t readBytes(uint8_t* data, size_t len) {
    if (available() > 0) {
      size_t result = udp.readBytes(data, len);
      yield();
      return result;
    }
    return 0;
  }

  int read() override { return udp.read(); }

  int peek() override { return udp.peek(); }

  size_t write(uint8_t val) override { return write(&val, 1); }

  void setTimeout(int timeout) { udp.setTimeout(); }

 protected:
  UDP_t udp;
  uint16_t remote_port_ext = 0;
  IPAddress remote_address_ext{};
  bool is_multicast = false;
};

/**
 * @brief AOO transport over a serial link using HDLC framing.
 *
 * Serial is a continuous byte stream with no inherent message boundaries.
 * This class wraps an HDLCStream around the underlying serial port so that
 * each write() is transmitted as a single HDLC frame (0x7E delimiters +
 * byte-stuffing), and each read cycle returns exactly one complete frame.
 * This gives the same one-write-per-message semantics that UDP provides
 * natively, making it suitable for point-to-point AOO links over UART or
 * USB-Serial.
 */
class AOOStreamSerial : public AOOStream {
 public:
  AOOStreamSerial(Stream& serial = Serial, int maxFrameSize = 2048)
      : serial_(serial), max_frame_size(maxFrameSize) {}

  bool begin() override {
    hdlc.resize(max_frame_size);
    hdlc.setStream(serial_);
    return true;
  }

  size_t write(const uint8_t* data, size_t len) override {
    return hdlc.write(data, len);
  }

  size_t write(uint8_t val) override { return hdlc.write(val); }

  int available() override { return hdlc.available(); }

  int availableForWrite() override { return hdlc.availableForWrite(); }

  int read() override { return hdlc.read(); }

  int peek() override { return hdlc.peek(); }

 private:
  Stream& serial_;
  int max_frame_size = 2048;
  HDLCStream hdlc;
};


}  // namespace arduino_aoo
