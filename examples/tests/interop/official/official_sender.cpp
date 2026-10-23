// Official aoo source: streams a 441 Hz sine (PCM int16, mono, 44.1 kHz)
// usage: official_sender <own_port> <sink_port> <sink_id> <seconds>
#include "aoo.h"
#include "aoo_source.hpp"
#include "aoo_client.hpp"
#include "codec/aoo_pcm.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

int main(int argc, char **argv) {
  if (argc < 5) return 2;
  int port = atoi(argv[1]), sink_port = atoi(argv[2]), sink_id = atoi(argv[3]);
  double seconds = atof(argv[4]);
  const int sr = 44100, block = 128;
  aoo_initialize(nullptr);
  auto source = AooSource::create(1);
  auto client = AooClient::create();
  AooClientSettings settings;
  settings.portNumber = port;
  if (client->setup(settings) != kAooOk) { printf("setup failed\n"); return 1; }
  client->addSource(source.get());
  AooSockAddrStorage addr; AooAddrSize addrlen = sizeof(addr);
  aoo_resolveIpEndpoint("127.0.0.1", sink_port, settings.socketType, &addr, &addrlen);
  AooEndpoint ep = {&addr, addrlen, sink_id};
  source->addSink(ep, kAooTrue);
  source->setup(1, sr, block, 0);
  AooFormatPcm fmt;
  AooFormatPcm_init(&fmt, 1, sr, block, kAooPcmInt16);
  source->setFormat(fmt.header);
  std::thread send_thread([&] { client->send(kAooInfinite); });
  std::thread receive_thread([&] { client->receive(kAooInfinite); });
  source->startStream(0, nullptr);

  std::vector<float> buf(block);
  AooSample *channels[1] = {buf.data()};
  double phase = 0;
  auto next = std::chrono::steady_clock::now();
  int blocks = (int)(seconds * sr / block);
  for (int b = 0; b < blocks; b++) {
    for (int i = 0; i < block; i++) {
      buf[i] = 0.5f * (float)std::sin(phase * 2 * M_PI);
      phase = std::fmod(phase + 441.0 / sr, 1.0);
    }
    source->process(channels, block, aoo_getCurrentNtpTime());
    client->notify();
    next += std::chrono::microseconds(1000000LL * block / sr);
    std::this_thread::sleep_until(next);
  }
  source->stopStream(0);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  client->stop();
  send_thread.join();
  receive_thread.join();
  printf("official sender: sent %d blocks\n", blocks);
  return 0;
}
