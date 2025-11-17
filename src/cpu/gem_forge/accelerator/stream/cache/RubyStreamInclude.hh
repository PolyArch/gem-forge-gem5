#ifndef __GEM_FORGE_RUBY_STREAM_INCLUDE_HH__
#define __GEM_FORGE_RUBY_STREAM_INCLUDE_HH__

/**
 * This file includes all necessary Ruby generated files for
 * stream cache engine.
 */
#include "mem/ruby/protocol/MESI_Three_Level_Stream/CoherenceMsg.hh"
#include "mem/ruby/protocol/MESI_Three_Level_Stream/RequestMsg.hh"
#include "mem/ruby/protocol/MESI_Three_Level_Stream/ResponseMsg.hh"
#include "mem/ruby/protocol/MESI_Three_Level_Stream/StreamMigrateRequestMsg.hh"

namespace gem5 {
namespace ruby_stream = ruby::MESI_Three_Level_Stream;
}

#endif