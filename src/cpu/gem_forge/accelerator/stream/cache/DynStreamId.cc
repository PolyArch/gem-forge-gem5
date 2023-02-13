#include "DynStreamId.hh"

#include <sstream>

namespace gem5 {

std::ostream &operator<<(std::ostream &os, const DynStreamId &streamId) {
  os << streamId.streamName << " -" << streamId.coreId << '-'
     << streamId.staticId << '-' << streamId.streamInstance << "-";
  return os;
}

std::string to_string(const DynStreamId &streamId) {
  std::ostringstream ss;
  ss << streamId;
  return ss.str();
}
} // namespace gem5

