#include "stream_nuca_map.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "base/trace.hh"

#include "debug/MLCStreamPUM.hh"
#include "debug/StreamNUCAMap.hh"

bool StreamNUCAMap::topologyInitialized = false;
int StreamNUCAMap::numRows = 0;
int StreamNUCAMap::numCols = 0;
bool StreamNUCAMap::cacheInitialized = false;
StreamNUCAMap::CacheParams StreamNUCAMap::cacheParams;
StreamNUCAMap::NonUniformNodeVec StreamNUCAMap::numaNodes;
std::map<Addr, StreamNUCAMap::RangeMap> StreamNUCAMap::rangeMaps;
std::map<int, Addr> StreamNUCAMap::pumWordlineToRangeMap;

void StreamNUCAMap::initializeTopology(int numRows, int numCols) {
  if (topologyInitialized) {
    if (numCols != StreamNUCAMap::numCols ||
        numRows != StreamNUCAMap::numRows) {
      panic("Mismatch in NumRows %d != %d or NumCols %d != %d.", numRows,
            StreamNUCAMap::numRows, numCols, StreamNUCAMap::numCols);
    }
    return;
  } else {
    StreamNUCAMap::numCols = numCols;
    StreamNUCAMap::numRows = numRows;
    StreamNUCAMap::topologyInitialized = true;
  }
}

void StreamNUCAMap::initializeCache(const CacheParams &cacheParams) {
  if (cacheInitialized) {
    if (StreamNUCAMap::cacheParams != cacheParams) {
      panic("Mismatch in CacheParams.\n");
    }
    return;
  } else {
    StreamNUCAMap::cacheParams = cacheParams;
    StreamNUCAMap::cacheInitialized = true;
  }
}

void StreamNUCAMap::addNonUniformNode(int routerId, MachineID machineId,
                                      const AddrRange &addrRange,
                                      const std::vector<int> &handleBanks) {
  if (machineId.getType() != MachineType_Directory) {
    return;
  }
  DPRINTF(StreamNUCAMap,
          "[StreamNUCA] Add NonUniformNode %s RouterId %d AddrRange %s.\n",
          machineId, routerId, addrRange.to_string());
  numaNodes.emplace_back(routerId, machineId, addrRange, handleBanks);
  std::sort(numaNodes.begin(), numaNodes.end(),
            [](const NonUniformNode &A, const NonUniformNode &B) -> bool {
              return A.machineId.getNum() < B.machineId.getNum();
            });
}

const StreamNUCAMap::NonUniformNode &
StreamNUCAMap::mapPAddrToNUMANode(Addr paddr) {
  if (numaNodes.empty()) {
    panic("No NUMA nodes found.");
  }
  for (const auto &numaNode : numaNodes) {
    if (numaNode.addrRange.contains(paddr)) {
      return numaNode;
    }
  }
  panic("Failed to Find NUMA Node for PAddr %#x.", paddr);
}

int StreamNUCAMap::mapPAddrToNUMARouterId(Addr paddr) {
  return mapPAddrToNUMANode(paddr).routerId;
}

int StreamNUCAMap::mapPAddrToNUMAId(Addr paddr) {
  return mapPAddrToNUMANode(paddr).machineId.getNum();
}

int64_t StreamNUCAMap::computeHops(int64_t bankA, int64_t bankB) {
  int64_t bankARow = bankA / getNumCols();
  int64_t bankACol = bankA % getNumCols();
  int64_t bankBRow = bankB / getNumCols();
  int64_t bankBCol = bankB % getNumCols();
  return std::abs(bankARow - bankBRow) + std::abs(bankACol - bankBCol);
}

void StreamNUCAMap::checkOverlapRange(Addr startPAddr, Addr endPAddr) {
  // Simple sanity check that not overlap with existing ranges.
  for (const auto &entry : rangeMaps) {
    const auto &range = entry.second;
    if (range.startPAddr >= endPAddr || range.endPAddr <= startPAddr) {
      continue;
    }
    panic("Overlap in StreamNUCA RangeMap [%#x, %#x) [%#x, %#x).", startPAddr,
          endPAddr, range.startPAddr, range.endPAddr);
  }
}

