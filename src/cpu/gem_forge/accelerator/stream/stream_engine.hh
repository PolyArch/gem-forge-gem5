#ifndef __CPU_GEM_FORGE_ACCELERATOR_STREAM_ENGINE_H__
#define __CPU_GEM_FORGE_ACCELERATOR_STREAM_ENGINE_H__

#include "insts.hh"
#include "prefetch_element_buffer.hh"
#include "stream.hh"
#include "stream_element.hh"
#include "stream_translation_buffer.hh"

#include "stream_float_policy.hh"
#include "stream_placement_manager.hh"

#include "base/statistics.hh"
#include "cpu/gem_forge/accelerator/gem_forge_accelerator.hh"
#include "cpu/gem_forge/lsq.hh"

#include "params/StreamEngine.hh"

#include <unordered_map>

namespace gem5 {

class StreamThrottler;
class StreamLQCallback;
class StreamSQCallback;
class StreamSQDeprecatedCallback;
class StreamNDCController;
class StreamFloatController;
class StreamComputeEngine;
class StreamRegionController;
class StreamRangeSyncController;
class StreamDataTrafficAccumulator;

class StreamEngine : public GemForgeAccelerator {
public:
  PARAMS(StreamEngine);
  StreamEngine(const Params &params);
  ~StreamEngine() override;

  void handshake(GemForgeCPUDelegator *_cpuDelegator,
                 GemForgeAcceleratorManager *_manager) override;
  void takeOverBy(GemForgeCPUDelegator *newCpuDelegator,
                  GemForgeAcceleratorManager *newManager) override;

  void tick() override;
  void dump() override;
  bool isAccelerating() override;
  bool checkProgress() override;
  void regStats() override;
  void resetStats() override;

  // Override the name as we don't want the default long name().
  std::string name() const override { return ""; }

  /**
   * To prepare for execution-driven simulation,
   * decouple StreamEngine from StreamInstruction, but
   * use the inst sequence number and protobuf field
   * as the arguments.
   */
  constexpr static InstSeqNum InvalidInstSeqNum = DynStream::InvalidInstSeqNum;
  struct StreamConfigArgs {
    using InputVec = DynStreamParamV;
    using InputMap = std::unordered_map<uint64_t, InputVec>;
    InstSeqNum seqNum; // Just the instruction sequence number.
    const std::string &infoRelativePath; // Where to find the info.
    const InputMap *inputMap;            // Live input of streams.
    // Only valid at dispatchStreamConfig for execution simulation.
    ThreadContext *const tc;
    /**
     * Optional information of OuterSE and DynRegion to track InnerLoopDep.
     */
    StreamEngine *outerSE = nullptr;
    InstSeqNum outerSeqNum = InvalidInstSeqNum;
    StreamConfigArgs(uint64_t _seqNum, const std::string &_infoRelativePath,
                     InputMap *_inputMap = nullptr,
                     ThreadContext *_tc = nullptr,
                     StreamEngine *_outerSE = nullptr,
                     InstSeqNum _outerSeqNum = InvalidInstSeqNum)
        : seqNum(_seqNum), infoRelativePath(_infoRelativePath),
          inputMap(_inputMap), tc(_tc), outerSE(_outerSE),
          outerSeqNum(_outerSeqNum) {}
  };

  bool canStreamConfig(const StreamConfigArgs &args) const;
  void dispatchStreamConfig(const StreamConfigArgs &args);
  void executeStreamConfig(const StreamConfigArgs &args);
  void commitStreamConfig(const StreamConfigArgs &args);
  void rewindStreamConfig(const StreamConfigArgs &args);

  struct StreamStepArgs {
    const uint64_t stepStreamId;
    DynStreamId::InstanceId dynInstanceId = DynStreamId::InvalidInstanceId;
    StreamStepArgs(uint64_t _stepStreamId) : stepStreamId(_stepStreamId) {}
  };

