
#include "gem5_wrapper.hh"

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <base/logging.hh>
#include <python/embedded.hh>
#include <sim/core.hh>
#include <sim/eventq.hh>
#include <sim/init.hh>
#include <sim/init_signals.hh>
#include <sim/root.hh>
#include <sim/sim_events.hh>
#include <sim/stat_control.hh>

#include "debug/GPGPUSim.hh"

namespace py = pybind11;

namespace flash_gpgpu_sim {

Gem5Wrapper::Gem5Wrapper(const std::string &config_script_path,
                         const std::vector<std::string> &script_arguments)
    : config_script(config_script_path), script_args(script_arguments),
      initialized(false), python_initialized(false) {}

Gem5Wrapper::~Gem5Wrapper() { printf("Gem5Wrapper destroyed\n"); }

void Gem5Wrapper::initPython(int argc, char **argv) {
  // Initialize Python interpreter first
  Py_Initialize();

  // Set sys.argv using Python C API (avoids deprecated PySys_SetArgv)
  try {
    py::module_ sys = py::module_::import("sys");
    py::list py_argv;
    for (int i = 0; i < argc; i++) {
      py_argv.append(py::str(argv[i]));
    }
    sys.attr("argv") = py_argv;

    // Import and install the gem5 importer, which will initialize all
    // embedded Python modules via importer.install() -> _init_all_embedded()
    auto importer = py::module_::import("importer");
    importer.attr("install")();

    // CRITICAL: Initialize the event queue for the main thread
    // This sets up _curTickPtr before any gem5 objects are created
    // Mimics what m5.main() does before running config scripts
    py::module_ event = py::module_::import("_m5.event");
    py::object mainq = event.attr("getEventQueue")(0);
    event.attr("setEventQueue")(mainq);

  } catch (py::error_already_set &e) {
    fprintf(stderr, "Error during Python initialization: %s\n", e.what());
    throw;
  }

  python_initialized = true;
}

void Gem5Wrapper::execPythonCommands(const std::vector<std::string> &commands) {
  for (const auto &cmd : commands) {
    py::exec(cmd);
  }
}

void Gem5Wrapper::setupDebugFlags(const std::vector<std::string> &debug_flags) {
  if (debug_flags.empty()) {
    return;
  }

  try {
    py::module_ debug = py::module_::import("m5.debug");
    py::module_ trace = py::module_::import("m5.trace");

    for (const auto &flag : debug_flags) {
      if (debug.attr("flags").contains(flag)) {
        debug.attr("flags")[flag.c_str()].attr("enable")();
        printf("Debug flag '%s' enabled via Python API\n", flag.c_str());
      } else {
        fprintf(stderr, "Warning: Debug flag '%s' not found\n", flag.c_str());
      }
    }

    // CRITICAL: Call trace.enable() to globally enable tracing
    // This must be called after enabling flags, just like m5.main() does
    trace.attr("enable")();

  } catch (py::error_already_set &e) {
    fprintf(stderr, "Error setting up debug flags: %s\n", e.what());
    throw;
  }
}

void Gem5Wrapper::initialize() {

  if (initialized) {
    printf("Gem5Wrapper already initialized\n");
    return;
  }

  // Build argv for gem5
  args.push_back(const_cast<char *>("gem5"));
  // Config script path
  args.push_back(const_cast<char *>(config_script.c_str()));

  for (size_t i = 0; i < script_args.size(); i++) {
    const char *arg_ptr = script_args[i].c_str();
    args.push_back(const_cast<char *>(arg_ptr));
  }

  // Initialize Python if not done yet
  if (!python_initialized) {
    gem5::initSignals();
    gem5::setOutputDir("m5out");
    initPython(args.size(), &args[0]);
  }

  // Run the configuration script
  // This will create the system and call m5.instantiate()
  printf("Running gem5 config script: %s\n", config_script.c_str());

  try {
    // Add gem5 configs path to Python import path
    py::module_ sys = py::module_::import("sys");
    py::list path = sys.attr("path");

    // Set sys.path[0] to the script's directory (mimics normal Python behavior)
    // This is needed for addToPath() to work correctly with relative paths
    std::string script_dir =
        config_script.substr(0, config_script.find_last_of("/\\"));
    if (script_dir.empty()) {
      script_dir = ".";
    }
    path.insert(0, script_dir);

    // Enable debug flags
    // setupDebugFlags({"GPGPUSim"});
    // setupDebugFlags({"ProtocolTrace"});
    // setupDebugFlags({"ProtocolTrace", "RubyGenerated", "RubyQueue"});

    // Set up m5.options before loading the config script
    // Note: m5.options is actually a module (m5/options.py), not just an
    // attribute We need to set attributes on that module object
    py::module_ m5 = py::module_::import("m5");
    py::module_ options_module = py::module_::import("m5.options");

    // Get Python's None object
    py::object none = py::none();

    // Set required attributes on the options module
    options_module.attr("outdir") = "m5out";
    options_module.attr("dump_config") = none;
    options_module.attr("json_config") = none;
    options_module.attr("dot_config") = none;
    options_module.attr("dot_dvfs_config") = none;

    // Execute the config script to load its functions
    py::eval_file(config_script);

    // Call initialize_system() which handles everything:
    // - Creates the system and Root
    // - Calls m5.instantiate()
    py::module_ main = py::module_::import("__main__");

    // Check if we should use a config file or command-line args
    // If script_args is a single file ending in .txt or .cfg, treat it as
    // config file
    py::object result;
    if (script_args.size() == 1 &&
        (script_args[0].find(".txt") != std::string::npos ||
         script_args[0].find(".cfg") != std::string::npos)) {
      // Pass config file path
      result = main.attr("initialize_system")(py::none(), script_args[0]);
    } else {
      // Let it parse from sys.argv (which we already set up)
      result = main.attr("initialize_system")();
    }

    // Extract the tuple: (root, system, future_cpus, args)
    py::tuple result_tuple = result.cast<py::tuple>();
    py::object root_obj = result_tuple[0];
    py::object py_system = result_tuple[1];

    // Extract the C++ System pointer from the Python SimObject wrapper
    // Use getCCObject() method which returns the underlying C++ object
    py::object cc_system = py_system.attr("getCCObject")();
    this->system = cc_system.cast<gem5::System *>();

    // Extract all gpgpusim requestors from the system.
    py::list requestors =
        py_system.attr("gpgpusim_requestors").cast<py::list>();
    for (size_t i = 0; i < requestors.size(); i++) {
      py::object py_req = requestors[i];
      py::object cc_req = py_req.attr("getCCObject")();
      gem5::GPGPUSimRequestor *req = cc_req.cast<gem5::GPGPUSimRequestor *>();
      if (req->coreId >= this->gpgpusim_requestors.size()) {
        this->gpgpusim_requestors.resize(req->coreId + 1, nullptr);
      }
      this->gpgpusim_requestors.at(req->coreId) = req;
    }
    // Check we have all requestors populated.
    for (size_t i = 0; i < this->gpgpusim_requestors.size(); i++) {
      if (!this->gpgpusim_requestors.at(i)) {
        panic("Missing GPGPUSimRequestor for GPU port %zu\n", i);
      }
    }

    // Initialize the simulate limit event
    if (!gem5::simulate_limit_event) {
      gem5::simulate_limit_event = new gem5::GlobalSimLoopExitEvent(
          gem5::mainEventQueue[0]->getCurTick(), "simulate() limit reached", 0);
    }

    initialized = true;
    DPRINTF(GPGPUSim,
            "Gem5 system initialized successfully with %u GPGPUSimRequestors\n",
            this->gpgpusim_requestors.size());

  } catch (py::error_already_set &e) {
    panic("Python error during initialization: %s\n", e.what());
    throw;
  }
}

gem5::GlobalSimLoopExitEvent *Gem5Wrapper::simulate(gem5::Tick n_cycles) {
  if (!initialized) {
    panic("Error: Must call initialize() before simulate()\n");
    return nullptr;
  }

  // Set eventq for this thread.
  gem5::curEventQueue(gem5::getEventQueue(0));

  // Call gem5's simulate function
  return gem5::simulate(n_cycles);
}

} // namespace flash_gpgpu_sim