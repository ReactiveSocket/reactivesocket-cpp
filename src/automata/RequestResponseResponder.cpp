// Copyright 2004-present Facebook. All Rights Reserved.

#include "RequestResponseResponder.h"

namespace reactivesocket {

    void RequestResponseResponder::onSubscribeImpl(
        std::shared_ptr<Subscription> subscription) {
      Base::onSubscribe(subscription);
    }

    void RequestResponseResponder::onNextImpl(Payload response) {
      debugCheckOnNextOnCompleteOnError();
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

    void RequestResponseResponder::onCompleteImpl() {
      debugCheckOnNextOnCompleteOnError();
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

    void RequestResponseResponder::onErrorImpl(folly::exception_wrapper ex) {
      debugCheckOnNextOnCompleteOnError();
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

    void RequestResponseResponder::endStream(StreamCompletionSignal signal) {
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

    void RequestResponseResponder::onNextFrame(Frame_CANCEL&& frame) {
      switch (state_) {
        case State::RESPONDING:
          state_ = State::CLOSED;
          connection_->endStream(streamId_, StreamCompletionSignal::GRACEFUL);
          break;
        case State::CLOSED:
          break;
      }
    }

    std::ostream& RequestResponseResponder::logPrefix(std::ostream& os) {
      return os << "RequestResponseResponder(" << &connection_ << ", " << streamId_
                << "): ";
    }
}
