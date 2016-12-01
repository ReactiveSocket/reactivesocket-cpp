// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include <iostream>
#include <type_traits>

#include <glog/logging.h>

#include <reactive-streams/utilities/SmartPointers.h>
#include "src/ConnectionAutomaton.h"
#include "src/Frame.h"
#include "src/Payload.h"
#include "src/ReactiveStreamsCompat.h"

namespace reactivesocket {

enum class StreamCompletionSignal;

/// A mixin that represents a flow-control-aware producer of data.
template <typename ProducedFrame, typename Base>
class PublisherMixin : public Base {
 public:
  using Base::Base;

  /// @{
  void onSubscribe(std::shared_ptr<Subscription> subscription) {
    DCHECK(!producingSubscription_);
    producingSubscription_.reset(std::move(subscription));
  }

  void onNext(Payload payload, FrameFlags flags = FrameFlags_EMPTY) {
    DCHECK(producingSubscription_);
    ProducedFrame frame(Base::streamId_, flags, std::move(payload));
    Base::connection_->outputFrameOrEnqueue(frame.serializeOut());
  }
  /// @}

  std::ostream& logPrefix(std::ostream& os) {
    return os << "PublisherMixin(" << &this->connection_ << ", "
              << this->streamId_ << "): ";
  }

  std::shared_ptr<Subscription> subscription() {
    return producingSubscription_;
  }

 protected:
  /// @{
  void endStream(StreamCompletionSignal signal) {
    producingSubscription_.cancel();
    Base::endStream(signal);
  }

  /// Intercept frames that carry allowance.
  template <typename Frame>
  typename std::enable_if<Frame::Trait_CarriesAllowance>::type onNextFrame(
      Frame&& frame) {
    // if producingSubscription_ == nullptr that means the instance is
    // terminated
    if (producingSubscription_) {
      if (size_t n = frame.requestN_) {
        producingSubscription_.request(n);
      }
      Base::onNextFrame(std::move(frame));
    }
  }

  void onNextFrame(Frame_REQUEST_RESPONSE&& frame) {
    // if producingSubscription_ == nullptr that means the instance is
    // terminated
    if (producingSubscription_) {
      producingSubscription_.request(1);
      Base::onNextFrame(std::move(frame));
    }
  }

  /// Remaining frames just pass through.
  template <typename Frame>
  typename std::enable_if<!Frame::Trait_CarriesAllowance>::type onNextFrame(
      Frame&& frame) {
    Base::onNextFrame(std::move(frame));
  }
  /// @}

 private:
  /// A Subscription that constrols production of payloads.
  /// This mixin is responsible for delivering a terminal signal to the
  /// Subscription once the stream ends.
  reactivestreams::SubscriptionPtr<Subscription> producingSubscription_;
};
}