  bool canDispatchStreamStep(const StreamStepArgs &args) const;
  void dispatchStreamStep(const StreamStepArgs &args);
  bool canCommitStreamStep(const StreamStepArgs &args);
  void commitStreamStep(const StreamStepArgs &args);
  void rewindStreamStep(const StreamStepArgs &args);

  struct StreamUserArgs {
    static constexpr int MaxElementSize = 64;
    using Value = std::array<uint8_t, MaxElementSize>;
    using ValueVec = std::vector<Value>;
    uint64_t seqNum;
    Addr pc;
    const std::vector<uint64_t> &usedStreamIds;
    // Is this a StreamStore (to distinguish for UpdateStream).
    bool isStore = false;
    // Used to return the stream values.
    // Only used in executeStreamUser().
    ValueVec *values;
    StreamUserArgs(uint64_t _seqNum, Addr _pc,
                   const std::vector<uint64_t> &_usedStreamIds,
                   bool _isStore = false, ValueVec *_values = nullptr)
        : seqNum(_seqNum), pc(_pc), usedStreamIds(_usedStreamIds),
          isStore(_isStore), values(_values) {}
  };

  int getStreamUserLQEntries(const StreamUserArgs &args) const;
  /**
   * Create LSQ callbacks for first stream user.
   * @return The number of LSQ callbacks created.
   */
  int createStreamUserLSQCallbacks(const StreamUserArgs &args,
                                   GemForgeLSQCallbackList &callbacks);

  bool hasUnsteppedElement(const StreamUserArgs &args);
  bool hasIllegalUsedLastElement(const StreamUserArgs &args);
  bool canDispatchStreamUser(const StreamUserArgs &args);
  void dispatchStreamUser(const StreamUserArgs &args);
  bool areUsedStreamsReady(const StreamUserArgs &args);
  void executeStreamUser(const StreamUserArgs &args);
  void commitStreamUser(const StreamUserArgs &args);
  void rewindStreamUser(const StreamUserArgs &args);

  struct StreamEndArgs {
    uint64_t seqNum;
    const std::string &infoRelativePath;
    StreamEndArgs(uint64_t _seqNum, const std::string &_infoRelativePath)
        : seqNum(_seqNum), infoRelativePath(_infoRelativePath) {}
  };
  bool canDispatchStreamEnd(const StreamEndArgs &args);
  void dispatchStreamEnd(const StreamEndArgs &args);
  bool canExecuteStreamEnd(const StreamEndArgs &args);
  bool canCommitStreamEnd(const StreamEndArgs &args);
  void commitStreamEnd(const StreamEndArgs &args);
  void rewindStreamEnd(const StreamEndArgs &args);

  bool canStreamStoreDispatch(const StreamStoreInst *inst) const;
  std::list<std::unique_ptr<GemForgeSQDeprecatedCallback>>
  createStreamStoreSQCallbacks(StreamStoreInst *inst);
  void dispatchStreamStore(StreamStoreInst *inst);
  void executeStreamStore(StreamStoreInst *inst);
  void commitStreamStore(StreamStoreInst *inst);

  /*************************************************************************
   * Tough part: handling misspeculation.
   ************************************************************************/
  void cpuStoreTo(InstSeqNum seqNum, Addr vaddr, int size);
  void addPendingWritebackElement(StreamElement *releaseElement);

  Stream *getStream(uint64_t streamId) const;
  Stream *getStream(const std::string &streamName) const;
  Stream *tryGetStream(uint64_t streamId) const;

  /**
   * Send StreamEnd packet for all the ended dynamic stream ids.
   */
  void sendStreamFloatEndPacket(const std::vector<DynStreamId> &endedIds);

  /**
   * Send atomic operation packet.
   */
  void sendAtomicPacket(StreamElement *element, AtomicOpFunctorPtr atomicOp);

  StreamPlacementManager *getStreamPlacementManager() {
    return this->streamPlacementManager;
  }

