#include "CacheStreamConfigureData.hh"
#include "../stream.hh"

#include "debug/LLCRubyStreamBase.hh"
#include "debug/MLCRubyStrandSplit.hh"
#include "debug/StreamLoopBound.hh"

#define DEBUG_TYPE StreamLoopBound
#include "../stream_log.hh"

namespace gem5 {

CacheStreamConfigureData::CacheStreamConfigureData(
    Stream *_stream, const DynStreamId &_dynamicId, int _elementSize,
    const DynStreamFormalParamV &_addrGenFormalParams,
    AddrGenCallbackPtr _addrGenCallback)
    : stream(_stream), dynamicId(_dynamicId), elementSize(_elementSize),
      initVAddr(0), initPAddr(0), addrGenFormalParams(_addrGenFormalParams),
      addrGenCallback(_addrGenCallback), isPointerChase(false),
      isOneIterationBehind(false), initCreditedIdx(0) {
  assert(this->addrGenCallback && "Invalid addrGenCallback.");
}

CacheStreamConfigureData::~CacheStreamConfigureData() {}

CacheStreamConfigureDataPtr CacheStreamConfigureData::getUsedByBaseConfig() {
  for (auto &edge : this->baseEdges) {
    if (!edge.isUsedBy) {
      continue;
    }
    auto baseConfig = edge.data.lock();
    if (!baseConfig) {
      DYN_S_PANIC(this->dynamicId, "UsedByBaseConfig %s already released.",
                  edge.dynStreamId);
    }
    return baseConfig;
  }
  DYN_S_PANIC(this->dynamicId, "Failed to get UsedByBaseConfig.");
}

void CacheStreamConfigureData::addUsedBy(CacheStreamConfigureDataPtr &data,
                                         int reuse, bool predBy, int predId,
                                         bool predValue) {
  StreamReuseInfo reuseInfo(reuse);
  this->addUsedBy(data, reuseInfo, predBy, predId, predValue);
}

void CacheStreamConfigureData::addUsedBy(CacheStreamConfigureDataPtr &data,
                                         const StreamReuseInfo &reuseInfo,
                                         bool predBy, int predId,
                                         bool predValue) {
  int skip = 0;
  this->depEdges.emplace_back(DepEdge::Type::UsedBy, data, reuseInfo, skip);
  data->baseEdges.emplace_back(BaseEdge::Type::BaseOn, this->shared_from_this(),
                               reuseInfo, skip, true /* isUsedBy */);
  if (predBy) {
    data->baseEdges.back().isPredBy = true;
    data->baseEdges.back().predId = predId;
    data->baseEdges.back().predValue = predValue;
  }
}

void CacheStreamConfigureData::addSendTo(CacheStreamConfigureDataPtr &data,
                                         int reuse, int skip) {
  StreamReuseInfo reuseInfo(reuse);
  this->addSendTo(data, reuseInfo, skip);
}

void CacheStreamConfigureData::addSendTo(CacheStreamConfigureDataPtr &data,
                                         const StreamReuseInfo &reuseInfo,
                                         int skip) {
  for (const auto &edge : this->depEdges) {
    if (edge.type == DepEdge::Type::SendTo && edge.data == data) {
      // This is already here.
      assert(edge.reuseInfo == reuseInfo && "Mismatch Reuse in SendTo.");
      assert(edge.skip == skip && "Mismatch Skip in SendTo.");
      return;
    }
  }
  this->depEdges.emplace_back(DepEdge::Type::SendTo, data, reuseInfo, skip);
}

void CacheStreamConfigureData::addPUMSendTo(
    const CacheStreamConfigureDataPtr &data, const AffinePattern &broadcastPat,
    const AffinePattern &recvPat, const AffinePattern &recvTile) {
  StreamReuseInfo reuseInfo;
  this->depEdges.emplace_back(
      CacheStreamConfigureData::DepEdge::Type::PUMSendTo, data, reuseInfo,
      0 /* skip */);
  this->depEdges.back().broadcastPat = broadcastPat;
  this->depEdges.back().recvPat = recvPat;
  this->depEdges.back().recvTile = recvTile;
}

void CacheStreamConfigureData::addBaseOn(CacheStreamConfigureDataPtr &data,
                                         int reuse, int skip) {
  StreamReuseInfo reuseInfo(reuse);
  this->addBaseOn(data, reuseInfo, skip);
}

void CacheStreamConfigureData::addBaseOn(CacheStreamConfigureDataPtr &data,
                                         const StreamReuseInfo &reuseInfo,
                                         int skip) {
  if (reuseInfo.getTotalReuse() <= 0 || skip < 0) {
    panic("Illegal BaseOn Reuse %s Skip %d This %s -> Base %s.", reuseInfo,
          skip, this->dynamicId, data->dynamicId);
  }
  this->baseEdges.emplace_back(BaseEdge::Type::BaseOn, data, reuseInfo, skip);
}

void CacheStreamConfigureData::addBaseAffineIV(
    CacheStreamConfigureDataPtr &data, int reuse, int skip) {
  StreamReuseInfo reuseInfo(reuse);
  this->addBaseAffineIV(data, reuseInfo, skip);
}

void CacheStreamConfigureData::addBaseAffineIV(
    CacheStreamConfigureDataPtr &data, const StreamReuseInfo &reuseInfo,
    int skip) {
  if (reuseInfo.getTotalReuse() <= 0 || skip < 0) {
    panic("Illegal BaseAffineIV Reuse %s Skip %d This %s -> Base %s.",
          reuseInfo, skip, this->dynamicId, data->dynamicId);
  }
  this->baseEdges.emplace_back(data, reuseInfo, skip);
}

void CacheStreamConfigureData::addPredBy(CacheStreamConfigureDataPtr &data,
                                         int reuse, int skip, int predFuncId,
                                         bool predValue) {
  StreamReuseInfo reuseInfo(reuse);
  this->addPredBy(data, reuseInfo, skip, predFuncId, predValue);
}

void CacheStreamConfigureData::addPredBy(CacheStreamConfigureDataPtr &data,
                                         const StreamReuseInfo &reuseInfo,
                                         int skip, int predFuncId,
                                         bool predValue) {
  if (reuseInfo.getTotalReuse() <= 0 || skip < 0) {
    panic("Illegal PredBy Reuse %s Skip %d This %s -> Base %s.", reuseInfo,
          skip, this->dynamicId, data->dynamicId);
  }
  this->baseEdges.emplace_back(data, reuseInfo, skip, predValue, predFuncId);
}

uint64_t CacheStreamConfigureData::convertBaseToDepElemIdx(
    uint64_t baseElemIdx, const StreamReuseInfo &reuseInfo, int skip) {
  auto depElemIdx = baseElemIdx;
  if (reuseInfo.hasReuse()) {
    assert(skip == 0);
    depElemIdx = reuseInfo.convertBaseToDepElemIdx(baseElemIdx);
  }
  if (skip != 0) {
    assert(!reuseInfo.hasReuse());
    depElemIdx = baseElemIdx / skip;
  }
  return depElemIdx;
}

uint64_t CacheStreamConfigureData::convertDepToBaseElemIdx(
    uint64_t depElemIdx, const StreamReuseInfo &reuseInfo, int skip) {
  auto baseElemIdx = depElemIdx;
  if (reuseInfo.hasReuse()) {
    assert(skip == 0);
    baseElemIdx = reuseInfo.convertDepToBaseElemIdx(depElemIdx);
  }
  if (skip != 0) {
    assert(!reuseInfo.hasReuse());
    baseElemIdx = depElemIdx * skip;
  }
  return baseElemIdx;
}

bool CacheStreamConfigureData::sendToInnerLoopStream() const {
  auto check = [this](const CacheStreamConfigureData &config) -> bool {
    for (const auto &e : config.depEdges) {
      if (e.type == CacheStreamConfigureData::DepEdge::Type::SendTo) {
        const auto &recvConfig = e.data;
        if (recvConfig->stream->getLoopLevel() > this->stream->getLoopLevel()) {
          return true;
        }
      }
    }
    return false;
  };
  if (check(*this)) {
    return true;
  }
  for (const auto &e : this->depEdges) {
    if (e.type == CacheStreamConfigureData::DepEdge::Type::UsedBy) {
      const auto &indConfig = e.data;
      if (check(*indConfig)) {
        return true;
      }
    }
  }
  return false;
}

DynStreamFormalParamV
CacheStreamConfigureData::splitLinearParam1D(const StrandSplitInfo &strandSplit,
                                             int strandIdx) {

  const auto &params = this->addrGenFormalParams;
  auto callback = this->addrGenCallback;

  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(callback);
  assert(linearAddrGen && "Callback is not linear.");
  assert(params.size() == 3 && "Only support 1D linear pattern so far.");

  /**
   * * Split an 1D stream pattern of:
   * *   start : stride : tripCount
   * * ->
   * *   start + strandIdx * interleave * stride
   * * : stride
   * * : interleave
   * * : totalStrands * interleave * stride
   * * : strandTripCount
   */
  auto start = params.at(2).invariant.uint64();
  auto stride = params.at(0).invariant.uint64();
  auto tripCount = params.at(1).invariant.uint64();
  auto interleave = strandSplit.getInterleave();
  auto totalStrands = strandSplit.getTotalStrands();
  auto strandTripCount = strandSplit.getStrandTripCount(tripCount, strandIdx);

  if (strandTripCount >= interleave) {
    assert(strandTripCount % interleave == 0);
  }

  auto strandStart = start + strandIdx * interleave * stride;
  auto strandStride = totalStrands * interleave * stride;

  DynStreamFormalParamV strandParams;

#define addStrandParam(x)                                                      \
  {                                                                            \
    strandParams.emplace_back();                                               \
    strandParams.back().isInvariant = true;                                    \
    strandParams.back().invariant.uint64() = x;                                \
  }
  addStrandParam(stride);
  if (strandTripCount < interleave) {
    addStrandParam(strandTripCount);
  } else {
    addStrandParam(interleave);
  }
  addStrandParam(strandStride);
  addStrandParam(strandTripCount);
  addStrandParam(strandStart);
  DYN_S_DPRINTF_(MLCRubyStrandSplit, this->dynamicId, "Split 1D Continuous.\n");
  DYN_S_DPRINTF_(MLCRubyStrandSplit, this->dynamicId,
                 "start %#x stride %d tripCount %llu.\n", start, stride,
                 tripCount);
  DYN_S_DPRINTF_(MLCRubyStrandSplit, this->dynamicId,
                 "interleave %d totalStrands %llu.\n", interleave,
                 totalStrands);
  DYN_S_DPRINTF_(MLCRubyStrandSplit, this->dynamicId,
                 "strandStart %#x strandStride %d strandTripCount %lu.\n",
                 strandStart, strandStride, strandTripCount);

  return strandParams;
}

DynStreamFormalParamV CacheStreamConfigureData::splitAffinePatternByElem(
    int64_t startElem, int64_t endElem, int strandIdx, int totalStrands) {
  /**
   * * This is used to implement the "ByElem" StrandSplit, used to increase
   * * parallelism for edge list streams in graph workloads.
   *
   * * So far we assume the streams are 1D pattern with:
   * * start : S1 : T1
   *
   * * ->
   * * start + startElem * S1 : S1 : endElem - startElem
   */

  const auto &params = this->addrGenFormalParams;
  auto callback = this->addrGenCallback;

  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(callback);
  assert(linearAddrGen && "Callback is not linear.");
  assert(params.size() == 3);

  // Copy the original params.
  DynStreamFormalParamV strandParams = this->addrGenFormalParams;

#define setTrip(dim, t)                                                        \
  {                                                                            \
    strandParams.at((dim) * 2 + 1).isInvariant = true;                         \
    strandParams.at((dim) * 2 + 1).invariant.uint64() = t;                     \
  }
#define setStride(dim, t)                                                      \
  {                                                                            \
    strandParams.at((dim) * 2).isInvariant = true;                             \
    strandParams.at((dim) * 2).invariant.uint64() = t;                         \
  }
#define setStart(t)                                                            \
  {                                                                            \
    strandParams.back().isInvariant = true;                                    \
    strandParams.back().invariant.uint64() = t;                                \
  }

  auto stride = params.at(0).invariant.int64();
  auto start = params.back().invariant.uint64();
  [[maybe_unused]] auto trip = params.at(1).invariant.uint64();

  assert(endElem <= trip);
  assert(startElem < endElem);

  auto strandStart = start + startElem * stride;
  auto strandStride = stride;
  auto strandTrip = endElem - startElem;

  setTrip(0, strandTrip);
  setStride(0, strandStride);
  setStart(strandStart);

#undef setTrip
#undef setStride
#undef setStart

  return strandParams;
}

DynStreamFormalParamV CacheStreamConfigureData::splitAffinePatternAtDim(
    int splitDim, int64_t interleave, int strandIdx, int totalStrands) {
  const auto &params = this->addrGenFormalParams;
  auto callback = this->addrGenCallback;

  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(callback);
  assert(linearAddrGen && "Callback is not linear.");

  /**
   * * Split an AffineStream at SplitDim. This is similar to OpenMP static
   * * scheduling.
   * *   start : S1 : T1 : ... : Ss : Ts : ... : Sn : Tn
   *
   * * We assume interleave % (T1 * ... * Ts-1) == 0, and define
   * * Tt = interleave / (T1 * ... * Ts-1)
   * * Tn = Tt * totalStrands
   *
   * * ->
   * *   start + strandIdx * Ss * Tt
   * * : S1      : T1 : ...
   * * : Ss      : Tt
   * * : Ss * Tn : Ts / Tn + (strandIdx < (Ts % Tn) ? 1 : 0) : ...
   * * : Sn      : Tn
   *
   * * Notice that we have to take care when Ts % Tn != 0 by adding one to
   * * strands with strandIdx < (Ts % Tn).
   */

  std::vector<uint64_t> trips;
  std::vector<int64_t> strides;
  uint64_t prevTrip = 1;
  assert((this->addrGenFormalParams.size() % 2) == 1);
  for (int i = 0; i + 1 < this->addrGenFormalParams.size(); i += 2) {
    const auto &s = this->addrGenFormalParams.at(i);
    assert(s.isInvariant);
    strides.push_back(s.invariant.int64());

    const auto &t = this->addrGenFormalParams.at(i + 1);
    assert(t.isInvariant);
    auto trip = t.invariant.uint64();
    trips.push_back(trip / prevTrip);
    prevTrip = trip;
  }
  assert(!trips.empty());
  assert(splitDim < trips.size());

  auto splitDimTrip = trips.at(splitDim);
  auto splitDimStride = strides.at(splitDim);

  auto innerTrip = 1;
  for (int i = 0; i < splitDim; ++i) {
    innerTrip *= trips.at(i);
  }
  assert(interleave % innerTrip == 0);
  auto intrlvTrip = interleave / innerTrip;
  auto totalIntrlvTrip = intrlvTrip * totalStrands;

  auto start = params.back().invariant.uint64();
  auto strandStart = start + strandIdx * splitDimStride * intrlvTrip;

  // Copy the original params.
  DynStreamFormalParamV strandParams = this->addrGenFormalParams;

#define setTrip(dim, t)                                                        \
  {                                                                            \
    strandParams.at((dim) * 2 + 1).isInvariant = true;                         \
    strandParams.at((dim) * 2 + 1).invariant.uint64() = t;                     \
  }
#define setStride(dim, t)                                                      \
  {                                                                            \
    strandParams.at((dim) * 2).isInvariant = true;                             \
    strandParams.at((dim) * 2).invariant.uint64() = t;                         \
  }
#define setStart(t)                                                            \
  {                                                                            \
    strandParams.back().isInvariant = true;                                    \
    strandParams.back().invariant.uint64() = t;                                \
  }

  // Insert another dimension after SplitDim.
  strandParams.insert(strandParams.begin() + 2 * splitDim + 1,
                      DynStreamFormalParam());
  strandParams.insert(strandParams.begin() + 2 * splitDim + 1,
                      DynStreamFormalParam());

  // Adjust the strand start.
  setStart(strandStart);

  int64_t splitOutTrip = 1;
  int64_t splitTrip = intrlvTrip;

  DYN_S_DPRINTF_(
      MLCRubyStrandSplit, this->dynamicId,
      "Intrlv %d IntrlvTrip %d SplitDimTrip %d TotalStrands %d Pat %s.\n",
      interleave, intrlvTrip, splitDimTrip, totalStrands,
      printAffinePatternParams(this->addrGenFormalParams));

  if (totalIntrlvTrip <= splitDimTrip) {
    // Compute the SplitOutTrip.
    auto remainderTrip = splitDimTrip % totalIntrlvTrip;
    if (remainderTrip % intrlvTrip != 0) {
      if (splitDim + 1 != trips.size()) {
        DYN_S_PANIC(this->dynamicId,
                    "Cannot handle remainderTrip %ld % intrlvTrip %ld != 0.",
                    remainderTrip, intrlvTrip);
      }
    }
    auto remainderStrandIdx = (remainderTrip + intrlvTrip - 1) / intrlvTrip;
    auto splitOutTripRemainder = (strandIdx < remainderStrandIdx) ? 1 : 0;
    splitOutTrip = splitDimTrip / totalIntrlvTrip + splitOutTripRemainder;

  } else {
    /**
     * Strands beyond FinalStrandIdx would have no trip count.
     */
    auto finalStrandIdx = splitDimTrip / intrlvTrip;
    if (strandIdx == finalStrandIdx) {
      splitTrip = splitDimTrip - finalStrandIdx * intrlvTrip;
    } else if (strandIdx > finalStrandIdx) {
      splitTrip = 0;
    }

    // In this case, SplitOutDimTrip is always 1.
    splitOutTrip = 1;
  }

  // Adjust the SplitOutDim.
  setTrip(splitDim, splitTrip * innerTrip);
  setStride(splitDim + 1, splitDimStride * totalIntrlvTrip);
  assert(splitOutTrip > 0);
  setTrip(splitDim + 1, splitOutTrip * splitTrip * innerTrip);

  // We need to fix all upper dimension's trip count.
  for (int dim = splitDim + 2; dim < trips.size() + 1; ++dim) {
    auto fixedOuterTrip =
        strandParams.at(dim * 2 - 1).invariant.uint64() * trips.at(dim - 1);
    setTrip(dim, fixedOuterTrip);
  }

#undef setTrip
#undef setStride
#undef setStart

  return strandParams;
}

DynStrandId CacheStreamConfigureData::getStrandIdFromStreamElemIdx(
    uint64_t streamElemIdx) const {
  if (this->streamConfig) {
    // This is a StrandConfig.
    return this->streamConfig->getStrandIdFromStreamElemIdx(streamElemIdx);
  }
  if (this->totalStrands == 1) {
    // There is no strand.
    return DynStrandId(this->dynamicId);
  } else {
    auto strandElemSplit = this->strandSplit.mapStreamToStrand(streamElemIdx);
    return DynStrandId(this->dynamicId, strandElemSplit.strandIdx,
                       this->strandSplit.getTotalStrands());
  }
}

uint64_t CacheStreamConfigureData::getStrandElemIdxFromStreamElemIdx(
    uint64_t streamElemIdx) const {
  if (this->streamConfig) {
    // This is a StrandConfig.
    return this->streamConfig->getStrandElemIdxFromStreamElemIdx(streamElemIdx);
  }
  if (this->totalStrands == 1) {
    // There is no strand.
    return streamElemIdx;
  } else {
    auto strandElemSplit = this->strandSplit.mapStreamToStrand(streamElemIdx);
    return strandElemSplit.elemIdx;
  }
}

uint64_t CacheStreamConfigureData::getStreamElemIdxFromStrandElemIdx(
    uint64_t strandElemIdx) const {
  if (!this->isSplitIntoStrands()) {
    // If not splitted, StrandElemIdx == StreamElemIdx.
    return strandElemIdx;
  }
  assert(this->streamConfig && "We need StrandConfig");
  // This is a strand.
  StrandElemSplitIdx elemSplit(this->strandIdx, strandElemIdx);
  return this->strandSplit.mapStrandToStream(elemSplit);
}

uint64_t CacheStreamConfigureData::getStreamElemIdxFromStrandElemIdx(
    const DynStrandId &strandId, uint64_t strandElemIdx) const {
  StrandElemSplitIdx elemSplit(strandId.strandIdx, strandElemIdx);
  return this->strandSplit.mapStrandToStream(elemSplit);
}

std::tuple<DynStrandId, uint64_t, Addr, ruby::MachineType>
CacheStreamConfigureData::translateSendToRecv(
    const DepEdge &sendToEdge, const CacheStreamConfigureDataPtr &sendConfig,
    uint64_t sendStrandElemIdx) {

  /**
   * Unlike sending data to MLC, we have to calculate the virtual
   * address of the receiving stream and translate that. Also, we can
   * only handle the simpliest case so far: no spliting, and no
   * multi-line receiver element.
   *
   * Now that we have strands, we have to be careful translating between
   * StreamElemIdx and StrandElemIdx.
   *
   *
   * If the RecvConfig is Strand, then we don't translate to StreamElemIdx.
   *
   * SendStrandElemIdx -> RecvStrandElemIdx
   */

  auto recvConfig = sendToEdge.data;
  /**
   * If RecvConfig is StreamConfig, we go through:
   * SendStrand -> SendStream -> RecvStream -> RecvStrand
   */
  if (!recvConfig->isStrandConfig()) {

    // SendStrandElemIdx -> SendStreamElemIdx.
    auto sendStreamElemIdx =
        sendConfig->getStreamElemIdxFromStrandElemIdx(sendStrandElemIdx);

    // SendStreamElemIdx -> RecvStreamElemIdx.
    auto recvStreamElemIdx = CacheStreamConfigureData::convertBaseToDepElemIdx(
        sendStreamElemIdx, sendToEdge.reuseInfo, sendToEdge.skip);

    // RecvStreamElemIdx -> RecvStrandElemIdx.
    auto recvStrandId =
        recvConfig->getStrandIdFromStreamElemIdx(recvStreamElemIdx);
    auto recvStrandElemIdx =
        recvConfig->getStrandElemIdxFromStreamElemIdx(recvStreamElemIdx);

    // Get the VAddr.
    auto recvElemVAddr =
        recvConfig->addrGenCallback
            ->genAddr(recvStreamElemIdx, recvConfig->addrGenFormalParams,
                      getStreamValueFail)
            .front();

    auto recvElemMachineType =
        recvConfig->floatPlan.getMachineTypeAtElem(recvStreamElemIdx);

    DYN_S_DPRINTF_(LLCRubyStreamBase, sendConfig->getStrandId(),
                   "[Fwd] SendStrnd %lu -> SendStrm %lu R/S %s/%ld -> "
                   "RecvStrm %lu -> RecvStrnd %s%lu %s.\n",
                   sendStrandElemIdx, sendStreamElemIdx, sendToEdge.reuseInfo,
                   sendToEdge.skip, recvStreamElemIdx, recvStrandId,
                   recvStrandElemIdx, recvElemMachineType);

    return std::make_tuple(recvStrandId, recvStrandElemIdx, recvElemVAddr,
                           recvElemMachineType);
  } else {
    // The RecvConfig is StrandConfig, we skip the stream part.
    auto recvStrandId = recvConfig->getStrandId();
    auto recvStrandElemIdx = CacheStreamConfigureData::convertBaseToDepElemIdx(
        sendStrandElemIdx, sendToEdge.reuseInfo, sendToEdge.skip);

    // Get the VAddr.
    auto recvElemVAddr =
        recvConfig->addrGenCallback
            ->genAddr(recvStrandElemIdx, recvConfig->addrGenFormalParams,
                      getStreamValueFail)
            .front();

    auto recvStreamElemIdx =
        recvConfig->getStreamElemIdxFromStrandElemIdx(recvStrandElemIdx);
    auto recvElemMachineType =
        recvConfig->streamConfig->floatPlan.getMachineTypeAtElem(
            recvStreamElemIdx);

    DYN_S_DPRINTF_(LLCRubyStreamBase, sendConfig->getStrandId(),
                   "[LLCFwd] SendStrnd %lu R/S %s/%ld -> RecvStrnd %s%lu %s.\n",
                   sendStrandElemIdx, sendToEdge.reuseInfo, sendToEdge.skip,
                   recvStrandId, recvStrandElemIdx, recvElemMachineType);

    return std::make_tuple(recvStrandId, recvStrandElemIdx, recvElemVAddr,
                           recvElemMachineType);
  }
}

} // namespace gem5
