// Copyright 2004-present Facebook. All Rights Reserved.

#include "StreamSubscriptionResponderBase.h"

#include <folly/ExceptionWrapper.h>
#include <folly/io/IOBuf.h>
#include <glog/logging.h>

#include "src/ConnectionAutomaton.h"
#include "src/Frame.h"
#include "src/Payload.h"
#include "src/ReactiveStreamsCompat.h"

namespace reactivesocket {

void StreamSubscriptionResponderBase::onNext(Payload response) {
  switch (state_) {
    case State::RESPONDING:
      Base::onNext(std::move(response));
      break;
    case State::CLOSED:
      break;
  }
}

void StreamSubscriptionResponderBase::onComplete() {
  switch (state_) {
    case State::RESPONDING: {
      state_ = State::CLOSED;
      connection_->onNextFrame(Frame_RESPONSE::complete(streamId_));
      connection_->endStream(streamId_, StreamCompletionSignal::GRACEFUL);
    } break;
    case State::CLOSED:
      break;
  }
}

void StreamSubscriptionResponderBase::onError(folly::exception_wrapper ex) {
  switch (state_) {
    case State::RESPONDING: {
      state_ = State::CLOSED;
      auto msg = ex.what().toStdString();
      connection_->onNextFrame(Frame_ERROR::applicationError(streamId_, msg));
      connection_->endStream(streamId_, StreamCompletionSignal::ERROR);
    } break;
    case State::CLOSED:
      break;
  }
}

void StreamSubscriptionResponderBase::endStream(StreamCompletionSignal signal) {
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

void StreamSubscriptionResponderBase::onNextFrame(Frame_CANCEL&& frame) {
  switch (state_) {
    case State::RESPONDING:
      state_ = State::CLOSED;
      connection_->endStream(streamId_, StreamCompletionSignal::GRACEFUL);
      break;
    case State::CLOSED:
      break;
  }
}
}
