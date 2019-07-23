#ifndef __CPU_TDG_ACCELERATOR_LLC_STREAM_ENGINE_H__
#define __CPU_TDG_ACCELERATOR_LLC_STREAM_ENGINE_H__

#include "LLCDynamicStream.hh"

#include "mem/ruby/common/Consumer.hh"

/**
 * Derive from Consumer to schedule wakeup event.
 */

#include <list>
#include <memory>

class AbstractController;

class LLCStreamEngine : public Consumer {
public:
  LLCStreamEngine(AbstractController *_controller);
  ~LLCStreamEngine();

  void receiveStreamConfigure(PacketPtr pkt);
  void wakeup() override;
  void print(std::ostream &out) const override;

private:
  AbstractController *controller;
  const int issueWidth;
  const int migrateWidth;

  using StreamList = std::list<LLCDynamicStream *>;
  using StreamListIter = StreamList::iterator;
  StreamList streams;
  /**
   * Streams waiting to be migrated to other LLC bank.
   */
  StreamList migratingStreams;

  /**
   * Issue streams in a round-robin way.
   */
  void issueStreams();

  /**
   * Issue a single stream.
   */
  bool issueStream(LLCDynamicStream *stream);

  /**
   * Migrate streams.
   */
  void migrateStreams();

  /**
   * Migrate a single stream.
   */
  void migrateStream(LLCDynamicStream *stream);

  /**
   * Check if this address is handled by myself.
   */
  bool isPAddrHandledByMe(Addr paddr) const;
};

#endif