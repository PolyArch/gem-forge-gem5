#include "stream_nuca_manager.hh"
#include "numa_page_allocator.hh"
#include "stream_nuca_map.hh"

#include "base/trace.hh"
#include "cpu/gem_forge/accelerator/stream/cache/pum/PUMHWConfiguration.hh"
#include "cpu/thread_context.hh"
#include "params/Process.hh"

#include <iomanip>
#include <unordered_set>

#include "debug/StreamNUCAManager.hh"

std::shared_ptr<StreamNUCAManager> StreamNUCAManager::singleton = nullptr;

// There is only one StreamNUCAManager.
std::shared_ptr<StreamNUCAManager>
StreamNUCAManager::initialize(Process *_process, ProcessParams *_params) {
  if (!singleton) {
    singleton = std::make_shared<StreamNUCAManager>(_process, _params);
  }
  return singleton;
}

StreamNUCAManager::StreamNUCAManager(Process *_process, ProcessParams *_params)
    : process(_process), enabledMemStream(_params->enableMemStream),
      enabledNUCA(_params->enableStreamNUCA),
      enablePUM(_params->enableStreamPUMMapping),
      enablePUMTiling(_params->enableStreamPUMTiling),
      enableIndirectPageRemap(_params->streamNUCAEnableIndPageRemap) {
  const auto &directRegionFitPolicy = _params->streamNUCADirectRegionFitPolicy;
  if (directRegionFitPolicy == "crop") {
    this->directRegionFitPolicy = DirectRegionFitPolicy::CROP;
  } else if (directRegionFitPolicy == "drop") {
    this->directRegionFitPolicy = DirectRegionFitPolicy::DROP;
  } else {
    panic("Unknown DirectRegionFitPolicy %s.", directRegionFitPolicy);
  }
}

StreamNUCAManager::StreamNUCAManager(const StreamNUCAManager &other)
    : process(other.process), enabledMemStream(other.enabledMemStream),
      enabledNUCA(other.enabledNUCA), enablePUM(other.enablePUM),
      enablePUMTiling(other.enablePUMTiling),
      directRegionFitPolicy(other.directRegionFitPolicy),
      enableIndirectPageRemap(other.enableIndirectPageRemap) {
  panic("StreamNUCAManager does not have copy constructor.");
}

StreamNUCAManager &
StreamNUCAManager::operator=(const StreamNUCAManager &other) {
  panic("StreamNUCAManager does not have copy constructor.");
}