  /**
   * Used by StreamPlacementManager to get all cache.
   */
  std::vector<SimObject *> &getSimObjectList() {
    return SimObject::getSimObjectList();
  }

  bool isTraceSim() const {
    assert(cpuDelegator && "Missing cpuDelegator.");
    return cpuDelegator->cpuType == GemForgeCPUDelegator::CPUTypeE::LLVM_TRACE;
  }

  bool isMergeEnabled() const { return this->enableMerge; }
  bool isOracleEnabled() const { return this->isOracle; }

  bool isPlacementEnabled() const { return this->enableStreamPlacement; }
  bool isPlacementBusEnabled() const { return this->enableStreamPlacementBus; }
  bool isPlacementNoBypassingStore() const { return this->noBypassingStore; }
  bool isContinuousStoreOptimized() const { return this->continuousStore; }
  bool isPlacementPeriodReset() const {
    return this->enablePlacementPeriodReset;
  }
  bool isOraclePlacementEnabled() const {
    return this->enableStreamPlacementOracle;
  }
  bool isStreamFloatCancelEnabled() const {
    return this->enableStreamFloatCancel;
  }
  bool isStreamRangeSyncEnabled() const {
    return this->myParams->enableRangeSync;
  }
  const std::string &getPlacementLat() const { return this->placementLat; }
  const std::string &getPlacement() const { return this->placement; }

  /**************************************************************************
   * Received StreamLoopBound TotalTripCount from Cache.
   **************************************************************************/
  void receiveOffloadedLoopBoundRet(const DynStreamId &dynStreamId,
                                    int64_t tripCount, bool brokenOut);

  void exitDump() const;

  void fetchedCacheBlock(Addr cacheBlockVAddr, StreamMemAccess *memAccess);
  void incrementInflyStreamRequest() { this->numInflyStreamRequests++; }
  void decrementInflyStreamRequest() {
    assert(this->numInflyStreamRequests > 0);
    this->numInflyStreamRequests--;
  }

  int currentTotalRunAheadLength;
  int totalRunAheadLength;
  int totalRunAheadBytes;

  /**
   * Stats
   */
  mutable statistics::Scalar numConfigured;
  mutable statistics::Scalar numStepped;
  mutable statistics::Scalar numUnstepped;
  mutable statistics::Scalar numElementsAllocated;
  mutable statistics::Scalar numElementsUsed;
  mutable statistics::Scalar numCommittedStreamUser;
  mutable statistics::Scalar entryWaitCycles;

  mutable statistics::Scalar numStoreElementsAllocated;
  mutable statistics::Scalar numStoreElementsStepped;
  mutable statistics::Scalar numStoreElementsUsed;

  mutable statistics::Scalar numLoadElementsAllocated;
  mutable statistics::Scalar numLoadElementsFetched;
  mutable statistics::Scalar numLoadElementsStepped;
  mutable statistics::Scalar numLoadElementsUsed;
  mutable statistics::Scalar numLoadElementWaitCycles;
  mutable statistics::Scalar numLoadCacheLineFetched;
  mutable statistics::Scalar numLoadCacheLineUsed;
  /**
   * * How many times a StreamUser/StreamStore not dispatched due to LSQ full.
   */
  mutable statistics::Scalar streamUserNotDispatchedByLoadQueue;
  mutable statistics::Scalar streamStoreNotDispatchedByStoreQueue;

  Stats::Distribution numTotalAliveElements;
  Stats::Distribution numTotalAliveCacheBlocks;
  Stats::Distribution numRunAHeadLengthDist;
  Stats::Distribution numTotalAliveMemStreams;
  Stats::Distribution numInflyStreamRequestDist;

  /**
   * Statistics for stream placement manager.
   */
  statistics::Vector numAccessPlacedInCacheLevel;
  statistics::Vector numAccessHitHigherThanPlacedCacheLevel;
  statistics::Vector numAccessHitLowerThanPlacedCacheLevel;

