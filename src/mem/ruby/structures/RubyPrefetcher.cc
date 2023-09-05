/*
 * Copyright (c) 2020 Inria
 * Copyright (c) 2020 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 1999-2012 Mark D. Hill and David A. Wood
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mem/ruby/structures/RubyPrefetcher.hh"

#include <cassert>

#include "sim/system.hh"
#include "base/bitfield.hh"
#include "debug/RubyPrefetcher.hh"
#include "mem/ruby/slicc_interface/RubySlicc_ComponentMapping.hh"
#include "mem/ruby/system/RubySystem.hh"

namespace gem5
{

namespace ruby
{

RubyPrefetcher::RubyPrefetcher(const Params &p)
    : SimObject(p), m_num_streams(p.num_streams),
    m_array(p.num_streams), m_train_misses(p.train_misses),
    m_num_startup_pfs(p.num_startup_pfs),
    m_bulk_prefetch_size(p.bulk_prefetch_size),
    unitFilter(p.unit_filter),
    negativeFilter(p.unit_filter),
    nonUnitFilter(p.nonunit_filter),
    m_prefetch_cross_pages(p.cross_page),
    pageShift(p.page_shift),
    rubyPrefetcherStats(this)
{
    assert(m_num_streams > 0);
    assert(m_num_startup_pfs <= MAX_PF_INFLIGHT);
}

RubyPrefetcher::
RubyPrefetcherStats::RubyPrefetcherStats(statistics::Group *parent)
    : statistics::Group(parent, "RubyPrefetcher"),
      ADD_STAT(numMissObserved, "Number of misses observed"),
      ADD_STAT(numAllocatedStreams, "Number of streams allocated for "
                                    "prefetching"),
      ADD_STAT(numPrefetchRequested, "Number of prefetch requests made"),
      ADD_STAT(numPrefetchedHits, "Number of prefetched blocks accessed "
                        "(for the first time)"),
      ADD_STAT(numUnprefetchedHits,
        "Number of hits on blocks that is not prefetched."),
      ADD_STAT(numPartialHits, "Number of misses observed for a block being "
                               "prefetched"),
      ADD_STAT(numUnusedPrefetchedBlocks,
        "Num of prefetched but evicted as unused blocks"),
      ADD_STAT(numPrefetchAlreadyCachedBlocks,
        "Num of prefetched but already cached blocks"),
      ADD_STAT(numPrefetchNextButStreamReleased,
        "Num of prefetch next but the stream already released."),
      ADD_STAT(numPagesCrossed, "Number of prefetches across pages"),
      ADD_STAT(numMissedPrefetchedBlocks, "Number of misses for blocks that "
                                          "were prefetched, yet missed")
{
}

void
RubyPrefetcher::observeMissWithPC(
    Addr address, const RubyRequestType& type, Addr pc)
{
    if (type == RubyRequestType::RubyRequestType_IFETCH
        && !params().prefetch_inst) {
        return;
    }
    DPRINTF(RubyPrefetcher, "ObserveMiss for %#x pc %#x %s\n",
        address, pc, RubyRequestType_to_string(type));
    Addr line_addr = makeLineAddress(address);
    rubyPrefetcherStats.numMissObserved++;

    // check to see if we have already issued a prefetch for this block
    uint32_t index = 0;
    PrefetchEntry *pfEntry = getPrefetchEntry(line_addr, index);
    if (pfEntry != NULL) {
        if (pfEntry->requestIssued[index]) {
            if (pfEntry->requestCompleted[index]) {
                // We prefetched too early and now the prefetch block no
                // longer exists in the cache
                rubyPrefetcherStats.numMissedPrefetchedBlocks++;
                return;
            } else {
                // The controller has issued the prefetch request,
                // but the request for the block arrived earlier.
                observePfMiss(line_addr);
                return;
            }
        } else {
            // The request is still in the prefetch queue of the controller.
            // Or was evicted because of other requests.
            return;
        }
    }

    // Check if address is in any of the stride filters
    if (accessUnitFilter(&unitFilter, line_addr, 1, type)) {
        return;
    }
    if (accessUnitFilter(&negativeFilter, line_addr, -1, type)) {
        return;
    }
    if (accessNonunitFilter(line_addr, type)) {
        return;
    }
}

void
RubyPrefetcher::observeHitWithPC(
    Addr address, const RubyRequestType& type, Addr pc)
{
    if (type == RubyRequestType::RubyRequestType_IFETCH
        && !params().prefetch_inst) {
        return;
    }
    DPRINTF(RubyPrefetcher, "ObserveHit for %#x pc %#x %s\n",
        address, pc, RubyRequestType_to_string(type));
    rubyPrefetcherStats.numUnprefetchedHits++;

    if (!this->params().observe_hit) {
        // Do not enable prefetch on hits.
        return;
    }

    Addr line_addr = makeLineAddress(address);

    // check to see if we have already issued a prefetch for this block
    uint32_t index = 0;
    PrefetchEntry *pfEntry = getPrefetchEntry(line_addr, index);
    if (pfEntry != NULL) {
        // We have an allocated stream. Try to issue next one.
        issueNextPrefetch(line_addr, pfEntry);
        return;
    }

    // Check if address is in any of the stride filters
    if (accessUnitFilter(&unitFilter, line_addr, 1, type)) {
        return;
    }
    if (accessUnitFilter(&negativeFilter, line_addr, -1, type)) {
        return;
    }
    if (accessNonunitFilter(line_addr, type)) {
        return;
    }

}

void
RubyPrefetcher::observePfMiss(Addr address)
{
    rubyPrefetcherStats.numPartialHits++;
    DPRINTF(RubyPrefetcher, "Observed partial hit for %#x\n", address);
    issueNextPrefetch(address, NULL);
}

void
RubyPrefetcher::observePfHit(Addr address)
{
    rubyPrefetcherStats.numPrefetchedHits++;
    DPRINTF(RubyPrefetcher, "Observed hit for %#x\n", address);
    issueNextPrefetch(address, NULL);
}

void
RubyPrefetcher::observePfEvictUnused(Addr paddr)
{
    rubyPrefetcherStats.numUnusedPrefetchedBlocks++;
    DPRINTF(RubyPrefetcher, "Observed evict unused pf for %#x\n", paddr);
}

void
RubyPrefetcher::observePfAlreadyCached(Addr paddr)
{
    rubyPrefetcherStats.numPrefetchAlreadyCachedBlocks++;
    DPRINTF(RubyPrefetcher, "Observed already cached pf for %#x\n", paddr);
}

void
RubyPrefetcher::issueNextPrefetch(Addr address, PrefetchEntry *stream)
{
    // get our corresponding stream fetcher
    if (stream == NULL) {
        uint32_t index = 0;
        stream = getPrefetchEntry(address, index);
    }

    // if (for some reason), this stream is unallocated, return.
    if (stream == NULL) {
        DPRINTF(RubyPrefetcher, "Unallocated stream, returning\n");
        rubyPrefetcherStats.numPrefetchNextButStreamReleased++;
        return;
    }

    /**
     * If we are using bulk prefetching, we delay prefetches
     * until we have half the prefetch distance.
     */
    stream->m_num_delayed_prefetches++;
    if (this->m_bulk_prefetch_size > 1 &&
        stream->m_num_delayed_prefetches < this->m_bulk_prefetch_size) {
        DPRINTF(RubyPrefetcher, "Delayed %d pfs, returning\n",
            stream->m_num_delayed_prefetches);
        return;
    }

    RubyAddressBulk addrBulk;
    // First is our base addr.
    for (int i = 0; i < stream->m_num_delayed_prefetches; ++i) {
        // extend this prefetching stream by 1
        Addr page_addr = pageAddress(stream->m_address);
        Addr line_addr = makeNextStrideAddress(stream->m_address,
                                               stream->m_stride);

        // possibly stop prefetching at page boundaries
        if (page_addr != pageAddress(line_addr)) {
            if (!m_prefetch_cross_pages) {
                // Deallocate the stream since we are not prefetching
                // across page boundries
                stream->m_is_valid = false;
                break;
            }
            rubyPrefetcherStats.numPagesCrossed++;
        }

        // This line address should be prefetched.
        addrBulk.push(line_addr);
        stream->m_address = line_addr;
    }

    if (addrBulk.empty()) {
        return;
    }

    // launch next prefetch
    rubyPrefetcherStats.numPrefetchRequested += addrBulk.size();

    stream->m_use_time = m_controller->curCycle();
    stream->m_num_delayed_prefetches = 0;
    auto line_addr = addrBulk.getAt(0);
    if (addrBulk.size() == 1) {
        // Normal case.
        DPRINTF(RubyPrefetcher, "Requesting prefetch for %#x\n", line_addr);
        m_controller->enqueuePrefetch(line_addr, stream->m_type);
    } else {
        // Bulk prefetch.
        DPRINTF(RubyPrefetcher,
            "Requesting bulk prefetch (size %d) for %#x\n",
            addrBulk.size(), line_addr);
        m_controller->enqueueBulkPrefetch(line_addr, stream->m_type, addrBulk);
    }
}

