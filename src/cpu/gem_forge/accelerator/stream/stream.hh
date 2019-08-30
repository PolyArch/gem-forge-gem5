#ifndef __CPU_TDG_ACCELERATOR_STREAM_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_HH__

#include "cache/CacheStreamConfigureData.hh"
#include "cpu/gem_forge/llvm_insts.hh"
#include "stream_element.hh"
#include "stream_statistic.hh"

#include "base/types.hh"
#include "mem/packet.hh"

// Parse the instructions from a protobuf.
#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream info."
#endif

#include "StreamMessage.pb.h"

#include <list>

class LLVMTraceCPU;

class StreamEngine;
class StreamConfigInst;
class StreamEndInst;

class Stream {
public:
  struct StreamArguments {
    LLVMTraceCPU *cpu;
    StreamEngine *se;
    int maxSize;
    const ::LLVM::TDG::StreamRegion *streamRegion;
  };

  Stream(const StreamArguments &args);

  virtual ~Stream();

  virtual const std::string &getStreamName() const = 0;
  virtual const std::string &getStreamType() const = 0;
  bool isMemStream() const;
  virtual uint32_t getLoopLevel() const = 0;
  virtual uint32_t getConfigLoopLevel() const = 0;
  virtual int32_t getElementSize() const = 0;

  virtual void prepareNewElement(StreamElement *element) = 0;

  /**
   * Simple bookkeeping information for the stream engine.
   */
  bool configured;
  StreamElement *head;
  StreamElement *stepped;
  StreamElement *tail;
  size_t allocSize;
  size_t stepSize;
  size_t maxSize;
  FIFOEntryIdx FIFOIdx;
  int lateFetchCount;

  const ::LLVM::TDG::StreamRegion *streamRegion;

  /**
   * Step root stream, three possible cases:
   * 1. this: I am the step root.
   * 2. other: I am controlled by other step stream.
   * 3. nullptr: I am a constant stream.
   */
  Stream *stepRootStream;
  std::unordered_set<Stream *> baseStreams;
  std::unordered_set<Stream *> dependentStreams;
  /**
   * Back edge dependence on previous iteration.
   */
  std::unordered_set<Stream *> backBaseStreams;

  /**
   * Per stream statistics.
   */
  StreamStatistic statistic;
  void dumpStreamStats(std::ostream &os) const;

  void tick();

  void addBaseStream(Stream *baseStream);
  void addBaseStepStream(Stream *baseStepStream);
  virtual void initializeBackBaseStreams() = 0;
  void addBackBaseStream(Stream *backBaseStream);
  void registerStepDependentStreamToRoot(Stream *newDependentStream);

  virtual uint64_t getTrueFootprint() const = 0;
  virtual uint64_t getFootprint(unsigned cacheBlockSize) const = 0;
  virtual bool isContinuous() const = 0;

  LLVMTraceCPU *getCPU() { return this->cpu; }

  virtual void configure(StreamConfigInst *inst) = 0;

  void dispatchStreamConfigure(StreamConfigInst *inst);
  void executeStreamConfigure(StreamConfigInst *inst);
  bool isStreamConfigureExecuted(uint64_t configInstSeqNum);
  void commitStreamEnd(StreamEndInst *inst);

  /**
   * ! Sean: StreamAwareCache
   * Allocate the CacheStreamConfigureData.
   */
  virtual CacheStreamConfigureData *
  allocateCacheConfigureData(uint64_t configSeqNum) = 0;

  /**
   * Helper function used in StreamAwareCache.
   */
  virtual bool isDirectLoadStream() const { return false; }
  virtual bool isPointerChaseLoadStream() const { return false; }

  /**
   * Helper structure to remember status of dynamic instance of this stream.
   * Mainly for StreamAwareCache.
   */
  struct DynamicInstanceState {
    DynamicStreamId dynamicStreamId;
    uint64_t configSeqNum;
    bool offloadedToCache;
    DynamicInstanceState(const DynamicStreamId &_dynamicStreamId,
                         uint64_t _configSeqNum)
        : dynamicStreamId(_dynamicStreamId), configSeqNum(_configSeqNum),
          offloadedToCache(false) {}
  };

  std::deque<DynamicInstanceState> dynamicInstanceStates;

protected:
  LLVMTraceCPU *cpu;
  StreamEngine *se;

  std::unordered_set<Stream *> baseStepStreams;
  std::unordered_set<Stream *> baseStepRootStreams;
  std::unordered_set<Stream *> dependentStepStreams;

  StreamElement nilTail;

  /**
   * Step the dependent streams in this order.
   */
  std::list<Stream *> stepStreamList;

  bool isStepRoot() const {
    const auto &type = this->getStreamType();
    return this->baseStepStreams.empty() && (type == "phi" || type == "store");
  }

  /**
   * Manager stream_configuration executed status.
   * A list of all stream_config instruction sequence number, along with a
   * flag to remember whether this instruction is executed.
   */
  std::list<std::pair<uint64_t, bool>> configInstExecuted;

  /**
   * For debug.
   */
  virtual void dump() const = 0;
};

#endif