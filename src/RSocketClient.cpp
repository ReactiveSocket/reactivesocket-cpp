// Copyright 2004-present Facebook. All Rights Reserved.

#include "RSocketClient.h"
#include "RSocketRequester.h"
#include "RSocketResponder.h"
#include "RSocketStats.h"
#include "src/internal/FollyKeepaliveTimer.h"
#include "src/framing/FrameTransport.h"
#include "src/framing/FramedDuplexConnection.h"

using namespace folly;

namespace rsocket {

RSocketClient::RSocketClient(
    std::unique_ptr<ConnectionFactory> connectionFactory)
    : connectionFactory_(std::move(connectionFactory)) {
  VLOG(1) << "Constructing RSocketClient";
}

folly::Future<std::shared_ptr<RSocketRequester>> RSocketClient::connect() {
  VLOG(2) << "Starting connection";

  auto promise =
      std::make_shared<folly::Promise<std::shared_ptr<RSocketRequester>>>();
  auto future = promise->getFuture();

  connectionFactory_->connect([this, promise = std::move(promise)](
      std::unique_ptr<DuplexConnection> connection,
      bool isFramedConnection,
      folly::EventBase& eventBase) mutable {
    VLOG(3) << "onConnect received DuplexConnection";

    auto rs = std::make_shared<RSocketStateMachine>(
        eventBase,
        // need to allow Responder being passed in optionally
        std::make_shared<RSocketResponder>(),
        // need to allow stats being passed in
        RSocketStats::noop(),
        // TODO need to optionally allow defining the keepalive timer
        std::make_unique<FollyKeepaliveTimer>(
            eventBase, std::chrono::milliseconds(5000)),
        ReactiveSocketMode::CLIENT);

    // TODO need to allow this being passed in
    auto setupPayload =
        SetupParameters("text/plain", "text/plain", Payload("meta", "data"));

    // TODO ---> this code needs to be moved inside RSocketStateMachine

    rs->setFrameSerializer(
        setupPayload.protocolVersion == ProtocolVersion::Unknown
            ? FrameSerializer::createCurrentVersion()
            : FrameSerializer::createFrameSerializer(
                  setupPayload.protocolVersion));

    rs->setResumable(setupPayload.resumable);

    if (setupPayload.protocolVersion != ProtocolVersion::Unknown) {
      CHECK_EQ(
          setupPayload.protocolVersion, rs->getSerializerProtocolVersion());
    }

    std::unique_ptr<DuplexConnection> framedConnection;
    if(isFramedConnection) {
      framedConnection = std::move(connection);
    } else {
      framedConnection = std::make_unique<FramedDuplexConnection>(
          std::move(connection),
          setupPayload.protocolVersion);
    }

    auto frameTransport =
        yarpl::make_ref<FrameTransport>(std::move(framedConnection));
    rs->setUpFrame(std::move(frameTransport), std::move(setupPayload));

    // TODO <---- up to here
    // TODO and then a simple API such as:
    // TODO rs->connectAndSendSetup(frameTransport, params, setupPayload)

    auto rsocket = RSocketRequester::create(std::move(rs), eventBase);
    // store it so it lives as long as the RSocketClient
    rsockets_.push_back(rsocket);
    promise->setValue(rsocket);
  });

  return future;
}

RSocketClient::~RSocketClient() {
  VLOG(1) << "Destroying RSocketClient";
}
}
