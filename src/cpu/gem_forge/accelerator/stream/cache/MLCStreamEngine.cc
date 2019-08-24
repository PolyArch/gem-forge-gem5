
#include "MLCStreamEngine.hh"
#include "cpu/gem_forge/accelerator/stream/stream.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "base/trace.hh"
#include "debug/RubyStream.hh"

MLCStreamEngine::MLCStreamEngine(AbstractStreamAwareController *_controller,
                                 MessageBuffer *_responseToUpperMsgBuffer,
                                 MessageBuffer *_requestToLLCMsgBuffer)
    : controller(_controller),
      responseToUpperMsgBuffer(_responseToUpperMsgBuffer),
      requestToLLCMsgBuffer(_requestToLLCMsgBuffer) {}

MLCStreamEngine::~MLCStreamEngine() {
  for (auto &stream : this->streams) {
    delete stream;
    stream = nullptr;
  }
  this->streams.clear();
}

void MLCStreamEngine::receiveStreamConfigure(PacketPtr pkt) {
  assert(this->controller->isStreamFloatEnabled() &&
         "Receive stream configure when stream float is disabled.\n");
  auto streamConfigureData = *(pkt->getPtr<CacheStreamConfigureData *>());
  DPRINTF(RubyStream, "MLCStreamEngine: Received StreamConfigure\n");
  // Create the stream.
  auto stream = new MLCDynamicStream(streamConfigureData, this->controller,
                                     this->responseToUpperMsgBuffer,
                                     this->requestToLLCMsgBuffer);
  this->idToStreamMap.emplace(stream->getDynamicStreamId(), stream);
  // Check if there is indirect stream.
  if (streamConfigureData->indirectStreamConfigure != nullptr) {
    // Let's create an indirect stream.
    auto indirectStream = new MLCDynamicIndirectStream(
        streamConfigureData->indirectStreamConfigure.get(), this->controller,
        this->responseToUpperMsgBuffer, this->requestToLLCMsgBuffer);
    this->idToStreamMap.emplace(indirectStream->getDynamicStreamId(),
                                indirectStream);
  }
}

void MLCStreamEngine::receiveStreamData(const ResponseMsg &msg) {
  assert(this->controller->isStreamFloatEnabled() &&
         "Receive stream data when stream float is disabled.\n");
  const auto &sliceId = msg.m_sliceId;
  assert(sliceId.isValid() && "Invalid stream slice id for stream data.");
  for (auto &iter : this->idToStreamMap) {
    if (iter.second->getDynamicStreamId() == sliceId.streamId) {
      // Found the stream.
      iter.second->receiveStreamData(msg);
      return;
    }
  }
  assert(false && "Failed to find configured stream for stream data.");
}

bool MLCStreamEngine::isStreamRequest(const DynamicStreamSliceId &slice) {
  if (!this->controller->isStreamFloatEnabled()) {
    return false;
  }
  if (!slice.isValid()) {
    return false;
  }
  // So far just check if the target stream is configured here.
  auto stream = this->getMLCDynamicStreamFromSlice(slice);
  return stream != nullptr;
}

bool MLCStreamEngine::isStreamOffloaded(const DynamicStreamSliceId &slice) {
  assert(this->isStreamRequest(slice) && "Should be a stream request.");
  auto stream = this->getMLCDynamicStreamFromSlice(slice);
  auto staticStream = stream->getStaticStream();
  // So far always offload for load stream.
  if (staticStream->getStreamType() == "load") {
    return true;
  }
  return false;
}

bool MLCStreamEngine::isStreamCached(const DynamicStreamSliceId &slice) {
  assert(this->isStreamRequest(slice) && "Should be a stream request.");
  auto stream = this->getMLCDynamicStreamFromSlice(slice);
  auto staticStream = stream->getStaticStream();
  // So far do not cache for load stream.
  if (staticStream->getStreamType() == "load") {
    return false;
  }
  return true;
}

bool MLCStreamEngine::receiveOffloadStreamRequest(
    const DynamicStreamSliceId &slice) {
  assert(this->isStreamOffloaded(slice) &&
         "Should be an offloaded stream request.");
  auto startIdx = slice.startIdx;
  auto stream = this->getMLCDynamicStreamFromSlice(slice);
  stream->receiveStreamRequest(startIdx);
  return true;
}

void MLCStreamEngine::receiveOffloadStreamRequestHit(
    const DynamicStreamSliceId &slice) {
  assert(this->isStreamOffloaded(slice) &&
         "Should be an offloaded stream request.");
  auto startIdx = slice.startIdx;
  auto stream = this->getMLCDynamicStreamFromSlice(slice);
  stream->receiveStreamRequestHit(startIdx);
}

MLCDynamicStream *MLCStreamEngine::getMLCDynamicStreamFromSlice(
    const DynamicStreamSliceId &slice) const {
  if (!slice.isValid()) {
    return nullptr;
  }
  auto iter = this->idToStreamMap.find(slice.streamId);
  if (iter != this->idToStreamMap.end()) {
    return iter->second;
  } else {
    return nullptr;
  }
}