void StreamNUCAMap::addRangeMap(Addr startPAddr, Addr endPAddr,
                                uint64_t interleave, int startBank,
                                int startSet) {
  checkOverlapRange(startPAddr, endPAddr);
  DPRINTF(StreamNUCAMap, "Add PAddrRangeMap [%#x, %#x) %% %lu + %d.\n",
          startPAddr, endPAddr, interleave, startBank);
  rangeMaps.emplace(std::piecewise_construct, std::forward_as_tuple(startPAddr),
                    std::forward_as_tuple(startPAddr, endPAddr, interleave,
                                          startBank, startSet));
}

void StreamNUCAMap::addRangeMap(Addr startPAddr, Addr endPAddr,
                                const AffinePattern &pumTile, int elementBits,
                                int startWordline, int vBitlines) {
  checkOverlapRange(startPAddr, endPAddr);
  DPRINTF(
      StreamNUCAMap,
      "Add PUM PAddrRangeMap [%#x, %#x) ElemBits %d StartWdLine %d Tile %s.\n",
      startPAddr, endPAddr, elementBits, startWordline, pumTile);
  rangeMaps.emplace(std::piecewise_construct, std::forward_as_tuple(startPAddr),
                    std::forward_as_tuple(startPAddr, endPAddr, pumTile,
                                          elementBits, startWordline,
                                          vBitlines));
}

StreamNUCAMap::RangeMap &
StreamNUCAMap::getRangeMapByStartPAddr(Addr startPAddr) {
  auto iter = rangeMaps.find(startPAddr);
  if (iter == rangeMaps.end()) {
    panic("Failed to find Range by StartPAddr %#x.", startPAddr);
  }
  return iter->second;
}

StreamNUCAMap::RangeMap *StreamNUCAMap::getRangeMapContaining(Addr paddr) {
  auto iter = rangeMaps.upper_bound(paddr);
  if (iter == rangeMaps.begin()) {
    return nullptr;
  }
  --iter;
  auto &range = iter->second;
  if (range.endPAddr <= paddr) {
    return nullptr;
  }
  return &range;
}

int StreamNUCAMap::getNUCABank(Addr paddr, const RangeMap &range) {
  assert(!range.isStreamPUM);
  auto interleave = range.interleave;
  auto startPAddr = range.startPAddr;
  auto endPAddr = range.endPAddr;
  auto startBank = range.startBank;
  auto diffPAddr = paddr - startPAddr;
  auto bank = startBank + diffPAddr / interleave;
  bank = bank % (getNumRows() * getNumCols());
  DPRINTF(StreamNUCAMap,
          "Map PAddr %#x in [%#x, %#x) %% %lu + StartBank(%d) to Bank %d of "
          "%dx%d.\n",
          paddr, startPAddr, endPAddr, interleave, startBank, bank,
          getNumRows(), getNumCols());
  return bank;
}

StreamNUCAMap::SRAMLocation
StreamNUCAMap::getPUMLocation(Addr paddr, const RangeMap &range) {
  assert(range.isStreamPUM);

  auto elemIdx = (paddr - range.startPAddr) / (range.elementBits / 8);
  auto bitlineIdx = range.pumTileRev(elemIdx);

  const auto &cacheParams = getCacheParams();

  /**
   * Now we have virtual bitlines, handle the possible wrap around.
   */

  auto tileSize = range.pumTile.getCanonicalTotalTileSize();
  auto pBitlines = cacheParams.bitlines;

  auto vBitlines = range.vBitlines;

  auto tileIdx = bitlineIdx / tileSize;
  auto vBitlineIdx = tileIdx * vBitlines + bitlineIdx % tileSize;

  auto vBitlineIdxWithinTile = vBitlineIdx % vBitlines;

  auto pBitlineIdxWithinTile = vBitlineIdxWithinTile % pBitlines;
  auto pWordlineIdx = (vBitlineIdxWithinTile / pBitlines) * range.elementBits +
                      range.startWordline;

  // One tile is always one SRAM array.
  auto arrayIdx = tileIdx;
  auto wayIdx = arrayIdx / cacheParams.arrayPerWay;
  auto bankIdx = wayIdx / cacheParams.assoc;

  auto bank = bankIdx % (getNumRows() * getNumCols());

  // So far wordline is not modelled, always 0.
  SRAMLocation loc;
  loc.bank = bank;
  loc.way = wayIdx % cacheParams.assoc;
  loc.array = arrayIdx % cacheParams.arrayPerWay;
  loc.bitline = pBitlineIdxWithinTile;
  loc.wordline = pWordlineIdx;

  DPRINTF(StreamNUCAMap,
          "[PUM] Map PAddr %#x in [%#x, %#x) Tile %s to Bank %d Way %d Array "
          "%d BL %d WL %d.\n",
          paddr, range.startPAddr, range.endPAddr, range.pumTile, loc.bank,
          loc.way, loc.array, loc.bitline, loc.wordline);
  return loc;
}

