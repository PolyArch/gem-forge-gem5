#include "stdint.h"

namespace {
/**
 * Read this to prevent it being optimized away.
 */
volatile int dummy;
} // namespace

extern "C" {
__attribute__((noinline)) extern void m5_reset_stats(uint64_t ns_delay,
                                                     uint64_t ns_period) {
  volatile int x = dummy;
  (void)x;
}
__attribute__((noinline)) extern void m5_dump_stats(uint64_t ns_delay,
                                                    uint64_t ns_period) {
  volatile int x = dummy;
  (void)x;
}
__attribute__((noinline)) extern void m5_fail(uint64_t ns_delay,
                                              uint64_t code) {
  volatile int x = dummy;
  (void)x;
}
__attribute__((noinline)) extern void m5_panic() {
  volatile int x = dummy;
  (void)x;
}
__attribute__((noinline)) extern void m5_work_begin(uint64_t workid,
                                                    uint64_t threadid) {
  volatile int x = dummy;
  (void)x;
}
__attribute__((noinline)) extern void m5_work_end(uint64_t workid,
                                                  uint64_t threadid) {
  volatile int x = dummy;
  (void)x;
}
__attribute__((noinline)) extern void m5_switch_cpu() {
  volatile int x = dummy;
  (void)x;
}
__attribute__((noinline)) extern void
m5_stream_nuca_region(const char *regionName, const void *buffer,
                      uint64_t elementSize, uint64_t dim1, uint64_t dim2,
                      uint64_t dim3) {
  volatile int x = dummy;
  (void)x;
}
__attribute__((noinline)) extern void
m5_stream_nuca_align(const void *A, const void *B, int64_t elementOffset) {
  volatile int x = dummy;
  (void)x;
}
__attribute__((noinline)) extern void m5_stream_nuca_remap() {
  volatile int x = dummy;
  (void)x;
}
__attribute__((noinline)) extern uint64_t
m5_stream_nuca_get_cached_bytes(void *buffer) {
  return 0;
}
}