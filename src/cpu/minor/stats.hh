/*
 * Copyright (c) 2011-2014 ARM Limited
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
 *
 * Authors: Andrew Bardsley
 */

/**
 * @file
 *
 * The stats for MinorCPU separated from the CPU definition.
 */

#ifndef __CPU_MINOR_STATS_HH__
#define __CPU_MINOR_STATS_HH__

#include "base/statistics.hh"
#include "cpu/base.hh"
#include "sim/ticked_object.hh"

namespace Minor
{

/** Currently unused stats class. */
class MinorStats
{
  public:
    /** Number of simulated instructions */
    Stats::Scalar numInsts;

    /** Number of simulated insts and microops */
    Stats::Scalar numOps;

    /** Number of ops discarded before committing */
    Stats::Scalar numDiscardedOps;

    /** Number of times fetch was asked to suspend by Execute */
    Stats::Scalar numFetchSuspends;

    /** Number of cycles in quiescent state */
    Stats::Scalar quiesceCycles;

    /**
     * ! GemForge
     * Number of cycles issue blocked by a load.
     */
    Stats::Scalar loadBlockedIssueCycles;
    Stats::Scalar loadBlockedIssueInsts;
    Stats::Formula loadBlockedIssueCPI;
    Stats::Formula loadBlockedIssueCyclesPercentage;

    /** CPI/IPC for total cycle counts and macro insts */
    Stats::Formula cpi;
    Stats::Formula ipc;

    /** Number of instructions by type (OpClass) */
    Stats::Vector2d committedInstType;

  public:
    MinorStats();

  public:
    void regStats(const std::string &name, BaseCPU &baseCpu);
};

}

#endif /* __CPU_MINOR_STATS_HH__ */
