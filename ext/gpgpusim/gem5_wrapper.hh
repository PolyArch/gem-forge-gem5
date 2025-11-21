#ifndef __FLASH_GEM_FORGE_GPGPUSIM_GEM5_WRAPPER_HH__
#define __FLASH_GEM_FORGE_GPGPUSIM_GEM5_WRAPPER_HH__

#define TRACING_ON 1

#include "mem/gpgpusim/gpgpusim_requestor.hh"
#include "sim/simulate.hh"
#include "sim/system.hh"

#include <string>
#include <vector>

namespace flash_gpgpu_sim {

/**
 * Gem5Wrapper provides a C++ interface for GPGPU-Sim to interact with gem5.
 *
 * Usage pattern:
 * 1. Create Gem5Wrapper with configuration script path
 * 2. Call initialize() to set up gem5 Python environment and create system
 * 3. Call simulate() to advance gem5 simulation
 * 4. Destructor handles cleanup
 */
class Gem5Wrapper {
public:
  /**
   * Construct wrapper with path to gem5 config script.
   * @param config_script_path Path to Python configuration script (e.g.,
   *                      "configs/example/gem_forge/run.py")
   * @param script_arguments Additional arguments to pass to the config script
   */
  Gem5Wrapper(const std::string &config_script_path,
              const std::vector<std::string> &script_args = {});
  ~Gem5Wrapper();

  /**
   * Initialize gem5: run Python script to create system and call
   * m5.instantiate() Must be called before simulate().
   */
  void initialize();

  /**
   * Simulate gem5 for n_cycles ticks.
   * @param n_cycles Number of ticks to simulate
   * @return The exit event that caused simulation to stop
   */
  gem5::GlobalSimLoopExitEvent *simulate(gem5::Tick n_cycles);

  /**
   * Check if gem5 has been initialized
   */
  bool isInitialized() const { return initialized; }

  gem5::System *getSystem() const { return system; }

  using GPGPUSimReqVec = std::vector<gem5::GPGPUSimRequestor *>;
  const GPGPUSimReqVec &getGPGPUSimRequestors() const {
    return gpgpusim_requestors;
  }

private:
  /**
   * Initialize Python interpreter and gem5's Python modules
   */
  void initPython(int argc, char **argv);

  /**
   * Execute Python commands
   */
  void execPythonCommands(const std::vector<std::string> &commands);

  /**
   * Set up debug flags for gem5 tracing
   * @param debug_flags List of debug flag names to enable
   */
  void setupDebugFlags(const std::vector<std::string> &debug_flags);

  std::string config_script;
  std::vector<std::string> script_args;
  std::vector<char *> args; // For argv passed to Python
  bool initialized;
  bool python_initialized;

  gem5::System *system = nullptr;
  GPGPUSimReqVec gpgpusim_requestors;
};

} // namespace flash_gpgpu_sim

#endif