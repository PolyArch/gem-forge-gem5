#ifndef __CPU_GEM_FORGE_PACKET_HANDLER_H__
#define __CPU_GEM_FORGE_PACKET_HANDLER_H__

#include "mem/packet.hh"

#include "gem_forge_cpu_delegator.hh"

/**
 * Drived from SenderState so that it's able to distinguish a GemForgePacket
 * from other normal packet.
 */
class GemForgePacketHandler : public Packet::SenderState {
public:
  /**
   * Handle a packet response.
   * Remember to release the packet at the end of this function.
   *
   * delete packet;
   */
  virtual void handlePacketResponse(GemForgeCPUDelegator *cpuDelegator,
                                    PacketPtr packet) = 0;
  virtual void issueToMemoryCallback(GemForgeCPUDelegator *cpuDelegator) = 0;
  virtual ~GemForgePacketHandler() {}

  static PacketPtr createGemForgePacket(Addr paddr, int size,
                                        GemForgePacketHandler *handler,
                                        uint8_t *data, MasterID masterID,
                                        int contextId, Addr pc);
  static PacketPtr createStreamControlPacket(Addr paddr, MasterID masterID,
                                             int contextId, MemCmd::Command cmd,
                                             uint64_t data);
  static void handleGemForgePacketResponse(GemForgeCPUDelegator *cpuDelegator,
                                           PacketPtr pkt);
  static void issueToMemory(GemForgeCPUDelegator *cpuDelegator, PacketPtr pkt);

  /**
   * Check if the request requires response.
   * TODO: Improve this to support other CPU model.
   */
  static bool needResponse(PacketPtr pkt);
};

#endif