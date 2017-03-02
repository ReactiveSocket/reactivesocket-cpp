// Copyright 2004-present Facebook. All Rights Reserved.

#include <folly/io/async/ScopedEventBaseThread.h>
#include <iostream>
#include "examples/util/ExampleSubscriber.h"
#include "rsocket/RSocket.h"
#include "rsocket/transports/TcpConnectionFactory.h"

using namespace ::reactivesocket;
using namespace ::folly;
using namespace ::rsocket_example;
using namespace ::rsocket;

DEFINE_string(host, "localhost", "host to connect to");
DEFINE_int32(port, 9898, "host:port to connect to");

int main(int argc, char* argv[]) {
  FLAGS_logtostderr = true;
  FLAGS_minloglevel = 0;
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  auto rsf = RSocket::createClient(
      TcpConnectionFactory::create(FLAGS_host, FLAGS_port));

  {
    LOG(INFO) << "------------------ Run in future.then";
    auto s = std::make_shared<ExampleSubscriber>(5, 6);
    rsf->connect().then([s](std::shared_ptr<RSocketRequester> rs) {
      rs->requestStream(Payload("Bob"), s);
    });
    s->awaitTerminalEvent();
  }

  {
    LOG(INFO) << "------------------ Run after future.get";
    auto s = std::make_shared<ExampleSubscriber>(5, 6);
    auto rs = rsf->connect().get();
    rs->requestStream(Payload("Jane"), s);
    s->awaitTerminalEvent();
  }
  LOG(INFO) << "------------- main() terminating -----------------";
  return 0;
}
