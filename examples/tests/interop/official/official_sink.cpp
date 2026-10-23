// Official aoo sink: receives a stream and reports what it got
// usage: official_sink <own_port> <sink_id> <seconds>
// output: "official sink: starts=N stops=N rms=X crossings_per_sec=Y"
#include "aoo.h"
#include "aoo_sink.hpp"
#include "aoo_client.hpp"
#include "aoo_events.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

std::atomic<int> starts{0}, stops{0};

int main(int argc, char **argv) {
  if (argc < 4) return 2;
  int port = atoi(argv[1]), id = atoi(argv[2]);
  double seconds = atof(argv[3]);
  const int sr = 44100, block = 128;
  aoo_initialize(nullptr);
  auto sink = AooSink::create(id);
  sink->setEventHandler(
      [](void *, const AooEvent *event, AooThreadLevel) {
        if (event->type == kAooEventStreamStart) starts++;
        if (event->type == kAooEventStreamStop) stops++;
      }, nullptr, kAooEventModeCallback);
  auto client = AooClient::create();
  AooClientSettings settings;
  settings.portNumber = port;
  if (client->setup(settings) != kAooOk) { printf("setup failed\n"); return 1; }
  client->addSink(sink.get());
  sink->setup(1, sr, block, 0);
  std::thread send_thread([&] { client->send(kAooInfinite); });
  std::thread receive_thread([&] { client->receive(kAooInfinite); });

  std::vector<float> buf(block);
  AooSample *channels[1] = {buf.data()};
  double sum2 = 0; long n = 0, crossings = 0; float last = 0;
  auto next = std::chrono::steady_clock::now();
  int blocks = (int)(seconds * sr / block);
  for (int b = 0; b < blocks; b++) {
    sink->process(channels, block, aoo_getCurrentNtpTime(), nullptr, nullptr);
    client->notify();
    for (int i = 0; i < block; i++) {
      if (buf[i] != 0.0f) {
        sum2 += buf[i] * buf[i]; n++;
        if ((last < 0) != (buf[i] < 0)) crossings++;
        last = buf[i];
      }
    }
    next += std::chrono::microseconds(1000000LL * block / sr);
    std::this_thread::sleep_until(next);
  }
  client->stop();
  send_thread.join();
  receive_thread.join();
  double rms = n ? std::sqrt(sum2 / n) : 0;
  double cps = n ? crossings * (double)sr / n : 0;
  printf("official sink: starts=%d stops=%d samples=%ld rms=%.3f crossings_per_sec=%.0f\n",
         starts.load(), stops.load(), n, rms, cps);
  return 0;
}