  Stats::Distribution numAccessFootprintL1;
  Stats::Distribution numAccessFootprintL2;
  Stats::Distribution numAccessFootprintL3;

  /**
   * Statistics for stream float.
   */
  statistics::Scalar numFloated;
  statistics::Scalar numLLCSentSlice;
  statistics::Scalar numLLCMigrated;
  statistics::Scalar numMLCResponse;

  /**
   * Statistics for stream computing.
   * Computation is classified as cross product of:
   *      Affine Indirect PointerChase Multi-Affine
   * x    Load   Store    Atomic       Update       Reduce
   */
  statistics::Scalar numScheduledComputation;
  statistics::Scalar numCompletedComputation;
  statistics::Scalar numCompletedComputeMicroOps;
  statistics::Scalar numCompletedAffineLoadComputeMicroOps;
  statistics::Scalar numCompletedAffineReduceMicroOps;
  statistics::Scalar numCompletedAffineUpdateMicroOps;
  statistics::Scalar numCompletedAffineStoreComputeMicroOps;
  statistics::Scalar numCompletedAffineAtomicComputeMicroOps;
  statistics::Scalar numCompletedIndirectLoadComputeMicroOps;
  statistics::Scalar numCompletedIndirectReduceMicroOps;
  statistics::Scalar numCompletedIndirectUpdateMicroOps;
  statistics::Scalar numCompletedIndirectStoreComputeMicroOps;
  statistics::Scalar numCompletedIndirectAtomicComputeMicroOps;
  statistics::Scalar numCompletedPointerChaseLoadComputeMicroOps;
  statistics::Scalar numCompletedPointerChaseReduceMicroOps;
  statistics::Scalar numCompletedPointerChaseUpdateMicroOps;
  statistics::Scalar numCompletedPointerChaseStoreComputeMicroOps;
  statistics::Scalar numCompletedPointerChaseAtomicComputeMicroOps;
  statistics::Scalar numCompletedMultiAffineLoadComputeMicroOps;
  statistics::Scalar numCompletedMultiAffineReduceMicroOps;
  statistics::Scalar numCompletedMultiAffineUpdateMicroOps;
  statistics::Scalar numCompletedMultiAffineStoreComputeMicroOps;
  statistics::Scalar numCompletedMultiAffineAtomicComputeMicroOps;

  /**
   * Number of committed elements since last check,
   * including those committed offloaded elements.
   * Used to check progress.
   */
  uint64_t numSteppedSinceLastCheck = 0;
  uint64_t numOffloadedSteppedSinceLastCheck = 0;

private:
  friend class DynStream;
  friend class Stream;
  friend class StreamElement;
  friend class StreamThrottler;
  friend class StreamLQCallback;
  friend class StreamSQCallback;
  friend class StreamSQDeprecatedCallback;
  friend class StreamNDCController;
  friend class StreamFloatController;
  friend class StreamComputeEngine;
  friend class StreamRegionController;
  friend class StreamRangeSyncController;
  friend class StreamDataTrafficAccumulator;

  // A global array of all SEs. Used to implement RemoteConfig.
  static std::vector<StreamEngine *> GlobalStreamEngines;
  static StreamEngine *
  getStreamEngineAtCPU(GemForgeCPUDelegator::CPUTypeE cpuType, int cpuId);

  LLVMTraceCPU *cpu;

  std::unique_ptr<StreamTranslationBuffer<void *>> translationBuffer;
  StreamPlacementManager *streamPlacementManager;

  std::vector<StreamElement> FIFOArray;
  StreamElement *FIFOFreeListHead;
  size_t numFreeFIFOEntries;

  /**
   * Incremented when dispatch StreamConfig and decremented when commit
   * StreamEnd.
   */
  int numInflyStreamConfigurations = 0;

