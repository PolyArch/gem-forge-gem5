#include "amx.hh"

#include <sstream>

namespace gem5 {
namespace X86ISA {
namespace AMX {

bool parseAMXTileConfig(const uint8_t *raw, AMXTileConfig &config) {

  bool error = false;
  auto temp = reinterpret_cast<const AMXTileConfig *>(raw);
  config = *temp;

  // So far we only have 1 palette: 8 tile, each 16x64B.
  const int max_palette = 1;
  const int max_colsb = 64;
  const int max_row = 16;
  const int max_tiles = 8;

  if (config.palette > max_palette) {
    error = true;
  }

  // We need some sanity check.
  if (config.palette != 0) {

    // Check first reserved fields are 0.
    for (const auto &r : config.reserved) {
      if (r != 0) {
        error = true;
      }
    }

    // Check the colsb.
    for (int i = 0; i < max_tiles; ++i) {
      auto colsb = config.tile_colsb[i];
      if (colsb > max_colsb) {
        error = true;
      }
    }

    // Check reserved.
    for (const auto &r : config.reserved2) {
      if (r != 0) {
        error = true;
      }
    }

    // Check row.
    for (const auto &row : config.tile_rows) {
      if (row > max_row) {
        error = true;
      }
    }

    // Check that row and col can be all zero or non-zero.
    for (int i = 0; i < max_tiles; ++i) {
      const auto row = config.tile_rows[i];
      const auto col = config.tile_colsb[i];
      if ((row == 0 && col != 0) || (row != 0 && col == 0)) {
        error = true;
      }
    }

    for (const auto &r : config.reserved3) {
      if (r != 0) {
        error = true;
      }
    }
  }
  return error;
}

std::ostream &operator<<(std::ostream &os, const AMXTileConfig &cfg) {
  os << "Tile Pallete " << (int)cfg.palette;
  if (cfg.palette != cfg.InvalidPalette) {
    os << " StartRow " << (int)cfg.start_row;
    for (int i = 0; i < cfg.MaxTiles; ++i) {
      if (cfg.isTileValid(i)) {
        os << " T" << i << " = " << (int)cfg.tile_rows[i] << "x"
           << cfg.tile_colsb[i] << "B";
      }
    }
  }
  return os;
}

std::string to_string(const AMXTileConfig &cfg) {
  std::ostringstream ss;
  ss << cfg;
  return ss.str();
}

} // namespace AMX
} // namespace X86ISA
} // namespace gem5