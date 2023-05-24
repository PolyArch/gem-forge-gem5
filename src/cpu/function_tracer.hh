#ifndef __CPU_FUNCTION_TRACER_HH__
#define __CPU_FUNCTION_TRACER_HH__

#include "base/types.hh"

#include <iostream>
#include <string>
#include <unordered_map>

namespace gem5 {

class FunctionTracer {
public:
  FunctionTracer(const std::string &_name, Tick _clockPeriod)
      : name(_name), clockPeriod(_clockPeriod) {}
  void enableFunctionTrace();
  void enableFunctionAccumulateTick();
  void traceFunctions(Addr pc);

private:
  const std::string name;
  const Tick clockPeriod;
  Tick functionTraceFirstTick = 0;
  bool functionTracingEnabled = false;
  bool functionAccumulateTickEnabled = false;
  std::ostream *functionTraceStream = nullptr;
  std::ostream *functionAccumulateTickStream = nullptr;

  Addr currentFunctionStart = 0;
  Addr currentFunctionEnd = 0;
  Tick functionEntryTick = 0;

  // We also record ticks in every function.
  struct FuncProfile {
    Tick ticks = 0;
    uint64_t microOps = 0;
  };
  std::unordered_map<Addr, FuncProfile> addrFuncProfileMap;

  void accumulateTick(Addr funcStart, Tick ticks);
  void accumulateMicroOps(Addr funcStart, uint64_t microOps);

  // Stats callback for funcAccumulateTicks.
  void resetFuncAccumulateTick();
  void dumpFuncAccumulateTick();
};
} // namespace gem5

#endif