  /**
   * Incremented when issue a stream request out and decremented when the
   * result comes back.
   */
  int numInflyStreamRequests = 0;

  /**
   * Map from the user instruction seqNum to all the actual element to use.
   * Update at dispatchStreamUser and commitStreamUser.
   */
  std::unordered_map<uint64_t, std::unordered_set<StreamElement *>>
      userElementMap;

  /**
   * Map from the stream element to the user instruction.
   * A reverse map of userElementMap.
   */
  std::unordered_map<StreamElement *, std::unordered_set<uint64_t>>
      elementUserMap;

  /**
   * Holds the prefetch element buffer.
   */
  PrefetchElementBuffer peb;

  using StreamId = uint64_t;
  std::unordered_map<StreamId, Stream *> streamMap;

  /**
   * One level indirection for from the original stream id to coalesced stream
   * id. This is to make sure that streamMap always maintains an one-to-one
   * mapping.
   */
  std::unordered_map<StreamId, StreamId> coalescedStreamIdMap;

  /**
   * Flags.
   */
  const Params *myParams;
  bool isOracle;
  unsigned defaultRunAheadLength;
  bool enableLSQ;
  bool enableCoalesce;
  bool enableMerge;
  bool enableStreamPlacement;
  bool enableStreamPlacementOracle;
  bool enableStreamPlacementBus;
  bool enablePlacementPeriodReset;
  bool noBypassingStore;
  bool continuousStore;
  bool enableStreamFloat;
  bool enableStreamFloatIndirect;
  bool enableStreamFloatPseudo;
  bool enableStreamFloatCancel;
  std::string placementLat;
  std::string placement;
  /**
   * A dummy cacheline of data for write back.
   */
  uint8_t *writebackCacheLine;

  /**
   * ! This is a hack to avoid having alias for IndirectUpdateStream.
   * The DynStream tracks writes from this stream that has not been writen back.
   * And delay issuing new elements if found aliased here.
   */
  struct PendingWritebackElement {
    FIFOEntryIdx fifoIdx;
    Addr vaddr;
    int size;
    PendingWritebackElement(FIFOEntryIdx _fifoIdx, Addr _vaddr, int _size)
        : fifoIdx(_fifoIdx), vaddr(_vaddr), size(_size) {}
  };
  std::map<InstSeqNum, PendingWritebackElement> pendingWritebackElements;
  /**
   * Check if this access aliased with my pending writeback elements.
   */
  bool hasAliasWithPendingWritebackElements(StreamElement *checkElement,
                                            Addr vaddr, int size) const;
  void removePendingWritebackElement(InstSeqNum seqNum, Addr vaddr, int size);

  /**
   * Memorize the StreamConfigureInfo.
   */
  mutable std::unordered_map<std::string, ::LLVM::TDG::StreamRegion>
      memorizedStreamRegionMap;

  struct CacheBlockInfo {
    int reference;
    enum Status {
      INIT,
      FETCHING,
      FETCHED,
    };
    Status status;
    bool used;
    bool requestedByLoad;
    std::list<StreamMemAccess *> pendingAccesses;
    CacheBlockInfo()
        : reference(0), status(Status::INIT), used(false),
          requestedByLoad(false) {}
  };
  std::unordered_map<Addr, CacheBlockInfo> cacheBlockRefMap;

  void tryInitializeStreams(const ::LLVM::TDG::StreamRegion &streamRegion);
  void initializeStreams(const ::LLVM::TDG::StreamRegion &streamRegion);
  void
  generateCoalescedStreamIdMap(const ::LLVM::TDG::StreamRegion &streamRegion,
                               Stream::StreamArguments &args);

  Stream *getOrInitializeStream(uint64_t stepRootStreamId,
                                int32_t coalesceGroup);

  void updateAliveStatistics();

  void initializeFIFO(size_t totalElements);
  void addFreeElement(StreamElement *S);
  StreamElement *removeFreeElement();
  bool hasFreeElement() const;

