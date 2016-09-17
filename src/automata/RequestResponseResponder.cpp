// Copyright 2004-present Facebook. All Rights Reserved.

#include "RequestResponseResponder.h"

#include "src/ConnectionAutomaton.h"
#include "src/Frame.h"
#include "src/Payload.h"
#include "src/ReactiveStreamsCompat.h"

namespace reactivesocket {

void RequestResponseResponderBase::onNext(Payload response) {
  switch (state_) {
    case State::RESPONDING: {
      state_ = State::CLOSED;
      Base::onNext(std::move(response), FrameFlags_COMPLETE);
      connection_->endStream(streamId_, StreamCompletionSignal::GRACEFUL);
      break;
    }
    case State::CLOSED:
      break;
  }
}

void RequestResponseResponderBase::onComplete() {
  switch (state_) {
    case State::RESPONDING: {
      state_ = State::CLOSED;
      connection_->outputFrameOrEnqueue(
          Frame_RESPONSE::complete(streamId_).serializeOut());
      connection_->endStream(streamId_, StreamCompletionSignal::GRACEFUL);
    } break;
    case State::CLOSED:
      break;
  }
}

void RequestResponseResponderBase::onError(folly::exception_wrapper ex) {
  switch (state_) {
    case State::RESPONDING: {
      state_ = State::CLOSED;
      auto msg = ex.what().toStdString();
      connection_->outputFrameOrEnqueue(
          Frame_ERROR::applicationError(streamId_, msg).serializeOut());
      connection_->endStream(streamId_, StreamCompletionSignal::ERROR);
    } break;
    case State::CLOSED:
      break;
  }
}

void RequestResponseResponderBase::endStream(StreamCompletionSignal signal) {
  switch (state_) {
    case State::RESPONDING:
      // Spontaneous ::endStream signal means an error.
      DCHECK(StreamCompletionSignal::GRACEFUL != signal);
      state_ = State::CLOSED;
      break;
    case State::CLOSED:
      break;
  }
  Base::endStream(signal);
}

void RequestResponseResponderBase::onNextFrame(Frame_CANCEL&& frame) {
  switch (state_) {
    case State::RESPONDING:
      state_ = State::CLOSED;
      connection_->endStream(streamId_, StreamCompletionSignal::GRACEFUL);
      break;
    case State::CLOSED:
      break;
  }
}

std::ostream& RequestResponseResponderBase::logPrefix(std::ostream& os) {
  return os << "RequestResponseResponder(" << &connection_ << ", " << streamId_
            << "): ";
}
}
