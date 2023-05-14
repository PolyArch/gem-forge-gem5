#include "MLCStreamNDCController.hh"
#include "LLCStreamNDCController.hh"

// Generated by slicc.
#include "mem/ruby/protocol/RequestMsg.hh"

#include "debug/StreamNearDataComputing.hh"

#define DEBUG_TYPE StreamNearDataComputing
#include "../stream_log.hh"

#define MLCSE_DPRINTF(format, args...)                                         \
  DPRINTF(StreamNearDataComputing, "[MLC_SE%d]: " format,                      \
          mlcSE->controller->getMachineID().num, ##args)
#define MLCSE_PANIC(format, args...)                                           \
  panic("[MLC_SE%d]: " format, mlcSE->controller->getMachineID().num, ##args)

#define MLC_NDC_DPRINTF(ndc, format, args...)                                  \
  MLCSE_DPRINTF("%s: " format, (ndc)->entryIdx, ##args)
#define MLC_NDC_PANIC(ndc, format, args...)                                    \
  MLCSE_PANIC("%s: " format, (ndc)->entryIdx, ##args)

namespace gem5 {

MLCStreamNDCController::MLCStreamNDCController(MLCStreamEngine *_mlcSE)
    : mlcSE(_mlcSE) {}

void MLCStreamNDCController::receiveStreamNDCRequest(PacketPtr pkt) {
  auto streamNDCs = *(pkt->getPtr<StreamNDCPacketVec *>());
  for (auto streamNDC : *streamNDCs) {

    // Remember the NDC packet.
    this->addNDCPacket(streamNDC);

    // Allocate the context in LLCStreamNDCController.
    LLCStreamNDCController::allocateContext(mlcSE->controller, streamNDC);

    // Create a new packet and forward it to LLC bank (L2 cache).
    auto paddrLine = ruby::makeLineAddress(streamNDC->paddr);
    auto llcBank = mlcSE->controller->mapAddressToLLCOrMem(
        paddrLine, ruby::MachineType_L2Cache);
    MLC_NDC_DPRINTF(streamNDC, "Receive NDC PAddr %#x LLC Bank %s.\n",
                    streamNDC->paddr, llcBank);

    RequestPtr req = std::make_shared<Request>(
        paddrLine, streamNDC->stream->getMemElementSize(), 0, pkt->requestorId());
    PacketPtr pkt = new Packet(req, MemCmd::StreamNDCReq);
    uint8_t *pktData =
        reinterpret_cast<uint8_t *>(new StreamNDCPacketPtr(streamNDC));
    pkt->dataDynamic(pktData);
    // Enqueue a packet to the LLC bank.
    auto msg = std::make_shared<ruby::RequestMsg>(mlcSE->controller->clockEdge());
    msg->m_addr = paddrLine;
    msg->m_Type = ruby::CoherenceRequestType_STREAM_NDC;
    msg->m_Requestors.add(mlcSE->controller->getMachineID());
    msg->m_Destination.add(llcBank);
    msg->m_MessageSize = ruby::MessageSizeType_Control;
    msg->m_pkt = pkt;

    Cycles latency(1);
    mlcSE->requestToLLCMsgBuffer->enqueue(
        msg, mlcSE->controller->clockEdge(),
        mlcSE->controller->cyclesToTicks(latency));
  }

  delete streamNDCs;
  delete pkt;
}

void MLCStreamNDCController::receiveStreamNDCResponse(const ruby::ResponseMsg &msg) {

  auto sliceId = msg.m_sliceIds.singleSliceId();
  assert(sliceId.getNumElements() == 1 && "NDC Slice with Multiple Elements.");

  auto ndcMapIter = this->getNDCPacket(sliceId);
  if (ndcMapIter == this->ndcPacketMap.end()) {
    MLCSE_PANIC("%s: Failed to find NDCPacket.", sliceId);
  }

  auto &ndc = ndcMapIter->second;
  auto S = ndc->stream;
  auto dynS = S->getDynStream(sliceId.getDynStreamId());
  if (!dynS) {
    MLC_NDC_PANIC(ndc, "Failed to get DynS for NDC response.");
  }
  auto element = dynS->getElemByIdx(ndc->entryIdx.entryIdx);
  if (!element) {
    MLC_NDC_PANIC(ndc, "Failed to get Element for NDC response.");
  }
  /**
   * So far we just simply set the value.
   */
  if (S->isAtomicComputeStream()) {
    auto vaddr = ndc->vaddr;
    auto size = S->getCoreElementSize();
    auto vaddrLine = ruby::makeLineAddress(vaddr);
    auto lineOffset = vaddr - vaddrLine;
    auto data = msg.getDataBlk().getData(lineOffset, size);
    MLC_NDC_DPRINTF(ndc, "Receive NDC Response, vaddr %#x size %d.\n", vaddr,
                    size);
    element->setValue(vaddr, size, data);
    dynS->ackCacheElement(ndc->entryIdx.entryIdx);
  } else if (S->isStoreComputeStream()) {
    MLC_NDC_DPRINTF(ndc, "Receive NDC Ack.\n");
    dynS->ackCacheElement(ndc->entryIdx.entryIdx);
  } else {
    MLC_NDC_PANIC(ndc, "Illegal StreamType for NDC.");
  }

  ndc = nullptr;
  this->ndcPacketMap.erase(ndcMapIter);
}

void MLCStreamNDCController::addNDCPacket(StreamNDCPacketPtr &ndc) {
  /**
   * We ignore Forward requests as they has no response.
   *
   */
  if (ndc->isForward) {
    return;
  }
  bool added = this->ndcPacketMap.emplace(ndc->entryIdx, ndc).second;
  if (!added) {
    MLC_NDC_PANIC(ndc, "Already in the NDCPacketMap.");
  }
}

MLCStreamNDCController::NDCPacketMapIter
MLCStreamNDCController::getNDCPacket(const DynStreamSliceId &sliceId) {
  FIFOEntryIdx entryIdx(sliceId.getDynStreamId(), sliceId.getStartIdx());
  return this->ndcPacketMap.find(entryIdx);
}} // namespace gem5

