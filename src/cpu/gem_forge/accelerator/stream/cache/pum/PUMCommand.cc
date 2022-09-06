#include "PUMCommand.hh"

#include <iomanip>

std::ostream &operator<<(std::ostream &os, const PUMCommand &command) {
  os << command.to_string();
  return os;
}

std::string PUMCommand::to_string(int llcBankIdx) const {
  std::stringstream os;
  os << "[PUMCmd " << type << " WD-" << wordline_bits << "]\n";
  if (hasReuse()) {
    os << "  Reuse          " << reuse.dim << " x" << reuse.count << "\n";
  }
  if (srcRegion != "none") {
    os << "  Src " << srcRegion << " Acc " << srcAccessPattern << " Map "
       << srcMapPattern << '\n';
  }
  if (dstRegion != "none") {
    os << "  Dst " << dstRegion << " Acc " << dstAccessPattern << " Map "
       << dstMapPattern << '\n';
  }
  os << "  BitlineMask    " << bitline_mask << '\n';
  os << "  TileMask       " << tile_mask << '\n';
  if (type == "intra-array") {
    os << "  BitlineDist    " << bitline_dist << '\n';
  } else if (type == "inter-array") {
    os << "  TileDist       " << tile_dist << '\n';
    os << "  DstBitlineMask " << dst_bitline_mask << '\n';
    for (auto i = 0; i < inter_array_splits.size(); ++i) {
      os << "    InterArraySplit " << std::setw(2) << i << '\n';
      const auto &patterns = inter_array_splits[i];
      for (auto j = 0; j < patterns.size(); ++j) {
        os << "      " << getAffinePatternFromImpl(patterns[j]) << '\n';
      }
    }
  } else {
    // Compute command.
    os << "  Op " << Enums::OpClassStrings[opClass]
       << (isReduction ? " [Reduce] " : "") << '\n';
  }
  for (auto i = 0; i < llcSplitTileCmds.NumBanks; ++i) {
    if (llcBankIdx != -1 && i != llcBankIdx) {
      continue;
    }
    auto subRegionCount = llcSplitTileCmds.getBankSubRegionCount(i);
    if (subRegionCount > 0) {
      os << "    LLCCmd " << std::setw(2) << i;
      for (auto j = 0; j < subRegionCount; ++j) {
        os << "  " << llcSplitTileCmds.getAffinePattern(i, j);
        if (type == "inter-array" && hasReuse()) {

          const auto &dstTilePat = llcSplitDstTileCmds[i][j];

          os << " -> " << dstTilePat.dstTilePattern << "\n";

          const auto &dstSplitPats = dstTilePat.dstSplitTilePatterns;
          for (auto dstBankIdx = 0; dstBankIdx < dstSplitPats.size();
               ++dstBankIdx) {
            if (dstSplitPats[dstBankIdx].empty()) {
              continue;
            }
            os << "        DstBank " << dstBankIdx << " ";
            for (const auto &dstPat : dstSplitPats[dstBankIdx]) {
              os << dstPat << " ";
            }
            os << "\n";
          }
        } else {
          os << "\n";
        }
      }
    }
  }
  return os.str();
}