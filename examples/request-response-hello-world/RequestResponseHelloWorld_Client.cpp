// Copyright 2004-present Facebook. All Rights Reserved.

#include <iostream>

#include <folly/init/Init.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/portability/GFlags.h>

#include "examples/util/ExampleSubscriber.h"
#include "rsocket/RSocket.h"
#include "rsocket/transports/tcp/TcpConnectionFactory.h"

#include "yarpl/Single.h"

using namespace rsocket_example;
using namespace rsocket;
using namespace yarpl;
using namespace yarpl::single;

DEFINE_string(host, "localhost", "host to connect to");
DEFINE_int32(port, 9898, "host:port to connect to");

int main(int argc, char* argv[]) {
  FLAGS_logtostderr = true;
  FLAGS_minloglevel = 0;
  folly::init(&argc, &argv);

  folly::SocketAddress address;
  address.setFromHostPort(FLAGS_host, FLAGS_port);

  std::shared_ptr<RSocketClient> client;

  RSocket::createConnectedClient(
      std::make_unique<TcpConnectionFactory>(std::move(address)))
      .then([&client](std::shared_ptr<RSocketClient> cl) mutable {
        LOG(INFO) << "Connected";
        client = std::move(cl);
        client->getRequester()
            ->requestResponse(Payload("Jane"))
            ->subscribe([](Payload p) {
              std::cout << "Received >> " << p.moveDataToString() << std::endl;
            });
      })
      .onError([](folly::exception_wrapper ex) {
        LOG(INFO) << "Exception received " << ex;
      });

  // Wait for a newline on the console to terminate the client.
  std::getchar();

  return 0;
}
