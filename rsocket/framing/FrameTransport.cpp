// Copyright 2004-present Facebook. All Rights Reserved.

#include "rsocket/framing/FrameTransport.h"

#include <folly/ExceptionWrapper.h>
#include <folly/io/IOBuf.h>
#include <glog/logging.h>

#include "rsocket/DuplexConnection.h"
#include "rsocket/framing/FrameProcessor.h"

namespace rsocket {

using namespace yarpl::flowable;

FrameTransport::FrameTransport(std::unique_ptr<DuplexConnection> connection)
    : connection_(std::move(connection)) {
  CHECK(connection_);
}

FrameTransport::~FrameTransport() {
  VLOG(1) << "~FrameTransport";
}

void FrameTransport::connect() {
  CHECK(connection_);

  if (connectionOutput_) {
    // Already connected.
    return;
  }

  connectionOutput_ = connection_->getOutput();
  connectionOutput_->onSubscribe(yarpl::get_ref(this));

  // The onSubscribe call on the previous line may have called the terminating
  // signal which would call disconnect/close.
  if (connection_) {
    // This may call ::onSubscribe in-line, which calls ::request on the
    // provided subscription, which might deliver frames in-line.  It can also
    // call onComplete which will call disconnect/close and reset the
    // connection_ while still inside of the connection_::setInput method.  We
    // will create a hard reference for that case and keep the object alive
    // until setInput method returns
    auto connectionCopy = connection_;
    connectionCopy->setInput(yarpl::get_ref(this));
  }
}

void FrameTransport::setFrameProcessor(
    std::shared_ptr<FrameProcessor> frameProcessor) {
  frameProcessor_ = std::move(frameProcessor);
  if (frameProcessor_) {
    CHECK(!isClosed());
    connect();
  }
}

void FrameTransport::close() {
  closeImpl(folly::exception_wrapper());
}

void FrameTransport::closeWithError(folly::exception_wrapper ew) {
  if (!ew) {
    VLOG(1) << "FrameTransport::closeWithError() called with empty exception";
    ew = std::runtime_error("Undefined error");
  }
  closeImpl(std::move(ew));
}

void FrameTransport::closeImpl(folly::exception_wrapper ew) {
  // Make sure we never try to call back into the processor.
  frameProcessor_ = nullptr;

  if (!connection_) {
    return;
  }

  auto oldConnection = std::move(connection_);

  // Send terminal signals to the DuplexConnection's input and output before
  // tearing it down.  We must do this per DuplexConnection specification (see
  // interface definition).
  if (auto subscriber = std::move(connectionOutput_)) {
    if (ew) {
      subscriber->onError(std::move(ew));
    } else {
      subscriber->onComplete();
    }
  }
  if (auto subscription = std::move(connectionInputSub_)) {
    subscription->cancel();
  }
}

void FrameTransport::onSubscribe(yarpl::Reference<Subscription> subscription) {
  if (!connection_) {
    return;
  }

  CHECK(!connectionInputSub_);
  CHECK(frameProcessor_);
  connectionInputSub_ = std::move(subscription);
  connectionInputSub_->request(std::numeric_limits<int64_t>::max());
}

void FrameTransport::onNext(std::unique_ptr<folly::IOBuf> frame) {
  CHECK(frameProcessor_);
  frameProcessor_->processFrame(std::move(frame));
}

void FrameTransport::terminateProcessor(folly::exception_wrapper ex) {
  // This method can be executed multiple times while terminating.

  if(!frameProcessor_) {
    // already terminated
    return;
  }

  auto frameProcessor = std::move(frameProcessor_);
  VLOG(3) << this << " terminating frame processor ex=" << ex.what();
  frameProcessor->onTerminal(std::move(ex));
}

void FrameTransport::onComplete() {
  VLOG(6) << "onComplete";
  terminateProcessor(folly::exception_wrapper());
}

void FrameTransport::onError(folly::exception_wrapper ex) {
  VLOG(6) << "onError" << ex;
  terminateProcessor(std::move(ex));
}

void FrameTransport::request(int64_t n) {
  // we are expecting we can write output without back pressure
  CHECK_EQ(n, std::numeric_limits<int64_t>::max());
}

void FrameTransport::cancel() {
  VLOG(6) << "cancel";
  terminateProcessor(folly::exception_wrapper());
}

void FrameTransport::outputFrameOrDrop(std::unique_ptr<folly::IOBuf> frame) {
  if (!connection_) {
    // if the connection was closed we will drop the frame
    return;
  }

  CHECK(connectionOutput_); // the connect method has to be already executed
  connectionOutput_->onNext(std::move(frame));
}

}
