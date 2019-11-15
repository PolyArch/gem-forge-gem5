#ifndef __CPU_TDG_ACCELERATOR_STREAM_ELEMENT_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_ELEMENT_HH__

#include "base/types.hh"
#include "cache/DynamicStreamSliceId.hh"
#include "cpu/gem_forge/gem_forge_packet_handler.hh"
#include "fifo_entry_idx.hh"

#include <unordered_map>
#include <unordered_set>

class Stream;
class DynamicStream;
class StreamEngine;
class StreamStoreInst;

/**
 * Represent the breakdown of one element according to cache block size.
 */
struct CacheBlockBreakdownAccess {
  // Which cache block this access belongs to.
  uint64_t cacheBlockVAddr;
  // The actual virtual address.
  uint64_t virtualAddr;
  // The actual size.
  uint8_t size;
  /**
   * State of the cache line.
   */
  enum StateE { None, Initialized, Issued, PrevElement, Ready } state;
};

class StreamElement;
/**
 * This is used as a handler to the response packet.
 * The stream aware cache also use this to find the stream the packet belongs
 * to.
 */
class StreamMemAccess final : public GemForgePacketHandler {
public:
  StreamMemAccess(Stream *_stream, StreamElement *_element,
                  Addr _cacheBlockAddr, Addr _vaddr, int _size,
                  int _additionalDelay = 0);
  virtual ~StreamMemAccess() {}
  void handlePacketResponse(GemForgeCPUDelegator *cpuDelegator,
                            PacketPtr packet) override;
  void issueToMemoryCallback(GemForgeCPUDelegator *cpuDelegator) override;
  void handlePacketResponse(PacketPtr packet);
  // This cache block is fetched in by some other StreamMemAccess.
  void handleStreamEngineResponse();

  Stream *getStream() const { return this->stream; }

  const DynamicStreamId &getDynamicStreamId() const {
    return this->FIFOIdx.streamId;
  }
  /**
   * TODO: Return a reference.
   */
  DynamicStreamSliceId getSliceId() const { return this->sliceId; }

  void setAdditionalDelay(int additionalDelay) {
    this->additionalDelay = additionalDelay;
  }

  struct ResponseEvent : public Event {
  public:
    GemForgeCPUDelegator *cpuDelegator;
    StreamMemAccess *memAccess;
    PacketPtr pkt;
    std::string n;
    ResponseEvent(GemForgeCPUDelegator *_cpuDelegator,
                  StreamMemAccess *_memAccess, PacketPtr _pkt)
        : cpuDelegator(_cpuDelegator), memAccess(_memAccess), pkt(_pkt),
          n("StreamMemAccessResponseEvent") {}
    void process() override {
      this->memAccess->handlePacketResponse(this->cpuDelegator, this->pkt);
    }

    const char *description() const { return "StreamMemAccessResponseEvent"; }

    const std::string name() const { return this->n; }
  };

  Stream *const stream;
  StreamElement *const element;
  // Make a copy of the FIFOIdx in case element is released.
  const FIFOEntryIdx FIFOIdx;

  Addr cacheBlockVAddr;
  Addr vaddr;
  int size;

  // The slice of the this memory request.
  DynamicStreamSliceId sliceId;

  /**
   * Additional delay we want to add after we get the response.
   */
  int additionalDelay;
};

struct StreamElement {
  std::unordered_set<StreamElement *> baseElements;
  StreamElement *next;
  Stream *stream;
  StreamEngine *se;
  FIFOEntryIdx FIFOIdx;
  int cacheBlockSize;
  /**
   * Whether this element's value managed in cache block level.
   * So far all memory streams are managed in cache block level.
   * TODO: Handle indirect stream where this is not true.
   */
  bool isCacheBlockedValue;
  /**
   * Whether the first user of this stream element has been dispatched.
   * This is used to determine the first user the of the stream element
   * and allocate entry in the load queue.
   */
  uint64_t firstUserSeqNum;
  bool isFirstUserDispatched() const;
  bool isStepped;
  bool isAddrReady;
  bool isValueReady;
  bool flushed;

  Cycles allocateCycle;
  Cycles addrReadyCycle;
  Cycles issueCycle;
  Cycles valueReadyCycle;
  Cycles firstCheckCycle;

  /**
   * Small vector stores the cache blocks this element touched.
   */
  uint64_t addr;
  uint64_t size;
  static constexpr int MAX_CACHE_BLOCKS = 10;
  int cacheBlocks;
  CacheBlockBreakdownAccess cacheBlockBreakdownAccesses[MAX_CACHE_BLOCKS];
  /**
   * Small vector stores all the data.
   * * The value should be indexed in cache line granularity.
   * * i.e. the first byte of value is the byte at
   * * cacheBlockBreakdownAccesses[0].cacheBlockVAddr.
   * * Please use setValue() and getValue() to interact with value so that this
   * * is always respected.
   * This design is a compromise with existing implementation of coalescing
   * continuous stream elements, which allows an element to hold a little bit of
   * more data in the last cache block beyond its size.
   */
  std::vector<uint8_t> value;
  void setValue(StreamElement *prevElement);
  void setValue(Addr vaddr, int size, const uint8_t *val);
  void getValue(Addr vaddr, int size, uint8_t *val) const;
  uint64_t mapVAddrToValueOffset(Addr vaddr, int size) const;
  // Some helper template.
  template <typename T> void setValue(Addr vaddr, T *val) {
    this->setValue(vaddr, sizeof(*val), reinterpret_cast<uint8_t *>(val));
  }
  template <typename T> void getValue(Addr vaddr, T *val) {
    this->getValue(vaddr, sizeof(*val), reinterpret_cast<uint8_t *>(val));
  }

  // Store the infly mem accesses for this element, basically for load.
  std::unordered_set<StreamMemAccess *> inflyMemAccess;
  // Store the infly writeback memory accesses.
  std::unordered_map<StreamStoreInst *, std::unordered_set<StreamMemAccess *>>
      inflyWritebackMemAccess;
  // Store all the allocated mem accesses. May be different to inflyMemAccess
  // if some the element is released before the result comes back, e.g. unused
  // element.
  std::unordered_set<StreamMemAccess *> allocatedMemAccess;

  bool stored;

  /**
   * Mark if the next element is also marked value ready by this element.
   */
  bool markNextElementValueReady = false;

  StreamElement(StreamEngine *_se);

  StreamMemAccess *
  allocateStreamMemAccess(const CacheBlockBreakdownAccess &cacheBlockBreakDown);
  void handlePacketResponse(StreamMemAccess *memAccess, PacketPtr pkt);
  void markAddrReady(GemForgeCPUDelegator *cpuDelegator);
  void tryMarkValueReady();
  void markValueReady();

  void splitIntoCacheBlocks(GemForgeCPUDelegator *cpuDelegator);

  void dump() const;

  void clear();
  void clearCacheBlocks();
  Stream *getStream() {
    assert(this->stream != nullptr && "Null stream in the element.");
    return this->stream;
  }
};

#endif