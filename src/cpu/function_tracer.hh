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
      : myName(_name), clockPeriod(_clockPeriod) {}
  void enableFunctionTrace();
  void enableFunctionAccumulateTick(bool enablePCAccTick);
  void traceFunctions(Addr pc);

private:
  const std::string &name() const { return myName; }
  const std::string myName;
  const Tick clockPeriod;
  Tick functionTraceFirstTick = 0;
  bool functionTracingEnabled = false;
  bool functionAccumulateTickEnabled = false;
  bool pcAccumulateTickEnabled = false;
  std::ostream *functionTraceStream = nullptr;
  std::ostream *functionAccumulateTickStream = nullptr;

  Addr currentPC = 0;
  Addr currentFunctionStart = 0;
  Addr currentFunctionEnd = 0;
  Tick functionEntryTick = 0;
  Tick currentPCTick = 0;

  // We also record ticks in every function.
  struct Profile {
    Tick ticks = 0;
    uint64_t microOps = 0;
  };
  using ProfileMap = std::unordered_map<Addr, Profile>;
  ProfileMap addrFuncProfileMap;
  ProfileMap addrPCProfileMap;

  void accumulateTick(ProfileMap &map, Addr funcStart, Tick ticks);
  void accumulateMicroOps(ProfileMap &map, Addr funcStart, uint64_t microOps);

  // Stats callback for funcAccumulateTicks.
  void resetFuncAccumulateTick();
  void dumpFuncAccumulateTick();
};
} // namespace gem5

#endif