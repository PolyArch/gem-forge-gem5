#ifndef MEM_GPGPUSIM_GPGPUSIM_REQUESTOR_HH_
#define MEM_GPGPUSIM_GPGPUSIM_REQUESTOR_HH_

#include "mem/port_wrapper.hh"
#include "sim/clocked_object.hh"

#include "params/GPGPUSimRequestor.hh"

#include <deque>

namespace gem5 {

class GPGPUSimRequestor : public ClockedObject {
public:
  PARAMS(GPGPUSimRequestor);
  GPGPUSimRequestor(const Params &p);

  Port &getPort(const std::string &if_name,
                PortID idx = InvalidPortID) override;

  // Guarantee success with internal retry queue.
  void sendTimingReq(PacketPtr pkt);

  // Simply hold in my response queue for now.
  bool recvTimingResp(PacketPtr pkt);
  void recvReqRetry();

  // Return nullptr if no response is available.
  PacketPtr popRespQueue();

  const int coreId;
  RequestorID requestorId;
  RequestPortWrapper reqPort;

private:
  bool blocked = false;
  std::deque<PacketPtr> retryQueue;
  std::deque<PacketPtr> respQueue;
};

} // namespace gem5

#endif