#include "stream_atomic_op.hh"

#include "stream.hh"

#include "debug/StreamAtomicOp.hh"
#define DEBUG_TYPE StreamAtomicOp
#include "stream_log.hh"

void StreamAtomicOp::operator()(uint8_t *p) {
  // Read in the final atomic operand from p to params.
  DynamicStreamParamV::value_type operand = 0;
  for (size_t i = 0; i < this->size; ++i) {
    reinterpret_cast<uint8_t *>(&operand)[i] = p[i];
  }
  this->params.back() = operand;
  auto result = func->invoke(params);

  S_DPRINTF(stream, "Entry (%lu, %lu): AtomicOp %lu -> %lu.\n",
            entryIdx.streamId.streamInstance, entryIdx.entryIdx, operand,
            result);

  for (size_t i = 0; i < this->size; ++i) {
    p[i] = reinterpret_cast<uint8_t *>(&result)[i];
  }
}