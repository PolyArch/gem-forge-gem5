from __future__ import print_function
from __future__ import absolute_import

from m5.params import *
from m5.objects import *

from common import FileSystemConfig

from topologies.BaseTopology import SimpleTopology

import math
import sys

class Intel_Chiplet(SimpleTopology):
    description = ''

    def __init__(self, controllers):
        self.nodes = controllers

    def makeTopology(self, options, network, IntLink, ExtLink, Router):
        nodes = self.nodes
        num_cpus = options.num_cpus        
        
        self.link_latency = options.link_latency
        router_latency = options.router_latency
        print(f'Base Link Latency: {self.link_latency}')
        # not sure if should be adding or what, if it can be 0 or not, or how to handle when inside the bridge
        self.bridge_latency = options.chiplet_latency_increase + self.link_latency
        print(f'Bridge link latency: {self.bridge_latency}')

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
                raise Exception('Unkown node controller {t}'.format(t=node.type))
        print(f'Number of Each Level Cache Controller: {len(cache_nodes)//3}')
        print(f'Number of DMA Controllers: {len(dma_nodes)}')
        print(f'Number of Directory Controllers: {len(dir_nodes)}')        

        num_dir_nodes = len(dir_nodes)
        assert(num_dir_nodes == 4)
        num_cpu_chiplets = 4
        num_cpus_per_chiplet = int(num_cpus / num_cpu_chiplets)

        print(f'Number of CPUs per Chiplet: {num_cpus_per_chiplet}')
        
        self.num_chiplet_rows = math.isqrt(num_cpus_per_chiplet)
        assert(num_cpus_per_chiplet / self.num_chiplet_rows == self.num_chiplet_rows)
        self.num_chiplet_cols = self.num_chiplet_rows
        print(f'there are {num_cpu_chiplets} chiplets that are each {self.num_chiplet_rows} x {self.num_chiplet_cols}')

        print(f'Total Number of CPUs in Chiplets: {self.num_chiplet_rows * self.num_chiplet_cols * num_cpu_chiplets}')
        assert ((self.num_chiplet_rows * self.num_chiplet_cols * num_cpu_chiplets) == num_cpus)

        # the number of caches must be a multiple of the number of cpus
        caches_per_cpu_router, remainder = divmod(len(cache_nodes), num_cpus)
        assert (remainder == 0)

        self.num_routers = num_cpus
        print(f'Total Number of Routers for chiplets: {self.num_routers}')
        bridge_routers = self.num_chiplet_rows * num_cpu_chiplets
        self.num_routers += bridge_routers
        
        # Create the routers in the mesh
        routers = [Router(router_id=i, latency=router_latency) \
                   for i in range(self.num_routers)]
        
        # Set the enable trace flag.
        for router in routers:
            router.enable_trace = options.gem_forge_enable_llc_stream_engine_trace
        network.routers = routers

        # link counter to set unique link ids
        self.link_count = 0

        # Connect each cache controller to the appropriate router
        ext_links = []
        for (i, n) in enumerate(cache_nodes):
            cntrl_level, router_id = divmod(i, num_cpus)
            assert (cntrl_level < caches_per_cpu_router)
            n.router_id = router_id
            ext_links.append(ExtLink(link_id=self.link_count, ext_node=n,
                                     int_node=routers[router_id],
                                     latency=self.link_latency))
            print(f'[Topology] Connect {n.type} {n.version} to Router {router_id} with Link Latency {self.link_latency}')
            self.link_count += 1

        # Connect the dma nodes to router 0.  These should only be DMA nodes.
        # Feature unused
        for (i, node) in enumerate(dma_nodes):
            assert (node.type == 'DMA_Controller')
            node.router_id = 0
            ext_links.append(ExtLink(link_id=self.link_count, ext_node=node,
                                     int_node=routers[0],
                                     latency=self.link_latency))
            self.link_count += 1

        # Directories in corners
        # for the chiplet routers
        dir_routers = None
        if num_cpus == 64:
            dir_routers = [0, 19, 44, 63]
        elif num_cpus == 16:
            dir_routers = [0, 5, 10, 15]
        else:
            raise Exception('not implemented number of CPUs')
        
        for (i, node) in enumerate(dir_nodes):
            r_id = dir_routers[i]            
            print(f'Directory Controller {i} -> Router {r_id} with Link Latency {self.link_latency}')
            dir_nodes[i].router_id = r_id
            ext_links.append(ExtLink(link_id=self.link_count, ext_node=dir_nodes[i],
                                     int_node=routers[r_id], latency=self.link_latency))
            self.link_count += 1

        # leave empty for now, not useful once we do 1:(num_chiplets_llc) address mapping            
        for dir_idx in range(len(dir_nodes)):
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

        # Create the mesh links.
        int_links = []
            
        test_num_cpus = 0
        print('Num CPU Chiplets', num_cpu_chiplets)
        for chiplet in range(num_cpu_chiplets):
            print('Topology for CPU Chiplet ' + str(chiplet) + ': ')

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
                        print(f'[CPU Chiplet] Router East {east_out} -> Router West {west_in} with Link Latency{self.link_latency}')
                        self.link_count += 1

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
                        print(f'[CPU Chiplet] Router West {west_out} -> Router East {east_in} with Link Latency{self.link_latency}')
                        self.link_count += 1

            for col in range(self.num_chiplet_cols):
                for row in range(self.num_chiplet_rows):
                    if row + 1 < self.num_chiplet_rows:
                        south_out = (chiplet * num_cpus_per_chiplet) + col + (row * self.num_chiplet_cols)
                        north_in = (chiplet * num_cpus_per_chiplet) + col + ((row + 1) * self.num_chiplet_cols)
                        int_links.append(IntLink(link_id=self.link_count,
                                                 src_node=routers[south_out],
                                                 dst_node=routers[north_in],
                                                 src_outport="South",
                                                 dst_inport="North",
                                                 latency=self.link_latency,
                                                 weight=weightY))
                        print(f'[CPU Chiplet] Router South {south_out} -> Router North {north_in} with Link Latency{self.link_latency}')
                        self.link_count += 1

            for col in range(self.num_chiplet_cols):
                for row in range(self.num_chiplet_rows):
                    if row + 1 < self.num_chiplet_rows:
                        south_in = (chiplet * num_cpus_per_chiplet) + col + (row * self.num_chiplet_cols)
                        north_out = (chiplet * num_cpus_per_chiplet) + col + ((row + 1) * self.num_chiplet_cols)
                        int_links.append(IntLink(link_id=self.link_count,
                                                 src_node=routers[north_out],
                                                 dst_node=routers[south_in],
                                                 src_outport="North",
                                                 dst_inport="South",
                                                 latency=self.link_latency,
                                                 weight=weightY))
                        print(f'[CPU Chiplet] Router North {north_out} -> Router South {south_in} with Link Latency{self.link_latency}')
                        self.link_count += 1

        assert (test_num_cpus == num_cpus)

        # chiplets need to be connected through the bridge routers
        # set the bridge routers to have their appropriate vcs
        for r in routers[num_cpus:]:
            r.vcs_per_vnet = options.chiplet_vcs_per_vnet   

        # now we must connect the chiplets to their proper bridge routers with the appropriate latencies
        if num_cpus == 64:
            # format: bridge_router: (chiplet_router, direction of src outport from bridge to chiplet, dest inport at chiplet from bridge to chiplet)
            mp = {
                64: [(15, "West", "East"), (28, "East", "West")],
                65: [(11, "West", "East"), (24, "East", "West")],
                66: [(7, "West", "East"), (20, "East", "West")],
                67: [(3, "West", "East"), (16, "East", "West")],
                68: [(28, "North", "South"), (48, "South", "North")],
                69: [(29, "North", "South"), (49, "South", "North")],
                70: [(30, "North", "South"), (50, "South", "North")],
                71: [(31, "North", "South"), (51, "South", "North")],
                72: [(48, "East", "West"), (35, "West", "East")],
                73: [(52, "East", "West"), (39, "West", "East")],
                74: [(56, "East", "West"), (43, "West", "East")],
                75: [(60, "East", "West"), (47, "West", "East")],
                76: [(15, "North", "South"), (35, "South", "North")],
                77: [(14, "North", "South"), (34, "South", "North")],
                78: [(13, "North", "South"), (33, "South", "North")],
                79: [(12, "North", "South"), (32, "South", "North")]
                }
        elif num_cpus == 16:
            mp = {
                16: [(3, "West", "East"), (6, "East", "West")],
                17: [(1, "West", "East"), (4, "East", "West")],
                18: [(6, "North", "South"), (12, "South", "North")],
                19: [(7, "North", "South"), (13, "South", "North")],
                20: [(12, "East", "West"), (9, "West", "East")],
                21: [(14, "East", "West"), (11, "West", "East")],
                22: [(3, "North", "South"), (9, "South", "North")],
                23: [(2, "North", "South"), (8, "South", "North")]
                }
        else:
            raise Exception(f'not implemented for num_cpus {num_cpus}')

        assert(len(mp) == self.num_chiplet_rows * num_cpu_chiplets)
        
        for i in mp:
            for j in mp[i]:
                if j[1] == "East" or j[1] == "West":
                    weight = weightX
                else:
                    weight = weightY
                int_links.append(IntLink(link_id=self.link_count,
                                         src_node=routers[i],
                                         dst_node=routers[j[0]],
                                         src_outport=j[1],
                                         dst_inport=j[2],
                                         latency=self.link_latency,
                                         weight=weight))
                self.link_count += 1
                print(f'[Cross] Bridge Router {j[1]} {i} -> Router {j[2]} {j[0]} with latency {self.link_latency}')
    
                int_links.append(IntLink(link_id=self.link_count,
                                         src_node=routers[j[0]],
                                         dst_node=routers[i],
                                         src_outport=j[2],
                                         dst_inport=j[1],
                                         latency=self.bridge_latency,
                                         weight=weight))
                self.link_count += 1
                print(f'[Cross] Router {j[2]} {j[0]} -> Bridge Router {j[1]} {i} with latency {self.bridge_latency}')
        
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
