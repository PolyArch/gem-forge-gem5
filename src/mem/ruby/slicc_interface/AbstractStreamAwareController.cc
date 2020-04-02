#include "AbstractStreamAwareController.hh"

#include "RubySlicc_ComponentMapping.hh"

AbstractStreamAwareController::AbstractStreamAwareController(const Params *p)
    : AbstractController(p), llcSelectLowBit(p->llc_select_low_bit),
      llcSelectNumBits(p->llc_select_num_bits),
      numCoresPerRow(p->num_cores_per_row),
      enableStreamFloat(p->enable_stream_float),
      enableStreamSubline(p->enable_stream_subline),
      enableStreamMulticast(p->enable_stream_multicast),
      streamMulticastGroupSize(p->stream_multicast_group_size),
      streamMulticastGroupPerRow(1),
      mlcStreamBufferInitNumEntries(p->mlc_stream_buffer_init_num_entries) {
  if (this->streamMulticastGroupSize > 0) {
    this->streamMulticastGroupPerRow =
        (this->numCoresPerRow + this->streamMulticastGroupSize - 1) /
        this->streamMulticastGroupSize;
  }
  if (p->stream_multicast_issue_policy == "any") {
    this->streamMulticastIssuePolicy = MulticastIssuePolicy::Any;
  } else if (p->stream_multicast_issue_policy == "first_allocated") {
    this->streamMulticastIssuePolicy = MulticastIssuePolicy::FirstAllocated;
  } else if (p->stream_multicast_issue_policy == "first") {
    this->streamMulticastIssuePolicy = MulticastIssuePolicy::First;
  } else {
    panic("Illegal StreamMulticastIssuePolicy %s.\n",
          p->stream_multicast_issue_policy);
  }
}

MachineID
AbstractStreamAwareController::mapAddressToLLC(Addr addr,
                                               MachineType mtype) const {
  // Ideally we should check mtype to be LLC or directory, etc.
  // But here I ignore it.
  return mapAddressToRange(addr, mtype, this->llcSelectLowBit,
                           this->llcSelectNumBits, 0 /* cluster_id. */
  );
}

Addr AbstractStreamAwareController::getAddressToOurLLC() const {
  // Make it simple.
  return this->getMachineID().num << this->llcSelectLowBit;
}

GemForgeCPUDelegator *AbstractStreamAwareController::getCPUDelegator() {
  if (!this->cpu) {
    // Start to search for the CPU.
    for (auto so : this->getSimObjectList()) {
      auto cpu = dynamic_cast<BaseCPU *>(so);
      if (!cpu) {
        continue;
      }
      if (cpu->cpuId() != this->getMachineID().num) {
        // This is not my local cpu.
        continue;
      }
      if (!cpu->getCPUDelegator()) {
        // This one has no GemForgeCPUDelegator, should not be our target.
        continue;
      }
      this->cpu = cpu;
      break;
    }
  }
  assert(this->cpu && "Failed to find CPU.");
  auto cpuDelegator = this->cpu->getCPUDelegator();
  assert(cpuDelegator && "Missing CPUDelegator.");
  return cpuDelegator;
}