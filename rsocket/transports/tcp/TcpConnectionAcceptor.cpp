// Copyright 2004-present Facebook. All Rights Reserved.

#include "rsocket/transports/tcp/TcpConnectionAcceptor.h"

#include <folly/Format.h>
#include <folly/futures/Future.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/system/ThreadName.h>

#include "rsocket/framing/FramedDuplexConnection.h"
#include "rsocket/transports/tcp/TcpDuplexConnection.h"

namespace rsocket {

class TcpConnectionAcceptor::SocketCallback
    : public folly::AsyncServerSocket::AcceptCallback {
 public:
  explicit SocketCallback(OnDuplexConnectionAccept& onAccept, bool batchIo)
      : onAccept_{onAccept}, batchIo_{batchIo} {}

  void connectionAccepted(
      int fd,
      const folly::SocketAddress& address) noexcept override {
    VLOG(2) << "Accepting TCP connection from " << address << " on FD " << fd;

    folly::AsyncTransportWrapper::UniquePtr socket(
        new folly::AsyncSocket(eventBase(), fd));

    std::unique_ptr<DuplexConnection> connection;
    if (batchIo_) {
      connection =
          std::make_unique<BatchingTcpDuplexConnection>(std::move(socket));
    } else {
      connection = std::make_unique<TcpDuplexConnection>(std::move(socket));
    }

    onAccept_(std::move(connection), *eventBase());
  }

  void acceptError(const std::exception& ex) noexcept override {
    VLOG(2) << "TCP error: " << ex.what();
  }

  folly::EventBase* eventBase() const {
    return thread_.getEventBase();
  }

 private:
  /// The thread running this callback.
  folly::ScopedEventBaseThread thread_;

  /// Reference to the ConnectionAcceptor's callback.
  OnDuplexConnectionAccept& onAccept_;

  const bool batchIo_;
};

////////////////////////////////////////////////////////////////////////////////

TcpConnectionAcceptor::TcpConnectionAcceptor(Options options)
    : options_(std::move(options)) {}

TcpConnectionAcceptor::~TcpConnectionAcceptor() {
  if (serverThread_) {
    stop();
    serverThread_.reset();
  }
}

////////////////////////////////////////////////////////////////////////////////

void TcpConnectionAcceptor::start(OnDuplexConnectionAccept onAccept) {
  if (onAccept_ != nullptr) {
    throw std::runtime_error("TcpConnectionAcceptor::start() already called");
  }

  onAccept_ = std::move(onAccept);
  serverThread_ = std::make_unique<folly::ScopedEventBaseThread>();
  serverThread_->getEventBase()->runInEventBaseThread(
      [] { folly::setThreadName("TcpConnectionAcceptor.Listener"); });

  callbacks_.reserve(options_.threads);
  for (size_t i = 0; i < options_.threads; ++i) {
    callbacks_.push_back(
        std::make_unique<SocketCallback>(onAccept_, options_.batchIo));
    callbacks_[i]->eventBase()->runInEventBaseThread([i] {
      folly::EventBaseManager::get()->getEventBase()->setName(
          folly::sformat("TCPWrk.{}", i));
    });
  }

  VLOG(1) << "Starting TCP listener on port " << options_.address.getPort()
          << " with " << options_.threads << " request threads";

  serverSocket_.reset(
      new folly::AsyncServerSocket(serverThread_->getEventBase()));

  // The AsyncServerSocket needs to be accessed from the listener thread only.
  // This will propagate out any exceptions the listener throws.
  folly::via(
      serverThread_->getEventBase(),
      [this] {
        serverSocket_->bind(options_.address);

        for (auto const& callback : callbacks_) {
          serverSocket_->addAcceptCallback(
              callback.get(), callback->eventBase());
        }

        serverSocket_->listen(options_.backlog);
        serverSocket_->startAccepting();

        for (auto& i : serverSocket_->getAddresses()) {
          VLOG(1) << "Listening on " << i.describe();
        }
      })
      .get();
}

void TcpConnectionAcceptor::stop() {
  VLOG(1) << "Shutting down TCP listener";

  serverThread_->getEventBase()->runInEventBaseThread(
      [serverSocket = std::move(serverSocket_)]() {});
}

folly::Optional<uint16_t> TcpConnectionAcceptor::listeningPort() const {
  if (!serverSocket_) {
    return folly::none;
  }
  return serverSocket_->getAddress().getPort();
}

} // namespace rsocket