int StreamNUCAMap::getBank(Addr paddr) {
  if (auto *range = getRangeMapContaining(paddr)) {
    if (range->isStreamPUM) {
      return getPUMLocation(paddr, *range).bank;
    } else {
      return getNUCABank(paddr, *range);
    }
  }
  return -1;
}

int StreamNUCAMap::getNUCASet(Addr paddr, const RangeMap &range) {
  assert(!range.isStreamPUM);

  auto interleave = range.interleave;
  auto startPAddr = range.startPAddr;
  auto endPAddr = range.endPAddr;
  auto startSet = range.startSet;
  auto diffPAddr = paddr - startPAddr;
  auto globalBankInterleave = interleave * getNumCols() * getNumRows();
  /**
   * Skip the line bits and bank bits.
   */
  auto localBankOffset = diffPAddr % interleave;
  auto globalBankOffset = diffPAddr / globalBankInterleave;

  auto setNum = (globalBankOffset * (interleave / getCacheBlockSize())) +
                localBankOffset / getCacheBlockSize();

  auto finalSetNum = (setNum + startSet) % getCacheNumSet();

  DPRINTF(StreamNUCAMap,
          "Map PAddr %#x in [%#x, %#x) %% %lu + StartSet(%d) "
          "to Set %d of %d.\n",
          paddr, startPAddr, endPAddr, interleave, startSet, finalSetNum,
          getCacheNumSet());

  return finalSetNum;
}

int StreamNUCAMap::getPUMSet(Addr paddr, const RangeMap &range) {
  assert(range.isStreamPUM);

  /**
   * I found it not eazy to specify the set of the cache line in PUM mapping.
   *
   * In normal cache setting, one line is splitted among the arrays within
   * that way, to better utitlize the internal bandwidth.
   *
   * For example, with 8 SRAM arrays per way, and 64B cache line size, each
   * array is holding 64/8 = 8B data of each cache line.
   * If the array size is 256x256, each row is 32B. There are 256*32/8=1k sets.
   * And they can be indexed as:
   *
   * SRAM Array:
   * ---------------------------------
   * | Set 0 | Set 1 | Set 2 | Set 3 |
   * | Set 4 | Set 5 | Set 6 | Set 7 |
   * |  ...  |  ...  |  ...  |  ...  |
   * ---------------------------------
   *
   * In PUM, data is tranposed and tiled. We try to get an approximate set
   * number by looking at the specific bitline index within that way, divided by
   * the number of elements per cache line, and multiple by the starting
   * wordline.
   *
   */

  const auto &cacheParams = getCacheParams();
  auto location = getPUMLocation(paddr, range);

  auto vBitlineWrap =
      (location.wordline - range.startWordline) / range.elementBits;
  auto vBitlineIdxInWay = location.array * range.vBitlines +
                          vBitlineWrap * cacheParams.bitlines +
                          location.bitline;

  const auto cacheBlockSize = getCacheBlockSize();
  assert(paddr % cacheBlockSize == 0 && "Not Align to Line.");
  const auto elementsPerLine = cacheBlockSize / (range.elementBits / 8);

  auto cacheSetIdx = vBitlineIdxInWay / elementsPerLine;
  auto finalCacheSetIdx = cacheSetIdx + range.startSet;

  assert(finalCacheSetIdx < getCacheNumSet() && "CacheSet Overflow.");

  DPRINTF(StreamNUCAMap,
          "[PUM] Map PAddr %#x in [%#x, %#x) Tile %s to Set %d+%d=%d.\n", paddr,
          range.startPAddr, range.endPAddr, range.pumTile, cacheSetIdx,
          range.startSet, finalCacheSetIdx);
  return finalCacheSetIdx;
}

