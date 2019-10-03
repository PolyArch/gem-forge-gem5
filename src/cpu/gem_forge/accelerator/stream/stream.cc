#include "stream.hh"
#include "insts.hh"
#include "stream_engine.hh"

#include "cpu/gem_forge/llvm_trace_cpu_delegator.hh"

// #include "base/misc.hh""
#include "base/trace.hh"
#include "debug/StreamEngine.hh"
#include "proto/protoio.hh"

#define STREAM_DPRINTF(format, args...)                                        \
  DPRINTF(StreamEngine, "Stream %s: " format, this->getStreamName().c_str(),   \
          ##args)

#define STREAM_ENTRY_DPRINTF(entry, format, args...)                           \
  STREAM_DPRINTF("Entry (%lu, %lu): " format, (entry).idx.streamInstance,      \
                 (entry).idx.entryIdx, ##args)

#define STREAM_HACK(format, args...)                                           \
  hack("Stream %s: " format, this->getStreamName().c_str(), ##args)

#define STREAM_ENTRY_HACK(entry, format, args...)                              \
  STREAM_HACK("Entry (%lu, %lu): " format, (entry).idx.streamInstance,         \
              (entry).idx.entryIdx, ##args)

#define STREAM_PANIC(format, args...)                                          \
  {                                                                            \
    this->dump();                                                              \
    panic("Stream %s: " format, this->getStreamName().c_str(), ##args);        \
  }

#define STREAM_ENTRY_PANIC(entry, format, args...)                             \
  STREAM_PANIC("Entry (%lu, %lu): " format, (entry).idx.streamInstance,        \
               (entry).idx.entryIdx, ##args)

Stream::Stream(const StreamArguments &args)
    : FIFOIdx(DynamicStreamId(args.cpuDelegator->cpuId(), args.staticId,
                              0 /*StreamInstance*/)),
      staticId(args.staticId), streamName(args.name), cpu(args.cpu),
      cpuDelegator(args.cpuDelegator), se(args.se), nilTail(args.se) {

  this->configured = false;
  this->head = &this->nilTail;
  this->stepped = &this->nilTail;
  this->tail = &this->nilTail;
  this->allocSize = 0;
  this->stepSize = 0;
  this->maxSize = args.maxSize;
  this->stepRootStream = nullptr;
  this->lateFetchCount = 0;
  this->streamRegion = args.streamRegion;

  // The name field in the dynamic id has to be set here after we initialize
  // streamName.
  this->FIFOIdx.streamId.streamName = this->streamName.c_str();
}

Stream::~Stream() {}

void Stream::dumpStreamStats(std::ostream &os) const {
  os << this->getStreamName() << '\n';
  this->statistic.dump(os);
}

bool Stream::isMemStream() const {
  return this->getStreamType() == "load" || this->getStreamType() == "store";
}

void Stream::addBaseStream(Stream *baseStream) {
  if (baseStream == this) {
    STREAM_PANIC("Base stream should not be self.");
  }
  this->baseStreams.insert(baseStream);
  baseStream->dependentStreams.insert(this);
}

void Stream::addBackBaseStream(Stream *backBaseStream) {
  if (backBaseStream == this) {
    STREAM_PANIC("Base stream should not be self.");
  }
  this->backBaseStreams.insert(backBaseStream);
  backBaseStream->backDependentStreams.insert(this);
}

void Stream::addBaseStepStream(Stream *baseStepStream) {
  if (baseStepStream == this) {
    STREAM_PANIC("Base stream should not be self.");
  }
  this->baseStepStreams.insert(baseStepStream);
  baseStepStream->dependentStepStreams.insert(this);
  if (baseStepStream->isStepRoot()) {
    this->baseStepRootStreams.insert(baseStepStream);
  } else {
    for (auto stepRoot : baseStepStream->baseStepRootStreams) {
      this->baseStepRootStreams.insert(stepRoot);
    }
  }
}

void Stream::registerStepDependentStreamToRoot(Stream *newStepDependentStream) {
  if (!this->isStepRoot()) {
    STREAM_PANIC("Try to register step instruction to non-root stream.");
  }
  for (auto &stepStream : this->stepStreamList) {
    if (stepStream == newStepDependentStream) {
      STREAM_PANIC(
          "The new step dependent stream has already been registered.");
    }
  }
  this->stepStreamList.emplace_back(newStepDependentStream);
}

void Stream::dispatchStreamConfig(uint64_t seqNum, ThreadContext *tc) {
  // Create new index.
  this->FIFOIdx.newInstance(seqNum);
  // Allocate the new DynamicStream.
  this->dynamicStreams.emplace_back(this->FIFOIdx.streamId, seqNum, tc);
}

void Stream::executeStreamConfig(uint64_t seqNum,
                                 const std::vector<uint64_t> *inputVec) {
  auto &dynStream = this->getDynamicStream(seqNum);
  assert(!dynStream.configExecuted && "StreamConfig already executed.");
  dynStream.configExecuted = true;
  this->setupAddrGen(dynStream, inputVec);
}

bool Stream::isStreamConfigureExecuted(uint64_t seqNum) {
  auto &dynStream = this->getDynamicStream(seqNum);
  return dynStream.configExecuted;
}

void Stream::commitStreamEnd(uint64_t seqNum) {
  assert(!this->dynamicStreams.empty() &&
         "Empty dynamicStreams for StreamEnd.");
  auto &endedDynamicStream = this->dynamicStreams.front();
  assert(endedDynamicStream.configSeqNum < seqNum && "End before config.");
  assert(endedDynamicStream.configExecuted && "End before config executed.");
  this->dynamicStreams.pop_front();
  if (!this->dynamicStreams.empty()) {
    // There is another config inst waiting.
    assert(this->dynamicStreams.front().configSeqNum > seqNum &&
           "Next StreamConfig not younger than previous StreamEnd.");
  }
}

DynamicStream &Stream::getDynamicStream(uint64_t seqNum) {
  for (auto &dynStream : this->dynamicStreams) {
    if (dynStream.configSeqNum == seqNum) {
      return dynStream;
    }
  }
  panic("Failed to find DynamicStream %llu.\n", seqNum);
}