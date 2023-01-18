
#ifndef __CPU_GEM_FORGE_DYN_STREAM_ID_HH__
#define __CPU_GEM_FORGE_DYN_STREAM_ID_HH__

#include <functional>
#include <iostream>

/**
 * Uniquely identifies a dynamic stream in the system.
 * I try to define it as independent as implementation of stream.
 */
struct DynStreamId {
  using StaticId = uint64_t;
  using InstanceId = uint64_t;
  static constexpr StaticId InvalidStaticStreamId = 0;
  static constexpr InstanceId InvalidInstanceId = 0;
  // Use coreId to distinguish streams in multi-core context.
  // TODO: ThreadID may be a better option.
  int coreId = -1;
  StaticId staticId = 0;
  InstanceId streamInstance = 0;
  // Used for debug purpose. User should guarantee the life cycle of name.
  // TODO: How to improve this?
  const char *streamName = "Unknown_Stream";

  DynStreamId() = default;
  DynStreamId(int _coreId, StaticId _staticId, InstanceId _streamInstance)
      : coreId(_coreId), staticId(_staticId), streamInstance(_streamInstance) {}
  DynStreamId(int _coreId, StaticId _staticId, InstanceId _streamInstance,
              const char *_streamName)
      : coreId(_coreId), staticId(_staticId), streamInstance(_streamInstance),
        streamName(_streamName) {}
  DynStreamId(const DynStreamId &other)
      : coreId(other.coreId), staticId(other.staticId),
        streamInstance(other.streamInstance), streamName(other.streamName) {}
  DynStreamId &operator=(const DynStreamId &other) {
    this->coreId = other.coreId;
    this->staticId = other.staticId;
    this->streamInstance = other.streamInstance;
    this->streamName = other.streamName;
    return *this;
  }

  bool isSameStaticStream(const DynStreamId &other) const {
    return this->coreId == other.coreId && this->staticId == other.staticId;
  }
  bool operator==(const DynStreamId &other) const {
    return this->coreId == other.coreId && this->staticId == other.staticId &&
           this->streamInstance == other.streamInstance;
  }
  bool operator!=(const DynStreamId &other) const {
    return !(this->operator==(other));
  }
  bool operator<(const DynStreamId &other) const {
    if (this->coreId != other.coreId) {
      return this->coreId < other.coreId;
    }
    if (this->staticId != other.staticId) {
      return this->staticId < other.staticId;
    }
    return this->streamInstance < other.streamInstance;
  }
};

std::ostream &operator<<(std::ostream &os, const DynStreamId &streamId);

std::string to_string(const DynStreamId &streamId);

struct DynStreamIdHasher {
  std::size_t operator()(const DynStreamId &key) const {
    auto x = std::hash<int>()(key.coreId);
    auto y = std::hash<DynStreamId::StaticId>()(key.staticId);
    auto z = std::hash<DynStreamId::InstanceId>()(key.streamInstance);
    return mergeTwoHashes(mergeTwoHashes(x, y), z);
  }

  static std::size_t mergeTwoHashes(std::size_t x, std::size_t y) {
    auto yLZ = __builtin_clzl(y | 1);
    /**
     * Circular shift to avoid that x and y both have a few LSB.
     */
    return ((x << (64 - yLZ)) | (x >> yLZ)) ^ y;
  }
};

#endif