int StreamNUCAMap::getSet(Addr paddr) {
  if (auto *range = getRangeMapContaining(paddr)) {
    if (range->isStreamPUM) {
      return getPUMSet(paddr, *range);
    } else {
      return getNUCASet(paddr, *range);
    }
  }
  return -1;
}

PUMHWConfiguration StreamNUCAMap::getPUMHWConfig() {
  const auto &p = StreamNUCAMap::getCacheParams();

  auto meshLayers = 1;
  auto meshRows = StreamNUCAMap::getNumRows();
  auto meshCols = StreamNUCAMap::getNumCols();

  return PUMHWConfiguration(p.wordlines, p.bitlines, p.arrayPerWay,
                            p.arrayTreeDegree, p.arrayTreeLeafBandwidth,
                            p.assoc, p.wayTreeDegree, meshLayers, meshRows,
                            meshCols);
}

void StreamNUCAMap::setWordlineForRange(Addr startPAddr, int wordline) {
  clearWordline(wordline, startPAddr);

  auto &range = getRangeMapByStartPAddr(startPAddr);

  auto prevWordline = range.startWordline;
  if (prevWordline != RangeMap::InvalidWordline) {
    // Release the previous mapping.
    pumWordlineToRangeMap.erase(prevWordline);
  }

  DPRINTF(MLCStreamPUM, "[PUM] SetWL [%#x, %#x) WL %d Tile %s.\n",
          range.startPAddr, range.endPAddr, wordline, range.pumTile);
  range.startWordline = wordline;
  pumWordlineToRangeMap.emplace(wordline, startPAddr);
}

void StreamNUCAMap::clearWordline(int wordline, Addr skipPAddr) {

  if (!pumWordlineToRangeMap.count(wordline)) {
    return;
  }

  // Release the previous mapped region as uncached.
  auto prevStartPAddr = pumWordlineToRangeMap.at(wordline);
  auto &prevRange = getRangeMapByStartPAddr(prevStartPAddr);
  prevRange.startWordline = RangeMap::InvalidWordline;
  if (prevStartPAddr != skipPAddr) {
    evictRange(prevRange);
    prevRange.isCached = false;
  }
  pumWordlineToRangeMap.erase(wordline);
  DPRINTF(MLCStreamPUM, "[PUM] ClearWL [%#x, %#x) WL %d Cached? %d.\n",
          prevRange.startPAddr, prevRange.endPAddr, wordline,
          prevRange.isCached);
}

void StreamNUCAMap::evictRange(RangeMap &range) {

  /**
   * Evict all cache lines within this range.
   */
  const auto lineSize = getCacheBlockSize();
  assert(range.startPAddr % lineSize == 0);
  assert(range.endPAddr % lineSize == 0);
  for (auto paddr = range.startPAddr; paddr < range.endPAddr;
       paddr += lineSize) {

    MachineID machineId(MachineType_L2Cache, 0);
    auto controller = AbstractStreamAwareController::getController(machineId);

    auto llcMachineId =
        controller->mapAddressToLLCOrMem(paddr, MachineType_L2Cache);
    auto llc = AbstractStreamAwareController::getController(llcMachineId);
    llc->evictCleanLine(paddr);

    auto dirMachineId =
        controller->mapAddressToLLCOrMem(paddr, MachineType_Directory);
    auto dir = AbstractStreamAwareController::getController(dirMachineId);
    dir->evictCleanLine(paddr);
  }
}