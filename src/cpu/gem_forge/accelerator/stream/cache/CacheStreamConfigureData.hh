#ifndef __CPU_TDG_ACCELERATOR_STREAM_CACHE_STREAM_CONFIGURE_DATA_H__
#define __CPU_TDG_ACCELERATOR_STREAM_CACHE_STREAM_CONFIGURE_DATA_H__

#include "cpu/gem_forge/accelerator/stream/StreamMessage.pb.h"

#include "DynamicStreamId.hh"

#include "base/types.hh"

#include <memory>

class Stream;

struct CacheStreamConfigureData {
public:
  using HistoryPtr = std::shared_ptr<::LLVM::TDG::StreamHistory>;
  CacheStreamConfigureData(Stream *_stream, const DynamicStreamId &_dynamicId,
                           HistoryPtr _history);
  CacheStreamConfigureData(const CacheStreamConfigureData &other);

  Stream *stream;
  const DynamicStreamId dynamicId;
  int elementSize;

  HistoryPtr history;
  Addr initVAddr;
  Addr initPAddr;

  /**
   * The above basically represent a direct stream.
   * We allow one additional indirect stream so far.
   * TODO: Support multiple indirect streams.
   */
  Stream *indirectStream;
  DynamicStreamId indirectDynamicId;
  HistoryPtr indirectHistory;

  // Set by the MLC stream, for flow control.
  int initAllocatedIdx;
};

#endif