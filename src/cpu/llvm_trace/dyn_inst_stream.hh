#ifndef __CPU_DYN_INST_STREAM_HH__
#define __CPU_DYN_INST_STREAM_HH__

#include "base/misc.hh"

// Parse the instructions from a protobuf.
#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse llvm instructions."
#endif

#include "TDGInstruction.pb.h"
#include "llvm_insts.hh"

#include "proto/protoio.hh"

#include <list>
#include <utility>

/**
 * This class represents a buffer, where elements are allocated and deallocated
 * in a FIFO order.
 * The "deallocated" elements are reused for later allocation.
 */
template <typename T> class QueueBuffer {
public:
  explicit QueueBuffer() : size(0) {
    this->list.push_back(new T());
    this->_end = this->list.begin();
  }

  ~QueueBuffer() {
    for (auto &allocated : this->list) {
      delete allocated;
    }
  }

  // For simplicity, no copy or move.
  QueueBuffer(const QueueBuffer &Other) = delete;
  QueueBuffer(QueueBuffer &&Other) = delete;
  QueueBuffer &operator=(const QueueBuffer &Other) = delete;
  QueueBuffer &operator=(QueueBuffer &&Other) = delete;

  using container = std::list<T *>;
  using iterator = typename container::iterator;

  iterator begin() { return this->list.begin(); }
  iterator end() { return this->_end; }

  bool empty() const { return this->size == 0; }

  void release_front(const T *allocated) {
    if (this->empty()) {
      panic("Release from empty queue buffer.");
    }

    // Move the allocated one to the back for reuse.
    auto released = this->list.front();
    if (allocated != released) {
      panic("Release not in order.");
    }
    this->list.pop_front();
    this->list.push_back(released);
    this->size--;
  }

  // Get an allocated element, but without moving _end iterator.
  // If there is no following alloc_back() call, this element is essentially not
  // used by the user.
  T *peek_back() { return *this->_end; }

  T *alloc_back() {
    auto allocated = *this->_end;
    this->_end++;
    this->size++;
    this->expand();
    return allocated;
  }

private:
  std::list<T *> list;
  iterator _end;
  size_t size;

  void expand() {
    if (this->_end == this->list.end()) {
      // We ran out of space.
      // Really allocate one element.
      this->_end = this->list.insert(this->_end, new T());
    }
    if (this->_end == this->list.end()) {
      panic("Run out of space for allocation.");
    }
  }
};

/**
 * Represent a instruction stream from a file.
 * The instructions are read by fetch stage, and are only released when
 * committed. Instructions must be committed in order.
 */
class DynamicInstructionStream {
public:
  DynamicInstructionStream(const std::string &_fn);
  ~DynamicInstructionStream();

  DynamicInstructionStream(const DynamicInstructionStream &other) = delete;
  DynamicInstructionStream(DynamicInstructionStream &&other) = delete;
  DynamicInstructionStream &
  operator=(const DynamicInstructionStream &other) = delete;
  DynamicInstructionStream &
  operator=(DynamicInstructionStream &&other) = delete;

  using ListContainer = std::list<std::pair<LLVMDynamicInst *, bool>>;
  using Iterator = ListContainer::iterator;

  /**
   * Get the static info at the header of the stream.
   */
  const LLVM::TDG::StaticInformation &getStaticInfo() const;

  /**
   * Parse more instructions from the file (currently at most 1000).
   * Returns the number of instructions parsed.
   *
   * NOTE: user should do some flow control based on the size(), otherwise the
   * memory usage will blow up.
   */

  size_t size() const { return this->insts.size(); }
  bool empty() const { return this->size() == 0; }

  /**
   * Interface for fetch stage.
   */
  size_t fetchSize() const { return this->_fetchSize; }
  bool fetchEmpty() const { return this->fetchSize() == 0; }
  LLVMDynamicInst *fetch();

  /**
   * Fetch the iterator.
   */
  Iterator fetchIter();

  /**
   * Interface for commit stage.
   */
  void commit(LLVMDynamicInst *inst);

  /**
   * Special case for out-of-order release.
   */
  void commit(Iterator instIter);

private:
  std::string fn;
  ProtoInputStream *input;
  QueueBuffer<LLVM::TDG::TDGInstruction> buffer;

  LLVM::TDG::StaticInformation staticInfo;

  Iterator fetchPos;
  size_t _fetchSize;

  ListContainer insts;

  void release();
  size_t parse();
};

/**
 * A wrapper over the underneath DynamicInstructionStream.
 */
class DynamicInstructionStreamInterface {
public:
  /**
   * Fetch next instruction, nullptr if we reached the end of the stream.
   */
  virtual ~DynamicInstructionStreamInterface() {}
  virtual LLVMDynamicInst *fetch() = 0;
  virtual bool hasEnded() const = 0;
  virtual void commit(LLVMDynamicInst *inst) = 0;
};

/**
 * Implement a conditional ending the interface,
 * i.e. terminate the stream by a user-defined end token.
 * Note: The end token is consumed by the interface.
 */
class DynamicInstructionStreamInterfaceConditionalEnd
    : public DynamicInstructionStreamInterface {
public:
  using EndFunc = std::function<bool(LLVMDynamicInst *)>;
  using Iterator = DynamicInstructionStream::Iterator;
  DynamicInstructionStreamInterfaceConditionalEnd(
      DynamicInstructionStream *_stream, EndFunc _endFunc);

  ~DynamicInstructionStreamInterfaceConditionalEnd() override;

  LLVMDynamicInst *fetch() override;
  bool hasEnded() const override { return this->ended; }
  void commit(LLVMDynamicInst *inst) override;

private:
  DynamicInstructionStream *stream;
  EndFunc endFunc;

  Iterator headIter;
  LLVMDynamicInst *endToken;
  size_t fetchedSize;
  bool ended;
};

/**
 * Implment a fixed end strean interface.
 */
class DynamicInstructionStreamInterfaceFixedEnd
    : public DynamicInstructionStreamInterface {
public:
  using Iterator = DynamicInstructionStream::Iterator;
  DynamicInstructionStreamInterfaceFixedEnd(DynamicInstructionStream *_stream,
                                            Iterator _lhs, Iterator _rhs);

  ~DynamicInstructionStreamInterfaceFixedEnd() override;

  LLVMDynamicInst *fetch() override;
  bool hasEnded() const override { return this->fetchIter == this->rhs; }
  void commit(LLVMDynamicInst *inst) override;

private:
  DynamicInstructionStream *stream;
  Iterator lhs, rhs;

  Iterator fetchIter;
  size_t fetchedSize;
};

#endif