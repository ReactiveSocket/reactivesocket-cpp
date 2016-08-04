// Copyright 2004-present Facebook. All Rights Reserved.

#include "src/ConnectionAutomaton.h"

#include <limits>

#include <folly/ExceptionWrapper.h>
#include <folly/Optional.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBaseManager.h>
#include <iostream>

#include "src/AbstractStreamAutomaton.h"
#include "src/DuplexConnection.h"
#include "src/Frame.h"
#include "src/ReactiveStreamsCompat.h"

namespace reactivesocket {

ConnectionAutomaton::ConnectionAutomaton(
    std::unique_ptr<DuplexConnection> connection,
    StreamAutomatonFactory factory,
    Stats& stats,
bool client)
    : connection_(std::move(connection)), factory_(std::move(factory)), stats_(stats), client_(client) {
  // We deliberately do not "open" input or output to avoid having c'tor on the
  // stack when processing any signals from the connection. See ::connect and
  // ::onSubscribe.
}

void ConnectionAutomaton::connect() {
  connectionOutput_.reset(&connection_->getOutput());
  connectionOutput_.get()->onSubscribe(*this);
  // This may call ::onSubscribe in-line, which calls ::request on the provided
  // subscription, which might deliver frames in-line.
  connection_->setInput(*this);

  if (client_) {
    // TODO set correct version
    auto flags = FrameFlags_METADATA;
    Frame_SETUP frame(
        flags,
        0,
        std::numeric_limits<uint32_t>::max(),
        std::numeric_limits<uint32_t>::max(),
        "",
        "",
        FrameMetadata::empty(),
        folly::IOBuf::create(0));
    onNext(frame.serializeOut());

    scheduleKeepalive();
  }
  stats_.socketCreated();
}

void ConnectionAutomaton::disconnect() {
  // Send terminal signals to the DuplexConnection's input and output before
  // tearing it down. We must do this per DuplexConnection specification (see
  // interface definition).
  connectionOutput_.onComplete();
  connectionInputSub_.cancel();
  connection_.reset();
  stats_.socketClosed();
}

void onClose(std::unique_ptr<ConnectionCloseCallback> closeCallback) {
  if (closeCallback) {
    closeCallback->closed();
  }
}

ConnectionAutomaton::~ConnectionAutomaton() {
  // We rely on SubscriptionPtr and SubscriberPtr to dispatch appropriate
  // terminal signals.
}

void ConnectionAutomaton::addStream(
    StreamId streamId,
    AbstractStreamAutomaton& automaton) {
  auto result = streams_.emplace(streamId, &automaton);
  (void)result;
  assert(result.second);
}

template <typename Frame>
void ConnectionAutomaton::onNextFrame(Frame& frame) {
  onNextFrame(frame.serializeOut());
}
template void ConnectionAutomaton::onNextFrame(Frame_REQUEST_SUB&);
template void ConnectionAutomaton::onNextFrame(Frame_REQUEST_CHANNEL&);
template void ConnectionAutomaton::onNextFrame(Frame_REQUEST_N&);
template void ConnectionAutomaton::onNextFrame(Frame_REQUEST_FNF&);
template void ConnectionAutomaton::onNextFrame(Frame_CANCEL&);
template void ConnectionAutomaton::onNextFrame(Frame_RESPONSE&);
template void ConnectionAutomaton::onNextFrame(Frame_ERROR&);

void ConnectionAutomaton::onNextFrame(Payload frame) {
  if (!connectionOutput_) {
    return;
  }
  if (pendingWrites_.empty() && writeAllowance_.tryAcquire()) {
    connectionOutput_.onNext(std::move(frame));
  } else {
    // We either have no allowance to perform the operation, or the queue has
    // not been drained (e.g. we're looping in ::request).
    pendingWrites_.emplace_back(std::move(frame));
  }
}

void ConnectionAutomaton::endStream(
    StreamId streamId,
    StreamCompletionSignal signal) {
  // The signal must be idempotent.
  if (!endStreamInternal(streamId, signal)) {
    return;
  }
  // TODO(stupaq): handle connection-level errors
  assert(
      signal == StreamCompletionSignal::GRACEFUL ||
      signal == StreamCompletionSignal::ERROR);
}

bool ConnectionAutomaton::endStreamInternal(
    StreamId streamId,
    StreamCompletionSignal signal) {
  auto it = streams_.find(streamId);
  if (it == streams_.end()) {
    // Unsubscribe handshake initiated by the connection, we're done.
    return false;
  }
  // Remove from the map before notifying the automaton.
  auto automaton = it->second;
  streams_.erase(it);
  automaton->endStream(signal);
  return true;
}

/// @{
void ConnectionAutomaton::onSubscribe(Subscription& subscription) {
  assert(!connectionInputSub_);
  connectionInputSub_.reset(&subscription);
  // This may result in signals being issued by the connection in-line, see
  // ::connect.
  connectionInputSub_.request(std::numeric_limits<size_t>::max());
}

void ConnectionAutomaton::onNext(Payload frame) {
  auto streamIdPtr = FrameHeader::peekStreamId(*frame);
  if (!streamIdPtr) {
    // Failed to deserialize the frame.
    // TODO(stupaq): handle connection-level error
    assert(false);
    return;
  }
  auto streamId = *streamIdPtr;
  if (streamId == 0) {
    onConnectionFrame(std::move(frame));
    return;
  }
  auto it = streams_.find(streamId);
  if (it == streams_.end()) {
    handleUnknownStream(streamId, std::move(frame));
    return;
  }
  auto automaton = it->second;
  // Can deliver the frame.
  automaton->onNextFrame(std::move(frame));
}

void ConnectionAutomaton::onComplete() {
  onTerminal(folly::exception_wrapper());
}

void ConnectionAutomaton::onError(folly::exception_wrapper ex) {
  onTerminal(std::move(ex));
}

void ConnectionAutomaton::onConnectionFrame(Payload payload) {
  auto type = FrameHeader::peekType(*payload);

  switch (type) {
    case FrameType::KEEPALIVE: {
      Frame_KEEPALIVE frame;
      if (frame.deserializeFrom(std::move(payload))) {
        assert(frame.header_.flags_ & FrameFlags_KEEPALIVE_RESPOND);
        frame.header_.flags_ &= ~(FrameFlags_KEEPALIVE_RESPOND);
        connectionOutput_.onNext(frame.serializeOut());
      } else {
        Frame_ERROR errorFrame(
            0,
            ErrorCode::INVALID,
            folly::IOBuf::copyBuffer("unexpected frame"));
        connectionOutput_.onNext(errorFrame.serializeOut());
        // TODO(yschimke) should this be onTerminal
        cancel();
      }
    }
      return;
    case FrameType::SETUP: {
      Frame_SETUP frame;
      if (frame.deserializeFrom(std::move(payload))) {
        if (frame.header_.flags_ & FrameFlags_LEASE) {
          // We don't have the correct lease and wait logic above yet
          Frame_ERROR errorFrame(
              0,
              ErrorCode::UNSUPPORTED_SETUP,
              folly::IOBuf::copyBuffer("leases not supported"));
          connectionOutput_.onNext(errorFrame.serializeOut());
          // TODO(yschimke) should this be onTerminal
          cancel();
        }
      } else {
        // TODO(yschimke) make this conditional if we have versioning problems
        Frame_ERROR errorFrame(
            0,
            ErrorCode::INVALID_SETUP,
            folly::IOBuf::copyBuffer("bad setup frame"));
        connectionOutput_.onNext(errorFrame.serializeOut());
        // TODO(yschimke) should this be onTerminal
        cancel();
      }
    }
      return;
    default:
      // TODO(yschimke) make this conditional if we have versioning problems
      Frame_ERROR errorFrame(
          0, ErrorCode::INVALID, folly::IOBuf::copyBuffer("unexpected frame"));
      connectionOutput_.onNext(errorFrame.serializeOut());
      // TODO(yschimke) should this be onTerminal
      cancel();
      return;
  }
}
/// @}

void ConnectionAutomaton::onTerminal(folly::exception_wrapper ex) {
  // TODO(stupaq): we should rather use error codes that we do understand
  // instead of exceptions we have no idea about
  auto signal = ex ? StreamCompletionSignal::CONNECTION_ERROR
                   : StreamCompletionSignal::CONNECTION_END;
  // Close all streams.
  while (!streams_.empty()) {
    auto oldSize = streams_.size();
    auto result = endStreamInternal(streams_.begin()->first, signal);
    (void)oldSize;
    (void)result;
    // TODO(stupaq): what kind of a user action could violate these assertions?
    assert(result);
    assert(streams_.size() == oldSize - 1);
  }

  // Complete the handshake.
  connectionInputSub_.cancel();
  connectionOutput_.onComplete();
}

/// @{
void ConnectionAutomaton::request(size_t n) {
  if (writeAllowance_.release(n) > 0) {
    // There are no pending writes or we already have this method on the
    // stack.
    return;
  }
  // Drain the queue or the allowance.
  while (!pendingWrites_.empty() && writeAllowance_.tryAcquire()) {
    auto frame = std::move(pendingWrites_.front());
    pendingWrites_.pop_front();
    connectionOutput_.onNext(std::move(frame));
  }
}

void ConnectionAutomaton::cancel() {
  if (!connectionOutput_) {
    // Unsubscribe handshake completed.
    return;
  }
  // We will tear down all streams after receiving a terminal signal on the read
  // path, therefore we just drop the queue and complete the handshake.
  pendingWrites_.clear();
  connectionOutput_.onComplete();
}
/// @}

void ConnectionAutomaton::scheduleKeepalive() {
  auto eventBase = folly::EventBaseManager::get()->getExistingEventBase();
  CHECK(eventBase);

  eventBase->runAfterDelay([this]() { sendKeepalive(); }, 5000);
}

void ConnectionAutomaton::sendKeepalive() {
  // TODO is this check safe? or needs to check a sycnhronized shared flag?
  if (!connectionOutput_) {
    return;
  }

  Frame_KEEPALIVE pingFrame(
      FrameFlags_KEEPALIVE_RESPOND,
      folly::IOBuf::create(0));
  connectionOutput_.onNext(pingFrame.serializeOut());
}

/// @{
void ConnectionAutomaton::handleUnknownStream(
    StreamId streamId,
    Payload payload) {
  // TODO(stupaq): there are some rules about monotonically increasing stream
  // IDs -- let's forget about them for a moment
  if (!factory_(streamId, payload)) {
    Frame_ERROR errorFrame(
        0,
        ErrorCode::INVALID,
        folly::IOBuf::copyBuffer("unknown stream " + std::to_string(streamId)));
    connectionOutput_.onNext(errorFrame.serializeOut());
    // TODO(yschimke) should this be onTerminal
    cancel();
  }
}
/// @}
}
