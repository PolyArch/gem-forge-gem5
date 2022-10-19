/*
 * Copyright (c) 2003-2006 The Regents of The University of Michigan
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

#ifndef __GEM5_M5OP_H__
#define __GEM5_M5OP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void m5_arm(uint64_t address);
void m5_quiesce(void);
void m5_quiesce_ns(uint64_t ns);
void m5_quiesce_cycle(uint64_t cycles);
uint64_t m5_quiesce_time(void);
uint64_t m5_rpns();
void m5_wake_cpu(uint64_t cpuid);

void m5_exit(uint64_t ns_delay);
void m5_fail(uint64_t ns_delay, uint64_t code);
uint64_t m5_init_param(uint64_t key_str1, uint64_t key_str2);
void m5_checkpoint(uint64_t ns_delay, uint64_t ns_period);
void m5_reset_stats(uint64_t ns_delay, uint64_t ns_period);
void m5_dump_stats(uint64_t ns_delay, uint64_t ns_period);
void m5_dump_reset_stats(uint64_t ns_delay, uint64_t ns_period);
uint64_t m5_read_file(void *buffer, uint64_t len, uint64_t offset);
uint64_t m5_write_file(void *buffer, uint64_t len, uint64_t offset,
                       const char *filename);
void m5_debug_break(void);
void m5_switch_cpu(void);
void m5_dist_toggle_sync(void);
void m5_add_symbol(uint64_t addr, const char *symbol);
void m5_load_symbol();
void m5_panic(void);
void m5_work_begin(uint64_t workid, uint64_t threadid);
void m5_work_end(uint64_t workid, uint64_t threadid);
void m5_work_mark(uint64_t workid, uint64_t threadid);

void m5_llvm_trace_map(const char *base, void *vaddr);
void m5_llvm_trace_replay(const char *trace, void *vaddr);

void m5_stream_nuca_region(const char *regionName, void *buffer,
                           uint64_t elementSize, uint64_t dim1, uint64_t dim2,
                           uint64_t dim3);
/**
 * Specify the alignment requirement between two arrays.
 *
 * Negative element offset will specify some indirect alignment.
 *
 * To support arbitrary indirect field alignment, e.g. in weighted graph
 * edge.v is used for indirect access while edge.w is only for compute.
 * Suppose the indirect region has this data structure:
 * IndElement {
 *   int32_t out_v;
 *   int32_t weight;
 *   ...
 * };
 *
 * Then the indirect field offset is 0, with size 4.
 * We use eight bits for each, and the final alignment is:
 * - ((offset << 8) | size).
 */
#define m5_stream_nuca_encode_ind_align(offset, size)                          \
  (-(int64_t)((offset) << 8 | (size)))
void m5_stream_nuca_align(void *A, void *B, int64_t elementOffset);

/**
 * A generic implementation to set some property of the region.
 * Such a long name to avoid polluting C global scope.
 */
enum StreamNUCARegionProperty {
  // Manually overrite the interleaving (in elements).
  STREAM_NUCA_REGION_PROPERTY_INTERLEAVE = 0,
  // Manually set if the region is used as PUM.
  STREAM_NUCA_REGION_PROPERTY_USE_PUM,
  // Manually set if region need initilization (from DRAM) when used as PUM.
  STREAM_NUCA_REGION_PROPERTY_PUM_NO_INIT,
  // Manually set PUM tile size.
  STREAM_NUCA_REGION_PROPERTY_PUM_TILE_SIZE_DIM0,
  // Specify which dimension we are going to reduce over.
  STREAM_NUCA_REGION_PROPERTY_REDUCE_DIM,
  // Specify which dimension we are going to broadcast.
  STREAM_NUCA_REGION_PROPERTY_BROADCAST_DIM,
};
void m5_stream_nuca_set_property(void *buffer,
                                 enum StreamNUCARegionProperty property,
                                 uint64_t value);

void m5_stream_nuca_remap();
uint64_t m5_stream_nuca_get_cached_bytes(void *buffer);

void m5_se_syscall();
void m5_se_page_fault();

#define m5_detail_sim_start() m5_switch_cpu();

#define m5_detail_sim_end() m5_dump_stats(0, 0);

#define m5_gem_forge_region_simpoint()                                         \
  m5_reset_stats(0, 0);                                                        \
  m5_switch_cpu();

#ifdef __cplusplus
}
#endif
#endif // __GEM5_M5OP_H__
