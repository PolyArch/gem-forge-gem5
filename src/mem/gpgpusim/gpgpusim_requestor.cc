#include "gpgpusim_requestor.hh"

#include "sim/system.hh"

#include "debug/GPGPUSim.hh"

namespace gem5 {

GPGPUSimRequestor::GPGPUSimRequestor(const Params &p)
    : ClockedObject(p), coreId(p.core_id),
      requestorId(p.system->getRequestorId(this)),
      reqPort("req_port", requestorId) {

  this->reqPort.setTimingCallbacks(
      [this](PacketPtr pkt) { return this->recvTimingResp(pkt); },
      [this]() { this->recvReqRetry(); });
}

Port &GPGPUSimRequestor::getPort(const std::string &if_name, PortID idx) {
  if (if_name == "req_port") {
    return reqPort;
  }
  return ClockedObject::getPort(if_name, idx);
}

bool GPGPUSimRequestor::recvTimingResp(PacketPtr pkt) {

  DPRINTF(GPGPUSim, "Received response packet %p\n", pkt);
  this->respQueue.push_back(pkt);

  return true;
}

PacketPtr GPGPUSimRequestor::popRespQueue() {
  if (this->respQueue.empty()) {
    return nullptr;
  }
  PacketPtr pkt = this->respQueue.front();
  this->respQueue.pop_front();
  return pkt;
}

void GPGPUSimRequestor::recvReqRetry() {
  this->blocked = false;
  while (!this->retryQueue.empty()) {
    PacketPtr pkt = this->retryQueue.front();
    if (this->reqPort.sendTimingReq(pkt)) {
      DPRINTF(GPGPUSim, "Sent packet %p successfully\n", pkt);
    } else {
      this->blocked = true;
      break;
    }
    this->retryQueue.pop_front();
  }
}

void GPGPUSimRequestor::sendTimingReq(PacketPtr pkt) {
  if (this->blocked || !this->retryQueue.empty()) {
    // Queue the packet for later sending
    this->retryQueue.push_back(pkt);
    return;
  }

  if (this->reqPort.sendTimingReq(pkt)) {
    DPRINTF(GPGPUSim, "Sent packet %p successfully\n", pkt);
  } else {
    this->blocked = true;
    this->retryQueue.push_back(pkt);
  }
}

} // namespace gem5