uint32_t
RubyPrefetcher::getLRUindex(void)
{
    uint32_t lru_index = 0;
    Cycles lru_access = m_array[lru_index].m_use_time;

    for (uint32_t i = 0; i < m_num_streams; i++) {
        if (!m_array[i].m_is_valid) {
            return i;
        }
        if (m_array[i].m_use_time < lru_access) {
            lru_access = m_array[i].m_use_time;
            lru_index = i;
        }
    }

    return lru_index;
}

void
RubyPrefetcher::initializeStream(Addr address, int stride,
     uint32_t index, const RubyRequestType& type)
{

    DPRINTF(RubyPrefetcher,
        "Initialize stream, line %#x, page %#x, stride %d, LRU pos %u.\n",
        makeLineAddress(address), pageAddress(address), stride, index);
    if (Debug::RubyPrefetcher) {
        for (int i = 0; i < m_array.size(); ++i) {
            const auto &stream = m_array[i];
            if (!stream.m_is_valid) {
                continue;
            }
            DPRINTF(RubyPrefetcher,
                "[CurStrm] %3d page %#x line %#x stride %3d\n",
                i,
                pageAddress(stream.m_address),
                makeLineAddress(stream.m_address),
                stream.m_stride);
        }
    }
    if (params().filter_dup) {
        /**
         * We first deduplicate streams.
         * Then check if the new stream is also duplicated.
         */
        for (int i = 1; i < m_array.size(); ++i) {
            auto &si= m_array[i];
            if (!si.m_is_valid) {
                continue;
            }
            for (int j = 0; j < i; ++j) {
                const auto &sj = m_array[j];
                if (!sj.m_is_valid) {
                    continue;
                }
                if (si.m_stride == sj.m_stride &&
                    si.m_address == sj.m_address &&
                    si.m_type == sj.m_type) {
                    DPRINTF(RubyPrefetcher,
                        "Dedup stream %d.\n", i);
                    si.m_is_valid = false; 
                    break;
                }
            }
        }
        for (int i = 0; i < m_array.size(); ++i) {
            const auto &stream = m_array[i];
            if (stream.m_is_valid &&
                pageAddress(address) == pageAddress(stream.m_address) &&
                stride == stream.m_stride &&
                type == stream.m_type) {
                DPRINTF(RubyPrefetcher, "Filtered duplicated stream.\n");
                return;
            }
        }
    }

    rubyPrefetcherStats.numAllocatedStreams++;

    // initialize the stream prefetcher
    PrefetchEntry *mystream = &(m_array[index]);
    mystream->m_address = makeLineAddress(address);
    mystream->m_stride = stride;
    mystream->m_use_time = m_controller->curCycle();
    mystream->m_is_valid = true;
    mystream->m_type = type;

    // create a number of initial prefetches for this stream
    Addr page_addr = pageAddress(mystream->m_address);
    Addr line_addr = makeLineAddress(mystream->m_address);

    // insert a number of prefetches into the prefetch table
    for (int k = 0; k < m_num_startup_pfs; k++) {
        line_addr = makeNextStrideAddress(line_addr, stride);
        // possibly stop prefetching at page boundaries
        if (page_addr != pageAddress(line_addr)) {
            if (!m_prefetch_cross_pages) {
                // deallocate this stream prefetcher
                mystream->m_is_valid = false;
                return;
            }
            rubyPrefetcherStats.numPagesCrossed++;
        }
        /**
         * ! GemForge
         * When cross-page enabled, we have to make sure this is a valid address.
         */
        auto sys = params().sys;
        if (!sys->isMemAddr(line_addr)) {
            DPRINTF(RubyPrefetcher, "NonMem prefetching line %#x\n", line_addr);
            mystream->m_is_valid = false;
            return;
        }

        // launch prefetch
        rubyPrefetcherStats.numPrefetchRequested++;
        DPRINTF(RubyPrefetcher, "Requesting prefetch for %#x\n", line_addr);
        m_controller->enqueuePrefetch(line_addr, m_array[index].m_type);
    }

    // update the address to be the last address prefetched
    mystream->m_address = line_addr;
    mystream->m_num_delayed_prefetches = 0;
}

