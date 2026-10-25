/**
 * @brief Command line tool for the desktop to send and receive audio with
 * AOO (Audio over OSC). Run "aoo --help" for the options.
 *
 * Examples:
 *   aoo send --host 192.168.1.33 --codec opus --input sine:440
 *   aoo send --input music.wav --redundancy 2
 *   aoo send --input mic
 *   aoo receive --output speaker
 *   aoo receive --output out.wav --time 60 --verbose
 *   aoo receive --output - | aplay -f S16_LE -r 48000 -c 2
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "AOO.h"
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecOpusMultiStream.h"
#include "AudioTools/AudioLibs/MiniAudioStream.h"
#include "AudioTools/AudioLibs/StdioStream.h"

using namespace audio_tools;
using namespace arduino_aoo;

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

static volatile bool is_stopped = false;
static void onSignal(int) { is_stopped = true; }

/// Print to stderr: stdout can be used for the audio data
class StderrPrint : public Print {
 public:
  size_t write(uint8_t ch) override { return fwrite(&ch, 1, 1, stderr); }
  size_t write(const uint8_t *data, size_t len) override {
    return fwrite(data, 1, len, stderr);
  }
} err_out;

/// Stream on a file
class StdFileStream : public AudioStream {
 public:
  ~StdFileStream() { close(); }
  bool open(const char *path, const char *mode) {
    file = fopen(path, mode);
    return file != nullptr;
  }
  /// Uses an already open file (e.g. stdin)
  void setFile(FILE *f) {
    file = f;
    is_owner = false;
  }
  void close() {
    if (file != nullptr && is_owner) fclose(file);
    file = nullptr;
  }
  FILE *handle() { return file; }
  /// Limits the readable data (e.g. to the data chunk of a WAV file)
  void setReadLimit(long bytes) { read_limit = bytes; }
  int available() override {
    if (file == nullptr || feof(file)) return 0;
    return read_limit >= 0 ? (int)std::min(read_limit, (long)DEFAULT_BUFFER_SIZE)
                           : DEFAULT_BUFFER_SIZE;
  }
  size_t readBytes(uint8_t *data, size_t len) override {
    if (file == nullptr) return 0;
    if (read_limit >= 0 && (long)len > read_limit) len = read_limit;
    size_t result = fread(data, 1, len, file);
    if (read_limit >= 0) read_limit -= result;
    return result;
  }
  size_t write(const uint8_t *data, size_t len) override {
    return file != nullptr ? fwrite(data, 1, len, file) : 0;
  }
  size_t write(uint8_t ch) override {
    return file != nullptr ? fwrite(&ch, 1, 1, file) : 0;
  }

 protected:
  FILE *file = nullptr;
  bool is_owner = true;
  long read_limit = -1;
};

static void writeLE(FILE *f, uint32_t value, int bytes) {
  for (int j = 0; j < bytes; j++) fputc((value >> (8 * j)) & 0xFF, f);
}

static uint32_t readLE(const uint8_t *data, int bytes) {
  uint32_t result = 0;
  for (int j = bytes - 1; j >= 0; j--) result = (result << 8) | data[j];
  return result;
}

/// Writes a 16 bit PCM WAV file: the sizes are updated by close()
class WavFileOutput : public AudioOutput {
 public:
  bool open(const char *path, AudioInfo info) {
    if (!file.open(path, "wb")) return false;
    FILE *f = file.handle();
    int frame_bytes = info.channels * 2;
    fwrite("RIFF", 1, 4, f);
    writeLE(f, 36, 4);  // updated by close()
    fwrite("WAVEfmt ", 1, 8, f);
    writeLE(f, 16, 4);
    writeLE(f, 1, 2);  // PCM
    writeLE(f, info.channels, 2);
    writeLE(f, info.sample_rate, 4);
    writeLE(f, info.sample_rate * frame_bytes, 4);
    writeLE(f, frame_bytes, 2);
    writeLE(f, 16, 2);
    fwrite("data", 1, 4, f);
    writeLE(f, 0, 4);  // updated by close()
    return true;
  }
  size_t write(const uint8_t *data, size_t len) override {
    size_t result = file.write(data, len);
    data_size += result;
    return result;
  }
  void close() {
    FILE *f = file.handle();
    if (f == nullptr) return;
    fseek(f, 4, SEEK_SET);
    writeLE(f, 36 + data_size, 4);
    fseek(f, 40, SEEK_SET);
    writeLE(f, data_size, 4);
    file.close();
  }

 protected:
  StdFileStream file;
  uint32_t data_size = 0;
};

/// Opens a 16 bit PCM WAV file for reading and determines its format
static bool openWav(StdFileStream &file, const char *path, AudioInfo &info) {
  if (!file.open(path, "rb")) return false;
  uint8_t header[12];
  if (fread(header, 1, 12, file.handle()) != 12 ||
      memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0)
    return false;
  bool has_format = false;
  uint8_t chunk[8];
  while (fread(chunk, 1, 8, file.handle()) == 8) {
    uint32_t size = readLE(chunk + 4, 4);
    if (memcmp(chunk, "fmt ", 4) == 0) {
      uint8_t fmt[16];
      if (size < 16 || fread(fmt, 1, 16, file.handle()) != 16) return false;
      if (readLE(fmt, 2) != 1 || readLE(fmt + 14, 2) != 16) {
        fprintf(stderr, "only 16 bit PCM WAV files are supported\n");
        return false;
      }
      info.channels = readLE(fmt + 2, 2);
      info.sample_rate = readLE(fmt + 4, 4);
      info.bits_per_sample = 16;
      fseek(file.handle(), size - 16 + (size & 1), SEEK_CUR);
      has_format = true;
    } else if (memcmp(chunk, "data", 4) == 0) {
      file.setReadLimit(size);
      return has_format;
    } else {
      fseek(file.handle(), size + (size & 1), SEEK_CUR);
    }
  }
  return false;
}

static bool endsWith(const std::string &str, const char *suffix) {
  size_t n = strlen(suffix);
  return str.size() >= n && str.compare(str.size() - n, n, suffix) == 0;
}

static AudioDecoder *createOpusDecoder() {
  auto *decoder = new OpusMultiStreamAudioDecoder();
  decoder->config().default_channel_mapping = OPUS_CHANNEL_MAPPING_SEPARATE;
  return decoder;
}

// ------------------------------------------------------------------
// Options
// ------------------------------------------------------------------

struct Options {
  std::string command;
  int port = -1;  // default depends on the command
  int id = 1;
  int sample_rate = 48000;
  int channels = 2;
  double seconds = 0;  // 0: until Ctrl-C (or the end of the input)
  bool verbose = false;
  AudioToolsLogLevel log_level = AudioToolsLogLevel::Warning;
  // send
  std::string host = "127.0.0.1";
  int remote_port = 9999;
  int sink = 1;
  std::string codec = "pcm";
  int block = -1;  // pcm: frames, opus: ms
  int redundancy = 1;
  std::string input = "sine:440";
  // receive
  std::string output = "speaker";
  int buffer_depth = 10;
  int resend = 1;
  bool adaptive = true;
  bool task = false;
};

static void usage() {
  fprintf(stderr,
          "usage: aoo send [options]     send audio to an AOO receiver\n"
          "       aoo receive [options]  receive audio from AOO senders\n"
          "\n"
          "common options:\n"
          "  -p, --port N          local UDP port (send: 9998, receive: 9999)\n"
          "  -i, --id N            source id (send) or sink id (receive) [1]\n"
          "  -r, --rate N          sample rate [48000]\n"
          "  -c, --channels N      number of channels [2]\n"
          "  -t, --time SECONDS    stop after the indicated time [Ctrl-C]\n"
          "  -v, --verbose         print statistics every second\n"
          "      --log LEVEL       error, warning, info or debug [warning]\n"
          "\n"
          "send options:\n"
          "  -H, --host IP         address of the receiver [127.0.0.1]\n"
          "  -P, --remote-port N   port of the receiver [9999]\n"
          "  -s, --sink N          sink id of the receiver [1]\n"
          "  -C, --codec NAME      pcm or opus [pcm]\n"
          "  -b, --block N         pcm: frames per block [256],\n"
          "                        opus: frame size in ms: 10, 20, 40, 60 [20]\n"
          "  -R, --redundancy N    send each block N times [1]\n"
          "      --input SOURCE    sine[:FREQUENCY], mic, FILE.wav or - for\n"
          "                        raw 16 bit PCM from stdin [sine:440]\n"
          "\n"
          "receive options:\n"
          "      --output TARGET   speaker, FILE.wav, FILE.csv or - for raw\n"
          "                        16 bit PCM to stdout [speaker]\n"
          "  -d, --buffer-depth N  blocks to buffer (jitter buffer) [10]\n"
          "      --resend N        resend requests per lost block, 0 = off [1]\n"
          "      --no-adaptive     no adaptive resampling\n"
          "      --task            receive in a separate task\n");
}

static bool parseOptions(int argc, char **argv, Options &opt) {
  if (argc < 2) return false;
  opt.command = argv[1];
  if (opt.command != "send" && opt.command != "receive") return false;
  for (int j = 2; j < argc; j++) {
    std::string arg = argv[j];
    auto value = [&]() -> const char * {
      if (j + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", arg.c_str());
        exit(2);
      }
      return argv[++j];
    };
    if (arg == "-p" || arg == "--port") opt.port = atoi(value());
    else if (arg == "-i" || arg == "--id") opt.id = atoi(value());
    else if (arg == "-r" || arg == "--rate") opt.sample_rate = atoi(value());
    else if (arg == "-c" || arg == "--channels") opt.channels = atoi(value());
    else if (arg == "-t" || arg == "--time") opt.seconds = atof(value());
    else if (arg == "-v" || arg == "--verbose") opt.verbose = true;
    else if (arg == "--log") {
      std::string level = value();
      if (level == "error") opt.log_level = AudioToolsLogLevel::Error;
      else if (level == "warning") opt.log_level = AudioToolsLogLevel::Warning;
      else if (level == "info") opt.log_level = AudioToolsLogLevel::Info;
      else if (level == "debug") opt.log_level = AudioToolsLogLevel::Debug;
      else return false;
    } else if (arg == "-H" || arg == "--host") opt.host = value();
    else if (arg == "-P" || arg == "--remote-port") opt.remote_port = atoi(value());
    else if (arg == "-s" || arg == "--sink") opt.sink = atoi(value());
    else if (arg == "-C" || arg == "--codec") opt.codec = value();
    else if (arg == "-b" || arg == "--block") opt.block = atoi(value());
    else if (arg == "-R" || arg == "--redundancy") opt.redundancy = atoi(value());
    else if (arg == "--input") opt.input = value();
    else if (arg == "--output") opt.output = value();
    else if (arg == "-d" || arg == "--buffer-depth") opt.buffer_depth = atoi(value());
    else if (arg == "--resend") opt.resend = atoi(value());
    else if (arg == "--no-adaptive") opt.adaptive = false;
    else if (arg == "--task") opt.task = true;
    else if (arg == "-h" || arg == "--help") return false;
    else {
      fprintf(stderr, "unknown option: %s\n", arg.c_str());
      return false;
    }
  }
  if (opt.codec != "pcm" && opt.codec != "opus") {
    fprintf(stderr, "unsupported codec: %s\n", opt.codec.c_str());
    return false;
  }
  return true;
}

/// True while the time limit is not reached and Ctrl-C was not pressed
static bool isRunning(const Options &opt, uint32_t start_ms) {
  if (is_stopped) return false;
  return opt.seconds <= 0 || millis() - start_ms < opt.seconds * 1000;
}

// ------------------------------------------------------------------
// Send
// ------------------------------------------------------------------

static int runSend(Options &opt) {
  AudioInfo info(opt.sample_rate, opt.channels, 16);

  // define the input
  SineWaveGenerator<int16_t> sine(16000);
  GeneratedSoundStream<int16_t> sine_stream(sine);
  StdFileStream wav_file;
  StdFileStream stdin_stream;
  MiniAudioStream mic;
  Stream *input = nullptr;
  bool is_real_time = false;  // the input is paced by the audio device
  bool has_end = false;       // the input ends (file)
  if (opt.input.rfind("sine", 0) == 0) {
    float freq = opt.input.size() > 5 ? atof(opt.input.c_str() + 5) : 440;
    sine.begin(info, freq);
    input = &sine_stream;
  } else if (opt.input == "mic") {
    auto cfg = mic.defaultConfig(RX_MODE);
    cfg.copyFrom(info);
    if (!mic.begin(cfg)) {
      fprintf(stderr, "could not open the microphone\n");
      return 1;
    }
    input = &mic;
    is_real_time = true;
  } else if (opt.input == "-") {
    stdin_stream.setFile(stdin);
    input = &stdin_stream;
    has_end = true;
  } else {
    if (!openWav(wav_file, opt.input.c_str(), info)) {
      fprintf(stderr, "could not open the WAV file %s\n", opt.input.c_str());
      return 1;
    }
    input = &wav_file;
    has_end = true;
  }
  // limit generators and files to the audio rate
  Throttle throttle(*input);
  throttle.begin(info);
  Stream &source = is_real_time ? *input : (Stream &)throttle;

  // define the sender
  IPAddress remote_ip;
  if (!remote_ip.fromString(opt.host.c_str())) {
    fprintf(stderr, "invalid IP address: %s\n", opt.host.c_str());
    return 1;
  }
  AOOStreamUDP<WiFiUDP> udp(remote_ip, opt.remote_port);
  udp.setLocalPort(opt.port > 0 ? opt.port : 9998);
  OpusMultiStreamAudioEncoder opus;  // must exist as long as the sender
  AOOSender sender(opt.id, udp);
  int block_frames = opt.block > 0 ? opt.block : 256;
  if (opt.codec == "opus") {
    int ms = opt.block > 0 ? opt.block : 20;
    auto &ocfg = opus.config();
    ocfg.default_channel_mapping = OPUS_CHANNEL_MAPPING_SEPARATE;
    switch (ms) {
      case 10: ocfg.frame_sizes_ms_x2 = OPUS_FRAMESIZE_10_MS; break;
      case 20: ocfg.frame_sizes_ms_x2 = OPUS_FRAMESIZE_20_MS; break;
      case 40: ocfg.frame_sizes_ms_x2 = OPUS_FRAMESIZE_40_MS; break;
      case 60: ocfg.frame_sizes_ms_x2 = OPUS_FRAMESIZE_60_MS; break;
      default:
        fprintf(stderr, "unsupported Opus frame size: %d ms\n", ms);
        return 1;
    }
    sender.setEncoder("opus", opus);
    block_frames = info.sample_rate * ms / 1000;
  }
  auto cfg = sender.defaultConfig();
  cfg.copyFrom(info);
  cfg.sink_targets.push_back(AOOSinkTarget(opt.sink));
  cfg.redundancy = opt.redundancy;
  if (!sender.begin(cfg)) {
    fprintf(stderr, "could not start the sender\n");
    return 1;
  }
  fprintf(stderr,
          "sending %s (%d Hz, %d channels, %s) to %s:%d, sink %d, from port "
          "%d with source id %d\n",
          opt.input.c_str(), info.sample_rate, info.channels, opt.codec.c_str(),
          opt.host.c_str(), opt.remote_port, opt.sink,
          opt.port > 0 ? opt.port : 9998, opt.id);

  // copy one block at a time (for PCM a block is one AOO message)
  StreamCopy copier(sender, source, block_frames * info.channels * 2);
  uint32_t start_ms = millis(), next_report = start_ms + 1000;
  while (isRunning(opt, start_ms)) {
    size_t copied = copier.copy();
    if (copied == 0) {
      if (has_end && input->available() <= 0) break;
      delay(1);
      sender.receive();  // keep processing pings and requests
    }
    if (opt.verbose && millis() >= next_report) {
      next_report += 1000;
      fprintf(stderr, "sent %.1f s, %u bytes, rtt %.1f ms\n",
              (millis() - start_ms) / 1000.0,
              (unsigned)sender.getTotalBytesSent(),
              sender.clockSync().rttMicros() / 1000.0);
    }
  }
  sender.end();
  fprintf(stderr, "sent %u bytes in %.1f s\n",
          (unsigned)sender.getTotalBytesSent(),
          (millis() - start_ms) / 1000.0);
  return 0;
}

// ------------------------------------------------------------------
// Receive
// ------------------------------------------------------------------

static void printReceiveStats(AOOReceiver &receiver, double audio_s,
                              double elapsed_s) {
  auto &st = receiver.stats();
  auto &ms = receiver.messageStats();
  fprintf(stderr,
          "%.1f s: audio %.1f s, sources %d, blocks %u, lost %u, resend "
          "requests %u, start requests %u, messages %u\n",
          elapsed_s, audio_s, receiver.getSourceCount(), st.segments_stored,
          receiver.xrunCount(), st.resend_requests, st.start_requests,
          ms.osc_data + ms.binary_data);
}

static int runReceive(Options &opt) {
  AudioInfo info(opt.sample_rate, opt.channels, 16);

  // define the output
  MiniAudioStream speaker;
  WavFileOutput wav;
  StdFileStream csv_file;
  CsvOutput<int16_t> csv(csv_file, info.channels);
  StdioStream stdout_stream;
  Print *output = nullptr;
  bool is_real_time = false;  // the output is paced by the audio device
  if (opt.output == "speaker") {
    auto cfg = speaker.defaultConfig(TX_MODE);
    cfg.copyFrom(info);
    if (!speaker.begin(cfg)) {
      fprintf(stderr, "could not open the speaker\n");
      return 1;
    }
    output = &speaker;
    is_real_time = true;
  } else if (opt.output == "-") {
    stdout_stream.begin();
    output = &stdout_stream;
  } else if (endsWith(opt.output, ".wav")) {
    if (!wav.open(opt.output.c_str(), info)) {
      fprintf(stderr, "could not create %s\n", opt.output.c_str());
      return 1;
    }
    output = &wav;
  } else if (endsWith(opt.output, ".csv")) {
    if (!csv_file.open(opt.output.c_str(), "w")) {
      fprintf(stderr, "could not create %s\n", opt.output.c_str());
      return 1;
    }
    csv.begin(info);
    output = &csv;
  } else {
    fprintf(stderr, "unsupported output: %s\n", opt.output.c_str());
    return 1;
  }
  // the receiver provides the data on demand: limit the reads to real time
  Throttle throttle(*output);
  throttle.begin(info);
  Print &target = is_real_time ? *output : (Print &)throttle;

  // define the receiver
  AOOStreamUDP<WiFiUDP> udp(opt.port > 0 ? opt.port : 9999);
  AOOReceiver plain(opt.id, udp);
  AOOReceiverTask with_task(opt.id, udp);
  AOOReceiver &receiver = opt.task ? (AOOReceiver &)with_task : plain;
  receiver.addDecoder("opus", createOpusDecoder);
  auto cfg = receiver.defaultConfig();
  cfg.copyFrom(info);
  cfg.buffer_depth = opt.buffer_depth;
  cfg.recovery_max_requests = opt.resend;
  cfg.adaptive_resampling = opt.adaptive;
  if (!receiver.begin(cfg)) {
    fprintf(stderr, "could not start the receiver\n");
    return 1;
  }
  fprintf(stderr,
          "receiving on port %d with sink id %d to %s (%d Hz, %d channels)\n",
          opt.port > 0 ? opt.port : 9999, opt.id, opt.output.c_str(),
          info.sample_rate, info.channels);

  StreamCopy copier(target, receiver);
  uint64_t audio_bytes = 0;
  uint32_t start_ms = millis(), next_report = start_ms + 1000;
  int frame_bytes = info.channels * 2;
  while (isRunning(opt, start_ms)) {
    size_t copied = copier.copy();
    audio_bytes += copied;
    if (copied == 0) delay(1);
    if (opt.verbose && millis() >= next_report) {
      next_report += 1000;
      printReceiveStats(receiver,
                        (double)audio_bytes / frame_bytes / info.sample_rate,
                        (millis() - start_ms) / 1000.0);
    }
  }
  printReceiveStats(receiver,
                    (double)audio_bytes / frame_bytes / info.sample_rate,
                    (millis() - start_ms) / 1000.0);
  if (opt.task) with_task.end();
  wav.close();
  return 0;
}

/// called by main() in main.cpp: the Arduino headers declare a main()
/// without arguments
int aoo_main(int argc, char **argv) {
  Options opt;
  if (!parseOptions(argc, argv, opt)) {
    usage();
    return 2;
  }
  AudioToolsLogger.begin(err_out, opt.log_level);
  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);
  int rc = opt.command == "send" ? runSend(opt) : runReceive(opt);
  fflush(stdout);
  fflush(stderr);
  // skip the destruction of the static objects of the emulator
  _exit(rc);
}
