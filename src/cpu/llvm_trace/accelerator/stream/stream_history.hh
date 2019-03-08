#ifndef __CPU_TDG_ACCELERATOR_STREAM_HISTORY_H__
#define __CPU_TDG_ACCELERATOR_STREAM_HISTORY_H__

#include "StreamMessage.pb.h"

#include <list>
#include <string>

class StreamHistory {
public:
  StreamHistory(const std::string &_historyPath);

  /**
   * Read the next history entry from the stream.
   */
  void configure();

  /**
   * Return the next value of the history.
   * The first boolean indicating the value is valid.
   */
  std::pair<bool, uint64_t> getNextAddr(bool &used);

  uint64_t getCurrentStreamLength() const;

  uint64_t getNumCacheLines() const;

private:
  using HistoryList = std::list<LLVM::TDG::StreamHistory>;
  HistoryList histories;

  HistoryList::const_iterator nextConfig;
  HistoryList::const_iterator currentConfig;

  size_t currentIdx;
  uint64_t previousAddr;
};

#endif