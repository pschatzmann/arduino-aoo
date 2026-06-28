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
  /// Starts the transport
  virtual bool begin() { return true; }
  /// Stops the transport
  virtual void end() {}
  /// Sets the remote address and port for sending
  virtual void setRemote(IPAddress ip, uint16_t port) {}
  /// Returns the configured remote IP address
  virtual IPAddress remoteIP() { return IPAddress(); }
  /// Returns the configured remote port
  virtual uint16_t remotePort() { return 0; }
  /// Returns the IP address of the last received message's sender
  virtual IPAddress senderIP() { return IPAddress(); }
  /// Returns the port of the last received message's sender
  virtual uint16_t senderPort() { return 0; }

  /// Returns the number of bytes available for reading
  int available() override { return 0; }
  /// Reads a single byte
  int read() override { return -1; }
  /// Peeks at the next byte without consuming it
  int peek() override { return -1; }
  /// Writes a single byte
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
  /// Default constructor
  AOOStreamUDP() = default;

  /// @param address remote IP address
  /// @param port remote port
  /// @param multicast true to join a multicast group
  AOOStreamUDP(IPAddress address, uint16_t port, bool multicast = false) {
    setRemote(address, port);
    is_multicast = multicast;
  }
  /// @param port local listening port
  AOOStreamUDP(uint16_t port) {
    remote_port_ext = port;
  }

  /// Enables or disables multicast mode
  void setMulticast(bool flag) { is_multicast = flag; }

  /// Starts the UDP socket
  bool begin() override {
    if (is_multicast) {
      return udp.beginMulticast(remote_address_ext, remote_port_ext);
    }
    return udp.begin(remote_port_ext);
  }

  /// Stops the UDP socket
  void end() override { udp.stop(); }

  /// Returns the number of bytes available for writing
  int availableForWrite() override { return udp.availableForWrite(); }

  /// Returns the number of bytes available for reading
  int available() override {
    int size = udp.available();
    if (size == 0) {
      size = udp.parsePacket();
    }
    yield();
    return size;
  }

  /// Returns the configured remote port
  uint16_t remotePort() override { return remote_port_ext; }

  /// Returns the configured remote IP address
  IPAddress remoteIP() override { return remote_address_ext; }

  /// Sets the remote address and port for sending
  void setRemote(IPAddress ip, uint16_t port) override {
    remote_address_ext = ip;
    remote_port_ext = port;
  }

  /// Returns the IP address of the last received packet's sender
  IPAddress senderIP() override { return udp.remoteIP(); }

  /// Returns the port of the last received packet's sender
  uint16_t senderPort() override { return udp.remotePort(); }

  /// Sends data as a single UDP packet
  size_t write(const uint8_t* data, size_t len) override {
    TRACED();
    udp.beginPacket(remoteIP(), remotePort());
    size_t result = udp.write(data, len);
    udp.endPacket();
    yield();
    return result;
  }

  /// Reads bytes from the current UDP packet
  size_t readBytes(uint8_t* data, size_t len) {
    if (available() > 0) {
      size_t result = udp.readBytes(data, len);
      yield();
      return result;
    }
    return 0;
  }

  /// Reads a single byte from the current packet
  int read() override { return udp.read(); }

  /// Peeks at the next byte without consuming it
  int peek() override { return udp.peek(); }

  /// Writes a single byte as a UDP packet
  size_t write(uint8_t val) override { return write(&val, 1); }

  /// Sets the read timeout
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
  /// @param serial the underlying serial stream
  /// @param maxFrameSize maximum HDLC frame size in bytes
  AOOStreamSerial(Stream& serial = Serial, int maxFrameSize = 2048)
      : serial_(serial), max_frame_size(maxFrameSize) {}

  /// Starts the HDLC framing layer
  bool begin() override {
    hdlc.resize(max_frame_size);
    hdlc.setStream(serial_);
    return true;
  }

  /// Writes data as an HDLC frame
  size_t write(const uint8_t* data, size_t len) override {
    return hdlc.write(data, len);
  }

  /// Writes a single byte as an HDLC frame
  size_t write(uint8_t val) override { return hdlc.write(val); }

  /// Returns the number of bytes available for reading
  int available() override { return hdlc.available(); }

  /// Returns the number of bytes available for writing
  int availableForWrite() override { return hdlc.availableForWrite(); }

  /// Reads a single byte from the current frame
  int read() override { return hdlc.read(); }

  /// Peeks at the next byte without consuming it
  int peek() override { return hdlc.peek(); }

 private:
  Stream& serial_;
  int max_frame_size = 2048;
  HDLCStream hdlc;
};


}  // namespace arduino_aoo
