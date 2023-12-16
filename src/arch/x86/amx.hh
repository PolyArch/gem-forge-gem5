#ifndef __ARCH_X86_AMX_TILE_CONFIG_HH__
#define __ARCH_X86_AMX_TILE_CONFIG_HH__

#include "base/types.hh"

namespace gem5 {

namespace X86ISA {

namespace AMX {

union AMXTileConfig {
	static constexpr int MaxTiles = 8;
	static constexpr int8_t InvalidPalette = 0;
	static constexpr int ConfigBytes = 64;
  struct __attribute__((packed)) {
    uint8_t palette;
    uint8_t start_row;
    uint8_t reserved[14];
    uint16_t tile_colsb[8];
    uint8_t reserved2[16];
    uint8_t tile_rows[8];
    uint8_t reserved3[8];
  };
  uint8_t raw_data[ConfigBytes];
  bool isTileValid(int tileId) const {
    return palette > 0 && tileId >= 0 && tileId < 8 && tile_colsb[tileId] > 0 &&
           tile_rows[tileId] > 0;
  }
};

bool parseAMXTileConfig(const uint8_t *raw, AMXTileConfig &config);

std::ostream &operator<<(std::ostream &os, const AMXTileConfig &cfg);
std::string to_string(const AMXTileConfig &cfg);

} // namespace AMX
} // namespace X86ISA
} // namespace gem5

#endif