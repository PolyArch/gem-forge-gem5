#ifndef __CPU_TDG_ACCELERATOR_COALESCED_STREAM_HH__
#define __CPU_TDG_ACCELERATOR_COALESCED_STREAM_HH__

#include "stream.hh"

#include "stream_history.hh"
#include "stream_pattern.hh"

class StreamInst;

/**
 * A simple logical stream managed by the coalesced stream.
 */
class LogicalStream {
public:
  LogicalStream(const std::string &_traceExtraFolder,
                const LLVM::TDG::StreamInfo &_info);

  LogicalStream(const LogicalStream &Other) = delete;
  LogicalStream(LogicalStream &&Other) = delete;
  LogicalStream &operator=(const LogicalStream &Other) = delete;
  LogicalStream &operator=(LogicalStream &&Other) = delete;

  ~LogicalStream();

  int32_t getCoalesceOffset() const {
    return this->info.coalesce_info().offset();
  }
  int32_t getElementSize() const { return this->info.element_size(); }
  uint64_t getStreamId() const { return this->info.id(); }

  LLVM::TDG::StreamInfo info;
  std::unique_ptr<StreamHistory> history;
  std::unique_ptr<StreamPattern> patternStream;
};

class CoalescedStream : public Stream {
public:
  CoalescedStream(const StreamArguments &args, bool _staticCoalesced);

  ~CoalescedStream();

  void addStreamInfo(const LLVM::TDG::StreamInfo &info);
  /**
   * User must call finalize after all stream infos are added.
   */
  void finalize();
  void prepareNewElement(StreamElement *element) override;
  void initializeBackBaseStreams() override;

  /**
   * Only to configure all the history.
   */
  void configure(uint64_t seqNum, ThreadContext *tc) override;

  const std::string &getStreamType() const override;
  uint32_t getLoopLevel() const override;
  uint32_t getConfigLoopLevel() const override;
  int32_t getElementSize() const override {
    assert(this->coalescedElementSize > 0 && "Invalid element size.");
    return this->coalescedElementSize;
  }

  /**
   * Get the number of unique cache blocks the stream touches.
   * Used for stream aware cache to determine if it should cache the stream.
   */
  uint64_t getFootprint(unsigned cacheBlockSize) const override;
  uint64_t getTrueFootprint() const override;

  bool isContinuous() const override;

  void setupAddrGen(DynamicStream &dynStream,
                    const std::vector<uint64_t> *inputVec) override;

  /**
   * ! Sean: StreamAwareCache
   * Allocate the CacheStreamConfigureData.
   */
  CacheStreamConfigureData *
  allocateCacheConfigureData(uint64_t configSeqNum) override;

  uint64_t getStreamLengthAtInstance(uint64_t streamInstance) const override;

  void getCoalescedOffsetAndSize(uint64_t streamId, int32_t &offset,
                                 int32_t &size) const;

protected:
  /**
   * Represented all the streams coalesced within this one.
   * The first one is "prime stream", whose stream id is used to represent
   * this coalesced stream.
   * In statically coalesced streams, this is the base stream with offset 0.
   */
  bool staticCoalesced;
  std::vector<LogicalStream *> coalescedStreams;
  LogicalStream *primeLStream;
  int32_t coalescedElementSize = -1;

  /**
   * For debug.
   */
  void dump() const override;
};

#endif