"""
Simplified gem5 configuration for GPGPU-Sim integration.

This script is called from GPGPU-Sim via Gem5Wrapper to set up a gem5 system
that can be driven by GPGPU-Sim's GPU simulation.

The key differences from run.py:
1. No command-line argument parsing (args passed programmatically)
2. Minimal CPU configuration (CPUs will be driven by GPGPU-Sim)
3. Focus on memory system and cache hierarchy
4. Called via C++ Gem5Wrapper instead of gem5 binary

Note: m5.options is set by the C++ wrapper before this script is loaded.
"""

import m5
from m5.objects import *
import sys

def create_system(args):
    """
    Create a minimal gem5 system for GPGPU-Sim integration.
    
    Args:
        args: Configuration parameters (can be a dict or argparse Namespace)
    """
    
    # Create the system
    system = System()
    
    # Set memory mode and ranges
    system.mem_mode = 'timing'
    system.mem_ranges = [AddrRange('8GB')]
    system.cache_line_size = 64
    
    # Create voltage and clock domains
    system.voltage_domain = VoltageDomain(voltage='1V')
    system.clk_domain = SrcClockDomain(clock='1GHz',
                                       voltage_domain=system.voltage_domain)
    
    # Create a simple memory controller
    system.mem_ctrl = MemCtrl()
    system.mem_ctrl.dram = DDR3_1600_8x8()
    system.mem_ctrl.dram.range = system.mem_ranges[0]
    
    # Create memory bus
    system.membus = SystemXBar()
    system.system_port = system.membus.cpu_side_ports
    
    # Connect memory controller to membus
    system.mem_ctrl.port = system.membus.mem_side_ports
    
    return system

def initialize_system(config_dict=None):
    """
    Main entry point called from Gem5Wrapper.
    
    Args:
        config_dict: Optional dictionary of configuration parameters
    """
    
    if config_dict is None:
        config_dict = {}
    
    # Create the system
    system = create_system(config_dict)
    
    # Create root object
    root = Root(full_system=False, system=system)
    
    # Instantiate the system
    m5.instantiate()
    
    print("gem5 system instantiated for GPGPU-Sim integration")
    
    # Note: startup() will be called automatically on the first m5.simulate() call
    # Don't call it manually here
    
    return root

# If called directly (for testing)
if __name__ == '__m5_main__':
    root = initialize_system()
    print("System ready for simulation")
