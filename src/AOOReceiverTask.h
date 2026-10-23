#pragma once
#include "AOOReceiver.h"
// Generic dispatcher: picks RTOS.h (ESP32), Desktop.h (USE_STD_CONCURRENCY,
// e.g. desktop/CMake builds with IS_DESKTOP), Zephyr.h, RP2040.h or STM32.h
// as appropriate, instead of hardcoding the ESP32/FreeRTOS headers.
#include "AudioTools/Concurrency/Concurrency.h"

namespace arduino_aoo {

/**
 * @brief AOOReceiver that processes UDP messages in a separate FreeRTOS task.
 *
 * On ESP32's dual-core architecture this eliminates packet loss by running
 * message reception on one core while audio decoding/output runs on the other.
 *
 * AOOReceiver itself has no concurrency and stays lock-free; all
 * synchronization needed for that split lives here, since this is the only
 * class where two threads actually touch shared state:
 *  - `mutex` guards each source's encoded buffer (written by the receive
 *    task, read by the audio-pulling thread).
 *  - `sources_mutex` (recursive, since AOOReceiver's sources-touching
 *    methods call into each other, e.g. onStart() calls setAudioInfo())
 *    guards the sources list and mixer: the receive task mutates them via
 *    processMessages() (new/stopped streams) while the audio-pulling thread
 *    reads them in readBytes()/available() and mutates them in
 *    postProcessing() (stale/poor-quality source removal). Every override
 *    below that touches sources/mixer takes this lock itself, so it stays
 *    correct even if called through a base AOOReceiver&/AOOReceiver*.
 *
 * @ingroup aoo
 * @author Phil Schatzmann
 * @copyright GPLv3
 */
class AOOReceiverTask : public AOOReceiver {
 public:
  /// Default constructor
  AOOReceiverTask() : AOOReceiver() {}

  /// @param id unique sink identifier used in AOO addressing
  /// @param io transport stream for receiving/sending OSC messages
  AOOReceiverTask(int id, AOOStream &io) : AOOReceiver(id, io) {}

  /// Destructor; stops the task and releases resources
  ~AOOReceiverTask() { end(); }

  /// Configure the receive task parameters
  void setTaskConfig(int stackSize = 4096, int priority = 1, int core = 0) {
    task_stack_size = stackSize;
    task_priority = priority;
    task_core = core;
  }

  /// Starts the processing with full configuration and launches the receive task
  bool begin(AOOReceiverConfig cfg) {
    aoo_cfg = cfg;
    return begin();
  }

  /// Starts processing and launches the receive task
  bool begin() {
    if (!AOOReceiver::begin()) return false;
    // Set mutex on all existing sources
    setMutexOnSources();
    task.create("aoo_recv", task_stack_size, task_priority, task_core);
    task.begin([this]() { taskLoop(); });
    return true;
  }

  /// Stops the receive task and releases all resources
  void end() {
    // Stop the task first: once it's joined, nothing else touches sources
    // concurrently, so AOOReceiver::end() doesn't need the lock here.
    task.end();
    task.remove();
    AOOReceiver::end();
  }

  /// readBytes does NOT process messages — the task handles that
  size_t readBytes(uint8_t *data, size_t len) override {
    if (!is_active) return 0;
    postProcessing();
    LockGuard lock(sources_mutex);
    if (sources.empty()) return 0;
    if (sources.size() == 1) {
      return sources[0]->readBytes(data, len);
    }
    return mixer.readBytes(data, len);
  }

  /// available does NOT process messages — the task handles that. The post
  /// processing adds primed sources to the mixer, so it is done here as well.
  int available() override {
    if (!is_active) return 0;
    postProcessing();
    LockGuard lock(sources_mutex);
    if (sources.empty()) return 0;
    if (sources.size() == 1) {
      return sources[0]->available();
    }
    return mixer.available();
  }

  /// Defines the output audio info; locked since this can be called
  /// directly by the audio framework (e.g. on a format change) from the
  /// audio thread, concurrently with the receive task's own internal call
  /// via onStart().
  void setAudioInfo(AudioInfo info) override {
    LockGuard lock(sources_mutex);
    AOOReceiver::setAudioInfo(info);
  }

  /// Returns the total number of buffer underruns across all sources
  uint32_t xrunCount() override {
    LockGuard lock(sources_mutex);
    return AOOReceiver::xrunCount();
  }

  /// Resets the xrun counters for all sources
  void resetXrunCount() override {
    LockGuard lock(sources_mutex);
    AOOReceiver::resetXrunCount();
  }

 protected:
  Task task;
  Mutex mutex;                        // guards each source's encoded buffer
  MutexRecursiveRTOS sources_mutex;   // guards the sources list and mixer
  int task_stack_size = 4096;
  int task_priority = 1;
  int task_core = 0;

  void taskLoop() {
    processMessages();
    delay(1);
  }

  /// Propagate the mutex to all source lines and their buffer views
  void setMutexOnSources() {
    for (auto &p : sources) {
      p->setMutex(&mutex);
    }
  }

  /// Process incoming messages; locked for the whole dispatch since
  /// onStart()/onData()/onStop() mutate the sources list and mixer.
  bool processMessages() {
    LockGuard lock(sources_mutex);
    return AOOReceiver::processMessages();
  }

  /// Sends resend requests, adjusts resamplers, removes stale sources;
  /// locked since it mutates/iterates the sources list and mixer.
  void postProcessing() {
    LockGuard lock(sources_mutex);
    AOOReceiver::postProcessing();
  }

  /// Sets the mutex on every new source line, whether it was created by a
  /// /start or a /data message. No lock of its own needed: only reachable
  /// via processMessages() above, which already holds sources_mutex.
  void onSourceLineCreated(AOOSourceLine &line) override {
    line.setMutex(&mutex);
  }
};

}  // namespace arduino_aoo
