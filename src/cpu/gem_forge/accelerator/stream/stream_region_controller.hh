
#ifndef __CPU_GEM_FORGE_ACCELERATOR_NEST_STREAM_CONTROLLER_H__
#define __CPU_GEM_FORGE_ACCELERATOR_NEST_STREAM_CONTROLLER_H__

#include "stream_engine.hh"

class StreamRegionController {
public:
  StreamRegionController(StreamEngine *_se);
  ~StreamRegionController();

  void initializeRegion(const ::LLVM::TDG::StreamRegion &region);

  using ConfigArgs = StreamEngine::StreamConfigArgs;
  void dispatchStreamConfig(const ConfigArgs &args);
  void executeStreamConfig(const ConfigArgs &args);
  void commitStreamConfig(const ConfigArgs &args);
  void rewindStreamConfig(const ConfigArgs &args);

  /**
   * Move the logic to handle StreamEnd from StreamEngine to here.
   * This helps to make the O3NestStreamEnd implementation more clear.
   */
  using EndArgs = StreamEngine::StreamEndArgs;
  bool canDispatchStreamEnd(const EndArgs &args);
  void dispatchStreamEnd(const EndArgs &args);
  bool canExecuteStreamEnd(const EndArgs &args);
  bool canCommitStreamEnd(const EndArgs &args);
  void rewindStreamEnd(const EndArgs &args);
  void commitStreamEnd(const EndArgs &args);

  void determineStepElemCount(const ConfigArgs &args);

  void tick();

  void takeOverBy(GemForgeCPUDelegator *newCPUDelegator);

  struct StaticRegion;
  struct DynRegion {
    StaticRegion *staticRegion;
    const uint64_t seqNum;
    bool configExecuted = false;
    bool configCommitted = false;
    bool canSkipToEnd = false;

    bool endDispatched = false;
    uint64_t endSeqNum = 0;
    void dispatchStreamEnd(uint64_t endSeqNum) {
      assert(!this->endDispatched);
      this->endDispatched = true;
      this->endSeqNum = endSeqNum;
    }
    void rewindStreamEnd() {
      assert(this->endDispatched);
      this->endDispatched = false;
      this->endSeqNum = 0;
    }

    DynRegion(StaticRegion *_staticRegion, uint64_t _seqNum)
        : staticRegion(_staticRegion), seqNum(_seqNum) {}

    /**
     * NestStream states.
     */
    struct DynNestConfig {
      const StaticRegion *staticRegion;
      ExecFuncPtr configFunc = nullptr;
      ExecFuncPtr predFunc = nullptr;
      DynStreamFormalParamV formalParams;
      DynStreamFormalParamV predFormalParams;
      uint64_t nextElemIdx = 0;

      /**
       * ConfigSeqNum of configured NestRegion.
       */
      std::vector<InstSeqNum> configSeqNums;

      DynNestConfig(const StaticRegion *_staticRegion)
          : staticRegion(_staticRegion) {}

      InstSeqNum getConfigSeqNum(StreamEngine *se, uint64_t elementIdx,
                                 uint64_t outSeqNum) const;
    };
    std::vector<DynNestConfig> nestConfigs;

    /**
     * StreamLoopBound states.
     */
    struct DynLoopBound {
      ExecFuncPtr boundFunc = nullptr;
      DynStreamFormalParamV formalParams;
      uint64_t nextElemIdx = 0;
      // We have reached the end of the loop.
      bool brokenOut = false;
      // We have offloaded the LoopBound.
      bool offloaded = false;
      uint64_t offloadedFirstElementIdx = 0;
    };
    DynLoopBound loopBound;

    /**
     * StreamStepper states for LoopEliminated region.
     * NOTE: There is no one-to-one mapping from DynGroup to StaticGroup, due to
     * some loop with 0 trip count. Instead, DynStepGroupInfo::staticGroupIdx
     * should be used to find the StepGroup.
     */
    struct DynStep {
      struct DynStepGroupInfo {
        uint64_t nextElemIdx = 0;
        int64_t totalTripCount = 0;
        int64_t levelTripCount = INT64_MAX;
        // Remember this info for simplicity.
        int loopLevel;
        // Remember the index to StaticGroup.
        int staticGroupIdx;
        // How many elements we want step at each time.
        uint64_t stepElemCount = 1;
        DynStepGroupInfo(int _loopLevel, int _staticGroupIdx)
            : loopLevel(_loopLevel), staticGroupIdx(_staticGroupIdx) {}
      };
      std::vector<DynStepGroupInfo> stepGroups;
      int nextDynGroupIdx = 0;
      enum StepState {
        // There is no Execute stage for StreamStep.
        BEFORE_DISPATCH,
        BEFORE_COMMIT,
      };
      StepState state = StepState::BEFORE_DISPATCH;
    };
    DynStep step;
  };

  struct StaticRegion {
    using StreamSet = std::unordered_set<Stream *>;
    using StreamVec = std::vector<Stream *>;
    const ::LLVM::TDG::StreamRegion &region;
    StreamVec streams;
    std::list<DynRegion> dynRegions;
    // Remember if all loops or some loops is eliminated.
    bool allStreamsLoopEliminated = false;
    bool someStreamsLoopEliminated = false;
    StaticRegion(const ::LLVM::TDG::StreamRegion &_region) : region(_region) {}

