
from m5.objects import *

def initializePrefetch(args, system):
    # Only works in classical memory system.
    if args.ruby:
        return
    if args.llvm_prefetch == 0:
        return
    for cpu in system.cpu:
        cpu.dcache.prefetch_on_access = args.gem_forge_prefetch_on_access
        if args.gem_forge_prefetcher == 'imp':
            cpu.dcache.prefetcher = IndirectMemoryPrefetcher(
                use_virtual_addresses=True,
                index_queue_size=16,
                on_inst=False,
                max_prefetch_distance=args.gem_forge_prefetch_dist,
                streaming_distance=args.gem_forge_prefetch_dist,
            )
            if not cpu.dcache.prefetch_on_access:
                raise ValueError('IMP must be used with PrefetchOnAccess.')
            if not hasattr(cpu, 'dtb'):
                raise ValueError('IMP requires TLB to work with virtual address.')
            cpu.dcache.prefetcher.registerTLB(cpu.dtb)
        else:
            cpu.dcache.prefetcher = StridePrefetcher(degree=8, latency=1)
        if args.l1_5dcache:
            cpu.l1_5dcache.prefetch_on_access = args.gem_forge_prefetch_on_access
            if args.gem_forge_prefetcher == 'isb':
                cpu.l1_5dcache.prefetcher = IrregularStreamBufferPrefetcher(
                    degree=8,
                    # address_map_cache_assoc=8,
                    address_map_cache_entries="65536",
                    # training_unit_assoc=8,
                    training_unit_entries="65536",
                    on_inst=False,
                )
            else:
                cpu.l1_5dcache.prefetcher = StridePrefetcher(degree=8, latency=1)
    system.l2.prefetch_on_access = args.gem_forge_prefetch_on_access
    if args.gem_forge_prefetcher == 'isb':
        # ISB should work at LLC.
        system.l2.prefetcher = IrregularStreamBufferPrefetcher(
            degree=8,
            # address_map_cache_assoc=8,
            address_map_cache_entries="65536",
            # training_unit_assoc=8,
            training_unit_entries="65536",
            on_inst=False,
        )
    else:
        system.l2.prefetcher = StridePrefetcher(degree=8, latency=1)