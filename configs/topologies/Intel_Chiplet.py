from __future__ import print_function
from __future__ import absolute_import

from m5.params import *
from m5.objects import *

from common import FileSystemConfig

from topologies.BaseTopology import SimpleTopology

import math
import sys

class Intel_Chiplet(SimpleTopology):
    description = 'A Chiplet config with a off-chip I/O'

    def __init__(self, controllers):
        self.nodes = controllers

    def makeTopology(self, options, network, IntLink, ExtLink, Router):
        nodes = self.nodes
        
        self.link_latency = options.link_latency
        router_latency = options.router_latency 
        print(f'Base Link Latency: {self.link_latency}')
        self.chiplet_link_latency = options.link_latency + options.chiplet_latency_increase
        print(f'Chiplet Link Latency: {self.chiplet_link_latency}')
        cache_nodes = []
        dir_nodes = []
        dma_nodes = []
        for node in nodes:
            if node.type == 'L1Cache_Controller' or \
                    node.type == 'L2Cache_Controller' or \
                    node.type == 'L0Cache_Controller':
                cache_nodes.append(node)
            elif node.type == 'Directory_Controller':
                dir_nodes.append(node)
            elif node.type == 'DMA_Controller':
                dma_nodes.append(node)
            else:
                print('Unkown node controller {t}'.format(t=node.type))
                assert (False)
        print(f'Number of Each Level Cache Controller: {len(cache_nodes)//3}')
        print(f'Number of DMA Controllers: {len(dma_nodes)}')
        print(f'Number of Directory Controllers: {len(dir_nodes)}')

        num_dir_nodes = len(dir_nodes)
        assert(num_dir_nodes == 4)
        num_cpu_chiplets = 4
        # we divy up our CPUs accordingly
        num_cpus_per_chiplet = int(options.num_cpus / num_cpu_chiplets)

        print(f'Number of CPUs per Chiplet: {num_cpus_per_chiplet}')
        
        # and we set our number of rows and number of columns for our chiplets like this
        self.num_chiplet_rows = int(math.sqrt(num_cpus_per_chiplet))  # 4 for 16 cpus
        self.num_chiplet_cols = int(num_cpus_per_chiplet / self.num_chiplet_rows)  # 4 for 16 cpus
        print(f'there are {num_cpu_chiplets} chiplets that are each {self.num_chiplet_rows} x {self.num_chiplet_cols}')

        print(f'Total Number of CPUs in Chiplets: {self.num_chiplet_rows * self.num_chiplet_cols * num_cpu_chiplets}')
        assert ((self.num_chiplet_rows * self.num_chiplet_cols * num_cpu_chiplets) == options.num_cpus)  # all is well

        # at this time, it can only be a square
        assert math.sqrt(num_dir_nodes) % 1 == 0

        # the number of caches must be a multiple of the number of cpus
        caches_per_cpu_router, remainder = divmod(len(cache_nodes), options.num_cpus)
        assert (remainder == 0)

        self.num_routers = options.num_cpus
        print(f'Total Number of Routers for all Chiplets: {self.num_routers}')
        
        # Create the routers in the mesh
        routers = [Router(router_id=i, latency=router_latency) \
                   for i in range(self.num_routers)]
        # Set the enable trace flag.
        for router in routers:
            router.enable_trace = options.gem_forge_enable_llc_stream_engine_trace
        network.routers = routers

        num_cpus = options.num_cpus
        assert (num_cpus == self.num_routers)

        # link counter to set unique link ids
        self.link_count = 0

        # Connect each cache controller to the appropriate router
        ext_links = []
        for (i, n) in enumerate(cache_nodes):
            cntrl_level, router_id = divmod(i, self.num_routers)
            assert (cntrl_level < caches_per_cpu_router)
            n.router_id = router_id
            ext_links.append(ExtLink(link_id=self.link_count, ext_node=n,
                                     int_node=routers[router_id],
                                     latency=self.link_latency))
            print(f'[Topology] Connect {n.type} {n.version} to Rounter {router_id} with Link Latency {self.link_latency}')
            self.link_count += 1

        # Connect the dma nodes to router 0.  These should only be DMA nodes.
        for (i, node) in enumerate(dma_nodes):
            assert (node.type == 'DMA_Controller')
            node.router_id = 0
            ext_links.append(ExtLink(link_id=self.link_count, ext_node=node,
                                     int_node=routers[0],
                                     latency=self.link_latency))
            # don't care where DMA nodes go, will not be using this feature

        # dirs in corners
        dir_routers = None
        if num_cpus == 64:
            dir_routers = [0, 19, 44, 63]
        elif num_cpus == 16:
            dir_routers = [0, 5, 10, 15]
        else:
            print('not implemented or invalid')
            assert(False)
        
        for (i, node) in enumerate(dir_nodes):
            r_id = dir_routers[i]            
            print(f'Directory Controller {i} -> Router {r_id} with Link Latency {self.link_latency}')
            dir_nodes[i].router_id = r_id
            ext_links.append(ExtLink(link_id=self.link_count, ext_node=dir_nodes[i],
                                     int_node=routers[r_id], latency=self.link_latency))
            self.link_count += 1

        # Create the mesh links.
        int_links = []

        # io_chiplet_router_start = self.num_routers - num_dir_nodes # start point for i/o chiplet

        # for row in range(self.num_io_chiplet_rows):
        #     for col in range(self.num_io_chiplet_cols):
        #         if col + 1 < self.num_io_chiplet_cols:  # if next col is within range
        #             east_out = io_chiplet_router_start + col + (row * self.num_io_chiplet_cols)
        #             west_in = io_chiplet_router_start + (col + 1) + (row * self.num_io_chiplet_cols)
        #             int_links.append(IntLink(link_id=self.link_count, # connect to next router
        #                                      src_node=routers[east_out],
        #                                      dst_node=routers[west_in],
        #                                      src_outport="East",
        #                                      dst_inport="West",
        #                                      latency=self.link_latency,
        #                                      weight=1))
        #             print(f'[I/O Chiplet] Router {east_out} -> Router {west_in} with Link Latency {self.link_latency}')
        #             self.link_count += 1

        # # West output to East input links (weight = 1)

        # for row in range(self.num_io_chiplet_rows):
        #     for col in range(self.num_io_chiplet_cols):
        #         if col + 1 < self.num_io_chiplet_cols:
        #             east_in = io_chiplet_router_start + col + (row * self.num_io_chiplet_cols)
        #             west_out = io_chiplet_router_start + (col + 1) + (row * self.num_io_chiplet_cols)
        #             int_links.append(IntLink(link_id=self.link_count,
        #                                      src_node=routers[west_out],
        #                                      dst_node=routers[east_in],
        #                                      src_outport="West",
        #                                      dst_inport="East",
        #                                      latency=self.link_latency,
        #                                      weight=1))
        #             print(f'[I/O Chiplet] Router {west_out} -> Router {east_in} with Link Latency {self.link_latency}')
        #             self.link_count += 1

        # # North output to South input links (weight = 1)

        # for col in range(self.num_io_chiplet_cols):
        #     for row in range(self.num_io_chiplet_rows):
        #         if row + 1 < self.num_io_chiplet_rows:
        #             north_out = io_chiplet_router_start + col +  (row * self.num_io_chiplet_cols)
        #             south_in = io_chiplet_router_start + col + ((row + 1) * self.num_io_chiplet_cols)
        #             int_links.append(IntLink(link_id=self.link_count,
        #                                      src_node=routers[north_out],
        #                                      dst_node=routers[south_in],
        #                                      src_outport="North",
        #                                      dst_inport="South",
        #                                      latency=self.link_latency,
        #                                      weight=1))
        #             print(f'[I/O Chiplet] Router {north_out} -> Router {south_in} with Link Latency {self.link_latency}')
        #             self.link_count += 1

        # # South output to North input links (weight = 1)

        # for col in range(self.num_io_chiplet_cols):
        #     for row in range(self.num_io_chiplet_rows):
        #         if row + 1 < self.num_io_chiplet_rows:
        #             north_in = io_chiplet_router_start + col + (row * self.num_io_chiplet_cols)
        #             south_out = io_chiplet_router_start + col + ((row + 1) * self.num_io_chiplet_cols)
        #             int_links.append(IntLink(link_id=self.link_count,
        #                                      src_node=routers[south_out],
        #                                      dst_node=routers[north_in],
        #                                      src_outport="South",
        #                                      dst_inport="North",
        #                                      latency=self.link_latency,
        #                                      weight=1))
        #             print(f'[I/O Chiplet] Router {south_out} -> Router {north_in} with Link Latency {self.link_latency}')
        #             self.link_count += 1

        # # print config

        # print('Configuration:\n Number of CPU Chiplets ' + str(num_cpu_chiplets) +
        #       '\nCPU Chiplet config: ' + str(self.num_chiplet_rows) + ' x ' + str(self.num_chiplet_cols) +
        #       '\nI/O Chiplet Config: ' + str(self.num_io_chiplet_rows) + ' x ' + str(self.num_io_chiplet_cols)
        #       + '\n Mesh')

        for dir_idx in range(len(dir_nodes)):
            # leave empty for now, not useful once we do 1:(num_chiplets_llc) address mapping
            dir_nodes[dir_idx].numa_banks = []

        # Smaller weight means higher priority 
        weightX = 1
        weightY = 2
        if options.routing_YX:
            print('YX Routing Selected')
            weightX = 2
            weightY = 1
        else:
            print('XY Routing')

        test_num_cpus = 0
        print('Num CPU Chiplets', num_cpu_chiplets)
        for chiplet in range(num_cpu_chiplets):
            print('Topology for CPU Chiplet ' + str(chiplet) + ': ')

            # East output to West input links (weight = 1)
            # s.t east_out = self router id and west_in = next router id, pattern continue

            for row in range(self.num_chiplet_rows):
                for col in range(self.num_chiplet_cols):
                    test_num_cpus += 1
                    if col + 1 < self.num_chiplet_cols:  # if next col is within range
                        east_out = (chiplet * num_cpus_per_chiplet) + col + (row * self.num_chiplet_cols)
                        west_in = (chiplet * num_cpus_per_chiplet) + (col + 1) + (row * self.num_chiplet_cols)
                        int_links.append(IntLink(link_id=self.link_count,
                                                 src_node=routers[east_out],
                                                 dst_node=routers[west_in],
                                                 src_outport="East",
                                                 dst_inport="West",
                                                 latency=self.link_latency,
                                                 weight=weightX))
                        print(f'[CPU Chiplet] Router {east_out} -> Router {west_in} with Link Latency{self.link_latency}')
                        self.link_count += 1

            # West output to East input links (weight = 1)

            for row in range(self.num_chiplet_rows):
                for col in range(self.num_chiplet_cols):
                    if col + 1 < self.num_chiplet_cols:
                        east_in = (chiplet * num_cpus_per_chiplet) + col + (row * self.num_chiplet_cols)
                        west_out = (chiplet * num_cpus_per_chiplet) + (col + 1) + (row * self.num_chiplet_cols)
                        int_links.append(IntLink(link_id=self.link_count,
                                                 src_node=routers[west_out],
                                                 dst_node=routers[east_in],
                                                 src_outport="West",
                                                 dst_inport="East",
                                                 latency=self.link_latency,
                                                 weight=weightX))
                        print(f'[CPU Chiplet] Router {west_out} -> Router {east_in} with Link Latency{self.link_latency}')
                        self.link_count += 1

            # North output to South input links (weight = 2)

            for col in range(self.num_chiplet_cols):
                for row in range(self.num_chiplet_rows):
                    if row + 1 < self.num_chiplet_rows:
                        north_out = (chiplet * num_cpus_per_chiplet) + col + (row * self.num_chiplet_cols)
                        south_in = (chiplet * num_cpus_per_chiplet) + col + ((row + 1) * self.num_chiplet_cols)
                        int_links.append(IntLink(link_id=self.link_count,
                                                 src_node=routers[north_out],
                                                 dst_node=routers[south_in],
                                                 src_outport="North",
                                                 dst_inport="South",
                                                 latency=self.link_latency,
                                                 weight=weightY))
                        print(f'[CPU Chiplet] Router {north_out} -> Router {south_in} with Link Latency{self.link_latency}')
                        self.link_count += 1

            # South output to North input links (weight = 2)

            for col in range(self.num_chiplet_cols):
                for row in range(self.num_chiplet_rows):
                    if row + 1 < self.num_chiplet_rows:
                        north_in = (chiplet * num_cpus_per_chiplet) + col + (row * self.num_chiplet_cols)
                        south_out = (chiplet * num_cpus_per_chiplet) + col + ((row + 1) * self.num_chiplet_cols)
                        int_links.append(IntLink(link_id=self.link_count,
                                                 src_node=routers[south_out],
                                                 dst_node=routers[north_in],
                                                 src_outport="South",
                                                 dst_inport="North",
                                                 latency=self.link_latency,
                                                 weight=weightY))
                        print(f'[CPU Chiplet] Router {south_out} -> Router {north_in} with Link Latency{self.link_latency}')
                        self.link_count += 1

        assert (test_num_cpus == num_cpus)

        # connect chiplets to each other
        # we want a topology with top-left, top-right, bot-left, bot-right chiplets 
        # perfectly, we make sure to pick out those nodes who have available ports on the respective sides
        # to make an exact 4 quadrant chiplet
        
        vert = None
        hor = None
        if num_cpus == 64:
            ver = [(3, 16), (7, 20), (11, 24), (15, 28), (35, 48), (39, 52), (43, 56), (47, 60)]
            hor = [(12, 32), (13, 33), (14, 34), (15, 35), (28, 48), (29, 49), (30, 50), (31, 51)]
        elif num_cpus == 16:
            ver = [(1,4), (3,6), (9,12), (11, 14)]
            hor = [(2,8), (3, 9), (6, 12), (7, 13)]
        else:
            print('not implemented or invalid')
            assert(False)

        chip_len = math.isqrt(num_cpus)
        assert(len(ver) == len(hor) == chip_len)
        
        for i in range(chip_len):
            east = ver[i][0]
            west = ver[i][1]
            int_links.append(IntLink(link_id=self.link_count,
                                     src_node=routers[east],
                                     dst_node=routers[west],
                                     src_outport="East",
                                     dst_inport="West",
                                     latency=self.chiplet_link_latency,
                                     weight=weightX))
            print(f'[Chiplet Connect] Router {east} -> Router {west} with Link Latency {self.chiplet_link_latency}')
            self.link_count += 1

            int_links.append(IntLink(link_id=self.link_count,
                                     src_node=routers[west],
                                     dst_node=routers[east],
                                     src_outport="West",
                                     dst_inport="East",
                                     latency=self.chiplet_link_latency,
                                     weight=weightX))
            print(f'[Chiplet Connect] Router {west} -> Router {east} with Link Latency{self.chiplet_link_latency}')
            self.link_count += 1
            
            north = hor[i][0]
            south = hor[i][1]
 
            int_links.append(IntLink(link_id=self.link_count,
                                     src_node=routers[north],
                                     dst_node=routers[south],
                                     src_outport="North",
                                     dst_inport="South",
                                     latency=self.chiplet_link_latency,
                                     weight=weightY))
            print(f'[Chiplet Connect] Router {north} -> Router {south} with Link Latency{self.chiplet_link_latency}')
            self.link_count += 1
            
            int_links.append(IntLink(link_id=self.link_count,
                                     src_node=routers[south],
                                     dst_node=routers[north],
                                     src_outport="South",
                                     dst_inport="North",
                                     latency=self.chiplet_link_latency,
                                     weight=weightY))
            print(f'[Chiplet Connect] Router {south} -> Router {north} with Link Latency{self.chiplet_link_latency}')
            self.link_count += 1
            
        
        network.ext_links = ext_links            
        network.int_links = int_links            

        sys.stdout.flush()


# Register nodes with filesystem
def registerTopology(self, options):
    if options.no_file_system:
        return
    i = 0
    for n in self.numa_nodes:
        if n:
            FileSystemConfig.register_node(n,
                                           MemorySize(options.mem_size) // self.num_numa_nodes, i)
        i += 1
