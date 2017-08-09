// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include <utility>

#include <gtest/gtest.h>

#include "rsocket/RSocket.h"
#include "rsocket/transports/tcp/TcpConnectionAcceptor.h"
#include "rsocket/transports/tcp/TcpConnectionFactory.h"
#include "test/handlers/HelloStreamRequestHandler.h"

namespace rsocket {
namespace tests {
namespace client_server {

std::unique_ptr<RSocketServer> makeServer(
    std::shared_ptr<rsocket::RSocketResponder> responder);

std::unique_ptr<RSocketServer> makeResumableServer(
    std::shared_ptr<RSocketServiceHandler> serviceHandler);

std::shared_ptr<RSocketClient> makeClient(
    folly::EventBase* eventBase,
    uint16_t port);

std::shared_ptr<RSocketClient> makeResumableClient(
    folly::EventBase* eventBase,
    uint16_t port,
    std::shared_ptr<RSocketConnectionEvents> connectionEvents = nullptr);

} // namespace client_server
} // namespace tests
} // namespace rsocket
