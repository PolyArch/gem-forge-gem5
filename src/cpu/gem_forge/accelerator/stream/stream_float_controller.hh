#ifndef __CPU_GEM_FORGE_ACCELERATOR_STREAM_FLOAT_CONTROLLER_HH__
#define __CPU_GEM_FORGE_ACCELERATOR_STREAM_FLOAT_CONTROLLER_HH__

#include "stream_engine.hh"

#include <map>

namespace gem5 {

/**
 * Helper class to manager the floating streams.
 * So far this is only in charge of floating streams at configuration.
 */
class StreamFloatController {
public:
  StreamFloatController(StreamEngine *_se,
                        std::unique_ptr<StreamFloatPolicy> _policy);

  using StreamList = std::list<Stream *>;
  using DynStreamList = std::list<DynStream *>;
  using DynStreamVec = std::vector<DynStream *>;

  using StreamConfigArgs = StreamEngine::StreamConfigArgs;
  void floatStreams(const StreamConfigArgs &args,
                    const ::LLVM::TDG::StreamRegion &region,
                    DynStreamList &dynStreams);

  void commitFloatStreams(const StreamConfigArgs &args,
                          const StreamList &streams);
  void rewindFloatStreams(const StreamConfigArgs &args,
                          const StreamList &streams);
  void endFloatStreams(const DynStreamVec &dynStreams);

  /**
   * Handle midway florat.
   */
  void processMidwayFloat();

private:
  StreamEngine *se;
  std::unique_ptr<StreamFloatPolicy> policy;

  /**
   * A internal structure to memorize the floating decision made so far.
   */
  using StreamCacheConfigMap = StreamFloatPolicy::StreamCacheConfigMap;

  struct Args {
    const ::LLVM::TDG::StreamRegion &region;
    InstSeqNum seqNum;
    DynStreamList &dynStreams;
    StreamCacheConfigMap &floatedMap;
    CacheStreamConfigureVec &rootConfigVec;
    Args(const ::LLVM::TDG::StreamRegion &_region, InstSeqNum _seqNum,
         DynStreamList &_dynStreams, StreamCacheConfigMap &_floatedMap,
         CacheStreamConfigureVec &_rootConfigVec)
        : region(_region), seqNum(_seqNum), dynStreams(_dynStreams),
          floatedMap(_floatedMap), rootConfigVec(_rootConfigVec) {}
  };

  void floatDirectLoadStreams(const Args &args);
  void floatDirectAtomicComputeStreams(const Args &args);
  void floatPointerChaseStreams(const Args &args);
  void floatIndStreams(const Args &args);
  bool floatIndStream(const Args &args, DynStream *dynS);
  void floatDirectUpdateStreams(const Args &args);
  void floatDirectStoreComputeStreams(const Args &args);
  void floatDirectStoreComputeOrUpdateStream(const Args &args, DynStream *dynS);
  void floatDirectOrPtrChaseReduceStreams(const Args &args);
  void floatIndirectReductionStreams(const Args &args);
  void floatIndReduceStream(const Args &args, DynStream *dynS);
  void floatMultiLevelIndirectStoreComputeStreams(const Args &args);
  void floatMultiLevelIndirectStoreComputeStream(const Args &args,
                                                 DynStream *dynS);

  /**
   * Calculate the float chain depth (of UsedBy dependence).
   */
  int getFloatChainDepth(const CacheStreamConfigureData &config) const;

  /**
   * Calculate the float chain size from this config.
   */
  int getFloatChainChildrenSize(const CacheStreamConfigureData &config) const;

  /**
   * Get the FloatChainRoot stream (with depth 0).
   */
  CacheStreamConfigureDataPtr
  getFloatRootConfig(CacheStreamConfigureDataPtr config) const;

  /**
   * Check if a Config is on our FloatChain.
   */
  bool isOnFloatChain(CacheStreamConfigureDataPtr chainEndConfig,
                      const CacheStreamConfigureDataPtr &config) const;

  /**
   * Fix the multi-predication relationship.
   */
  void fixMultiPredication(const Args &args);

  /**
   * If the loop is eliminated, we mark some addition fields in the
   * configuration.
   */
  void floatEliminatedLoop(const Args &args);

  /**
   * Set the number of slices allocated at the MLC stream engine.
   */
  void decideMLCBufferNumSlices(const Args &args);

  /**
   * For now we can rewind a floated stream that write to memory (Store/Atomic
   * Compute Stream). As a temporary fix, I delay sending out the floating
   * packet until the StreamConfig is committed, and raise the "offloadDelayed"
   * flag in the DynStream -- which will stop the StreamEngine issuing them.
   */
  using SeqNumToPktMapT = std::map<InstSeqNum, PacketPtr>;
  using SeqNumToPktMapIter = SeqNumToPktMapT::iterator;
  SeqNumToPktMapT configSeqNumToDelayedFloatPktMap;

  /**
   * These are ConfigPkts delayed to wait for FirstFloatedElemIdx.
   * Their StreamConfigs are committed.
   */
  SeqNumToPktMapT configSeqNumToMidwayFloatPktMap;

  /**
   * Check if there is an aliased StoreStream for this LoadStream, but
   * is not promoted into an UpdateStream.
   */
  bool checkAliasedUnpromotedStoreStream(DynStream *dynS);

  /**
   * Determine the FirstOffloadedElementIdx for LoopBound.
   */
  void setLoopBoundFirstOffloadedElemIdx(const Args &args);

  /**
   * Propagate FloatPlan to ConfigureData.
   */
  void propagateFloatPlan(const Args &args);

  /**
   * Try send out a midway float pkt.
   */
  bool trySendMidwayFloat(SeqNumToPktMapIter iter);

  /**
   * Check if a MidwayFloat is ready to issue.
   */
  bool isMidwayFloatReady(CacheStreamConfigureDataPtr &config);

  /**
   * Add AffineIV Config.
   */
  void addUsedAffineIV(CacheStreamConfigureDataPtr &config, DynStream *dynS,
                       Stream *affineIVS);
  void addUsedAffineIVWithReuseSkip(CacheStreamConfigureDataPtr &config,
                                    DynStream *dynS, Stream *affineIVS,
                                    int reuse, int skip);
};

} // namespace gem5

#endif