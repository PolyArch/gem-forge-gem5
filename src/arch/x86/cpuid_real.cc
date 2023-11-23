/*
 * Copyright (c) 2008 The Regents of The University of Michigan
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

#include "arch/x86/cpuid.hh"

#include "arch/x86/isa.hh"
#include "debug/X86CPUId.hh"

namespace gem5 {

namespace X86ISA {

CpuidResult x = {1, 2, 3, 4};

std::map<uint32_t, std::map<uint32_t, CpuidResult>>
    RealCPUId_Intel_Xeon_w7_3465X = {
  { 0x80000000, {{0, {0x80000008, 0, 0, 0}}, }},
  { 0x80000001, {{0, {0, 0, 0x2c100800, 0x121}}, }},
  { 0x80000002, {{0, {0x65746e49, 0x2952286c, 0x2952286e, 0x6f655820}}, }},
  { 0x80000003, {{0, {0x2d377720, 0x35363433, 0, 0x58}}, }},
  { 0x80000004, {{0, {0, 0, 0, 0}}, }},
  { 0x80000005, {{0, {0, 0, 0, 0}}, }},
  { 0x80000006, {{0, {0, 0, 0, 0x8007040}}, }},
  { 0x80000007, {{0, {0, 0, 0x100, 0}}, }},
  { 0x80000008, {{0, {0x3934, 0x200, 0, 0}}, }},
  { 0, {{0, {0x20, 0x756e6547, 0x49656e69, 0x6c65746e}}, }},
  { 0x1, {{0, {0x806f8, 0x25800800, 0xbfebfbff, 0x7ffefbff}}, }},
  { 0x2, {{0, {0xfeff01, 0xf0, 0, 0}}, }},
  { 0x3, {{0, {0, 0, 0, 0}}, }},
  { 0x4, {{0, {0xfc004121, 0x2c0003f, 0, 0x3f}}, {0x1, {0xfc004122, 0x1c0003f, 0, 0x3f}},
          {0x2, {0xfc004143, 0x3c0003f, 0, 0x7ff}}, {0x3, {0xfc1fc163, 0x380003f, 0x4, 0x13fff}}, }},
  { 0x5, {{0, {0x40, 0x40, 0x1020, 0x3}}, }},
  { 0x6, {{0, {0x45cef7, 0x2, 0, 0x9}}, }},
  { 0x7, {{0, {0x2, 0xf3bfb7eb, 0xffdd4430, 0xbb417ffe}}, }},
  { 0x8, {{0, {0, 0, 0, 0}}, }},
  { 0x9, {{0, {0, 0, 0, 0}}, }},
  { 0xa, {{0, {0x8300805, 0, 0x8604, 0xf}}, }},
  { 0xb, {{0, {0x1, 0x2, 0x2b, 0x100}}, {0x1, {0x7, 0x38, 0x2b, 0x201}}, {0x2, {0, 0, 0x2b, 0x2}}, }},
  { 0xc, {{0, {0, 0, 0, 0}}, }},
  { 0xd, {{0, {0x602e7, 0x2b00, 0, 0x2b00}}, {0x1, {0x1f, 0x2d00, 0, 0xdd00}},
          {0x2, {0x100, 0x240, 0, 0}}, {0x3, {0, 0, 0, 0}},
          {0x4, {0, 0, 0, 0}}, {0x5, {0x40, 0x440, 0, 0}},
          {0x6, {0x200, 0x480, 0, 0}}, {0x7, {0x400, 0x680, 0, 0}}, }},
  { 0xe, {{0, {0, 0, 0, 0}}, }},
  { 0xf, {{0, {0, 0x13f, 0x2, 0}}, }},
  { 0x10, {{0, {0, 0xe, 0, 0}}, }},
  { 0x11, {{0, {0, 0, 0, 0}}, }},
  { 0x12, {{0, {0, 0, 0, 0}}, }},
  { 0x13, {{0, {0, 0, 0, 0}}, }},
  { 0x14, {{0, {0x1, 0x5f, 0, 0x7}}, }},
  { 0x15, {{0, {0x2, 0xd0, 0, 0x16e3600}}, }},
  { 0x16, {{0, {0x9c4, 0x12c0, 0, 0x64}}, }},
  { 0x17, {{0, {0, 0, 0, 0}}, }},
  { 0x18, {{0, {0x8, 0, 0, 0}}, }},
  { 0x19, {{0, {0, 0, 0, 0}}, }},
  { 0x1a, {{0, {0, 0, 0, 0}}, }},
  { 0x1b, {{0, {0x1, 0x1, 0, 0}}, }},
  { 0x1c, {{0, {0x4000000b, 0x7, 0, 0x7}}, }},
  { 0x1d, {{0, {0x1, 0, 0, 0}}, }},
  { 0x1e, {{0, {0, 0x4010, 0, 0}}, }},
  { 0x1f, {{0, {0x1, 0x2, 0x25, 0x100}}, }},
  { 0x20, {{0, {0, 0, 0, 0}}, }},
};

bool doCpuidWithRealCPU(const std::string &realCPUId,
  ThreadContext *tc, uint32_t function, uint32_t index,
  CpuidResult &result) {

  // In certain case we need to ignore index.
  switch (function) {
    case 0x4:
    case 0x7:
    case 0xB:
    case 0xD:
    case 0xF:
    case 0x10:
    case 0x17:
    case 0x18:
    case 0x1F:
    case 0x20:
      break;
    default: {
      warn_if(index != 0, "CPUId ignore ECX %#x.\n", index);
      index = 0;
      break;
    }
  }

  // Clear the result.
  result = CpuidResult(0, 0, 0, 0);

  if (realCPUId == "Intel_Xeon_w7_3465X") {

    // Just look up a table.
    // Here we have Intel(R) Xeon(R) w7-3465X
    auto funcIter = RealCPUId_Intel_Xeon_w7_3465X.find(function);
    if (funcIter != RealCPUId_Intel_Xeon_w7_3465X.end()) {
      const auto &funcMap = funcIter->second;
      auto indexIter = funcMap.find(index);
      if (indexIter != funcMap.end()) {
        result = indexIter->second;
        DPRINTF(X86CPUId,
          "RealCPUId %s EAX %#x ECX %#x -> EAX %#x EBX %#x EDX %#x ECX %#x\n",
          realCPUId, function, index,
          result.rax, result.rbx, result.rdx, result.rcx
          );
        return true;
      }
    }
    warn("x86 cpuid real: unsupported %#x %#x", function, index);
    return false;
  }

  warn("x86 cpuid: unknown real cpu %s", realCPUId);
  return false;
}

} // namespace X86ISA
} // namespace gem5
