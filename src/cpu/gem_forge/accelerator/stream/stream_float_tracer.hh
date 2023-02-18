#ifndef __CPU_TDG_ACCELERATOR_STREAM_FLOAT_TRACER_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_FLOAT_TRACER_HH__

#include "cpu/gem_forge/accelerator/stream/StreamMessage.pb.h"
#include "proto/protoio.hh"
#include "mem/ruby/common/MachineID.hh"

#include <memory>
#include <vector>

namespace gem5 {

/**
 * Trace floating stream event in LLC.
 */

class Stream;
class StreamFloatTracer {
public:
  StreamFloatTracer(Stream *_S) : S(_S) {}

  void traceEvent(
      uint64_t cycle, ruby::MachineID machineId,
      const ::LLVM::TDG::StreamFloatEvent::StreamFloatEventType &type) const;

  void dump() const {
    if (this->protoStream) {
      this->write();
      // Simply reset.
      this->protoStream = nullptr;
    }
  }

private:
  void initialize() const;
  void write() const;

  Stream *S;
  static constexpr int DUMP_THRESHOLD = 1024;
  mutable std::unique_ptr<ProtoOutputStream> protoStream = nullptr;

  mutable std::vector<::LLVM::TDG::StreamFloatEvent> buffer;
  mutable int used = 0;
};

} // namespace gem5

#endif