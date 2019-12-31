#ifndef __CPU_TDG_ACCELERATOR_STREAM_MLC_DYNAMIC_STREAM_H__
#define __CPU_TDG_ACCELERATOR_STREAM_MLC_DYNAMIC_STREAM_H__

#include "cpu/gem_forge/accelerator/stream/stream.hh"

#include "mem/ruby/common/DataBlock.hh"

// Generated by slicc.
#include "mem/ruby/protocol/ResponseMsg.hh"

#include <list>

class AbstractStreamAwareController;
class MessageBuffer;

class MLCDynamicStream {
public:
  MLCDynamicStream(CacheStreamConfigureData *_configData,
                   AbstractStreamAwareController *_controller,
                   MessageBuffer *_responseMsgBuffer,
                   MessageBuffer *_requestToLLCMsgBuffer);

  virtual ~MLCDynamicStream();

  Stream *getStaticStream() const { return this->stream; }

  const DynamicStreamId &getDynamicStreamId() const {
    return this->dynamicStreamId;
  }

  virtual const DynamicStreamId &getRootDynamicStreamId() const {
    // By default this we are the root stream.
    return this->getDynamicStreamId();
  }

  /**
   * Helper function to check if a slice is valid within this stream context.
   * So far always valid, except the first element of indirect stream that is
   * behind by one iteration.
   */
  virtual bool isSliceValid(const DynamicStreamSliceId &sliceId) const {
    return true;
  }

  /**
   * Get where is the LLC stream is at the end of current allocated credits.
   */
  virtual Addr getLLCStreamTailPAddr() const {
    panic("Should only call this on direct stream.");
  }

  virtual void receiveStreamData(const ResponseMsg &msg) = 0;
  void receiveStreamRequest(const DynamicStreamSliceId &sliceId);
  void receiveStreamRequestHit(const DynamicStreamSliceId &sliceId);

  /**
   * Before end the stream, we have make dummy response to the request
   * we have seen to make the ruby system happy.
   */
  void endStream();

protected:
  Stream *stream;
  DynamicStreamId dynamicStreamId;
  bool isPointerChase;

  AbstractStreamAwareController *controller;
  MessageBuffer *responseMsgBuffer;
  MessageBuffer *requestToLLCMsgBuffer;
  const uint64_t maxNumSlices;
  /**
   * Represent an allocated stream slice at MLC.
   * Used as a meeting point for the request from core
   * and data from LLC stream engine.
   */
  struct MLCStreamSlice {
    DynamicStreamSliceId sliceId;
    DataBlock dataBlock;
    // Whether the core's request is already here.
    bool dataReady;
    enum CoreStatusE { NONE, WAIT, DONE, FAULTED };
    CoreStatusE coreStatus;
    // For debug purpose, we also remember core's request sliceId.
    DynamicStreamSliceId coreSliceId;

    MLCStreamSlice(const DynamicStreamSliceId &_sliceId)
        : sliceId(_sliceId), dataBlock(), dataReady(false),
          coreStatus(CoreStatusE::NONE) {}

    void setData(const DataBlock &dataBlock) {
      assert(!this->dataReady && "Data already ready.");
      this->dataBlock = dataBlock;
      this->dataReady = true;
    }

    static std::string convertCoreStatusToString(CoreStatusE status) {
      switch (status) {
      case CoreStatusE::NONE:
        return "NONE";
      case CoreStatusE::WAIT:
        return "WAIT";
      case CoreStatusE::DONE:
        return "DONE";
      case CoreStatusE::FAULTED:
        return "FAULTED";
      default:
        return "ILLEGAL";
      }
    }
  };

  std::deque<MLCStreamSlice> slices;
  // Element index of allocated [head, tail).
  uint64_t headSliceIdx;
  uint64_t tailSliceIdx;

  EventFunctionWrapper advanceStreamEvent;

  virtual void advanceStream() = 0;
  void makeResponse(MLCStreamSlice &element);

  MLCStreamSlice &getSlice(uint64_t sliceIdx);
  const MLCStreamSlice &getSlice(uint64_t sliceIdx) const;

  /**
   * API for this to check if overflowed.
   */
  virtual bool hasOverflowed() const = 0;
  virtual int64_t getTotalTripCount() const = 0;
  virtual bool matchSliceId(const DynamicStreamSliceId &A,
                            const DynamicStreamSliceId &B) const {
    // By default match the vaddr.
    // TODO: This is really wrong.
    return A.vaddr == B.vaddr;
  }

  /**
   * Helper function to translate the vaddr to paddr.
   */
  Addr translateVAddr(Addr vaddr) const;

  /**
   * Map paddr line to LLC bank.
   */
  MachineID mapPAddrToLLCBank(Addr paddr) const;

  /**
   * Pop slices.
   */
  void popStream();

  /**
   * A helper function to dump some basic status of the stream when panic.
   */
  void panicDump() const;
};

#endif