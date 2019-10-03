#include "gem_forge_isa_handler.hh"

#include "cpu/base.hh"
#include "cpu/exec_context.hh"

namespace RiscvISA {

#define StreamInstCase(stage, xc...)                                           \
  case GemForgeStaticInstOpE::STREAM_CONFIG: {                                 \
    se.stage##StreamConfig(dynInfo, ##xc);                                     \
    break;                                                                     \
  }                                                                            \
  case GemForgeStaticInstOpE::STREAM_INPUT: {                                  \
    se.stage##StreamInput(dynInfo, ##xc);                                      \
    break;                                                                     \
  }                                                                            \
  case GemForgeStaticInstOpE::STREAM_READY: {                                  \
    se.stage##StreamReady(dynInfo, ##xc);                                      \
    break;                                                                     \
  }                                                                            \
  case GemForgeStaticInstOpE::STREAM_END: {                                    \
    se.stage##StreamEnd(dynInfo, ##xc);                                        \
    break;                                                                     \
  }                                                                            \
  case GemForgeStaticInstOpE::STREAM_STEP: {                                   \
    se.stage##StreamStep(dynInfo, ##xc);                                       \
    break;                                                                     \
  }                                                                            \
  case GemForgeStaticInstOpE::STREAM_LOAD:                                     \
  case GemForgeStaticInstOpE::STREAM_FLOAD: {                                  \
    se.stage##StreamLoad(dynInfo, ##xc);                                       \
    break;                                                                     \
  }

#define StreamInstRetCase(stage, xc...)                                        \
  case GemForgeStaticInstOpE::STREAM_CONFIG: {                                 \
    return se.stage##StreamConfig(dynInfo, ##xc);                              \
  }                                                                            \
  case GemForgeStaticInstOpE::STREAM_INPUT: {                                  \
    return se.stage##StreamInput(dynInfo, ##xc);                               \
  }                                                                            \
  case GemForgeStaticInstOpE::STREAM_READY: {                                  \
    return se.stage##StreamReady(dynInfo, ##xc);                               \
  }                                                                            \
  case GemForgeStaticInstOpE::STREAM_END: {                                    \
    return se.stage##StreamEnd(dynInfo, ##xc);                                 \
  }                                                                            \
  case GemForgeStaticInstOpE::STREAM_STEP: {                                   \
    return se.stage##StreamStep(dynInfo, ##xc);                                \
  }                                                                            \
  case GemForgeStaticInstOpE::STREAM_LOAD:                                     \
  case GemForgeStaticInstOpE::STREAM_FLOAD: {                                  \
    return se.stage##StreamLoad(dynInfo, ##xc);                                \
  }

bool GemForgeISAHandler::canDispatch(const GemForgeDynInstInfo &dynInfo) {
  auto &staticInstInfo = this->getStaticInstInfo(dynInfo);
  switch (staticInstInfo.op) {
    StreamInstRetCase(canDispatch);
  default: { return true; }
  }
}

void GemForgeISAHandler::dispatch(const GemForgeDynInstInfo &dynInfo,
                                  GemForgeLQCallbackList &extraLQCallbacks,
                                  bool &isGemForgeLoad) {
  auto &staticInstInfo = this->getStaticInstInfo(dynInfo);
  switch (staticInstInfo.op) {
    StreamInstCase(dispatch, extraLQCallbacks);
  default: { break; }
  }
  if (staticInstInfo.op == GemForgeStaticInstOpE::STREAM_LOAD) {
    isGemForgeLoad = true;
  } else {
    isGemForgeLoad = false;
  }
}

bool GemForgeISAHandler::canExecute(const GemForgeDynInstInfo &dynInfo) {
  auto &staticInstInfo = this->getStaticInstInfo(dynInfo);
  switch (staticInstInfo.op) {
    StreamInstRetCase(canExecute);
  default: { return true; }
  }
}

void GemForgeISAHandler::execute(const GemForgeDynInstInfo &dynInfo,
                                 ExecContext &xc) {
  auto &staticInstInfo = this->getStaticInstInfo(dynInfo);
  switch (staticInstInfo.op) {
    StreamInstCase(execute, xc);
  default: { break; }
  }
}

void GemForgeISAHandler::commit(const GemForgeDynInstInfo &dynInfo) {
  auto &staticInstInfo = this->getStaticInstInfo(dynInfo);
  switch (staticInstInfo.op) {
    StreamInstCase(commit);
  default: { break; }
  }
}

void GemForgeISAHandler::rewind(const GemForgeDynInstInfo &dynInfo) {
  auto &staticInstInfo = this->getStaticInstInfo(dynInfo);
  switch (staticInstInfo.op) {
    StreamInstCase(rewind);
  default: { break; }
  }
}

void GemForgeISAHandler::storeTo(Addr vaddr, int size) {
  se.storeTo(vaddr, size);
}

GemForgeISAHandler::GemForgeStaticInstInfo &
GemForgeISAHandler::getStaticInstInfo(const GemForgeDynInstInfo &dynInfo) {
  // TODO: Handle microop.
  auto pc = dynInfo.pc.pc();
  auto emplaceRet = this->cachedStaticInstInfo.emplace(
      std::piecewise_construct, std::forward_as_tuple(pc),
      std::forward_as_tuple());
  if (emplaceRet.second) {
    // Newly created. Do basic analysis.
    // I am very surprised that Gem5 does not provide an easy way for me to get
    // the opcode of the inst. So far this is so fragile.
    // TODO: Improve this.
    auto instName = dynInfo.staticInst->getName();
    auto &staticInstInfo = emplaceRet.first->second;

    if (instName == "ssp_stream_config") {
      staticInstInfo.op = GemForgeStaticInstOpE::STREAM_CONFIG;
    } else if (instName == "ssp_stream_end") {
      staticInstInfo.op = GemForgeStaticInstOpE::STREAM_END;
    } else if (instName == "ssp_stream_step") {
      staticInstInfo.op = GemForgeStaticInstOpE::STREAM_STEP;
    } else if (instName == "ssp_stream_input") {
      staticInstInfo.op = GemForgeStaticInstOpE::STREAM_INPUT;
    } else if (instName == "ssp_stream_ready") {
      staticInstInfo.op = GemForgeStaticInstOpE::STREAM_READY;
    } else if (instName == "ssp_stream_load") {
      staticInstInfo.op = GemForgeStaticInstOpE::STREAM_LOAD;
      // } else if (instName == "ssp_stream_fload") {
      //   staticInstInfo.op = GemForgeStaticInstOpE::STREAM_LOAD;
      // } else if (instName == "ssp_stream_flw") {
      //   // ? Do we have to distinguish fload and flw?
      //   staticInstInfo.op = GemForgeStaticInstOpE::STREAM_LOAD;
    }
  }
  return emplaceRet.first->second;
}

} // namespace RiscvISA