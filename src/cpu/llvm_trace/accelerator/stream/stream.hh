#ifndef __CPU_TDG_ACCELERATOR_STREAM_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_HH__

#include "cpu/llvm_trace/llvm_insts.hh"
#include "stream_element.hh"

#include "base/types.hh"
#include "mem/packet.hh"

#include <list>

class LLVMTraceCPU;

class StreamEngine;
class StreamConfigInst;
class StreamStepInst;
class StreamStoreInst;
class StreamEndInst;

class Stream {
public:
  Stream(LLVMTraceCPU *_cpu, StreamEngine *_se, bool _isOracle,
         size_t _maxRunAHeadLength, const std::string &_throttling);

  virtual ~Stream();

  virtual const std::string &getStreamName() const = 0;
  virtual const std::string &getStreamType() const = 0;
  bool isMemStream() const;
  virtual uint32_t getLoopLevel() const = 0;
  virtual uint32_t getConfigLoopLevel() const = 0;
  virtual int32_t getElementSize() const = 0;

  virtual void prepareNewElement(StreamElement *element) {}

  /**
   * Simple bookkeeping information for the stream engine.
   */
  bool configured;
  StreamElement *head;
  StreamElement *stepped;
  StreamElement *tail;
  size_t allocSize;
  size_t stepSize;
  size_t maxSize;
  FIFOEntryIdx FIFOIdx;
  int lateFetchCount;

  /**
   * Step root stream, three possible cases:
   * 1. this: I am the step root.
   * 2. other: I am controlled by other step stream.
   * 3. nullptr: I am a constant stream.
   */
  Stream *stepRootStream;
  std::unordered_set<Stream *> baseStreams;
  std::unordered_set<Stream *> dependentStreams;

  virtual void configure(StreamConfigInst *inst);
  virtual void commitConfigure(StreamConfigInst *inst);
  virtual void step(StreamStepInst *inst);
  virtual void commitStep(StreamStepInst *inst);
  virtual void store(StreamStoreInst *inst);
  virtual void commitStore(StreamStoreInst *inst);
  virtual void end(StreamEndInst *inst);
  virtual void commitEnd(StreamEndInst *inst);
  virtual void commitUser(const LLVMDynamicInst *inst);

  void tick();

  void addBaseStream(Stream *baseStream);
  void addBaseStepStream(Stream *baseStepStream);
  void registerStepDependentStreamToRoot(Stream *newDependentStream);

  uint64_t getFirstConfigSeqNum() const { return this->firstConfigSeqNum; }
  bool isBeforeFirstConfigInst(uint64_t seqNum) const {
    if (this->firstConfigSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
      return true;
    }
    return seqNum < this->firstConfigSeqNum;
  }

  int getAliveElements() const { return this->FIFO.size(); }
  int getRunAheadLength() const { return this->runAHeadLength; }

  virtual uint64_t getTrueFootprint() const = 0;
  virtual uint64_t getFootprint(unsigned cacheBlockSize) const = 0;
  virtual bool isContinuous() const = 0;

  LLVMTraceCPU *getCPU() { return this->cpu; }

  const std::unordered_map<uint64_t, int> &getAliveCacheBlocks() const {
    return this->aliveCacheBlocks;
  }

  bool isConfigured() const {
    if (this->configSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
      return false;
    }
    if (this->endSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
      return true;
    }
    return this->configSeqNum > this->endSeqNum;
  }

  bool isReady(const LLVMDynamicInst *user) const;
  void use(const LLVMDynamicInst *user);
  bool canStep() const;

  struct FIFOEntry {
    const FIFOEntryIdx idx;

    /**
     * Oracle information about whether this entry will be used eventually.
     */
    const bool oracleUsed;

    /**
     * For iv stream, address is the same as value.
     * TODO: Remove value, which is unused in the simulator.
     */
    const uint64_t address;
    const uint64_t size;

    /**
     * Small vector stores the cache blocks this element touched.
     */
    static constexpr int MAX_CACHE_BLOCKS = 4;
    uint64_t cacheBlockAddrs[MAX_CACHE_BLOCKS];
    int cacheBlocks;

    bool isAddressValid;
    bool isValueValid;
    bool used;
    int inflyLoadPackets;
    /**
     * The sequence number of the step instruction.
     */
    const uint64_t prevSeqNum;
    uint64_t stepSeqNum;
    uint64_t storeSeqNum;
    Cycles addressReadyCycles;
    Cycles valueReadyCycles;
    mutable Cycles firstCheckIfReadyCycles;
    mutable std::unordered_set<uint64_t> users;

    FIFOEntry(const FIFOEntryIdx &_idx, const bool _oracleUsed,
              const uint64_t _address, uint64_t _size,
              const uint64_t _prevSeqNum);

    void markAddressReady(Cycles readyCycles);
    void markValueReady(Cycles readyCycles);

    bool stored() const {
      return this->storeSeqNum != LLVMDynamicInst::INVALID_SEQ_NUM;
    }
    bool stepped() const {
      return this->stepSeqNum != LLVMDynamicInst::INVALID_SEQ_NUM;
    }
    void store(uint64_t storeSeqNum);
    void step(uint64_t stepSeqNum);
    void dump() const;
  };

