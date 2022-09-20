#ifndef __CPU_GEM_FORGE_MLC_DYN_INDIRECT_STREAM_H__
#define __CPU_GEM_FORGE_MLC_DYN_INDIRECT_STREAM_H__

#include "MLCDynStream.hh"

/**
 * MLCDynIndirectStream is a special stream.
 * 1. It does not send credit to LLC. The direct stream should perform the flow
 * control for the indirect stream.
 * 2. It always allocates elements one by one. No merge elements even if they
 * are from the same cache line, i.e. one slice must belong to one single
 * element.
 * 3. Due to coalescing, an indirect stream element may span multiple cache
 * lines. So we need to match lhsElementIdx and vaddr to find the correct slice.
 *
 * Indirect streams are more complicated to manage, as the address is only know
 * when the base stream data is ready.
 */
class MLCDynIndirectStream : public MLCDynStream {
public:
  MLCDynIndirectStream(CacheStreamConfigureDataPtr _configData,
                       AbstractStreamAwareController *_controller,
                       MessageBuffer *_responseMsgBuffer,
                       MessageBuffer *_requestToLLCMsgBuffer,
                       const DynStreamId &_rootStreamId);

  virtual ~MLCDynIndirectStream() {}

  const DynStreamId &getRootDynStreamId() const override {
    return this->rootStreamId;
  }

  bool isSliceValid(const DynStreamSliceId &sliceId) const override {
    assert(sliceId.getNumElements() == 1 &&
           "Multiple elements for indirect stream.");
    if (this->isOneIterationBehind) {
      if (sliceId.getStartIdx() == 0) {
        return false;
      }
    }
    return true;
  }

  /**
   * Receive data from LLC.
   */
  void receiveStreamData(const DynStreamSliceId &sliceId,
                         const DataBlock &dataBlock, Addr paddrLine) override;

  /**
   * Receive data from the base direct stream.
   * Try to generate more slices.
   */
  void receiveBaseStreamData(uint64_t baseStrandElemIdx, uint64_t baseData,
                             bool tryAdvance);

  void setBaseStream(MLCDynStream *baseStream) {
    assert(!this->baseStream && "Already has base stream.");
    this->baseStream = baseStream;
  }

  bool hasOverflowed() const override;
  /**
   * We query the DirectStream for TotalTripCount.
   */
  int64_t getTotalTripCount() const override {
    return this->baseStream->getTotalTripCount();
  }
  bool hasTotalTripCount() const override {
    return this->baseStream->hasTotalTripCount();
  }
  int64_t getInnerTripCount() const override {
    return this->baseStream->getInnerTripCount();
  }
  bool hasInnerTripCount() const override {
    return this->baseStream->hasInnerTripCount();
  }
  void setTotalTripCount(int64_t totalTripCount, Addr brokenPAddr,
                         MachineType brokenMachineType) override;

private:
  // Remember the root stream id.
  DynStreamId rootStreamId;
  DynStreamFormalParamV formalParams;
  AddrGenCallbackPtr addrGenCallback;
  const int32_t elementSize;
  MLCDynStream *baseStream = nullptr;

  // Remember if this indirect stream is behind one iteration.
  bool isOneIterationBehind;

  /**
   * The tail ElementIdx (Not allocated yet).
   * This is not the same as tailSliceIdx due to coalesced indirect stream:
   * a[b[i] + 0]; a[b[i] + 1];
   */
  uint64_t tailElementIdx;

  SliceIter findSliceForCoreRequest(const DynStreamSliceId &sliceId) override;

  void advanceStream() override;
  void allocateSlice();
  Addr genElemVAddr(uint64_t strandElemIdx, uint64_t baseData);

  /**
   * Generate or fill in the VAddr of one slice.
   */
  void fillElemVAddr(uint64_t strandElemIdx, uint64_t baseData);

  /**
   * Find the begin and end of the substream that belongs to an elementIdx.
   */
  std::pair<SliceIter, SliceIter> findSliceByElementIdx(uint64_t elementIdx);

  /**
   * Try to find the slice with the given vaddr. If failed, insert one and
   * maintain the sorted order.
   */
  SliceIter findOrInsertSliceBySliceId(const SliceIter &begin,
                                       const SliceIter &end,
                                       const DynStreamSliceId &sliceId);

  /**
   * Intercept the final reduction value.
   * @return whether this is handled as the FinalReductionValue.
   */
  bool receiveFinalReductionValue(const DynStreamSliceId &sliceId,
                                  const DataBlock &dataBlock, Addr paddrLine);
};

#endif