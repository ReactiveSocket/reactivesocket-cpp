// Copyright 2004-present Facebook. All Rights Reserved.

#include "TcpDuplexConnection.h"
#include <folly/Memory.h>
#include <reactivesocket-cpp/src/mixins/MemoryMixin.h>

namespace reactivesocket {
using namespace ::folly;

void TcpSubscriptionBase::request(size_t n) {
  // ignored for now, currently flow control at higher layers
}

void TcpSubscriptionBase::cancel() {
  // TODO should this close the socket, or is this better handled by framed protocol layer cleanly closing
  // and destructing?
}

Subscriber<Payload>& TcpDuplexConnection::getOutput() {
  if (!outputSubscriber_) {
    outputSubscriber_ = folly::make_unique<TcpOutputSubscriber>(*this);
  }
  return *outputSubscriber_;
};

void TcpDuplexConnection::setInput(Subscriber<Payload>& inputSubscriber) {
  inputSubscriber_.reset(&inputSubscriber);

  auto* subscription = new MemoryMixin<TcpSubscriptionBase>();

  inputSubscriber.onSubscribe(*subscription);

  socket_->setReadCB(this);
};

void TcpDuplexConnection::send(Payload element) {
  socket_->writeChain(this, std::move(element));
}

void TcpDuplexConnection::writeSuccess() noexcept {}

void TcpDuplexConnection::writeErr(
    size_t bytesWritten,
    const AsyncSocketException& ex) noexcept {
  std::cout << "TODO writeErr" << bytesWritten << ex.what() << "\n";
}

void TcpDuplexConnection::getReadBuffer(
    void** bufReturn,
    size_t* lenReturn) noexcept {
  std::tie(*bufReturn, *lenReturn) = readBuffer_.preallocate(4096, 4096);
}

void TcpDuplexConnection::readDataAvailable(size_t len) noexcept {
  readBuffer_.postallocate(len);

  if (inputSubscriber_) {
    readBufferAvailable(readBuffer_.split(len));
  }
}

void TcpDuplexConnection::readEOF() noexcept {
  std::cout << "TODO readEOF\n";
}

void TcpDuplexConnection::readErr(
    const folly::AsyncSocketException& ex) noexcept {
  std::cout << "TODO readErr " << ex.what() << "\n";
}

bool TcpDuplexConnection::isBufferMovable() noexcept {
  return true;
}

void TcpDuplexConnection::readBufferAvailable(
    std::unique_ptr<IOBuf> readBuf) noexcept {
  inputSubscriber_.onNext(std::move(readBuf));
}

void TcpOutputSubscriber::onSubscribe(Subscription& subscription) {
  // no flow control at tcp level, since we can't know the size of messages
  subscription.request(std::numeric_limits<size_t>::max());
};

void TcpOutputSubscriber::onNext(Payload element) {
  connection_.send(std::move(element));
};

void TcpOutputSubscriber::onComplete() {
  std::cout << "TODO onComplete"
            << "\n";
};

void TcpOutputSubscriber::onError(folly::exception_wrapper ex) {
  std::cout << "TODO onError" << ex.what() << "\n";
};
}