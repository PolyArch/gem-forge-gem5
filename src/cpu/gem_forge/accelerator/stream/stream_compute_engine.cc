#include "stream_compute_engine.hh"

#include "base/trace.hh"
#include "debug/StreamEngineBase.hh"

#define DEBUG_TYPE StreamEngineBase
#include "stream_log.hh"

namespace gem5 {

StreamComputeEngine::StreamComputeEngine(StreamEngine *_se,
                                         const StreamEngine::Params *_params)
    : se(_se), computeWidth(_params->computeWidth),
      forceZeroLatency(_params->enableZeroComputeLatency) {}

void StreamComputeEngine::pushReadyComputation(StreamElement *element,
                                               StreamValue result,
                                               Cycles latency) {
  if (this->forceZeroLatency) {
    latency = Cycles(0);
  }
  auto computation =
      std::make_unique<Computation>(element, std::move(result), latency);
  this->readyComputations.emplace_back(std::move(computation));
  element->scheduledComputation = true;
  this->se->numScheduledComputation++;
}

void StreamComputeEngine::startComputation() {

  int startedComputation = 0;
  while (startedComputation < this->computeWidth &&
         !this->readyComputations.empty() &&
         this->inflyComputations.size() <
             this->se->myParams->computeMaxInflyComputation) {
    auto computation = std::move(this->readyComputations.front());

    S_ELEMENT_DPRINTF(computation->element,
                      "Start computation. Charge Latency %llu.\n",
                      computation->latency);
    this->pushInflyComputation(std::move(computation));

    this->readyComputations.pop_front();
    startedComputation++;
  }
}

void StreamComputeEngine::completeComputation() {

  // We don't charge complete width.
  auto curCycle = this->se->curCycle();
  while (!this->inflyComputations.empty()) {
    auto &computation = this->inflyComputations.front();
    auto element = computation->element;
    auto S = element->stream;
    if (computation->readyCycle > curCycle) {
      S_ELEMENT_DPRINTF(
          element,
          "Cannot complete computation, readyCycle %llu, curCycle %llu.\n",
          computation->readyCycle, curCycle);
      break;
    }
    S_ELEMENT_DPRINTF(element, "Complete computation.\n");
    element->receiveComputeResult(computation->result);
    element->scheduledComputation = false;

    this->recordCompletedStats(S);

    this->inflyComputations.pop_front();
  }
}

void StreamComputeEngine::recordCompletedStats(Stream *S) {
  auto microOps = S->getComputationNumMicroOps();
  S->recordComputationInCoreStats();
  this->se->numCompletedComputation++;
  this->se->numCompletedComputeMicroOps += microOps;
  auto category = S->getComputationCategory();

#define record_micro_ops(Addr, Compute)                                        \
  if (category.first == Stream::ComputationType::Compute &&                    \
      category.second == Stream::ComputationAddressPattern::Addr) {            \
    this->se->numCompleted##Addr##Compute##MicroOps += microOps;               \
  }
  record_micro_ops(Affine, LoadCompute);
  record_micro_ops(Affine, StoreCompute);
  record_micro_ops(Affine, AtomicCompute);
  record_micro_ops(Affine, Update);
  record_micro_ops(Affine, Reduce);
  record_micro_ops(Indirect, LoadCompute);
  record_micro_ops(Indirect, StoreCompute);
  record_micro_ops(Indirect, AtomicCompute);
  record_micro_ops(Indirect, Update);
  record_micro_ops(Indirect, Reduce);
  record_micro_ops(PointerChase, LoadCompute);
  record_micro_ops(PointerChase, StoreCompute);
  record_micro_ops(PointerChase, AtomicCompute);
  record_micro_ops(PointerChase, Update);
  record_micro_ops(PointerChase, Reduce);
  record_micro_ops(MultiAffine, LoadCompute);
  record_micro_ops(MultiAffine, StoreCompute);
  record_micro_ops(MultiAffine, AtomicCompute);
  record_micro_ops(MultiAffine, Update);
  record_micro_ops(MultiAffine, Reduce);
#undef record_micro_ops
}

void StreamComputeEngine::pushInflyComputation(ComputationPtr computation) {

  assert(this->inflyComputations.size() <
             this->se->myParams->computeMaxInflyComputation &&
         "Too many infly results.");
  assert(computation->latency < 1024 && "Latency too long.");

  computation->readyCycle = this->se->curCycle() + computation->latency;
  for (auto iter = this->inflyComputations.rbegin(),
            end = this->inflyComputations.rend();
       iter != end; ++iter) {
    if ((*iter)->readyCycle <= computation->readyCycle) {
      this->inflyComputations.emplace(iter.base(), std::move(computation));
      return;
    }
  }
  this->inflyComputations.emplace_front(std::move(computation));
}

void StreamComputeEngine::discardComputation(StreamElement *element) {
  if (!element->scheduledComputation) {
    S_ELEMENT_PANIC(element, "No scheduled computation to be discarded.");
  }
  for (auto iter = this->inflyComputations.begin(),
            end = this->inflyComputations.end();
       iter != end; ++iter) {
    auto &computation = *iter;
    if (computation->element == element) {
      element->scheduledComputation = false;
      this->inflyComputations.erase(iter);
      return;
    }
  }
  for (auto iter = this->readyComputations.begin(),
            end = this->readyComputations.end();
       iter != end; ++iter) {
    auto &computation = *iter;
    if (computation->element == element) {
      element->scheduledComputation = false;
      this->readyComputations.erase(iter);
      return;
    }
  }
  S_ELEMENT_PANIC(element, "Failed to find the scheduled computation.");
}
} // namespace gem5
