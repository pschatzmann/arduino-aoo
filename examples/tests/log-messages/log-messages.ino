/**
 *   @brief Receive OSC messages over UDP and print them to the serial console.
 */
#include "AOO.h"

const int udpPort = 9999;
const char *ssid = "your-ssid";
const char *password = "your-password";
AOOStreamUDP udp;
OSCData osc;
Vector<uint8_t> data(1024 * 2);
HexDumpOutput dump(Serial);

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Info);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);

  udp.begin(udpPort);
}

// parse osc message and print information
void printOSC(int len) {
  if (osc.parse(data.data(), len)) {
    Serial.print("Received: ");
    Serial.println(osc.getAddress());
    Serial.print("Format: ");
    Serial.println(osc.getFormat());
  } else {
    StrView str((char *)data.data(), len);
    int pos = str.indexOf('/');
    Serial.println("Invalid OSC message:");
    if (pos > 0) {
      Serial.print("Found / at position: ");
      Serial.println(pos);
      Serial.print("Address: ");
      Serial.println(data.data()[pos]);
    }
    dump.write(data.data(), len);
    Serial.println();
  }
}

void loop() {
  int read = udp.readBytes(data.data(), data.size());
  if (read > 0) {
    printOSC(read);
  }
  delay(5);
}