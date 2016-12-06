// Copyright 2004-present Facebook. All Rights Reserved.

#include "AbstractStreamAutomaton.h"

#include <ostream>

#include <folly/io/IOBuf.h>

#include "src/Frame.h"

namespace reactivesocket {

std::ostream& operator<<(std::ostream& os, StreamCompletionSignal signal) {
  switch (signal) {
    case StreamCompletionSignal::GRACEFUL:
      return os << "GRACEFUL";
    case StreamCompletionSignal::ERROR:
      return os << "ERROR";
    case StreamCompletionSignal::INVALID_SETUP:
      return os << "INVALID_SETUP";
    case StreamCompletionSignal::UNSUPPORTED_SETUP:
      return os << "UNSUPPORTED_SETUP";
    case StreamCompletionSignal::REJECTED_SETUP:
      return os << "REJECTED_SETUP";
    case StreamCompletionSignal::CONNECTION_ERROR:
      return os << "CONNECTION_ERROR";
    case StreamCompletionSignal::CONNECTION_END:
      return os << "CONNECTION_END";
  }
  // this should be never hit because the switch is over all cases
  std::abort();
}

void AbstractStreamAutomaton::onNextFrame(
    std::unique_ptr<folly::IOBuf> payload) {
  assert(payload);

  auto type = FrameHeader::peekType(*payload);
  switch (type) {
    case FrameType::REQUEST_SUB:
      deserializeAndDispatch<Frame_REQUEST_SUB>(std::move(payload));
      return;
    case FrameType::REQUEST_CHANNEL:
      deserializeAndDispatch<Frame_REQUEST_CHANNEL>(std::move(payload));
      return;
    case FrameType::REQUEST_N:
      deserializeAndDispatch<Frame_REQUEST_N>(std::move(payload));
      return;
    case FrameType::REQUEST_RESPONSE:
      deserializeAndDispatch<Frame_REQUEST_RESPONSE>(std::move(payload));
      return;
    case FrameType::CANCEL:
      deserializeAndDispatch<Frame_CANCEL>(std::move(payload));
      return;
    case FrameType::RESPONSE:
      deserializeAndDispatch<Frame_RESPONSE>(std::move(payload));
      return;
    case FrameType::ERROR:
      deserializeAndDispatch<Frame_ERROR>(std::move(payload));
      return;
    case FrameType::RESERVED:
    default:
      onBadFrame();
      return;
  }
}

template <class Frame>
void AbstractStreamAutomaton::deserializeAndDispatch(
    std::unique_ptr<folly::IOBuf> payload) {
  Frame frame;
  if (frame.deserializeFrom(std::move(payload))) {
    onNextFrame(std::move(frame));
  } else {
    onBadFrame();
  }
}
}
