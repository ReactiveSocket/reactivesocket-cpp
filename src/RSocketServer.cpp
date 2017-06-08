// Copyright 2004-present Facebook. All Rights Reserved.

#include "src/RSocketServer.h"
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include "src/framing/FrameTransport.h"
#include "src/RSocketErrors.h"
#include "src/RSocketNetworkStats.h"
#include "src/RSocketStats.h"
#include "src/internal/RSocketConnectionManager.h"

namespace rsocket {

RSocketServer::RSocketServer(
    std::unique_ptr<ConnectionAcceptor> connectionAcceptor)
    : duplexConnectionAcceptor_(std::move(connectionAcceptor)),
      setupResumeAcceptors_([]{
        return new rsocket::SetupResumeAcceptor(
            ProtocolVersion::Unknown,
            folly::EventBaseManager::get()->getExistingEventBase());
      }),
      connectionManager_(std::make_unique<RSocketConnectionManager>()) {}

RSocketServer::~RSocketServer() {
  // Will stop forwarding connections from duplexConnectionAcceptor_ to
  // setupResumeAcceptors_
  isShutdown_ = true;

  // Stop accepting new connections.
  duplexConnectionAcceptor_->stop();

  std::vector<folly::Future<folly::Unit>> closingFutures;
  for (auto& acceptor : setupResumeAcceptors_.accessAllThreads()) {
    // this call will queue up the cleanup on the eventBase
    closingFutures.push_back(acceptor.close());
  }

  folly::collectAll(closingFutures).get();

  connectionManager_.reset(); // will close all existing RSockets and wait

  // All requests are fully finished, worker threads can be safely killed off.
}

void RSocketServer::start(OnRSocketSetup onRSocketSetup) {
  if (started) {
    throw std::runtime_error("RSocketServer::start() already called.");
  }
  started = true;

  LOG(INFO) << "Starting RSocketServer";

  duplexConnectionAcceptor_->start([ this, onRSocketSetup = std::move(onRSocketSetup) ](
      std::unique_ptr<DuplexConnection> connection,
      folly::EventBase & eventBase) {
    acceptConnection(std::move(connection), eventBase, onRSocketSetup);
  }).get(); // block until finished and return or throw
}

void RSocketServer::acceptConnection(
    std::unique_ptr<DuplexConnection> connection,
    folly::EventBase & eventBase,
    OnRSocketSetup onRSocketSetup) {
  if (isShutdown_) {
    // connection is getting out of scope and terminated
    return;
  }

  auto* acceptor = setupResumeAcceptors_.get();

  VLOG(2) << "Going to accept duplex connection";

  acceptor->accept(
      std::move(connection),
      std::bind(
          &RSocketServer::onRSocketSetup,
          this,
          std::move(onRSocketSetup),
          std::placeholders::_1,
          std::placeholders::_2),
      std::bind(
          &RSocketServer::onRSocketResume,
          this,
          OnRSocketResume(),
          std::placeholders::_1,
          std::placeholders::_2));
}

void RSocketServer::onRSocketSetup(
    OnRSocketSetup onRSocketSetup,
    yarpl::Reference<FrameTransport> frameTransport,
    SetupParameters setupParams) {
  // we don't need to check for isShutdown_ here since all callbacks are
  // processed by this time
  VLOG(1) << "Received new setup payload";

  auto* eventBase = folly::EventBaseManager::get()->getExistingEventBase();
  CHECK(eventBase);

  RSocketSetup setup(std::move(frameTransport), std::move(setupParams), *eventBase, *connectionManager_);
  onRSocketSetup(setup);
}

void RSocketServer::onRSocketResume(
    OnRSocketResume onRSocketResume,
    yarpl::Reference<FrameTransport> frameTransport,
    ResumeParameters setupPayload) {
  // we don't need to check for isShutdown_ here since all callbacks are
  // processed by this time

  CHECK(false) << "not implemented";
}

void RSocketServer::startAndPark(OnRSocketSetup onRSocketSetup) {
  start(std::move(onRSocketSetup));
  waiting_.wait();
}

void RSocketServer::unpark() {
  waiting_.post();
}

} // namespace rsocket