PrefetchEntry *
RubyPrefetcher::getPrefetchEntry(Addr address, uint32_t &index)
{
    // search all streams for a match
    for (int i = 0; i < m_num_streams; i++) {
        // search all the outstanding prefetches for this stream
        auto &stream = m_array[i];
        if (stream.m_is_valid) {
            for (int j = 0; j < m_num_startup_pfs; j++) {
                Addr pfAddr = makeNextStrideAddress(stream.m_address,
                    -(stream.m_stride * j));
                if (pfAddr == address) {
                    DPRINTF(RubyPrefetcher,
                        "Match Stream %#x, Stride %d, Index %u.\n",
                        stream.m_address, stream.m_stride, j);
                    return &stream;
                }
            }
        }
    }
    return NULL;
}

bool
RubyPrefetcher::accessUnitFilter(CircularQueue<UnitFilterEntry>* const filter,
    Addr line_addr, int stride, const RubyRequestType& type)
{
    for (auto& entry : *filter) {
        if (entry.addr == line_addr) {
            entry.addr = makeNextStrideAddress(entry.addr, stride);
            entry.hits++;
            DPRINTF(RubyPrefetcher, "  *** hit %d in unit stride %d buffer\n",
                entry.hits, stride);
            if (entry.hits >= m_train_misses) {
                // Allocate a new prefetch stream
                initializeStream(line_addr, stride, getLRUindex(), type);
            }
            return true;
        }
    }

    // Enter this address in the filter
    filter->push_back(UnitFilterEntry(
        makeNextStrideAddress(line_addr, stride)));

    return false;
}

