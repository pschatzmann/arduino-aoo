#pragma once
#include "AudioTools.h"
#include "AudioTools/Communication/HDLCStream.h"

/// The UDP class which AOOStreamUDP creates if no UDP object is provided:
/// WiFiUDP where WiFi is available, otherwise only the Arduino UDP base class
/// (then a UDP object e.g. an EthernetUDP must be provided)
#ifndef AOO_DEFAULT_UDP
#if defined(ESP32) || defined(ESP8266) || defined(IS_DESKTOP) || \
    (defined(ARDUINO_ARCH_RP2040) && !defined(ARDUINO_ARCH_MBED))
#include "WiFiUdp.h"
#define AOO_DEFAULT_UDP WiFiUDP
#else
#include "Udp.h"
#define AOO_DEFAULT_UDP UDP
#endif
#endif

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
  /// Sets the read timeout in ms. Virtual so calls through an AOOStream*
  /// (e.g. from AOOMessageHandler) reach a subclass's actual transport
  /// instead of only the inherited, unused Stream::_timeout field.
  virtual void setTimeout(unsigned long timeoutMs) {
    Stream::setTimeout(timeoutMs);
  }

  using Stream::readBytes;
  /// Reads up to len bytes of the current message. Virtual, so that calls
  /// through an AOOStream* (e.g. from AOOMessageHandler) reach a subclass's
  /// bulk read: Stream::readBytes() is not virtual in all Arduino cores and
  /// would read byte by byte via timedRead().
  virtual size_t readBytes(uint8_t* data, size_t len) {
    return Stream::readBytes((char*)data, len);
  }

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
 *
 * Any UDP implementation which provides the Arduino UDP API can be used
 * (WiFiUDP, EthernetUDP, ...): either provide your UDP object, e.g.
 * `AOOStreamUDP<EthernetUDP> udp(ethernet_udp, 9999)`, or let the stream
 * create one of the class indicated by the template parameter (defaults to
 * AOO_DEFAULT_UDP: WiFiUDP where WiFi is available).
 */
template <typename UDP_t = AOO_DEFAULT_UDP>
class AOOStreamUDP : public AOOStream {
 public:
  /// Default constructor: creates the UDP object
  AOOStreamUDP() { createUDP(); }

  /// Creates the UDP object
  /// @param address remote IP address
  /// @param port remote port
  /// @param multicast true to join a multicast group
  AOOStreamUDP(IPAddress address, uint16_t port, bool multicast = false) {
    createUDP();
    setRemote(address, port);
    is_multicast = multicast;
  }
  /// Creates the UDP object
  /// @param port local listening port
  AOOStreamUDP(uint16_t port) {
    createUDP();
    remote_port_ext = port;
  }

  /// Uses the provided UDP object (e.g. an EthernetUDP)
  /// @param udp UDP implementation
  /// @param address remote IP address
  /// @param port remote port
  /// @param multicast true to join a multicast group
  AOOStreamUDP(UDP_t &udp, IPAddress address, uint16_t port,
               bool multicast = false)
      : p_udp(&udp) {
    setRemote(address, port);
    is_multicast = multicast;
  }
  /// Uses the provided UDP object (e.g. an EthernetUDP)
  /// @param udp UDP implementation
  /// @param port local listening port
  AOOStreamUDP(UDP_t &udp, uint16_t port) : p_udp(&udp) {
    remote_port_ext = port;
  }

  /// Provides access to the UDP implementation
  UDP_t &udp() { return *p_udp; }

  /// Enables or disables multicast mode
  void setMulticast(bool flag) { is_multicast = flag; }

  /// Defines the local port to listen on; by default this is the remote port
  /// (which conflicts when sender and receiver run on the same host)
  void setLocalPort(uint16_t port) { local_port = port; }

  /// Starts the UDP socket
  bool begin() override {
    uint16_t port = local_port != 0 ? local_port : remote_port_ext;
    if (is_multicast) {
      return udpBeginMulticast(*p_udp, remote_address_ext, port, 0);
    }
    LOGI("Listening on port %d", (int)port);
    return p_udp->begin(port);
  }

  /// Stops the UDP socket
  void end() override { p_udp->stop(); }

  /// Returns the number of bytes available for writing
  int availableForWrite() override { return p_udp->availableForWrite(); }

  /// Returns the number of bytes available for reading
  int available() override {
    int size = p_udp->available();
    if (size == 0) {
      size = p_udp->parsePacket();
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
  IPAddress senderIP() override { return p_udp->remoteIP(); }

  /// Returns the port of the last received packet's sender
  uint16_t senderPort() override { return p_udp->remotePort(); }

  /// Sends data as a single UDP packet
  size_t write(const uint8_t* data, size_t len) override {
    TRACED();
    p_udp->beginPacket(remoteIP(), remotePort());
    size_t result = p_udp->write(data, len);
    p_udp->endPacket();
    yield();
    return result;
  }

  /// Reads bytes from the current UDP packet with the bulk read of the UDP
  /// API (Stream::readBytes() reads byte by byte)
  size_t readBytes(uint8_t* data, size_t len) override {
    if (available() <= 0) return 0;
    int result = p_udp->read(data, len);
    yield();
    return result > 0 ? result : 0;
  }

  /// Reads a single byte from the current packet
  int read() override { return p_udp->read(); }

  /// Peeks at the next byte without consuming it
  int peek() override { return p_udp->peek(); }

  /// Writes a single byte as a UDP packet
  size_t write(uint8_t val) override { return write(&val, 1); }

  /// Sets the read timeout on the underlying UDP socket
  void setTimeout(unsigned long timeoutMs) override { p_udp->setTimeout(timeoutMs); }

 protected:
  std::unique_ptr<UDP_t> owned_udp;
  UDP_t *p_udp = nullptr;
  uint16_t remote_port_ext = 0;
  uint16_t local_port = 0;
  IPAddress remote_address_ext{};
  bool is_multicast = false;

  /// beginMulticast(ip, port) of the Arduino UDP API
  template <typename U>
  static auto udpBeginMulticast(U &udp, IPAddress ip, uint16_t port, int)
      -> decltype(udp.beginMulticast(ip, port), bool()) {
    return udp.beginMulticast(ip, port);
  }
  /// ESP8266: beginMulticast(interface, ip, port)
  template <typename U>
  static auto udpBeginMulticast(U &udp, IPAddress ip, uint16_t port, long)
      -> decltype(udp.beginMulticast(IPAddress(), ip, port), bool()) {
    return udp.beginMulticast(IPAddress(), ip, port);
  }
  /// UDP implementations without multicast support
  template <typename U>
  static bool udpBeginMulticast(U &, IPAddress, uint16_t, ...) {
    LOGE("Multicast is not supported by this UDP class");
    return false;
  }

  void createUDP() {
    static_assert(!std::is_abstract<UDP_t>::value,
                  "No UDP class available: provide a UDP object e.g. "
                  "AOOStreamUDP<EthernetUDP> udp(ethernet_udp, port) or "
                  "define AOO_DEFAULT_UDP");
    owned_udp.reset(new UDP_t());
    p_udp = owned_udp.get();
  }
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

  /// Reads the current frame
  size_t readBytes(uint8_t* data, size_t len) override {
    return hdlc.readBytes(data, len);
  }

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