    /**
     * NestStream config (for this nested region).
     */
    struct StaticNestConfig {
      ExecFuncPtr configFunc = nullptr;
      ExecFuncPtr predFunc = nullptr;
      bool predRet;
      StreamSet baseStreams;
    };
    StaticNestConfig nestConfig;

    /**
     * StreamLoopBound config.
     */
    struct StaticLoopBound {
      ExecFuncPtr boundFunc;
      bool boundRet;
      StreamSet baseStreams;
    };
    StaticLoopBound loopBound;

    /**
     * StreamStepper states for LoopEliminated region.
     * To support stepping for multi-level loop streams, we
     * sort StepGroups by InnerMostLoopLevel (inner -> outer), and
     * remember the TripCount for each level in DynStep.
     */
    struct StaticStep {
      struct StepGroupInfo {
        Stream *stepRootS;
        bool needFinalValue = false;
        bool needSecondFinalValue = false;
        StepGroupInfo(Stream *_stepRootS) : stepRootS(_stepRootS) {}
      };
      // Just used for StreamAllocator and StreamThrottler.
      StreamVec stepRootStreams;
      std::vector<StepGroupInfo> stepGroups;
      std::set<Stream *> skipStepSecondLastElemStreams;
    };
    StaticStep step;
  };

  /******************************************************************
   * Util APIs.
   ******************************************************************/
  StaticRegion &getStaticRegion(const std::string &regionName);
  StaticRegion &getStaticRegion(Stream *S);
  DynRegion &getDynRegion(const std::string &msg, InstSeqNum seqNum);

  void receiveOffloadedLoopBoundRet(const DynStreamId &dynStreamId,
                                    int64_t tripCount, bool brokenOut);

private:
  StreamEngine *se;
  GemForgeISAHandler isaHandler;

  std::map<uint64_t, DynRegion *> activeDynRegionMap;

  /**
   * Remember all static region config.
   */
  std::unordered_map<std::string, StaticRegion> staticRegionMap;

  /**
   * For NestStream.
   */
  void initializeNestStreams(const ::LLVM::TDG::StreamRegion &region,
                             StaticRegion &staticRegion);
  void dispatchStreamConfigForNestStreams(const ConfigArgs &args,
                                          DynRegion &dynRegion);
  void executeStreamConfigForNestStreams(const ConfigArgs &args,
                                         DynRegion &dynRegion);
  bool checkRemainingNestRegions(const DynRegion &dynRegion);
  void configureNestStream(DynRegion &dynRegion,
                           DynRegion::DynNestConfig &dynNestConfig);

  /**
   * For StreamLoopBound.
   */
  void initializeStreamLoopBound(const ::LLVM::TDG::StreamRegion &region,
                                 StaticRegion &staticRegion);
  void dispatchStreamConfigForLoopBound(const ConfigArgs &args,
                                        DynRegion &dynRegion);
  void executeStreamConfigForLoopBound(const ConfigArgs &args,
                                       DynRegion &dynRegion);
  void checkLoopBound(DynRegion &dynRegion);

  /**
   * For StreamStepper (LoopEliminatedRegion)
   */
  void initializeStep(const ::LLVM::TDG::StreamRegion &region,
                      StaticRegion &staticRegion);
  void dispatchStreamConfigForStep(const ConfigArgs &args,
                                   DynRegion &dynRegion);
  void executeStreamConfigForStep(const ConfigArgs &args, DynRegion &dynRegion);
  void stepStream(DynRegion &dynRegion);

  /**
   * Allocate stream elements.
   */
  void allocateElements(StaticRegion &staticRegion);
  bool canSkipAllocatingDynS(StaticRegion &staticRegion,
                             DynStream &stepRootDynS);

  /**
   * Helper functions.
   */
  DynRegion &pushDynRegion(StaticRegion &staticRegion, uint64_t seqNum);

  /**
   * For LoopEliminated region, we can skip allocation and stepping to the last
   * element. This is mainly used to avoid bottleneck at the core for Strands
   * and PUM.
   */
  bool canSkipToStreamEnd(const DynRegion &dynRegion) const;
  void trySkipToStreamEnd(DynRegion &dynRegion);

  void buildFormalParams(const ConfigArgs::InputVec &inputVec, int &inputIdx,
                         const ::LLVM::TDG::ExecFuncInfo &funcInfo,
                         DynStreamFormalParamV &formalParams);

  struct GetStreamValueFromElementSet {
    using ElementSet = std::unordered_set<StreamElement *>;
    ElementSet &elements;
    const char *error;
    GetStreamValueFromElementSet(ElementSet &_elements, const char *_error)
        : elements(_elements), error(_error) {}
    StreamValue operator()(uint64_t streamId) const;
  };

  /**
   * Helper function to manage StreamEnd and enable out-of-order StreamEnd for
   * eliminated loop.
   */
  DynRegion *tryGetFirstAliveDynRegion(StaticRegion &staticRegion);
  DynRegion &getFirstAliveDynRegion(StaticRegion &staticRegion);
  DynRegion &getDynRegionByEndSeqNum(StaticRegion &staticRegion,
                                     uint64_t endSeqNum);
};

#endif