  // Memorize the step stream list.
  mutable std::unordered_map<Stream *, std::list<Stream *>>
      memorizedStreamStepListMap;
  const std::list<Stream *> &getStepStreamList(Stream *stepS) const;

  // Helper function to get streams configured in the region.
  mutable std::unordered_map<const LLVM::TDG::StreamRegion *,
                             std::list<Stream *>>
      memorizedRegionConfiguredStreamsMap;
  const std::list<Stream *> &
  getConfigStreamsInRegion(const LLVM::TDG::StreamRegion &streamRegion);

  // Allocate one element to stream.
  void allocateElement(DynStream &dynS);
  /**
   * Release an unstepped stream element.
   * Used to clear ended stream.
   */
  bool releaseElementUnstepped(DynStream &dynS);
  /**
   * Release a stepped stream element.
   * @param isEnd: this element is stepped by StreamEnd, not StreamStep.
   * @param toThrottle: perform stream throttling.
   */
  void releaseElementStepped(DynStream *dynS, bool isEnd, bool doThrottle);
  void issueElements();
  void issueElement(StreamElement *elem);
  void issueNDCElement(StreamElement *elem);
  void prefetchElement(StreamElement *elem);
  // For InCoreStoreCmpS.
  void writebackElement(StreamElement *elem);
  // This is the old implementation for LLVMTraceCPU.
  void writebackElement(StreamElement *elem, StreamStoreInst *inst);

  /**
   * Flush the PEB entries.
   */
  void flushPEB(Addr vaddr, int size);

  /**
   * LoadQueue RAW misspeculation.
   */
  void RAWMisspeculate(StreamElement *element);

  /**
   * List used to avoid scanning through all DynStreams' elements.
   * And helper function to manage this list.
   * NOTE: I don't really care about the order between DynS for now.
   *
   * A DynS is added to the list when:
   * 1. StreamConfig executed if not FloatedAsNDC.
   * 2. StreamConfig committed if FloatedAsNDC.
   * 3. New element allocated.
   * 4. Element flushed.
   *
   * A DynS is removed from the list when:
   * 1. StreamEnd committed.
   * 2. StreamConfig rewinded.
   * 3. All elements are AddrReady and
   *  if ShouldComputeValue -- all computation is scheduled.
   */
  using DynStreamList = std::list<DynStream *>;
  using DynStreamSet = std::set<DynStream *>;
  DynStreamSet issuingDynStreamSet;
  bool isDynSIssuing(DynStream *dynS) const {
    return this->issuingDynStreamSet.count(dynS);
  }
  void addIssuingDynS(DynStream *dynS);
  DynStreamSet::iterator removeIssuingDynS(DynStreamSet::iterator iter);
  void removeIssuingDynS(DynStream *dynS);

  std::vector<StreamElement *> findReadyElements();

  size_t getTotalRunAheadLength() const;

  const ::LLVM::TDG::StreamRegion &
  getStreamRegion(const std::string &relativePath) const;

  void dumpFIFO() const;
  void dumpUser() const;

  std::unique_ptr<StreamThrottler> throttler;
  std::unique_ptr<StreamNDCController> ndcController;
  std::unique_ptr<StreamFloatController> floatController;
  std::unique_ptr<StreamComputeEngine> computeEngine;
  std::unique_ptr<StreamRegionController> regionController;
  std::unique_ptr<StreamRangeSyncController> rangeSyncController;
  std::unique_ptr<StreamDataTrafficAccumulator> dataTrafficAccFix;
  std::unique_ptr<StreamDataTrafficAccumulator> dataTrafficAccFloat;

  /**
   * Try to coalesce continuous element of direct mem stream if they
   * overlap.
   */
  bool shouldCoalesceContinuousDirectMemStreamElement(StreamElement *elem);
  void coalesceContinuousDirectMemStreamElement(StreamElement *elem);
};

} // namespace gem5

#endif
