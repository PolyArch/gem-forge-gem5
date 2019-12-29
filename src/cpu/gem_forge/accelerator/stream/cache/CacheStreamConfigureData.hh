#ifndef __CPU_TDG_ACCELERATOR_STREAM_CACHE_STREAM_CONFIGURE_DATA_H__
#define __CPU_TDG_ACCELERATOR_STREAM_CACHE_STREAM_CONFIGURE_DATA_H__

#include "cpu/gem_forge/accelerator/stream/dyn_stream.hh"

#include "DynamicStreamId.hh"

#include "base/types.hh"

#include <memory>

class Stream;

struct CacheStreamConfigureData {
public:
  CacheStreamConfigureData(
      Stream *_stream, const DynamicStreamId &_dynamicId, int _elementSize,
      const std::vector<DynamicStreamFormalParam> &_formalParams,
      AddrGenCallbackPtr _addrGenCallback);
  CacheStreamConfigureData(const CacheStreamConfigureData &other);

  Stream *stream;
  DynamicStreamId dynamicId;
  int elementSize;

  // NOTE: Line address here.
  Addr initVAddr;
  Addr initPAddr;
  bool initPAddrValid = false;

  /**
   * Migrate to AddrGenCallback.
   */
  DynamicStreamFormalParamV formalParams;
  AddrGenCallbackPtr addrGenCallback;
  int64_t totalTripCount = -1;

  /**
   * Whether this is a pointer chase stream.
   * TODO: This should be included in the future Pattern structure
   * TODO: when introducing execution-driven simulation.
   */
  bool isPointerChase;

  /**
   * NOTE: Only valid for indirect streams.
   * Sometimes the dependence between the indirect stream and the base stream
   * has a difference of one iteration, e.g. pointer chase base stream + an
   * indirect stream loading the value.
   */
  bool isOneIterationBehind;

  /**
   * The above basically represent a direct stream.
   * We allow one additional indirect stream so far.
   * TODO: Support multiple indirect streams.
   */
  std::shared_ptr<CacheStreamConfigureData> indirectStreamConfigure;

  // Set by the MLC stream, for flow control.
  int initAllocatedIdx;
};

#endif