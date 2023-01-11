#include "stream.hh"
#include "stream_engine.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

// #include "base/misc.hh""
#include "base/trace.hh"
#include "proto/protoio.hh"

#include "debug/CoalescedStream.hh"
#define DEBUG_TYPE CoalescedStream
#include "stream_log.hh"

#include <sstream>

#define LOGICAL_STREAM_PANIC(S, format, args...)                               \
  panic("Logical Stream %s: " format, (S)->info.name().c_str(), ##args)

#define LS_DPRINTF(LS, format, args...)                                        \
  DPRINTF(CoalescedStream, "L-Stream %s: " format, (LS)->info.name().c_str(),  \
          ##args)

#define STREAM_DPRINTF(format, args...)                                        \
  DPRINTF(CoalescedStream, "C-Stream %s: " format,                             \
          this->getStreamName().c_str(), ##args)

LogicalStream::LogicalStream(const std::string &_traceExtraFolder,
                             const LLVM::TDG::StreamInfo &_info)
    : info(_info) {
  if (!this->info.history_path().empty()) {
    this->history = std::unique_ptr<StreamHistory>(
        new StreamHistory(_traceExtraFolder + "/" + this->info.history_path()));
    this->patternStream = std::unique_ptr<StreamPattern>(
        new StreamPattern(_traceExtraFolder + "/" + this->info.pattern_path()));
  }
}

void Stream::addStreamInfo(const LLVM::TDG::StreamInfo &info) {
  /**
   * Note: At this point the primary logical stream may not be created yet!
   */
  this->logicals.emplace_back(
      new LogicalStream(this->getCPUDelegator()->getTraceExtraFolder(), info));
}

void Stream::finalize() {
  this->selectPrimeLogicalStream();
  if (this->primeLogical->info.type() == ::LLVM::TDG::StreamInfo_Type_IV) {
    assert(this->getNumLogicalStreams() == 1 && "Never coalesce IVStream");
  }
  // Initialize the dependence graph.
  this->initializeBaseStreams();
  this->initializeAliasStreams();
  this->initializeCoalesceGroupStreams();
  for (auto LS : this->logicals) {
    if (LS->getIsNonSpec()) {
      this->isNonSpec = true;
    }
  }
  // Merge predicated info.
  for (auto LS : this->logicals) {
    if (!LS->getPredicatedStreams().empty()) {
      assert(this->predicateFuncInfo.name() == "" && "MultiPredCallback");
      this->predicateFuncInfo = LS->getPredicateFuncInfo();
      for (const auto &predicatedStreamId : LS->getPredicatedStreams()) {
        this->predicatedStreamIds.push_back(predicatedStreamId);
      }
    }
  }
  STREAM_DPRINTF("Finalized, ElementSize %d, LStreams: =========.\n",
                 this->coalescedMemElemSize);
  for (auto LS : this->logicals) {
    LS_DPRINTF(LS, "Offset %d, MemElementSize %d.\n", LS->getCoalesceOffset(),
               LS->getMemElementSize());
  }
  STREAM_DPRINTF("Finalized ====================================.\n");
}

void Stream::selectPrimeLogicalStream() {
  assert(!this->logicals.empty());
  // Other sanity check for coalesced streams.
  // Sort the streams with offset.
  std::sort(this->logicals.begin(), this->logicals.end(),
            [](const LogicalStream *LA, const LogicalStream *LB) -> bool {
              return LA->getCoalesceOffset() <= LB->getCoalesceOffset();
            });
  this->primeLogical = this->logicals.front();
  this->baseOffset = this->primeLogical->getCoalesceOffset();
  this->coalescedMemElemSize = this->primeLogical->getMemElementSize();
  assert(this->baseOffset >= 0 && "Illegal BaseOffset.");
  // Make sure we have the correct MemElemSize.
  for (const auto &LS : this->logicals) {
    assert(LS->getCoalesceBaseStreamId() ==
           this->primeLogical->getCoalesceBaseStreamId());
    // Compute the element size.
    this->coalescedMemElemSize = std::max(
        this->coalescedMemElemSize,
        LS->getCoalesceOffset() - this->baseOffset + LS->getMemElementSize());
  }

  // By default the CoreElemSize is the same as MemElemSize if we coalesced.
  if (this->logicals.size() == 1) {
    this->coalescedCoreElemSize = this->primeLogical->getCoreElementSize();
  } else {
    this->coalescedCoreElemSize = this->coalescedMemElemSize;
  }

  /**
   * Finalize the stream name and static id.
   * ! This is important to get the correct StreamInput values.
   * ! I feel like some day I will pay the price due to this hacky
   * ! implementation.
   */
  this->streamName = this->primeLogical->info.name();
  this->staticId = this->primeLogical->info.id();

  /**
   * Now the LoadComputeStream may have other LoadBaseStreams. So far we enforce
   * that there is only one LogicalStream has LoadFunc enabled with
   * LoadBaseStreams, and move that information to the PrimeLogicalStream.
   */
  for (const auto &LS : this->logicals) {
    if (LS == this->primeLogical) {
      continue;
    }
    if (LS->getEnabledLoadFunc()) {
      if (this->primeLogical->getEnabledLoadFunc()) {
        panic("Multi LoadFunc in %s %s.", primeLogical->info.name(),
              LS->info.name());
      }
      const auto &computeInfo = LS->info.static_info().compute_info();
      auto primeComputeInfo = this->primeLogical->info.mutable_static_info()
                                  ->mutable_compute_info();
      // No need to copy ValueBaseStreams, as we search all LogicalStreams to
      // find the BaseStreams.
      primeComputeInfo->set_enabled_load_func(true);
      (*primeComputeInfo->mutable_load_func_info()) =
          computeInfo.load_func_info();

      // If we have LoadCompute, we reset the CoreElemSize.
      this->coalescedCoreElemSize = LS->getCoreElementSize();
    }
  }

  // Sanity check for consistency between logical streams.
  for (const auto &LS : this->logicals) {
    const auto &LSStaticInfo = LS->info.static_info();
    const auto &PSStaticInfo = this->primeLogical->info.static_info();
#define CHECK_INFO(print, field)                                               \
  do {                                                                         \
    auto A = LSStaticInfo.field();                                             \
    auto B = PSStaticInfo.field();                                             \
    if (A != B) {                                                              \
      print("Mismatch in %s, %s, %s.", #field, LS->info.name(),                \
            primeLogical->info.name());                                        \
    }                                                                          \
  } while (false);
    CHECK_INFO(warn, no_core_user);
    CHECK_INFO(panic, is_merged_predicated_stream);
    CHECK_INFO(panic, is_trip_count_fixed);
    CHECK_INFO(panic, loop_level);
    CHECK_INFO(panic, config_loop_level);
    CHECK_INFO(panic, is_inner_most_loop);
    CHECK_INFO(panic, loop_eliminated);
    CHECK_INFO(panic, core_need_final_value);
    CHECK_INFO(panic, core_need_second_final_value);
    CHECK_INFO(panic, compute_info().enabled_store_func);
#undef CHECK_INFO
    /**
     * Check if any LogicalStream has core user.
     */
    if (!LSStaticInfo.no_core_user()) {
      this->coalescedNoCoreUser = false;
    }
    // If more than one coalesced stream, then CoreElementSize must be
    // the same as the MemElementSize.
    if (this->logicals.size() > 1) {
      if (LS->getCoreElementSize() != LS->getMemElementSize()) {
        panic("Mismatch in %s CoreElementSize %d and MemElementSize %d.\n",
              LS->info.name(), LS->getCoreElementSize(),
              LS->getMemElementSize());
      }
    }
  }
}

void Stream::fixInnerLoopBaseStreams() {
  for (auto LS : this->logicals) {
    const auto &info = LS->info;
    // Update the address dependence information.
    for (const auto &baseStreamId : info.chosen_base_streams()) {

      bool alreadyAdded = false;
      for (const auto &baseEdge : this->baseEdges) {
        if (baseEdge.type != StreamDepEdge::TypeE::Addr) {
          continue;
        }
        if (baseEdge.toStaticId == baseStreamId.id()) {
          alreadyAdded = true;
          break;
        }
      }
      if (alreadyAdded) {
        continue;
      }

      auto baseS = this->se->tryGetStream(baseStreamId.id());
      if (!baseS) {
        S_PANIC(this, "Failed to find BaseStream %s.", baseStreamId.name());
      }
      /**
       * As a sanity check: the BaseS should be a nested inner-loop stream.
       */
      if (baseS->getLoopLevel() != this->getLoopLevel() + 1 ||
          baseS->getConfigLoopLevel() != this->getConfigLoopLevel() + 1) {
        S_PANIC(this, "This is not InnerLoopBaseStream %s.",
                baseS->getStreamName());
      }
      if (!baseS->isNestStream()) {
        S_PANIC(this, "InnerLoopBaseStream should be nested %s.",
                baseS->getStreamName());
      }

      assert(baseS != this && "Should never have circular address dependency.");
      this->addBaseStream(StreamDepEdge::TypeE::Addr, true /* isInnerLoop */,
                          baseStreamId.id(), info.id(), baseS);
    }
  }
}

void Stream::initializeBaseStreams() {
  for (auto LS : this->logicals) {
    const auto &info = LS->info;
    const auto &loopLevel = info.static_info().loop_level();
    // Update the address dependence information.
    for (const auto &baseId : info.chosen_base_streams()) {
      if (auto baseS = this->se->tryGetStream(baseId.id())) {
        assert(baseS != this &&
               "Should never have circular address dependency.");
        this->addBaseStream(StreamDepEdge::TypeE::Addr, false /* isInnerLoop */,
                            baseId.id(), info.id(), baseS);
      }
    }

    // Update the value dependence information.
    for (const auto &baseId :
         info.static_info().compute_info().value_base_streams()) {
      auto baseS = this->se->getStream(baseId.id());
      if (baseS == this) {
        if (!this->getEnabledLoadFunc()) {
          // LoadCompute may depend on other LogicalStreams.
          S_PANIC(this, "Circular value dependence found.");
        }
      } else {
        this->addBaseStream(StreamDepEdge::TypeE::Value,
                            false /* isInnerLoop */, baseId.id(), info.id(),
                            baseS);
      }
    }

    // Update the back dependence information.
    for (const auto &baseId : info.chosen_back_base_streams()) {
      assert(this->getStreamType() == ::LLVM::TDG::StreamInfo_Type_IV &&
             "Only phi node can have back edge dependence.");
      if (this->logicals.size() != 1) {
        S_PANIC(this,
                "More than one logical stream has back edge dependence.\n");
      }
      auto baseS = this->se->getStream(baseId.id());
      this->addBaseStream(StreamDepEdge::TypeE::Back, false /* isInnerLoop */,
                          baseId.id(), info.id(), baseS);
    }

    // Update the predicate information.
    for (int i = 0; i < info.static_info().predicated_streams_size(); ++i) {
      const auto &predInfo = info.static_info().predicated_streams(i);
      auto predS = this->se->tryGetStream(predInfo.id().id());
      if (predS) {
        predS->addPredBaseStream(predInfo.pred_true(), false /* isInnerLoop */,
                                 info.id(), predInfo.id().id(), this);
      }
    }

    // Reduction stream always has myself as the back base stream.
    if (this->isReduction()) {
      this->addBaseStream(StreamDepEdge::TypeE::Back, false /* isInnerLoop */,
                          info.id(), info.id(), this);
    }

    // Try to update the step root stream.
    for (auto &baseS : this->addrBaseStreams) {
      if (baseS->getLoopLevel() != loopLevel) {
        continue;
      }
      if (baseS->stepRootStream != nullptr) {
        if (this->stepRootStream != nullptr &&
            this->stepRootStream != baseS->stepRootStream) {
          S_PANIC(this,
                  "Double StepRootStream found in Base %s %u %u: Mine %s vs "
                  "New %s.\n",
                  baseS->getStreamName(), baseS->getLoopLevel(), loopLevel,
                  this->stepRootStream->getStreamName(),
                  baseS->stepRootStream->getStreamName());
        }
        this->stepRootStream = baseS->stepRootStream;
      }
    }
  }
  // Remember to set step root stream.
  // If there are no AddrBaseStreams, we take it as StepRoot.
  if (this->addrBaseStreams.empty()) {
    this->stepRootStream = this;
  }
}

void Stream::initializeAliasStreams() {
  // First sanity check for alias stream information.
  const auto &primeSSI = this->primeLogical->info.static_info();
  const auto &aliasBaseStreamId = primeSSI.alias_base_stream();
  for (auto &LS : this->logicals) {
    const auto &SSI = LS->info.static_info();
    if (SSI.alias_base_stream().id() != aliasBaseStreamId.id()) {
      S_PANIC(
          this,
          "Mismatch AliasBaseStream %llu (prime %llu) %llu (logical %llu).\n",
          aliasBaseStreamId.id(), this->primeLogical->getStreamId(),
          SSI.alias_base_stream().id(), LS->getStreamId());
    }
  }
  this->initializeAliasStreamsFromProtobuf(primeSSI);
}

/**
 * Only to configure all the history.
 */
void Stream::configure(uint64_t seqNum, ThreadContext *tc) {
  this->dispatchStreamConfig(seqNum, tc);
  if (se->isTraceSim()) {
    // We still use the trace based history address.
    for (auto &S : this->logicals) {
      S->history->configure();
      S->patternStream->configure();
    }
  }
}

bool Stream::isContinuous() const {
  const auto &pattern = this->primeLogical->patternStream->getPattern();
  if (pattern.val_pattern() != "LINEAR") {
    return false;
  }
  return this->getMemElementSize() == pattern.stride_i();
}

void Stream::setupAddrGen(DynStream &dynStream,
                          const DynStreamParamV *inputVec) {

  if (se->isTraceSim()) {
    if (this->getNumLogicalStreams() == 1) {
      // For simplicity.
      dynStream.addrGenCallback =
          this->primeLogical->history->allocateCallbackAtInstance(
              dynStream.dynStreamId.streamInstance);
    } else {
      S_PANIC(this, "Cannot setup addr gen for trace coalesced stream so far.");
    }
  } else {
    // We generate the address based on the primeLogical.
    assert(inputVec && "Missing InputVec.");
    const auto &info = this->primeLogical->info;
    const auto &staticInfo = info.static_info();
    const auto &pattern = staticInfo.iv_pattern();
    if (pattern.val_pattern() == ::LLVM::TDG::StreamValuePattern::LINEAR) {
      this->setupLinearAddrFunc(dynStream, inputVec, info);
      return;
    } else {
      // See there is an address function.
      const auto &addrFuncInfo = info.addr_func_info();
      if (addrFuncInfo.name() != "") {
        this->setupFuncAddrFunc(dynStream, inputVec, info);
        return;
      }
    }
  }
}

bool Stream::isReductionDistributable() const {
  if (!this->isReduction()) {
    return false;
  }
  auto reduceOp = this->getAddrFuncComputeOp();
  if (reduceOp == ::LLVM::TDG::ExecFuncInfo_ComputeOp_FLOAT_ADD ||
      reduceOp == ::LLVM::TDG::ExecFuncInfo_ComputeOp_INT_ADD) {
    // Supported float addition.
    return true;
  } else if (this->getStreamName().find("bfs_pull") != std::string::npos ||
             this->getStreamName().find("mm_inner") != std::string::npos) {
    /**
     * ! We manually enable this for bfs_pull and mm_inner.
     */
    return true;
  } else {
    return false;
    ;
  }
}

uint64_t Stream::getFootprint(unsigned cacheBlockSize) const {
  /**
   * Estimate the memory footprint for this stream in number of unqiue cache
   * blocks. It is OK for us to under-estimate the footprint, as the cache will
   * try to cache a stream with low-memory footprint.
   */
  const auto &pattern = this->primeLogical->patternStream->getPattern();
  const auto totalElements =
      this->primeLogical->history->getCurrentStreamLength();
  if (pattern.val_pattern() == "LINEAR") {
    // One dimension linear stream.
    return totalElements * pattern.stride_i() / cacheBlockSize;
  } else if (pattern.val_pattern() == "QUARDRIC") {
    // For 2 dimention linear stream, first compute footprint of one row.
    auto rowFootprint =
        pattern.ni() * this->getMemElementSize() / cacheBlockSize;
    if (pattern.stride_i() > cacheBlockSize) {
      rowFootprint = pattern.ni();
    }
    /**
     * Now we check if there is any chance that the next row will overlap with
     * the previous row.
     */
    auto rowRange = std::abs(pattern.stride_i()) * pattern.ni();
    if (std::abs(pattern.stride_j()) < rowRange) {
      // There is a chance that the next row will overlap with the previous one.
      // Return one row footprint as an under-estimation.
      return rowFootprint;
    } else {
      // No chance of overlapping.
      return rowFootprint * (totalElements / pattern.ni());
    }
  } else {
    // For all other patterns, underestimate.
    return 1;
  }
}

uint64_t Stream::getTrueFootprint() const {
  return this->primeLogical->history->getNumCacheLines();
}

uint64_t Stream::getStreamLengthAtInstance(uint64_t streamInstance) const {
  panic("Coalesced stream length at instance is not supported yet.\n");
}

void Stream::getCoalescedOffsetAndSize(uint64_t streamId, int32_t &offset,
                                       int32_t &size) const {
  for (auto LS : this->logicals) {
    if (LS->getStreamId() == streamId) {
      offset = LS->getCoalesceOffset() - this->baseOffset;
      size = LS->getMemElementSize();
      return;
    }
  }
  S_PANIC(this, "Failed to find logical stream %llu.\n", streamId);
}

bool Stream::tryGetCoalescedOffsetAndSize(uint64_t streamId, int32_t &offset,
                                          int32_t &size) const {
  for (auto LS : this->logicals) {
    if (LS->getStreamId() == streamId) {
      offset = LS->getCoalesceOffset() - this->baseOffset;
      size = LS->getMemElementSize();
      return true;
    }
  }
  return false;
}
