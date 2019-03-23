#ifndef __CPU_TDG_ACCELERATOR_COALESCED_STREAM_HH__
#define __CPU_TDG_ACCELERATOR_COALESCED_STREAM_HH__

#include "stream.hh"

#include "stream_history.hh"
#include "stream_pattern.hh"

// Parse the instructions from a protobuf.
#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream info."
#endif

#include "cpu/llvm_trace/accelerator/stream/StreamMessage.pb.h"

class StreamInst;

/**
 * A simple logical stream managed by the coalesced stream.
 */
class LogicalStream {
public:
  LogicalStream(const LLVM::TDG::TDGInstruction_StreamConfigExtra_SingleConfig
                    &configInst);

  LogicalStream(const LogicalStream &Other) = delete;
  LogicalStream(LogicalStream &&Other) = delete;
  LogicalStream &operator=(const LogicalStream &Other) = delete;
  LogicalStream &operator=(LogicalStream &&Other) = delete;

  ~LogicalStream();

  LLVM::TDG::StreamInfo info;
  std::unique_ptr<StreamHistory> history;
  std::unique_ptr<StreamPattern> patternStream;
};

class CoalescedStream : public Stream {
public:
  CoalescedStream(LLVMTraceCPU *_cpu, StreamEngine *_se, bool _isOracle,
                  size_t _maxRunAHeadLength, const std::string &_throttling);

  ~CoalescedStream();

  void addLogicalStreamIfNecessary(
      const LLVM::TDG::TDGInstruction_StreamConfigExtra &configInst);

  const std::string &getStreamName() const override;
  const std::string &getStreamType() const override;
  uint32_t getLoopLevel() const override;
  uint32_t getConfigLoopLevel() const override;
  int32_t getElementSize() const override;

  void configure(StreamConfigInst *inst) override;
  void commitConfigure(StreamConfigInst *inst) override;
  void step(StreamStepInst *inst) override;
  void commitStep(StreamStepInst *inst) override;
  void store(StreamStoreInst *inst) override;
  void commitStore(StreamStoreInst *inst) override;
  void end(StreamEndInst *inst) override;
  void commitEnd(StreamEndInst *inst) override;

  /**
   * Get the number of unique cache blocks the stream touches.
   * Used for stream aware cache to determine if it should cache the stream.
   */
  uint64_t getFootprint(unsigned cacheBlockSize) const override;
  uint64_t getTrueFootprint() const override;

  bool isContinuous() const override;

protected:
  std::unordered_map<uint64_t, LogicalStream> logicalStreamMap;

  /**
   * Utility primary logical stream to represent this coalesced stream.
   */
  LogicalStream *primaryLogicalStream;

  bool shouldHandleStreamInst(StreamInst *inst) const;

  void enqueueFIFO() override;
  void markAddressReady(FIFOEntry &entry) override;
  void markValueReady(FIFOEntry &entry) override;

  void handlePacketResponse(const FIFOEntryIdx &entryId, PacketPtr packet,
                            StreamMemAccess *memAccess) override;

  /**
   * Merge the request from different logical streams.
   */
  // std::pair<uint64_t, uint64_t> getNextAddr();

  /**
   * For debug.
   */
  void dump() const override;
};

#endif