bool
RubyPrefetcher::accessNonunitFilter(Addr line_addr,
    const RubyRequestType& type)
{
    /// look for non-unit strides based on a (user-defined) page size
    Addr page_addr = pageAddress(line_addr);

    for (auto& entry : nonUnitFilter) {
        if (pageAddress(entry.addr) == page_addr) {
            // hit in the non-unit filter
            // compute the actual stride (for this reference)
            int delta = line_addr - entry.addr;

            if (delta != 0) {
                // no zero stride prefetches
                // check that the stride matches (for the last N times)
                DPRINTF(RubyPrefetcher,
                    "  *** hit in non-unit stride buffer. "
                    "hits %d stride %ld delta %ld\n",
                    entry.hits, entry.stride, delta);
                if (delta == entry.stride) {
                    // -> stride hit
                    // increment count (if > m_train_misses) allocate stream
                    entry.hits++;
                    if (entry.hits > m_train_misses) {
                        // This stride HAS to be the multiplicative constant of
                        // dataBlockBytes (bc makeNextStrideAddress is
                        // calculated based on this multiplicative constant!)
                        const int stride = entry.stride /
                            RubySystem::getBlockSizeBytes();

                        // clear this filter entry
                        entry.clear();

                        initializeStream(line_addr, stride, getLRUindex(),
                            type);
                    }
                } else {
                    // If delta didn't match reset entry's hit count
                    entry.hits = 0;
                }

                // update the last address seen & the stride
                entry.addr = line_addr;
                entry.stride = delta;
                return true;
            } else {
                return false;
            }
        }
    }

    // not found: enter this address in the table
    nonUnitFilter.push_back(NonUnitFilterEntry(line_addr));

    return false;
}

void
RubyPrefetcher::print(std::ostream& out) const
{
    out << name() << " Prefetcher State\n";
    // print out unit filter
    out << "unit table:\n";
    for (const auto& entry : unitFilter) {
        out << entry.addr << std::endl;
    }

    out << "negative table:\n";
    for (const auto& entry : negativeFilter) {
        out << entry.addr << std::endl;
    }

    // print out non-unit stride filter
    out << "non-unit table:\n";
    for (const auto& entry : nonUnitFilter) {
        out << entry.addr << " "
            << entry.stride << " "
            << entry.hits << std::endl;
    }

    // print out allocated stream buffers
    out << "streams:\n";
    for (int i = 0; i < m_num_streams; i++) {
        out << m_array[i].m_address << " "
            << m_array[i].m_stride << " "
            << m_array[i].m_is_valid << " "
            << m_array[i].m_use_time << std::endl;
    }
}

Addr
RubyPrefetcher::pageAddress(Addr addr) const
{
    return mbits<Addr>(addr, 63, pageShift);
}

} // namespace ruby
} // namespace gem5
