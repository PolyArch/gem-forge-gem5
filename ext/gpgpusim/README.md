# gem5-GPGPU-Sim Integration

This directory contains the integration layer between gem5 and GPGPU-Sim.

## Architecture

The integration follows this pattern:

```
GPGPU-Sim (C++) 
    ↓
Gem5Wrapper (C++)
    ↓
gem5 Python Config (Python)
    ↓
gem5 SimObjects (C++)
```

## Files

- `gem5_wrapper.hh/cc`: C++ wrapper that GPGPU-Sim calls to interact with gem5
- `Makefile`: Builds `libgem5_to_gpgpusim.so` for linking into GPGPU-Sim

## Configuration Scripts

### Option 1: Simplified Config (Recommended for initial integration)
Use `configs/example/gem_forge/gpgpusim_config.py`:
- Minimal system with memory hierarchy
- No CPU simulation (driven by GPGPU-Sim)
- Focus on cache and memory system

### Option 2: Full GemForge Config
Use `configs/example/gem_forge/run.py`:
- Full CPU + accelerator simulation
- More complex setup with Ruby/classic memory
- Requires more configuration parameters

## Usage from GPGPU-Sim

```cpp
#include "gem5_wrapper.hh"

// Create wrapper with config script
std::vector<std::string> args = {
    "--mem-size=8GB",
    "--caches",
    // ... other args
};

flash_gpgpu_sim::Gem5Wrapper gem5(
    "configs/example/gem_forge/gpgpusim_config.py",
    args
);

// Initialize gem5 system
gem5.initialize();

// Simulation loop
while (!done) {
    // Do GPGPU-Sim work...
    
    // Advance gem5 memory system
    auto exit_event = gem5.simulate(cycles_to_simulate);
    
    // Check exit event...
}
```

## Integration Strategies

### Strategy 1: Memory System Only
GPGPU-Sim owns GPU execution, gem5 provides:
- Cache hierarchy (L1/L2/LLC)
- DRAM controllers
- Memory timing models

**Pro**: Clean separation, easier to debug
**Con**: No CPU-GPU interaction

### Strategy 2: Hybrid Execution
- gem5 simulates CPU cores
- GPGPU-Sim simulates GPU cores
- Both share memory system

**Pro**: Full system simulation
**Con**: More complex synchronization

### Strategy 3: Accelerator Model
- Use gem5's accelerator framework
- GPGPU-Sim as a gem5 accelerator
- Tight integration through gem5 APIs

**Pro**: Leverage gem5 infrastructure
**Con**: Requires deeper integration

## Build Instructions

1. Build gem5 with GPGPU-Sim support:
```bash
cd $GEM_FORGE_TOP/gem5/ext/gpgpusim
make  # Creates libgem5_to_gpgpusim.so
```

2. Build GPGPU-Sim with gem5 integration:
```bash
cd $GEM_FORGE_TOP/gpgpusim
make FLASH_GEM_FORGE=1
```

## Environment Variables

- `GEM_FORGE_TOP`: Root of gem-forge-stack (set by sourcing envs.sh)
- `FLASH_GEM_FORGE`: Set to 1 to enable gem5 integration

## Implementation Notes

### Gem5Wrapper Class

The `Gem5Wrapper` class provides:
- **initialize()**: Sets up Python, runs config script, calls m5.instantiate()
- **simulate(n_cycles)**: Advances gem5 simulation by n cycles
- **Handles Python/C++ boundary**: Manages embedded Python interpreter

### Python Config Script

The config script should:
1. Create System object with memory hierarchy
2. Set up cache configuration
3. Call `m5.instantiate()` to finalize system
4. Return the Root object

### Synchronization

For hybrid execution, you'll need to:
- Synchronize clocks between GPGPU-Sim and gem5
- Handle memory request handoff
- Manage event queues

## Example: Minimal Integration

See `gpgpusim_config.py` for a minimal working example that:
- Creates a timing mode system
- Sets up DDR3 memory
- Provides memory bus for cache hierarchy
- Can be extended with Ruby or classic caches

## Next Steps

1. Test basic initialization from GPGPU-Sim
2. Add memory request forwarding
3. Implement synchronization mechanism
4. Add cache hierarchy configuration
5. Profile and optimize the integration