void StreamNUCAManager::regStats() {

  if (this->statsRegisterd) {
    return;
  }
  this->statsRegisterd = true;

  hack("Register %#x processor %#x name %s.\n", this, process, process->name());

  assert(this->process && "No process.");

#define scalar(stat, describe)                                                 \
  stat.name(this->process->name() + (".snm." #stat))                           \
      .desc(describe)                                                          \
      .prereq(this->stat)
#define distribution(stat, start, end, step, describe)                         \
  stat.name(this->process->name() + (".snm." #stat))                           \
      .init(start, end, step)                                                  \
      .desc(describe)                                                          \
      .flags(Stats::pdf)

  scalar(indRegionPages, "Pages in indirect region.");
  scalar(indRegionElements, "Elements in indirect region.");
  scalar(indRegionAllocPages,
         "Pages allocated (including fragments) to optimize indirect region.");
  scalar(indRegionRemapPages, "Pages remapped to optimize indirect region.");
  scalar(indRegionMemToLLCDefaultHops,
         "Default hops from Mem to LLC in indirect region.");
  scalar(indRegionMemToLLCMinHops,
         "Minimal hops from Mem to LLC in indirect region.");
  scalar(indRegionMemToLLCRemappedHops,
         "Remapped hops from Mem to LLC in indirect region.");

  auto numMemNodes = StreamNUCAMap::getNUMANodes().size();
  distribution(indRegionMemMinBanks, 0, numMemNodes - 1, 1,
               "Distribution of minimal IndRegion banks.");
  distribution(indRegionMemRemappedBanks, 0, numMemNodes - 1, 1,
               "Distribution of remapped IndRegion banks.");

#undef distribution
#undef scalar
}

void StreamNUCAManager::defineRegion(const std::string &regionName, Addr start,
                                     uint64_t elementSize,
                                     const std::vector<int64_t> &arraySizes) {

  int64_t numElement = 1;
  for (const auto &s : arraySizes) {
    numElement *= s;
  }
  DPRINTF(StreamNUCAManager,
          "[StreamNUCA] Define Region %s %#x %ld %ld=%ldx%ldx%ld %lukB.\n",
          regionName, start, elementSize, numElement, arraySizes[0],
          arraySizes.size() > 1 ? arraySizes[1] : 1,
          arraySizes.size() > 2 ? arraySizes[2] : 1,
          elementSize * numElement / 1024);
  this->startVAddrRegionMap.emplace(
      std::piecewise_construct, std::forward_as_tuple(start),
      std::forward_as_tuple(regionName, start, elementSize, numElement,
                            arraySizes));
}

void StreamNUCAManager::setProperty(Addr start, uint64_t property,
                                    uint64_t value) {
  DPRINTF(StreamNUCAManager, "[StreamNUCA] Set Property %#x %lu Value %lu.\n",
          start, property, value);
  auto &region = this->getRegionFromStartVAddr(start);
  switch (property) {
  default: {
    panic("[StreamNUCA] Invalid property %lu.", property);
  }
#define CASE(E)                                                                \
  case RegionProperty::E: {                                                    \
    region.userDefinedProperties.emplace(RegionProperty::E, value);            \
    break;                                                                     \
  }

    CASE(INTERLEAVE);
    CASE(USE_PUM);
    CASE(PUM_NO_INIT);
    CASE(PUM_TILE_SIZE_DIM0);

#undef CASE
  }
}

void StreamNUCAManager::defineAlign(Addr A, Addr B, int64_t elementOffset) {
  DPRINTF(StreamNUCAManager, "[StreamNUCA] Define Align %#x %#x Offset %ld.\n",
          A, B, elementOffset);
  auto &regionA = this->getRegionFromStartVAddr(A);
  regionA.aligns.emplace_back(A, B, elementOffset);
  if (elementOffset < 0) {
    regionA.isIndirect = true;
    IndirectAlignField indField = decodeIndirectAlign(elementOffset);
    DPRINTF(StreamNUCAManager, "[StreamNUCA]     IndAlign Offset %d Size %d.\n",
            indField.offset, indField.size);
  }
}

const StreamNUCAManager::StreamRegion &
StreamNUCAManager::getContainingStreamRegion(Addr vaddr) const {
  auto iter = this->startVAddrRegionMap.upper_bound(vaddr);
  if (iter == this->startVAddrRegionMap.begin()) {
    panic("Failed to find ContainingStreamRegion for %#x.", vaddr);
  }
  iter--;
  const auto &region = iter->second;
  if (region.vaddr + region.elementSize * region.numElement <= vaddr) {
    panic("Failed to find ContainingStreamRegion for %#x.", vaddr);
  }
  return region;
}

void StreamNUCAManager::remap(ThreadContext *tc) {
  DPRINTF(StreamNUCAManager,
          "Remap Regions EnabledMemStream %d EnabledNUCA %d.\n",
          this->enabledMemStream, this->enabledNUCA);

  /**
   * Even not enabled, we group direct regions by their alignement.
   * Also, if we enabled memory stream, we try to compute cached elements.
   */
  this->groupDirectRegionsByAlign();

  if (this->enabledMemStream) {
    this->computeCachedElements();
  }

  if (!this->enabledNUCA) {
    return;
  }

  bool hasAlign = false;
  for (const auto &entry : this->startVAddrRegionMap) {
    if (!entry.second.aligns.empty()) {
      hasAlign = true;
      break;
    }
  }
  if (!hasAlign) {
    DPRINTF(StreamNUCAManager, "Skip Remapping Region as No Alignments.\n");
  }

  /**
   * We perform a DFS on regions to try to satisfy alignment requirement.
   */
  std::unordered_map<Addr, int> regionRemapStateMap;
  std::vector<Addr> stack;
  while (true) {
    stack.clear();
    for (const auto &entry : this->startVAddrRegionMap) {
      auto regionVAddr = entry.second.vaddr;
      if (regionRemapStateMap.count(regionVAddr) == 0) {
        // We found a unprocessed region.
        regionRemapStateMap.emplace(regionVAddr, 0);
        stack.push_back(regionVAddr);
        break;
      }
    }
    if (stack.empty()) {
      // No region to process.
      break;
    }
    while (!stack.empty()) {
      auto regionVAddr = stack.back();
      auto &region = this->getRegionFromStartVAddr(regionVAddr);
      auto state = regionRemapStateMap.at(regionVAddr);
      if (state == 0) {
        // First time, push AlignToRegions into the stack.
        for (const auto &align : region.aligns) {
          if (align.vaddrB == regionVAddr) {
            // We need to ignore self-alignment.
            continue;
          }
          const auto &alignToRegion =
              this->getRegionFromStartVAddr(align.vaddrB);
          // Check the state of the AlignToRegion.
          auto alignToRegionState =
              regionRemapStateMap.emplace(align.vaddrB, 0).first->second;
          if (alignToRegionState == 0) {
            // The AlignToRegion has not been processed yet.
            stack.push_back(align.vaddrB);
          } else if (alignToRegionState == 1) {
            // The AlignToRegion is on the current DFS path. Must be cycle.
            panic("[StreamNUCA] Cycle in AlignGraph: %s -> %s.", region.name,
                  alignToRegion.name);
          } else {
            // The AlignToRegion has already been processed. Ignore it.
          }
        }
        // Set myself as in stack.
        regionRemapStateMap.at(regionVAddr) = 1;

      } else if (state == 1) {
        // Second time, we can try to remap this region.
        this->remapRegion(tc, region);
        regionRemapStateMap.at(regionVAddr) = 2;
        stack.pop_back();

      } else {
        // This region is already remapped. Ignore it.
        stack.pop_back();
      }
    }
  }

  this->computeCacheSet();

  DPRINTF(StreamNUCAManager,
          "[StreamNUCA] Remap Done. IndRegion: Pages %lu Elements %lu "
          "AllocPages %lu RemapPages %lu DefaultHops %lu MinHops %lu RemapHops "
          "%lu.\n",
          static_cast<uint64_t>(indRegionPages.value()),
          static_cast<uint64_t>(indRegionElements.value()),
          static_cast<uint64_t>(indRegionAllocPages.value()),
          static_cast<uint64_t>(indRegionRemapPages.value()),
          static_cast<uint64_t>(indRegionMemToLLCDefaultHops.value()),
          static_cast<uint64_t>(indRegionMemToLLCMinHops.value()),
          static_cast<uint64_t>(indRegionMemToLLCRemappedHops.value()));
}

void StreamNUCAManager::remapRegion(ThreadContext *tc, StreamRegion &region) {
  bool hasIndirectAlign = false;
  for (const auto &align : region.aligns) {
    if (align.elementOffset < 0) {
      hasIndirectAlign = true;
      break;
    }
  }
  if (hasIndirectAlign) {
    this->remapIndirectRegion(tc, region);
  } else {
    if (this->enablePUM && this->canRemapDirectRegionPUM(region)) {
      this->remapDirectRegionPUM(region);
    } else {
      this->remapDirectRegionNUCA(region);
    }
  }
}

void StreamNUCAManager::remapDirectRegionNUCA(const StreamRegion &region) {
  if (!this->isPAddrContinuous(region)) {
    panic("[StreamNUCA] Region %s %#x PAddr is not continuous.", region.name,
          region.vaddr);
  }
  auto startVAddr = region.vaddr;
  auto startPAddr = this->translate(startVAddr);

  auto endPAddr = startPAddr + region.elementSize * region.numElement;

  uint64_t interleave = this->determineInterleave(region);
  int startBank = this->determineStartBank(region, interleave);
  int startSet = 0;

  StreamNUCAMap::addRangeMap(startPAddr, endPAddr, interleave, startBank,
                             startSet);
  DPRINTF(StreamNUCAManager,
          "[StreamNUCA] Map Region %s %#x PAddr %#x Interleave %lu Bank %d.\n",
          region.name, startVAddr, startPAddr, interleave, startBank);
}

void StreamNUCAManager::remapIndirectRegion(ThreadContext *tc,
                                            StreamRegion &region) {

  /**
   * We divide this into multiple phases:
   * 1. Collect hops stats.
   * 2. Greedily allocate pages to the NUMA Nodes with minimal traffic.
   * 3. If imbalanced, we try to remap.
   * 4. Relocate pages if necessary.
   *
   * NOTE: This does not work with PUM.
   * NOTE: For now remapped indirect region is not cached.
   */
  if (this->enablePUM) {
    panic("[StreamNUCA] IndirectRegion with PUM.");
  }
  region.cachedElements = 0;

  auto regionHops = this->computeIndirectRegionHops(tc, region);

  this->greedyAssignIndirectPages(regionHops);
  this->rebalanceIndirectPages(regionHops);

  this->relocateIndirectPages(tc, regionHops);
}

StreamNUCAManager::IndirectRegionHops
StreamNUCAManager::computeIndirectRegionHops(ThreadContext *tc,
                                             const StreamRegion &region) {
  assert(region.aligns.size() == 1 &&
         "IndirectRegion should have only one align.");
  const auto &align = region.aligns.front();
  assert(align.vaddrB != region.vaddr && "Self-IndirectAlign?");

  /**
   * Scan through the indirect regions and collect hops.
   */
  auto pTable = this->process->pTable;
  auto pageSize = pTable->getPageSize();
  auto totalSize = region.elementSize * region.numElement;
  auto endVAddr = region.vaddr + totalSize;
  if (pTable->pageOffset(region.vaddr) != 0) {
    panic("[StreamNUCA] IndirectRegion %s VAddr %#x should align to page.",
          region.name, region.vaddr);
  }

  const auto &memNodes = StreamNUCAMap::getNUMANodes();
  auto numMemNodes = memNodes.size();
  IndirectRegionHops regionHops(region, numMemNodes);

  IndirectAlignField indField = decodeIndirectAlign(align.elementOffset);

  const auto &alignToRegion = this->getRegionFromStartVAddr(align.vaddrB);
  for (Addr vaddr = region.vaddr; vaddr < endVAddr; vaddr += pageSize) {
    auto pageVAddr = pTable->pageAlign(vaddr);
    auto pageHops = this->computeIndirectPageHops(tc, region, alignToRegion,
                                                  indField, pageVAddr);
    regionHops.pageHops.emplace_back(std::move(pageHops));
  }

  return regionHops;
}

StreamNUCAManager::IndirectPageHops StreamNUCAManager::computeIndirectPageHops(
    ThreadContext *tc, const StreamRegion &region,
    const StreamRegion &alignToRegion, const IndirectAlignField &indField,
    Addr pageVAddr) {

  auto pTable = this->process->pTable;
  auto pageSize = pTable->getPageSize();
  auto totalSize = region.elementSize * region.numElement;
  auto endVAddr = std::min(region.vaddr + totalSize, pageVAddr + pageSize);
  auto numBytes = endVAddr - pageVAddr;
  auto pageIndex = (pageVAddr - region.vaddr) / pageSize;
  auto pagePAddr = this->translate(pageVAddr);
  auto defaultNodeId = StreamNUCAMap::mapPAddrToNUMAId(pagePAddr);

  const auto &memNodes = StreamNUCAMap::getNUMANodes();
  auto numMemNodes = memNodes.size();
  auto numRows = StreamNUCAMap::getNumRows();
  auto numCols = StreamNUCAMap::getNumCols();

  char *pageData = reinterpret_cast<char *>(malloc(pageSize));
  tc->getVirtProxy().readBlob(pageVAddr, pageData, pageSize);

  indRegionPages++;
  indRegionElements += numBytes / region.elementSize;

  IndirectPageHops pageHops(pageVAddr, pagePAddr, defaultNodeId, numMemNodes,
                            numRows * numCols);

  for (int i = 0; i < numBytes; i += region.elementSize) {
    int64_t index = 0;
    if (indField.size == 4) {
      index = *reinterpret_cast<int32_t *>(pageData + i + indField.offset);
    } else if (indField.size == 8) {
      index = *reinterpret_cast<int64_t *>(pageData + i + indField.offset);
    } else {
      panic("[StreamNUCA] Invalid IndAlign %s ElementSize %d Field Offset %d "
            "Size %d.",
            region.name, region.elementSize, indField.offset, indField.size);
    }
    if (index < 0 || index >= alignToRegion.numElement) {
      panic("[StreamNUCA] %s InvalidIndex %d not in %s NumElement %d.",
            region.name, index, alignToRegion.name, alignToRegion.numElement);
    }
    auto alignToVAddr = alignToRegion.vaddr + index * alignToRegion.elementSize;
    auto alignToPAddr = this->translate(alignToVAddr);
    auto alignToBank = StreamNUCAMap::getBank(alignToPAddr);

    // DPRINTF(StreamNUCAManager,
    //         "  Index %ld AlignToVAddr %#x AlignToPAddr %#x AlignToBank
    //         %d.\n", index, alignToVAddr, alignToPAddr, alignToBank);

    if (alignToBank < 0 || alignToBank >= pageHops.bankFreq.size()) {
      panic("[StreamNUCA] IndirectAlign %s -> %s Page %lu Index %ld Invalid "
            "AlignToBank %d.",
            region.name, alignToRegion.name, pageIndex, index, alignToBank);
    }
    pageHops.bankFreq.at(alignToBank)++;
    pageHops.totalElements++;

    // Accumulate the traffic hops for all NUMA nodes.
    for (int NUMAId = 0; NUMAId < numMemNodes; ++NUMAId) {
      const auto &memNode = memNodes.at(NUMAId);
      auto hops = StreamNUCAMap::computeHops(alignToBank, memNode.routerId);
      pageHops.hops.at(NUMAId) += hops;
    }
  }

  pageHops.maxHops = pageHops.hops.front();
  pageHops.minHops = pageHops.hops.front();
  pageHops.maxHopsNUMANodeId = 0;
  pageHops.minHopsNUMANodeId = 0;
  for (int NUMAId = 1; NUMAId < numMemNodes; ++NUMAId) {
    auto hops = pageHops.hops.at(NUMAId);
    if (hops > pageHops.maxHops) {
      pageHops.maxHops = hops;
      pageHops.maxHopsNUMANodeId = NUMAId;
    }
    if (hops < pageHops.minHops) {
      pageHops.minHops = hops;
      pageHops.minHopsNUMANodeId = NUMAId;
    }
  }

  return pageHops;
}

void StreamNUCAManager::greedyAssignIndirectPages(
    IndirectRegionHops &regionHops) {

  const auto numRows = StreamNUCAMap::getNumRows();
  const auto numCols = StreamNUCAMap::getNumCols();

  for (uint64_t pageIdx = 0; pageIdx < regionHops.pageHops.size(); ++pageIdx) {
    auto &pageHops = regionHops.pageHops.at(pageIdx);

    auto minHops = pageHops.minHops;
    auto minHopsNUMAId = pageHops.minHopsNUMANodeId;

    /**
     * Sort by their difference between MaxHops and MinHops.
     */
    regionHops.addRemapPageId(pageIdx, minHopsNUMAId);

    indRegionMemToLLCMinHops += minHops;
    indRegionMemMinBanks.sample(minHopsNUMAId, 1);

    if (Debug::StreamNUCAManager) {
      int32_t avgBankFreq = pageHops.totalElements / pageHops.bankFreq.size();
      std::stringstream freqMatrixStr;
      for (int row = 0; row < numRows; ++row) {
        for (int col = 0; col < numCols; ++col) {
          auto bank = row * numCols + col;
          freqMatrixStr << std::setw(6)
                        << (pageHops.bankFreq[bank] - avgBankFreq);
        }
        freqMatrixStr << '\n';
      }
      DPRINTF(StreamNUCAManager,
              "[StreamNUCA] IndRegion %s PageIdx %lu AvgBankFreq %d Diff:\n%s.",
              regionHops.region.name, pageIdx, avgBankFreq,
              freqMatrixStr.str());
    }
  }

  if (Debug::StreamNUCAManager) {
    const auto &region = regionHops.region;
    DPRINTF(StreamNUCAManager,
            "[StreamNUCA] IndirectRegion %s Finish Greedy Assign:\n",
            region.name);
    for (int i = 0; i < regionHops.numMemNodes; ++i) {
      auto pages = regionHops.remapPageIds.at(i).size();
      auto totalPages = regionHops.pageHops.size();
      auto ratio = static_cast<float>(pages) / static_cast<float>(totalPages);
      DPRINTF(StreamNUCAManager,
              "[StreamNUCA]     NUMANode %5d Pages %8lu %3.2f\n", i, pages,
              ratio * 100);
    }
  }
}

void StreamNUCAManager::IndirectRegionHops::addRemapPageId(uint64_t pageId,
                                                           int NUMANodeId) {
  /**
   * Sorted by their difference between MaxHops and MinHops.
   */
  auto &remapPageIds = this->remapPageIds.at(NUMANodeId);
  auto &remapPageHops = this->pageHops.at(pageId);
  remapPageHops.remapNUMANodeId = NUMANodeId;

  auto remapDiffHops = remapPageHops.maxHops - remapPageHops.minHops;
  auto iter = remapPageIds.begin();
  while (iter != remapPageIds.end()) {
    auto pageId = *iter;
    const auto &pageHops = this->pageHops.at(pageId);
    auto diffHops = pageHops.maxHops - pageHops.minHops;
    if (diffHops < remapDiffHops) {
      break;
    }
    ++iter;
  }
  remapPageIds.insert(iter, pageId);
}

void StreamNUCAManager::rebalanceIndirectPages(IndirectRegionHops &regionHops) {

  auto remapPageIdsCmp =
      [](const IndirectRegionHops::RemapPageIdsPerNUMANodeT &A,
         const IndirectRegionHops::RemapPageIdsPerNUMANodeT &B) -> bool {
    return A.size() < B.size();
  };

  auto isBalanced = [&regionHops, &remapPageIdsCmp]() -> bool {
    auto minMaxPair =
        std::minmax_element(regionHops.remapPageIds.begin(),
                            regionHops.remapPageIds.end(), remapPageIdsCmp);
    auto diff = minMaxPair.second->size() - minMaxPair.first->size();
    auto ratio = static_cast<float>(diff) /
                 static_cast<float>(regionHops.pageHops.size());
    const float threshold = 0.02f;
    return ratio <= threshold;
  };

  auto selectPopNUMAIter =
      [&regionHops,
       &remapPageIdsCmp]() -> IndirectRegionHops::RemapPageIdsT::iterator {
    // For now always select the NUMAId with most pages.
    auto maxIter =
        std::max_element(regionHops.remapPageIds.begin(),
                         regionHops.remapPageIds.end(), remapPageIdsCmp);
    return maxIter;
  };

  auto selectPushNUMAIter =
      [&regionHops,
       &remapPageIdsCmp]() -> IndirectRegionHops::RemapPageIdsT::iterator {
    // For now always select the NUMAId with least pages.
    auto minIter =
        std::min_element(regionHops.remapPageIds.begin(),
                         regionHops.remapPageIds.end(), remapPageIdsCmp);
    return minIter;
  };

  while (!isBalanced()) {
    auto popIter = selectPopNUMAIter();
    auto pushIter = selectPushNUMAIter();

    auto pageIdx = popIter->back();
    popIter->pop_back();

    auto pushNUMANodeId = pushIter - regionHops.remapPageIds.begin();
    regionHops.addRemapPageId(pageIdx, pushNUMANodeId);
  }

  if (Debug::StreamNUCAManager) {
    const auto &region = regionHops.region;
    DPRINTF(StreamNUCAManager,
            "[StreamNUCA] IndirectRegion %s Finish Rebalance:\n", region.name);
    for (int i = 0; i < regionHops.numMemNodes; ++i) {
      auto pages = regionHops.remapPageIds.at(i).size();
      auto totalPages = regionHops.pageHops.size();
      auto ratio = static_cast<float>(pages) / static_cast<float>(totalPages);
      DPRINTF(StreamNUCAManager,
              "[StreamNUCA]     NUMANode %5d Pages %8lu %3.2f\n", i, pages,
              ratio * 100);
    }
  }
}

void StreamNUCAManager::relocateIndirectPages(
    ThreadContext *tc, const IndirectRegionHops &regionHops) {

  auto pTable = this->process->pTable;
  auto pageSize = pTable->getPageSize();

  char *pageData = reinterpret_cast<char *>(malloc(pageSize));

  for (uint64_t pageIdx = 0; pageIdx < regionHops.pageHops.size(); ++pageIdx) {

    const auto &pageHops = regionHops.pageHops.at(pageIdx);
    auto remapNUMANodeId = pageHops.remapNUMANodeId;
    auto defaultNUMANodeId = pageHops.defaultNUMANodeId;

    if (!this->enableIndirectPageRemap) {
      /**
       * IndirectRemap is disabled, we just set remapNUMA = defaultNUMA.
       */
      remapNUMANodeId = defaultNUMANodeId;
    }

    this->indRegionMemToLLCDefaultHops += pageHops.hops.at(defaultNUMANodeId);
    this->indRegionMemToLLCRemappedHops += pageHops.hops.at(remapNUMANodeId);
    this->indRegionMemRemappedBanks.sample(remapNUMANodeId, 1);

    if (remapNUMANodeId == defaultNUMANodeId) {
      continue;
    }

    auto pageVAddr = pageHops.pageVAddr;
    auto defaultPagePAddr = pageHops.defaultPagePAddr;
    tc->getVirtProxy().readBlob(pageVAddr, pageData, pageSize);

    /**
     * Try to allocate a page at selected bank. Remap the vaddr to the new paddr
     * by setting clobber flag (which will destroy the old mapping). Then copy
     * the data.
     */
    int allocPages = 0;
    int allocNUMANodeId = 0;
    auto newPagePAddr = NUMAPageAllocator::allocatePageAt(
        tc->getProcessPtr()->system, remapNUMANodeId, allocPages,
        allocNUMANodeId);

    indRegionRemapPages++;
    indRegionAllocPages += allocPages;

    bool clobber = true;
    pTable->map(pageVAddr, newPagePAddr, pageSize, clobber);
    tc->getVirtProxy().writeBlob(pageVAddr, pageData, pageSize);

    // Return the old page to the allocator.
    NUMAPageAllocator::returnPage(defaultPagePAddr, defaultNUMANodeId);
  }
}

void StreamNUCAManager::groupDirectRegionsByAlign() {
  std::map<Addr, Addr> unionFindParent;
  for (const auto &entry : this->startVAddrRegionMap) {
    unionFindParent.emplace(entry.first, entry.first);
  }

  auto find = [&unionFindParent](Addr vaddr) -> Addr {
    while (true) {
      auto iter = unionFindParent.find(vaddr);
      assert(iter != unionFindParent.end());
      if (iter->second == vaddr) {
        return vaddr;
      }
      vaddr = iter->second;
    }
  };

  auto merge = [&unionFindParent, &find](Addr vaddrA, Addr vaddrB) -> void {
    auto rootA = find(vaddrA);
    auto rootB = find(vaddrB);
    unionFindParent[rootA] = rootB;
  };

  for (const auto &entry : this->startVAddrRegionMap) {
    const auto &region = entry.second;
    for (const auto &align : region.aligns) {
      if (align.vaddrA == align.vaddrB) {
        // Ignore self alignment.
        continue;
      }
      merge(align.vaddrA, align.vaddrB);
      DPRINTF(StreamNUCAManager, "[AlignGroup] Union %#x %#x.\n", align.vaddrA,
              align.vaddrB);
    }
  }

  for (const auto &entry : unionFindParent) {
    /**
     * Ignore all indirect regions when contructing groups.
     */
    const auto &region = this->getRegionFromStartVAddr(entry.first);
    if (region.isIndirect) {
      continue;
    }
    auto root = find(entry.first);
    this->directRegionAlignGroupVAddrMap
        .emplace(std::piecewise_construct, std::forward_as_tuple(root),
                 std::forward_as_tuple())
        .first->second.emplace_back(entry.first);
  }

  for (auto &entry : this->directRegionAlignGroupVAddrMap) {
    auto &group = entry.second;
    // Sort for simplicity.
    std::sort(group.begin(), group.end());
  }
}

void StreamNUCAManager::computeCachedElements() {

  const auto totalBanks =
      StreamNUCAMap::getNumRows() * StreamNUCAMap::getNumCols();
  const auto llcNumSets = StreamNUCAMap::getCacheNumSet();
  const auto llcAssoc = StreamNUCAMap::getCacheAssoc();
  const auto llcBlockSize = StreamNUCAMap::getCacheBlockSize();
  const auto llcBankSize = llcNumSets * llcAssoc * llcBlockSize;
  /**
   * Let's reserve 1MB of LLC size for other data.
   */
  const auto reservedLLCSize = 1024 * 1024;
  const auto totalLLCSize = llcBankSize * totalBanks - reservedLLCSize;

  for (auto &entry : this->directRegionAlignGroupVAddrMap) {
    auto &group = entry.second;

    /**
     * First we estimate how many data can be cached.
     * NOTE: If a region has non-zero non-self alignment, we assume the
     * offset is the unused data, e.g. first layer of hotspot3D.powerIn.
     * This is different than homogeneous case:
     * A [--- Cached --- | --- Uncached ---]
     * B [--- Cached --- | --- Uncached ---]
     * C [--- Cached --- | --- Uncached ---]
     *
     * Now we have some extra bytes:
     * A [        --- Cached --- | --- Uncached ---]
     * B [        --- Cached --- | --- Uncached ---]
     * C - Extra [--- Cached --- | --- Uncached ---]
     *
     * For A and B
     *  CachedElementsA = (TotalLLCSize + Extra) / TotalElementSize
     * For C
     *  CachedElementsC = CachedElementsA - Extra / ElementCSize
     *
     */
    auto totalElementSize = 0;
    auto totalSize = 0ul;
    auto extraSize = 0ul;
    auto getExtraSize = [](const StreamRegion &region) -> uint64_t {
      auto extraSize = 0ul;
      for (const auto &align : region.aligns) {
        if (align.vaddrA != align.vaddrB && align.elementOffset > 0) {
          if (extraSize != 0 && align.elementOffset != extraSize) {
            panic("Region %s Multi-ExtraSize %lu %lu.", region.name, extraSize,
                  align.elementOffset);
          }
          extraSize = align.elementOffset;
        }
      }
      return extraSize;
    };
    for (auto startVAddr : group) {
      const auto &region = this->getRegionFromStartVAddr(startVAddr);
      totalElementSize += region.elementSize;
      totalSize += region.elementSize * region.numElement;
      extraSize += getExtraSize(region);
    }

    if (this->directRegionFitPolicy == DirectRegionFitPolicy::DROP &&
        totalSize > totalLLCSize) {
      for (auto iter = group.begin(), end = group.end(); iter != end; ++iter) {
        auto startVAddr = *iter;
        auto &region = this->getRegionFromStartVAddr(startVAddr);
        if (region.name == "gap.pr_push.out_neigh_index" ||
            region.name == "rodinia.hotspot3D.powerIn" ||
            region.name == "rodinia.hotspot.power" ||
            region.name == "rodinia.pathfinder.wall") {
          totalElementSize -= region.elementSize;
          totalSize -= region.elementSize * region.numElement;
          extraSize -= getExtraSize(region);
          region.cachedElements = 0;
          group.erase(iter);
          // This is a vector, we have to break after erase something.
          DPRINTF(StreamNUCAManager,
                  "[AlignGroup] Avoid cache %s Bytes %lu ElementSize %lu.\n",
                  region.name, region.elementSize * region.numElement,
                  region.elementSize);
          break;
        }
      }
    }

    uint64_t cachedElements = (totalLLCSize + extraSize) / totalElementSize;

    DPRINTF(StreamNUCAManager,
            "[AlignGroup] Analyzing Group %#x NumRegions %d ExtraSize %lu "
            "TotalElementSize %d CachedElements %lu.\n",
            group.front(), group.size(), extraSize, totalElementSize,
            cachedElements);
    for (auto vaddr : group) {
      auto &region = this->getRegionFromStartVAddr(vaddr);
      auto extraSize = getExtraSize(region);
      auto regionCachedElements =
          cachedElements - extraSize / region.elementSize;
      DPRINTF(StreamNUCAManager,
              "[AlignGroup]   Region %#x Elements %lu ExtraSize %lu Cached "
              "%.2f%%.\n",
              vaddr, region.numElement, extraSize,
              static_cast<float>(regionCachedElements) /
                  static_cast<float>(region.numElement) * 100.f);
      region.cachedElements = std::min(regionCachedElements, region.numElement);
    }
  }
}

void StreamNUCAManager::computeCacheSetNUCA() {

  /**
   * Compute the StartSet for arrays.
   * NOTE: We ignore indirect regions, as they will be remapped at page
   * granularity.
   */

  const auto totalBanks =
      StreamNUCAMap::getNumRows() * StreamNUCAMap::getNumCols();
  const auto llcNumSets = StreamNUCAMap::getCacheNumSet();
  const auto llcBlockSize = StreamNUCAMap::getCacheBlockSize();

  for (auto &entry : this->directRegionAlignGroupVAddrMap) {
    auto &group = entry.second;

    auto totalElementSize = 0;
    auto totalSize = 0ul;
    for (auto startVAddr : group) {
      const auto &region = this->getRegionFromStartVAddr(startVAddr);
      totalElementSize += region.elementSize;
      totalSize += region.elementSize * region.numElement;
    }

    DPRINTF(
        StreamNUCAManager,
        "[CacheSet] Analyzing Group %#x NumRegions %d TotalElementSize %d.\n",
        group.front(), group.size(), totalElementSize);

    auto startSet = 0;
    for (auto startVAddr : group) {
      const auto &region = this->getRegionFromStartVAddr(startVAddr);

      auto startPAddr = this->translate(startVAddr);
      auto &rangeMap = StreamNUCAMap::getRangeMapByStartPAddr(startPAddr);
      rangeMap.startSet = startSet;

      auto cachedElements = region.cachedElements;

      auto cachedBytes = cachedElements * region.elementSize;
      auto usedSets = cachedBytes / (llcBlockSize * totalBanks);

      DPRINTF(StreamNUCAManager,
              "[CacheSet] Range %s %#x ElementSize %d CachedElements %lu "
              "StartSet %d UsedSet %d.\n",
              region.name, region.vaddr, region.elementSize, cachedElements,
              startSet, usedSets);
      startSet = (startSet + usedSets) % llcNumSets;
    }
  }
}

void StreamNUCAManager::computeCacheSetPUM() {

  const auto llcNumSets = StreamNUCAMap::getCacheNumSet();
  const auto llcBlockSize = StreamNUCAMap::getCacheBlockSize();
  const auto llcArraysPerWay = StreamNUCAMap::getCacheParams().arrayPerWay;
  const auto llcBitlinesPerArray = StreamNUCAMap::getCacheParams().bitlines;

  auto startSet = 0;
  for (const auto &entry : this->startVAddrRegionMap) {
    const auto &startVAddr = entry.first;
    const auto &region = entry.second;

    auto startPAddr = this->translate(startVAddr);
    auto &rangeMap = StreamNUCAMap::getRangeMapByStartPAddr(startPAddr);
    rangeMap.startSet = startSet;

    auto elemSize = region.elementSize;
    auto usedBytesPerWay = elemSize * llcArraysPerWay * llcBitlinesPerArray;
    auto usedSets = usedBytesPerWay / llcBlockSize;
    assert(startSet + usedSets <= llcNumSets && "LLC Sets overflow.");

    DPRINTF(StreamNUCAManager,
            "[CacheSet] Range %s %#x ElementSize %d UsedBytesPerWay %lu "
            "StartSet %d UsedSet %d.\n",
            region.name, region.vaddr, region.elementSize, usedBytesPerWay,
            startSet, usedSets);
    startSet = (startSet + usedSets) % llcNumSets;
  }
}

void StreamNUCAManager::computeCacheSet() {
  if (this->enablePUM) {
    this->computeCacheSetPUM();
  } else {
    this->computeCacheSetNUCA();
  }
}

StreamNUCAManager::StreamRegion &
StreamNUCAManager::getRegionFromStartVAddr(Addr vaddr) {
  auto iter = this->startVAddrRegionMap.find(vaddr);
  if (iter == this->startVAddrRegionMap.end()) {
    panic("Failed to find StreamRegion at %#x.", vaddr);
  }
  return iter->second;
}

StreamNUCAManager::StreamRegion &
StreamNUCAManager::getRegionFromName(const std::string &name) {
  for (auto &entry : this->startVAddrRegionMap) {
    auto &region = entry.second;
    if (region.name == name) {
      return region;
    }
  }
  panic("Failed to find StreamRegion %s.", name);
}

bool StreamNUCAManager::isPAddrContinuous(const StreamRegion &region) {
  auto pTable = this->process->pTable;
  auto pageSize = pTable->getPageSize();
  auto startPageVAddr = pTable->pageAlign(region.vaddr);
  Addr startPagePAddr;
  if (!pTable->translate(startPageVAddr, startPagePAddr)) {
    panic("StreamNUCAManager failed to translate StartVAddr %#x.",
          region.vaddr);
  }
  auto endVAddr = region.vaddr + region.elementSize * region.numElement;
  for (auto vaddr = startPageVAddr; vaddr < endVAddr; vaddr += pageSize) {
    Addr paddr;
    if (!pTable->translate(vaddr, paddr)) {
      panic("StreamNUCAManager failed to translate vaddr %#x, StartVAddr %#x.",
            vaddr, startPageVAddr);
    }
    if (paddr - startPagePAddr != vaddr - startPageVAddr) {
      DPRINTF(
          StreamNUCAManager,
          "Range %s StartVAddr %#x StartPageVAddr %#x StartPagePAddr %#x not "
          "physically continuous at %#x paddr %#x.\n",
          region.name, region.vaddr, startPageVAddr, startPagePAddr, vaddr,
          paddr);
      return false;
    }
  }
  return true;
}

Addr StreamNUCAManager::translate(Addr vaddr) {
  Addr paddr;
  if (!this->process->pTable->translate(vaddr, paddr)) {
    panic("[StreamNUCA] failed to translate VAddr %#x.", vaddr);
  }
  return paddr;
}

uint64_t StreamNUCAManager::determineInterleave(const StreamRegion &region) {
  const uint64_t defaultInterleave = 1024;
  uint64_t interleave = defaultInterleave;

  /**
   * If the region has user-defined interleave, use it.
   * Check that there are no alignment defined.
   */
  if (region.userDefinedProperties.count(RegionProperty::INTERLEAVE)) {
    if (!region.aligns.empty()) {
      panic("Range %s has both aligns and user-defined interleave.",
            region.name);
    }
    return region.userDefinedProperties.at(RegionProperty::INTERLEAVE) *
           region.elementSize;
  }

  auto numRows = StreamNUCAMap::getNumRows();
  auto numCols = StreamNUCAMap::getNumCols();
  auto numBanks = numRows * numCols;

  auto defaultWrapAroundBytes = defaultInterleave * numBanks;
  auto defaultColWrapAroundBytes = defaultInterleave * numCols;

  for (const auto &align : region.aligns) {
    const auto &alignToRegion = this->getRegionFromStartVAddr(align.vaddrB);

    auto elementOffset = align.elementOffset;
    auto bytesOffset = elementOffset * alignToRegion.elementSize;
    DPRINTF(StreamNUCAManager,
            "Range %s %#x AlignTo %#x Offset Element %ld Bytes %lu.\n",
            region.name, region.vaddr, alignToRegion.vaddr, elementOffset,
            bytesOffset);

    if (elementOffset < 0) {
      panic("Range %s %#x with negative element offset %ld.\n", region.name,
            region.vaddr, elementOffset);
    }

    if ((&alignToRegion) == (&region)) {
      // Self alignment.
      if ((bytesOffset % defaultWrapAroundBytes) == 0) {
        // Already aligned.
        DPRINTF(StreamNUCAManager, "Range %s %#x Self Aligned.\n", region.name,
                region.vaddr);
      } else if ((bytesOffset % defaultColWrapAroundBytes) == 0) {
        // Try to align with one row.
        interleave =
            bytesOffset / defaultColWrapAroundBytes * defaultInterleave;
        DPRINTF(StreamNUCAManager,
                "Range %s %#x Self Aligned To Row Interleave %lu = %lu / %lu * "
                "%lu.\n",
                region.name, region.vaddr, interleave, bytesOffset,
                defaultColWrapAroundBytes, defaultInterleave);
      } else if (bytesOffset < defaultColWrapAroundBytes &&
                 (defaultColWrapAroundBytes % bytesOffset) == 0) {
        // Try to align with one row.
        interleave =
            (bytesOffset * defaultInterleave) / defaultColWrapAroundBytes;
        DPRINTF(StreamNUCAManager,
                "Range %s %#x Self Aligned To Row Interleave %lu = %lu * %lu / "
                "%lu.\n",
                region.name, region.vaddr, interleave, bytesOffset,
                defaultInterleave, defaultColWrapAroundBytes);
        if (interleave != 128 && interleave != 256 && interleave != 512) {
          panic("Weird Interleave Found: Range %s %#x SelfAlign ElemOffset %lu "
                "BytesOffset %lu Intrlv %llu.\n",
                region.name, region.vaddr, align.elementOffset, bytesOffset,
                interleave);
        }
      } else {
        panic("Not Support Yet: Range %s %#x Self Align ElemOffset %lu "
              "ByteOffset %lu.\n",
              region.name, region.vaddr, align.elementOffset, bytesOffset);
      }
    } else {
      // Other alignment.
      auto otherInterleave = this->determineInterleave(alignToRegion);
      DPRINTF(StreamNUCAManager,
              "Range %s %#x Align to Range %#x Interleave = %lu / %lu * %lu.\n",
              region.name, region.vaddr, alignToRegion.vaddr, otherInterleave,
              alignToRegion.elementSize, region.elementSize);
      interleave =
          otherInterleave / alignToRegion.elementSize * region.elementSize;
    }
  }
  return interleave;
}

int StreamNUCAManager::determineStartBank(const StreamRegion &region,
                                          uint64_t interleave) {

  auto startVAddr = region.vaddr;
  auto startPAddr = this->translate(startVAddr);

  int startBank = 0;
  if (region.name.find("rodinia.pathfinder.") == 0 ||
      region.name.find("rodinia.hotspot.") == 0 ||
      region.name.find("rodinia.hotspot.") == 0 ||
      region.name.find("rodinia.srad_v2.") == 0 ||
      region.name.find("rodinia.srad_v3.") == 0 ||
      region.name.find("gap.pr_push") == 0 ||
      region.name.find("gap.bfs_push") == 0 ||
      region.name.find("gap.sssp") == 0 ||
      region.name.find("gap.pr_pull") == 0 ||
      region.name.find("gap.bfs_pull") == 0) {
    // Pathfinder need to start at the original bank.
    startBank = (startPAddr / interleave) %
                (StreamNUCAMap::getNumCols() * StreamNUCAMap::getNumRows());
  }

  for (const auto &align : region.aligns) {
    if (align.vaddrB == align.vaddrA) {
      continue;
    }
    /**
     * Use alignToRegion startBank.
     */
    const auto &alignToRegion = this->getRegionFromStartVAddr(align.vaddrB);
    auto alignToRegionStartPAddr = this->translate(align.vaddrB);
    const auto &alignToRegionMap =
        StreamNUCAMap::getRangeMapByStartPAddr(alignToRegionStartPAddr);
    startBank = alignToRegionMap.startBank;
    DPRINTF(StreamNUCAManager,
            "[StreamNUCA] Region %s Align StartBank %d to %s.\n", region.name,
            startBank, alignToRegion.name);
  }

  return startBank;
}

uint64_t StreamNUCAManager::getCachedBytes(Addr start) {
  const auto &region = this->getRegionFromStartVAddr(start);
  return region.cachedElements * region.elementSize;
}

void StreamNUCAManager::markRegionCached(Addr regionVAddr) {
  if (!this->enabledNUCA) {
    return;
  }
  const auto &region = this->getRegionFromStartVAddr(regionVAddr);
  Addr regionPAddr;
  if (!this->process->pTable->translate(regionVAddr, regionPAddr)) {
    panic("Failed to translate RegionVAddr %#x.\n", regionVAddr);
  }
  auto &nucaMapEntry = StreamNUCAMap::getRangeMapByStartPAddr(regionPAddr);
  nucaMapEntry.isCached = true;
  DPRINTF(StreamNUCAManager, "[StreamNUCA] Region %s Marked Cached.\n",
          region.name);
}

StreamNUCAManager::IndirectAlignField
StreamNUCAManager::decodeIndirectAlign(int64_t indirectAlign) {
  assert(indirectAlign < 0 && "This is not IndirectAlign.");

  const int SIZE_BITWIDTH = 8;
  const int SIZE_MASK = (1 << SIZE_BITWIDTH) - 1;
  const int OFFSET_BITWIDTH = 8;
  const int OFFSET_MASK = (1 << OFFSET_BITWIDTH) - 1;

  int32_t offset = ((-indirectAlign) >> SIZE_BITWIDTH) & OFFSET_MASK;
  int32_t size = (-indirectAlign) & SIZE_MASK;
  return IndirectAlignField(offset, size);
}

bool StreamNUCAManager::canRemapDirectRegionPUM(const StreamRegion &region) {
  auto pumHWConfig = StreamNUCAMap::getPUMHWConfig();

  auto bitlines = pumHWConfig.array_cols;
  if (region.numElement < bitlines || region.numElement % bitlines != 0) {
    DPRINTF(
        StreamNUCAManager,
        "[StreamPUM] Region %s NumElem %llu not compatible with Bitlines %ld.",
        region.name, region.numElement, bitlines);
    return false;
  }
  /**
   * A heuristic to avoid mapping some arrays since they should never be mapped
   * to PUM.
   * TODO: Add pseudo-instructions to pass in this information.
   */
  if (region.userDefinedProperties.count(RegionProperty::USE_PUM) &&
      region.userDefinedProperties.at(RegionProperty::USE_PUM) == 0) {
    DPRINTF(StreamNUCAManager, "[StreamPUM] Region %s Manually Disabled PUM.\n",
            region.name);
    return false;
  }
  return true;
}

void StreamNUCAManager::remapDirectRegionPUM(const StreamRegion &region) {
  if (!this->isPAddrContinuous(region)) {
    panic("[StreamPUM] Region %s %#x PAddr is not continuous.", region.name,
          region.vaddr);
  }
  assert(this->canRemapDirectRegionPUM(region) && "Can not Map to PUM.");
  auto startVAddr = region.vaddr;
  auto startPAddr = this->translate(startVAddr);

  auto endPAddr = startPAddr + region.elementSize * region.numElement;

  auto pumHWConfig = StreamNUCAMap::getPUMHWConfig();

  auto dimensions = region.arraySizes.size();

  /**
   * We want to search for aligned dimensions from this region or it's
   * AlignedToRegion, and try to tile for those aligned dimensions.
   */
  auto bitlines = pumHWConfig.array_cols;

  AffinePattern::IntVecT arraySizes = region.arraySizes;
  AffinePattern::IntVecT tileSizes(dimensions, 1);

  auto alignDims = this->getAlignDimsForDirectRegion(region);
  auto numAlignDims = alignDims.size();
  assert(numAlignDims > 0 && "No AlignDims.");

  if (numAlignDims == 1) {
    /**
     * Just align to one dimension.
     * Pick the minimum of:
     *  bitlines, arraySize, userDefinedTileSize (if defined).
     *
     * Then -- if there is more space, try to map the next dimension.
     *
     */
    auto alignDim = alignDims.front();
    auto arraySize = arraySizes.at(alignDim);

    auto alignDimTileSize = std::min(bitlines, arraySize);
    if (region.userDefinedProperties.count(
            RegionProperty::PUM_TILE_SIZE_DIM0)) {
      auto userDefinedTileSize =
          region.userDefinedProperties.at(RegionProperty::PUM_TILE_SIZE_DIM0);
      if (userDefinedTileSize < alignDimTileSize) {
        alignDimTileSize = userDefinedTileSize;
      }
    }

    tileSizes.at(alignDim) = alignDimTileSize;

    if (alignDimTileSize < bitlines) {
      // Check if we have next dimension to map.
      assert(alignDim + 1 < dimensions);
      assert(bitlines % alignDimTileSize == 0);
      auto ratio = bitlines / alignDimTileSize;
      tileSizes.at(alignDim + 1) = ratio;
    }
  } else if (numAlignDims == 2) {
    // Just try to get square root of bitlines?
    if (this->enablePUMTiling) {
      auto &x = tileSizes.at(alignDims.at(0));
      auto &y = tileSizes.at(alignDims.at(1));
      x = bitlines;
      y = 1;
      while (y * 2 < x) {
        y *= 2;
        x /= 2;
      }
    } else {
      // Tiling is not enabled, however, we tile to handle the case when dim0 <
      // bitlines.
      auto &x = tileSizes.at(0);
      auto &y = tileSizes.at(1);
      x = bitlines;
      y = 1;
      auto size0 = arraySizes.at(0);
      if (size0 < bitlines) {
        assert(bitlines % size0 == 0);
        x = size0;
        y = bitlines / size0;
      }
    }
  } else if (dimensions == 3) {
    if (this->enablePUMTiling) {
      auto &x = tileSizes.at(alignDims.at(0));
      auto &y = tileSizes.at(alignDims.at(1));
      auto &z = tileSizes.at(alignDims.at(2));
      x = bitlines;
      y = 1;
      z = 1;
      while (y * 4 < x) {
        x /= 4;
        y *= 2;
        z *= 2;
      }
    } else {
      // Tiling is not enabled, however, we tile to handle the case when dim0 <
      // bitlines.
      auto &x = tileSizes.at(0);
      auto &y = tileSizes.at(1);
      auto &z = tileSizes.at(2);
      x = bitlines;
      y = 1;
      z = 1;
      auto size0 = arraySizes.at(0);
      if (size0 < bitlines) {
        assert(bitlines % size0 == 0);
        x = size0;
        y = bitlines / size0;
        z = 1;
      }
    }
  } else {
    panic("[StreamPUM] Region %s too many dimensions.", region.name);
  }

  for (auto dim = 0; dim < dimensions; ++dim) {
    auto arraySize = arraySizes[dim];
    auto tileSize = tileSizes[dim];
    if (arraySize < tileSize) {
      panic("[StreamPUM] Region %s Dim %d %ld < %ld.", region.name, dim,
            arraySize, tileSize);
    }
    if (arraySize % tileSize != 0) {
      panic("[StreamPUM] Region %s Dim %d %ld %% %ld != 0.", region.name, dim,
            arraySize, tileSize);
    }
  }

  auto pumTile = AffinePattern::construct_canonical_tile(tileSizes, arraySizes);
  auto elemBits = region.elementSize * 8;
  auto startWordline = 0;

  StreamNUCAMap::addRangeMap(startPAddr, endPAddr, pumTile, elemBits,
                             startWordline);
  DPRINTF(StreamNUCAManager,
          "[StreamPUM] Map %s PAddr %#x ElemBit %d StartWdLine %d Tile %s.\n",
          region.name, startPAddr, elemBits, startWordline, pumTile);

  if (region.userDefinedProperties.count(RegionProperty::PUM_NO_INIT) &&
      region.userDefinedProperties.at(RegionProperty::PUM_NO_INIT) == 1) {
    this->markRegionCached(region.vaddr);
  }
}

std::vector<int>
StreamNUCAManager::getAlignDimsForDirectRegion(const StreamRegion &region) {

  auto dimensions = region.arraySizes.size();
  std::vector<int> ret;

  if (region.userDefinedProperties.count(RegionProperty::PUM_TILE_SIZE_DIM0)) {
    /**
     * User specified dim0 tile size. So we just set align to dim0.
     */
    ret.push_back(0);
    return ret;
  }

  for (const auto &align : region.aligns) {
    if (align.vaddrB == region.vaddr) {
      // Found a self align.
      auto elemOffset = align.elementOffset;
      auto arrayDimSize = 1;
      auto foundDim = false;
      for (auto dim = 0; dim < dimensions; ++dim) {
        if (elemOffset == arrayDimSize) {
          // Found the dimension.
          ret.push_back(dim);
          foundDim = true;
          break;
        }
        arrayDimSize *= region.arraySizes.at(dim);
      }
      if (!foundDim) {
        panic("[StreamNUCA] Region %s SelfAlign %ld Not Align to Dim.",
              region.name, align.elementOffset);
      }
    } else {
      // This array aligns to some other array.
      const auto &alignToRegion = this->getRegionFromStartVAddr(align.vaddrB);
      assert(alignToRegion.arraySizes.size() == dimensions &&
             "Mismatch in AlignedArray Dimensions.");
      return this->getAlignDimsForDirectRegion(alignToRegion);
    }
  }
  /**
   * By default we align to the first dimension.
   */
  if (ret.empty()) {
    ret.push_back(0);
  }
  return ret;
}