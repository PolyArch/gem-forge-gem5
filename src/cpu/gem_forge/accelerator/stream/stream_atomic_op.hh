#ifndef __GEM_FORGE_ACCELERATOR_STREAM_ATOMIC_OP_HH__
#define __GEM_FORGE_ACCELERATOR_STREAM_ATOMIC_OP_HH__

#include "addr_gen_callback.hh"
#include "fifo_entry_idx.hh"

class Stream;
class StreamAtomicOp : public AtomicOpFunctor {
public:
  StreamAtomicOp(Stream *_stream, const FIFOEntryIdx &_entryIdx, uint8_t _size,
                 const DynamicStreamParamV &_params, const ExecFuncPtr &_func)
      : stream(_stream), entryIdx(_entryIdx), size(_size), params(_params),
        func(_func) {
    assert(!this->params.empty() && "Should at least have one atomic operand.");
    assert(this->size <= sizeof(DynamicStreamParamV::value_type) &&
           "Illegal size.");
  }

  void operator()(uint8_t *p) override;

  AtomicOpFunctor *clone() override {
    return new StreamAtomicOp(stream, entryIdx, size, params, func);
  }

private:
  Stream *stream;
  const FIFOEntryIdx entryIdx;
  // Size of the final atomic operand.
  const uint8_t size;
  DynamicStreamParamV params;
  ExecFuncPtr func;
};

#endif