  /**
   * This is used as a handler to the response packet.
   * The stream aware cache also use this to find the stream the packet belongs
   * to.
   */
  class StreamMemAccess final : public TDGPacketHandler {
  public:
    StreamMemAccess(Stream *_stream, const FIFOEntryIdx _entryId,
                    int _additionalDelay = 0)
        : stream(_stream), entryId(_entryId),
          additionalDelay(_additionalDelay) {}
    virtual ~StreamMemAccess() {}
    void handlePacketResponse(LLVMTraceCPU *cpu, PacketPtr packet) override;
    void handlePacketResponse(PacketPtr packet);

    Stream *getStream() const { return this->stream; }

    void setAdditionalDelay(int additionalDelay) {
      this->additionalDelay = additionalDelay;
    }

    struct ResponseEvent : public Event {
    public:
      LLVMTraceCPU *cpu;
      Stream::StreamMemAccess *memAccess;
      PacketPtr pkt;
      std::string n;
      ResponseEvent(LLVMTraceCPU *_cpu, Stream::StreamMemAccess *_memAccess,
                    PacketPtr _pkt)
          : cpu(_cpu), memAccess(_memAccess), pkt(_pkt),
            n("StreamMemAccessResponseEvent") {}
      void process() override {
        this->memAccess->handlePacketResponse(this->cpu, this->pkt);
      }

      const char *description() const { return "StreamMemAccessResponseEvent"; }

      const std::string name() const { return this->n; }
    };

  private:
    Stream *stream;
    FIFOEntryIdx entryId;
    /**
     * Additional delay we want to add after we get the response.
     */
    int additionalDelay;
  };

protected:
  LLVMTraceCPU *cpu;
  StreamEngine *se;
  bool isOracle;

  std::unordered_set<Stream *> baseStepStreams;
  std::unordered_set<Stream *> baseStepRootStreams;
  std::unordered_set<Stream *> dependentStepStreams;

  StreamElement nilTail;

  /**
   * Step the dependent streams in this order.
   */
  std::list<Stream *> stepStreamList;

  bool isStepRoot() const {
    const auto &type = this->getStreamType();
    return this->baseStepStreams.empty() && (type == "phi" || type == "store");
  }

  uint64_t firstConfigSeqNum;
  uint64_t configSeqNum;
  uint64_t endSeqNum;

  /**
   * Dummy stored data used for store stream.
   * For simplicity, we just allocate one cache block here and let the packet
   * size tailor it as needed, as maximum size of a packet is a cache block.
   */
  uint8_t *storedData;

  size_t maxRunAHeadLength;
  size_t runAHeadLength;
  std::string throttling;
  std::list<FIFOEntry> FIFO;

  std::unordered_set<StreamMemAccess *> memAccesses;

  mutable std::unordered_map<uint64_t, const FIFOEntry *> userToEntryMap;

  mutable std::unordered_map<uint64_t, int> aliveCacheBlocks;
  void addAliveCacheBlock(uint64_t addr) const;
  void removeAliveCacheBlock(uint64_t addr) const;
  bool isCacheBlockAlive(uint64_t addr) const;
  uint64_t getCacheBlockAddr(uint64_t addr) const;

  void updateRunAHeadLength(size_t newRunAHeadLength);

  virtual void enqueueFIFO() = 0;
  virtual void markAddressReady(FIFOEntry &entry) = 0;
  virtual void markValueReady(FIFOEntry &entry) = 0;

  bool checkIfEntryBaseValuesValid(const FIFOEntry &entry) const;

  virtual void handlePacketResponse(const FIFOEntryIdx &entryId,
                                    PacketPtr packet,
                                    StreamMemAccess *memAccess) = 0;
  void triggerReady(Stream *rootStream, const FIFOEntryIdx &entryId);
  void receiveReady(Stream *rootStream, Stream *baseStream,
                    const FIFOEntryIdx &entryId);

  /**
   * An overload step for other instructions with step semantics,
   * e.g. config/store
   */
  void step(uint64_t stepSeqNum);
  void triggerStep(uint64_t stepSeqNum, Stream *rootStream);
  void stepImpl(uint64_t stepSeqNum);

  void triggerCommitStep(uint64_t stepSeqNum, Stream *rootStream);
  void commitStepImpl(uint64_t stepSeqNum);

  /**
   * Find the correct used entry by comparing the userSeqNum and stepSeqNum of
   * the entry.
   * Returns nullptr if failed. This can happen when the last element is
   * stepped, but the FIFO is full and the next element is not allocated yet.
   */
  FIFOEntry *findCorrectUsedEntry(uint64_t userSeqNum);
  const FIFOEntry *findCorrectUsedEntry(uint64_t userSeqNum) const;

  /**
   * For throttler.
   * TODO: extract to another class.
   */
  void throttleLate();

  /**
   * For debug.
   */
  virtual void dump() const = 0;
};

#endif