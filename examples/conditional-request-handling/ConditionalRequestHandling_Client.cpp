// Copyright 2004-present Facebook. All Rights Reserved.

#include <folly/init/Init.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/portability/GFlags.h>
#include <iostream>

#include "examples/util/ExampleSubscriber.h"
#include "rsocket/RSocket.h"
#include "rsocket/transports/tcp/TcpConnectionFactory.h"
#include "yarpl/Flowable.h"

using namespace ::folly;
using namespace ::rsocket_example;
using namespace ::rsocket;
using namespace yarpl::flowable;

DEFINE_string(host, "localhost", "host to connect to");
DEFINE_int32(port, 9898, "host:port to connect to");

class RSocketNetworkStatsLog : public RSocketNetworkStats {
  void onConnected() override {
    LOG(INFO) << "onConnected";
  }

  void onDisconnected(const folly::exception_wrapper& ex) override {
    LOG(INFO) << "onDiconnected ex=" << ex.what();
  }

  void onClosed(const folly::exception_wrapper& ex) override {
    LOG(INFO) << "onClosed ex=" << ex.what();
  }
};

int main(int argc, char* argv[]) {
  FLAGS_logtostderr = true;
  FLAGS_minloglevel = 0;
  folly::init(&argc, &argv);

  folly::SocketAddress address;
  address.setFromHostPort(FLAGS_host, FLAGS_port);

  std::shared_ptr<RSocketClient> client;

  RSocket::createConnectedClient(
      std::make_unique<TcpConnectionFactory>(std::move(address)),
      SetupParameters("application/json", "application/json"),
      std::make_shared<RSocketResponder>(),
      nullptr,
      RSocketStats::noop(),
      std::make_shared<RSocketNetworkStatsLog>())
      .then([&client](std::shared_ptr<RSocketClient> cl) mutable {
        client = std::move(cl);
        client->getRequester()
            ->requestStream(Payload("Bob"))
            ->take(5)
            ->subscribe([](Payload p) {
              std::cout << "Received: " << p.moveDataToString() << std::endl;
            });
      })
      .onError([](folly::exception_wrapper ex) {
        LOG(INFO) << "Exception received " << ex;
      });

  // Wait for a newline on the console to terminate the server.
  std::getchar();
  return 0;
}
