#ifndef __GEM_FORGE_ACCELERATOR_DYN_STREAM_HH__
#define __GEM_FORGE_ACCELERATOR_DYN_STREAM_HH__

#include "fifo_entry_idx.hh"

#include "addr_gen_callback.hh"

#include <memory>
#include <vector>

class ThreadContext;

class StreamElement;

/**
 * Holds some information of a dynamic instance of a stream,
 * e.g. callback to generate addresses.
 */
struct DynamicStream {

  const DynamicStreamId dynamicStreamId;
  const uint64_t configSeqNum;
  ThreadContext *tc;
  const FIFOEntryIdx prevFIFOIdx;

  /**
   * Head is the newest element.
   * Tail is the dummy node before the oldest element.
   */
  StreamElement *head;
  StreamElement *stepped;
  StreamElement *tail;
  int allocSize = 0;
  int stepSize = 0;
  FIFOEntryIdx FIFOIdx;
  Cycles lastStepCycle = Cycles(0);

  // Whether the dynamic stream is offloaded to cache.
  bool offloadedToCache = false;

  // Whether the StreamConfig has executed (ready to go).
  bool configExecuted = false;

  // Whether the StreamEnd has dispatched (waiting to be released).
  bool endDispatched = false;

  // Params used to compute address.
  DynamicStreamFormalParamV formalParams;

  // Callback to generate the address.
  AddrGenCallbackPtr addrGenCallback;

  DynamicStream(const DynamicStreamId &_dynamicStreamId, uint64_t _configSeqNum,
                ThreadContext *_tc, const FIFOEntryIdx &_prevFIFOIdx,
                StreamElement *nilTail)
      : dynamicStreamId(_dynamicStreamId), configSeqNum(_configSeqNum), tc(_tc),
        prevFIFOIdx(_prevFIFOIdx), head(nilTail), stepped(nilTail),
        tail(nilTail), FIFOIdx(_dynamicStreamId, _configSeqNum) {}

  /***********************************************************************
   * API to manage the elements of this stream.
   ***********************************************************************/
  /**
   * Get the first unstepped element of the last dynamic stream.
   */
  StreamElement *getFirstUnsteppedElement();
  /**
   * Get previous element in the chain of the stream.
   * Notice that it may return the (dummy) element->stream->tail if this is
   * the first element for that stream.
   */
  StreamElement *getPrevElement(StreamElement *element);
  /**
   * Remove one unstepped element from the last dynamic stream.
   */
  StreamElement *releaseElementUnstepped();

  void dump() const;
};

#endif