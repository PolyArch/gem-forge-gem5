#include "function_tracer.hh"

#include "base/callback.hh"
#include "base/loader/symtab.hh"
#include "base/output.hh"
#include "base/statistics.hh"
#include "base/trace.hh"
#include "debug/FuncTrace.hh"
#include "sim/core.hh"

#define FUNC_TRACE_(X, format, args...)                                        \
  {                                                                            \
    DPRINTF(X, format, ##args);                                                \
    ccprintf(*this->functionTraceStream, format, ##args);                      \
  }

namespace gem5 {

void FunctionTracer::enableFunctionTrace() {
  assert(!this->functionTracingEnabled);
  const std::string fname = csprintf("ftrace.%s", this->name());
  auto funcTraceFolder = simout.findOrCreateSubdirectory("ftrace");
  this->functionTraceStream = funcTraceFolder->findOrCreate(fname)->stream();
  this->functionTracingEnabled = true;
  this->functionEntryTick = curTick();
  this->currentPCTick = curTick();
}

void FunctionTracer::enableFunctionAccumulateTick(bool enablePCAccTick) {
  assert(!this->functionAccumulateTickEnabled);
  this->functionAccumulateTickEnabled = true;
  this->pcAccumulateTickEnabled = enablePCAccTick;
  this->functionEntryTick = curTick();
  this->currentPCTick = curTick();

  // Register stats callback.
  statistics::registerResetCallback(
      [this]() -> void { this->resetFuncAccumulateTick(); });
  statistics::registerDumpCallback(
      [this]() -> void { this->dumpFuncAccumulateTick(); });
}

void FunctionTracer::traceFunctions(Addr pc) {

  if (!this->functionTracingEnabled && !this->functionAccumulateTickEnabled)
    return;

  // if pc enters different function, print new function symbol and
  // update saved range.  Otherwise do nothing.
  if (pc < this->currentFunctionStart || pc >= this->currentFunctionEnd) {
    std::string sym_str;
    auto oldFunctionStart = this->currentFunctionStart;
    bool found = loader::debugSymbolTable.findNearestSymbol(
        pc, sym_str, this->currentFunctionStart, this->currentFunctionEnd);

    if (!found) {
      // no symbol found: use addr as label
      sym_str = csprintf("0x%x", pc);
      this->currentFunctionStart = pc;
      this->currentFunctionEnd = pc + 1;
    }

    auto accumulateTick = curTick() - this->functionEntryTick;

    if (this->functionTracingEnabled) {
      std::string oldFuncName;
      Addr oldFuncLhs, oldFuncRhs;
      bool found = loader::debugSymbolTable.findNearestSymbol(
          oldFunctionStart, oldFuncName, oldFuncLhs, oldFuncRhs);
      if (!found) {
        oldFuncName = csprintf("0x%x", oldFunctionStart);
      }
      if (this->functionTraceFirstTick == 0) {
        this->functionTraceFirstTick = curTick();
      }
      FUNC_TRACE_(FuncTrace, " %lu-%lu-%lu-%5lu: %8#x %20s %10s %20s %#x\n",
                  curTick(), curTick() / this->clockPeriod,
                  (curTick() - this->functionTraceFirstTick) / clockPeriod,
                  accumulateTick / clockPeriod, this->currentPC, oldFuncName,
                  pc == this->currentFunctionStart ? ">>Enter" : ">>BackTo",
                  sym_str, pc);
    }

    if (this->functionAccumulateTickEnabled) {
      this->accumulateTick(this->addrFuncProfileMap, oldFunctionStart,
                           accumulateTick);
    }

    this->functionEntryTick = curTick();
  }
  if (this->functionAccumulateTickEnabled) {
    this->accumulateMicroOps(this->addrFuncProfileMap,
                             this->currentFunctionStart, 1);
  }

  // Update the PC accumulate map.
  if (this->pcAccumulateTickEnabled) {
    this->accumulateTick(this->addrPCProfileMap, pc,
                         curTick() - this->currentPCTick);
    // Only update this when we see new PC so that it's inst count.
    if (pc != this->currentPC) {
      this->accumulateMicroOps(this->addrPCProfileMap, pc, 1);
    }
  }

  this->currentPC = pc;
  this->currentPCTick = curTick();
}

void FunctionTracer::accumulateTick(ProfileMap &map, Addr funcStart,
                                    Tick ticks) {
  map.emplace(std::piecewise_construct, std::forward_as_tuple(funcStart),
              std::forward_as_tuple())
      .first->second.ticks += ticks;
}

void FunctionTracer::accumulateMicroOps(ProfileMap &map, Addr funcStart,
                                        uint64_t microOps) {
  map.emplace(std::piecewise_construct, std::forward_as_tuple(funcStart),
              std::forward_as_tuple())
      .first->second.microOps += microOps;
}

void FunctionTracer::resetFuncAccumulateTick() {
  this->addrFuncProfileMap.clear();
  this->addrPCProfileMap.clear();
  // We also reset the function entry tick.
  this->functionEntryTick = curTick();
  this->currentPCTick = curTick();
  this->functionTraceFirstTick = 0;
}

void FunctionTracer::dumpFuncAccumulateTick() {

  /**
   * Make sure we record the current accumulated ticks.
   */
  if (this->functionAccumulateTickEnabled && this->currentFunctionStart) {
    auto accumulateTick = curTick() - this->functionEntryTick;
    this->accumulateTick(this->addrFuncProfileMap, this->currentFunctionStart,
                         accumulateTick);
    this->functionEntryTick = curTick();
  }

  if (this->addrFuncProfileMap.empty()) {
    return;
  }

  if (!this->functionAccumulateTickStream) {
    auto funcTraceFolder = simout.findOrCreateSubdirectory("ftrace");
    const std::string fname = csprintf("ftick.%s", this->name());
    this->functionAccumulateTickStream =
        funcTraceFolder->findOrCreate(fname)->stream();
  }

  // Sort by ticks.
  std::vector<std::pair<Addr, Profile>> sorted(this->addrFuncProfileMap.begin(),
                                               this->addrFuncProfileMap.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const std::pair<Addr, Profile> &a,
               const std::pair<Addr, Profile> &b) -> bool {
              if (a.second.ticks != b.second.ticks) {
                return a.second.ticks > b.second.ticks;
              } else {
                // Break the tie with pc.
                return a.first < b.first;
              }
            });

  // Sum all ticks.
  Tick sumTicks = 0;
  for (const auto &pcTick : sorted) {
    sumTicks += pcTick.second.ticks;
  }

  ccprintf(*this->functionAccumulateTickStream, "======================\n");
  for (const auto &pcTick : sorted) {
    auto pc = pcTick.first;
    auto tick = pcTick.second.ticks;
    auto microOps = pcTick.second.microOps;
    std::string symbol;
    if (!loader::debugSymbolTable.findSymbol(pc, symbol)) {
      symbol = csprintf("0x%x", pc);
    }
    float percentage =
        static_cast<float>(tick) / static_cast<float>(sumTicks) * 100.f;
    ccprintf(*this->functionAccumulateTickStream, "%5.2f %20llu %15llu : %s\n",
             percentage, tick / clockPeriod, microOps, symbol);
  }

  if (!this->addrPCProfileMap.empty()) {
    // Dump PC accumulate tick in the top 80% of function.
    ccprintf(*this->functionAccumulateTickStream, "======================\n");

    float accTickPercentage = 0;
    for (const auto &pcTick : sorted) {
      auto pc = pcTick.first;
      auto tick = pcTick.second.ticks;
      auto microOps = pcTick.second.microOps;

      std::string symbol;
      Addr funcStart, funcEnd;
      bool found = loader::debugSymbolTable.findNearestSymbol(
          pc, symbol, funcStart, funcEnd);
      if (!found) {
        symbol = csprintf("0x%x", pc);
      }

      float percentage =
          static_cast<float>(tick) / static_cast<float>(sumTicks) * 100.f;
      accTickPercentage += percentage;
      ccprintf(*this->functionAccumulateTickStream,
               "%5.2f %20llu %15llu : %s\n", percentage, tick / clockPeriod,
               microOps, symbol);

      if (!found) {
        ccprintf(*this->functionAccumulateTickStream,
                 "FuncSymbol not found!\n");
      } else {
        for (auto addr = funcStart; addr < funcEnd; ++addr) {
          auto iter = this->addrPCProfileMap.find(addr);
          if (iter != this->addrPCProfileMap.end()) {

            auto tick = iter->second.ticks;
            auto microOps = iter->second.microOps;
            auto cyclePerOp = static_cast<float>(tick) /
                              static_cast<float>(microOps) / clockPeriod;
            ccprintf(*this->functionAccumulateTickStream,
                     "%#x %20llu %15llu %5.2f\n", addr, tick / clockPeriod,
                     microOps, cyclePerOp);
          }
        }
      }

      if (accTickPercentage > 80.0f) {
        break;
      }
    }
  }
}
} // namespace gem5