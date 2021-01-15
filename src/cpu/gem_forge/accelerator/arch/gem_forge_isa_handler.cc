#include "gem_forge_isa_handler.hh"

#include "cpu/base.hh"
#include "cpu/exec_context.hh"

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
  }                                                                            \
  case GemForgeStaticInstOpE::STREAM_STORE: {                                  \
    se.stage##StreamStore(dynInfo, ##xc);                                      \
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
  }                                                                            \
  case GemForgeStaticInstOpE::STREAM_STORE: {                                  \
    return se.stage##StreamStore(dynInfo, ##xc);                               \
  }

bool GemForgeISAHandler::shouldCountInPipeline(
    const GemForgeDynInstInfo &dynInfo) {
  if (!dynInfo.staticInst->isGemForge()) {
    return true;
  }
  auto &staticInstInfo = this->getStaticInstInfo(dynInfo);
  switch (staticInstInfo.op) {
  // Only step and load are considered no overhead in pipeline.
  // Store should still be counted, as it serves as the placehoder.
  case GemForgeStaticInstOpE::STREAM_STEP:
  case GemForgeStaticInstOpE::STREAM_LOAD:
  case GemForgeStaticInstOpE::STREAM_FLOAD: {
    return false;
  }
  default: {
    return true;
  }
  }
}

bool GemForgeISAHandler::canDispatch(const GemForgeDynInstInfo &dynInfo) {
  if (!dynInfo.staticInst->isGemForge()) {
    return true;
  }
  auto &staticInstInfo = this->getStaticInstInfo(dynInfo);
  switch (staticInstInfo.op) {
    StreamInstRetCase(canDispatch);
  default: {
    return true;
  }
  }
}

void GemForgeISAHandler::dispatch(const GemForgeDynInstInfo &dynInfo,
                                  GemForgeLSQCallbackList &extraLSQCallbacks) {
  if (!dynInfo.staticInst->isGemForge()) {
    return;
  }
  auto &staticInstInfo = this->getStaticInstInfo(dynInfo);
  switch (staticInstInfo.op) {
    StreamInstCase(dispatch, extraLSQCallbacks);
  default: {
    break;
  }
  }
}

bool GemForgeISAHandler::canExecute(const GemForgeDynInstInfo &dynInfo) {
  if (!dynInfo.staticInst->isGemForge()) {
    return true;
  }
  auto &staticInstInfo = this->getStaticInstInfo(dynInfo);
  switch (staticInstInfo.op) {
    StreamInstRetCase(canExecute);
  default: {
    return true;
  }
  }
}

void GemForgeISAHandler::execute(const GemForgeDynInstInfo &dynInfo,
                                 ExecContext &xc) {
  if (!dynInfo.staticInst->isGemForge()) {
    return;
  }
  auto &staticInstInfo = this->getStaticInstInfo(dynInfo);
  switch (staticInstInfo.op) {
    StreamInstCase(execute, xc);
  default: {
    break;
  }
  }
}

bool GemForgeISAHandler::canCommit(const GemForgeDynInstInfo &dynInfo) {
  if (!dynInfo.staticInst->isGemForge()) {
    return true;
  }
  auto &staticInstInfo = this->getStaticInstInfo(dynInfo);
  switch (staticInstInfo.op) {
    StreamInstRetCase(canCommit);
  default: {
    return true;
  }
  }
}

void GemForgeISAHandler::commit(const GemForgeDynInstInfo &dynInfo) {
  if (!dynInfo.staticInst->isGemForge()) {
    return;
  }
  auto &staticInstInfo = this->getStaticInstInfo(dynInfo);
  switch (staticInstInfo.op) {
    StreamInstCase(commit);
  default: {
    break;
  }
  }
}

void GemForgeISAHandler::rewind(const GemForgeDynInstInfo &dynInfo) {
  if (!dynInfo.staticInst->isGemForge()) {
    return;
  }
  auto &staticInstInfo = this->getStaticInstInfo(dynInfo);
  switch (staticInstInfo.op) {
    StreamInstCase(rewind);
  default: {
    break;
  }
  }
}

void GemForgeISAHandler::storeTo(Addr vaddr, int size) {
  se.storeTo(vaddr, size);
}

GemForgeISAHandler::GemForgeStaticInstInfo &
GemForgeISAHandler::getStaticInstInfo(const GemForgeDynInstInfo &dynInfo) {

  auto pcKey = std::make_pair<Addr, MicroPC>(dynInfo.pc.pc(), dynInfo.pc.upc());

  auto &infoMap = dynInfo.staticInst->isMicroop()
                      ? this->cachedStaticMicroInstInfo
                      : this->cachedStaticMacroInstInfo;

  auto emplaceRet =
      infoMap.emplace(std::piecewise_construct, std::forward_as_tuple(pcKey),
                      std::forward_as_tuple());
  if (emplaceRet.second) {
    // Newly created. Do basic analysis.
    // * Simply use the instruction name may be a bad idea, but it decouples
    // * us from the encoding of the instruction in a specific ISA.
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
    } else if (instName == "ssp_stream_fload") {
      staticInstInfo.op = GemForgeStaticInstOpE::STREAM_LOAD;
    } else if (instName == "ssp_stream_store") {
      staticInstInfo.op = GemForgeStaticInstOpE::STREAM_STORE;
    } else if (instName == "ssp_stream_store_unlock") {
      staticInstInfo.op = GemForgeStaticInstOpE::STREAM_STORE;
    }
  }
  return emplaceRet.first->second;
}

void GemForgeISAHandler::takeOverBy(GemForgeCPUDelegator *newDelegator) {
  this->cpuDelegator = newDelegator;
  this->se.takeOverBy(newDelegator);
}