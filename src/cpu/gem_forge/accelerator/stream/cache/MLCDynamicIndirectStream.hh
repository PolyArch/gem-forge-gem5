#ifndef __CPU_TDG_ACCELERATOR_STREAM_MLC_DYNAMIC_INDIRECT_STREAM_H__
#define __CPU_TDG_ACCELERATOR_STREAM_MLC_DYNAMIC_INDIRECT_STREAM_H__

#include "MLCDynamicStream.hh"

/**
 * MLCDynamicIndirectStream is a special stream.
 * 1. It does not send credit to LLC. The direct stream should perform the flow
 * control for the indirect stream.
 * 2. It always allocates elements one by one. No merge elements if they are
 * from the same cache line.
 */
class MLCDynamicIndirectStream : public MLCDynamicStream {
public:
  MLCDynamicIndirectStream(CacheStreamConfigureData *_configData,
                           AbstractStreamAwareController *_controller,
                           MessageBuffer *_responseMsgBuffer,
                           MessageBuffer *_requestToLLCMsgBuffer,
                           const DynamicStreamId &_rootStreamId);

  virtual ~MLCDynamicIndirectStream() {}

  const DynamicStreamId &getRootDynamicStreamId() const override {
    return this->rootStreamId;
  }

  void receiveStreamData(const ResponseMsg &msg) override;

private:
  // Remember the root stream id.
  DynamicStreamId rootStreamId;
  /**
   * Override the basic behavior, allocate elements one by one.
   */
  void allocateElement() override;

  /**
   * Override the basic behavior, never really sends out the credit.
   */
  void sendCreditToLLC() override;
};

#endif