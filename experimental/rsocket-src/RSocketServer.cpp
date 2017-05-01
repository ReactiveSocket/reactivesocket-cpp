// Copyright 2004-present Facebook. All Rights Reserved.

#include "rsocket/RSocketServer.h"

#include <folly/ExceptionWrapper.h>

#include "rsocket/RSocketConnectionHandler.h"

using namespace reactivesocket;

namespace rsocket {

class RSocketServerConnectionHandler : public virtual RSocketConnectionHandler {
 public:
  RSocketServerConnectionHandler(RSocketServer* server, OnAccept onAccept) {
    server_ = server;
    onAccept_ = onAccept;
  }

  std::shared_ptr<RSocketRequestHandler> getHandler(
      std::unique_ptr<ConnectionSetupRequest> request) override {
    return onAccept_(std::move(request));
  }

  void manageSocket(
      std::unique_ptr<reactivesocket::ReactiveSocket> socket) override {
    socket->onClosed([ this, socket = socket.get() ](
        const folly::exception_wrapper&) {
      // Enqueue another event to remove and delete it.  We cannot delete
      // the ReactiveSocket now as it still needs to finish processing the
      // onClosed handlers in the stack frame above us.
      socket->executor().add([this, socket] { server_->removeSocket(socket); });
    });

    server_->addSocket(std::move(socket));
  }

 private:
  RSocketServer* server_;
  OnAccept onAccept_;
};

RSocketServer::RSocketServer(
    std::unique_ptr<ConnectionAcceptor> connectionAcceptor)
    : lazyAcceptor_(std::move(connectionAcceptor)),
      acceptor_(ProtocolVersion::Unknown) {}

RSocketServer::~RSocketServer() {
  {
    auto locked = sockets_.lock();
    if (locked->empty()) {
      return;
    }

    shutdown_.emplace();

    for (auto& socket : *locked) {
      // close() has to be called on the same executor as the socket.
      socket->executor().add([s = socket.get()] { s->close(); });
    }
  }

  shutdown_->wait();
  DCHECK(sockets_.lock()->empty());
}

void RSocketServer::start(OnAccept onAccept) {
  if (connectionHandler_) {
    throw std::runtime_error("RSocketServer::start() already called.");
  }

  LOG(INFO) << "RSocketServer => initialize connection acceptor on start";

  LOG(INFO) << "RSocketServer => initialize connection acceptor on start";
  connectionHandler_ =
      std::make_unique<RSocketServerConnectionHandler>(this, onAccept);

  lazyAcceptor_
      ->start([this](
                  std::unique_ptr<DuplexConnection> conn,
                  folly::Executor& executor) {
        LOG(INFO) << "RSocketServer => received new connection";

        LOG(INFO) << "RSocketServer => going to accept duplex connection";
        // the callbacks above are wired up, now accept the connection
        // FIXME(alexanderm): This isn't thread safe
        acceptor_.accept(std::move(conn), connectionHandler_);
      })
      .onError([](const folly::exception_wrapper& ex) {
        LOG(FATAL) << "RSocketServer => failed to start HttpAcceptor: "
                   << ex.what();
      });
}

void RSocketServer::startAndPark(OnAccept onAccept) {
  start(std::move(onAccept));
  waiting_.wait();
}

void RSocketServer::unpark() {
  waiting_.post();
}

void RSocketServer::addSocket(std::unique_ptr<ReactiveSocket> socket) {
  sockets_.lock()->insert(std::move(socket));
}

void RSocketServer::removeSocket(ReactiveSocket* socket) {
  // This is a hack.  We make a unique_ptr so that we can use it to
  // search the set.  However, we release the unique_ptr so it doesn't
  // try to free the ReactiveSocket too.
  std::unique_ptr<ReactiveSocket> ptr{socket};

  auto locked = sockets_.lock();
  locked->erase(ptr);

  ptr.release();

  LOG(INFO) << "Removed ReactiveSocket";

  if (shutdown_ && locked->empty()) {
    shutdown_->post();
  }
}
} // namespace rsocket
