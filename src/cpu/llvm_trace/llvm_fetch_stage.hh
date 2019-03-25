#ifndef __CPU_LLVM_FETCH_STAGE_HH__
#define __CPU_LLVM_FETCH_STAGE_HH__

#include <vector>

#include "base/statistics.hh"
#include "cpu/llvm_trace/llvm_branch_predictor.hh"
#include "cpu/llvm_trace/llvm_insts.hh"
#include "cpu/llvm_trace/llvm_stage_signal.hh"
#include "cpu/pred/bpred_unit.hh"
#include "cpu/timebuf.hh"
#include "params/LLVMTraceCPU.hh"

class LLVMTraceCPU;
class LLVMTraceThreadContext;

class LLVMFetchStage {
public:
  using FetchStruct = std::vector<LLVMDynamicInstId>;

  LLVMFetchStage(LLVMTraceCPUParams *params, LLVMTraceCPU *_cpu);
  LLVMFetchStage(const LLVMFetchStage &fs) = delete;
  LLVMFetchStage(LLVMFetchStage &&fs) = delete;

  ~LLVMFetchStage();

  void setToDecode(TimeBuffer<FetchStruct> *toDecodeBuffer);
  void setSignal(TimeBuffer<LLVMStageSignal> *signalBuffer, int pos);
  void regStats();
  void tick();
  void clearContext(ThreadID contextId);

  std::string name();

  Stats::Scalar blockedCycles;
  Stats::Scalar branchInsts;
  Stats::Scalar branchPredMisses;
  /** Stat for total number of fetched instructions. */
  Stats::Scalar fetchedInsts;
  /** Total number of fetched branches. */
  Stats::Scalar fetchedBranches;
  /** Stat for total number of predicted branches. */
  Stats::Scalar predictedBranches;

private:
  LLVMTraceCPU *cpu;

  unsigned fetchWidth;

  /**
   * Stores all the per-context fetch states.
   */
  struct FetchState {
    /**
     * Flag to tell if we have just fetched an SerializeAfter instruction.
     * If yes, the next fetched instruction is marked SerializeBefore.
     */
    bool serializeAfter;
    // Penality cycles after the branch instruction is committed.
    uint8_t branchPredictPenalityCycles;
    // Branch missed instruction id.
    LLVMDynamicInstId blockedInstId;
    // TODO: Make the predictor also context-base.
    FetchState() : serializeAfter(false), branchPredictPenalityCycles(0) {}

    void clear() {
      this->serializeAfter = false;
      this->branchPredictPenalityCycles = 0;
    }
  };

  std::vector<FetchState> fetchStates;

  Cycles toDecodeDelay;
  TimeBuffer<FetchStruct>::wire toDecode;
  TimeBuffer<LLVMStageSignal>::wire signal;

  LLVMBranchPredictor *predictor;

  BPredUnit *branchPredictor;

  int lastFetchedContextId;
};

#endif