#pragma once
#include "AOOReceiver.h"
#include "AudioTools/Concurrency/RTOS/Task.h"
#include "AudioTools/Concurrency/RTOS/MutexRTOS.h"

namespace arduino_aoo {

/**
 * @brief AOOReceiver that processes UDP messages in a separate FreeRTOS task.
 *
 * On ESP32's dual-core architecture this eliminates packet loss by running
 * message reception on one core while audio decoding/output runs on the other.
 *
 * Synchronization is fine-grained: a mutex protects only the individual
 * buffer write (storeEncodedData) and read (IndexedRingBufferStreamView)
 * operations, not the entire processMessages() or readBytes() calls.
 *
 * @ingroup aoo
 * @author Phil Schatzmann
 * @copyright GPLv3
 */
class AOOReceiverTask : public AOOReceiver {
 public:
  AOOReceiverTask() : AOOReceiver() {}

  AOOReceiverTask(int id, AOOStream &io) : AOOReceiver(id, io) {}

  ~AOOReceiverTask() { end(); }

  /// Configure the receive task parameters
  void setTaskConfig(int stackSize = 4096, int priority = 1, int core = 0) {
    task_stack_size = stackSize;
    task_priority = priority;
    task_core = core;
  }

  bool begin(AOOReceiverConfig cfg) {
    aoo_cfg = cfg;
    return begin();
  }

  bool begin() {
    if (!AOOReceiver::begin()) return false;
    // Set mutex on all existing sources
    setMutexOnSources();
    task.create("aoo_recv", task_stack_size, task_priority, task_core);
    task.begin([this]() { taskLoop(); });
    return true;
  }

  void end() {
    task.end();
    task.remove();
    AOOReceiver::end();
  }

  /// readBytes does NOT process messages — the task handles that
  size_t readBytes(uint8_t *data, size_t len) override {
    if (!is_active || sources.empty()) return 0;
    postProcessing();
    if (sources.size() == 1) {
      return sources[0]->readBytes(data, len);
    }
    return mixer.readBytes(data, len);
  }

  /// available does NOT process messages — the task handles that
  int available() override {
    if (!is_active) return 0;
    if (sources.size() == 1) {
      return sources[0]->available();
    }
    return mixer.available();
  }

 protected:
  Task task;
  Mutex mutex;
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

  /// Override to set mutex on newly created sources
  bool onStart(OSCData &osc) override {
    bool result = AOOReceiver::onStart(osc);
    if (result) setMutexOnSources();
    return result;
  }
};

}  // namespace arduino_aoo
