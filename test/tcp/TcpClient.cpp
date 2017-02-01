// Copyright 2004-present Facebook. All Rights Reserved.

#include <folly/Memory.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <gmock/gmock.h>
#include "src/NullRequestHandler.h"
#include "src/SmartPointers.h"
#include "src/StandardReactiveSocket.h"
#include "src/SubscriptionBase.h"
#include "src/folly/FollyKeepaliveTimer.h"
#include "src/framed/FramedDuplexConnection.h"
#include "src/tcp/TcpDuplexConnection.h"
#include "test/simple/PrintSubscriber.h"
#include "test/simple/StatsPrinter.h"

using namespace ::testing;
using namespace ::reactivesocket;
using namespace ::folly;

DEFINE_string(host, "localhost", "host to connect to");
DEFINE_int32(port, 9898, "host:port to connect to");

namespace {
class Callback : public AsyncSocket::ConnectCallback {
 public:
  virtual ~Callback() = default;

  void connectSuccess() noexcept override {}

  void connectErr(const AsyncSocketException& ex) noexcept override {
    std::cerr << "connectErr " << ex.what() << " " << ex.getType() << std::endl;
  }
};

class ClientSubscription : public SubscriptionBase {
 public:
  explicit ClientSubscription(
      std::shared_ptr<Subscriber<Payload>> response,
      size_t numElems = 2)
      : ExecutorBase(defaultExecutor()),
        response_(std::move(response)),
        numElems_(numElems) {}

 private:
  // Subscription methods
  void requestImpl(size_t n) noexcept override {
    for (size_t i = 0; i < numElems_; i++) {
      response_.onNext(Payload("from server " + std::to_string(i)));
    }
    // response_.onComplete();
    response_.onError(std::runtime_error("XXX"));
  }

  void cancelImpl() noexcept override {}

  SubscriberPtr<Subscriber<Payload>> response_;
  size_t numElems_;
};
}

class ClientRequestHandler : public DefaultRequestHandler {
 public:
  /// Handles a new inbound Subscription requested by the other end.
  void handleRequestSubscription(
      Payload request,
      StreamId streamId,
      const std::shared_ptr<Subscriber<Payload>>& response) noexcept override {
    LOG(INFO) << "ServerRequestHandler.handleRequestSubscription " << request;

    response->onSubscribe(std::make_shared<ClientSubscription>(response));
  }

  /// Handles a new inbound Stream requested by the other end.
  void handleRequestStream(
      Payload request,
      StreamId streamId,
      const std::shared_ptr<Subscriber<Payload>>& response) noexcept override {
    LOG(INFO) << "ServerRequestHandler.handleRequestStream " << request;

    response->onSubscribe(std::make_shared<ClientSubscription>(response));
  }

  void handleRequestResponse(
      Payload request,
      StreamId streamId,
      const std::shared_ptr<Subscriber<Payload>>& response) noexcept override {
    LOG(INFO) << "ServerRequestHandler.handleRequestResponse " << request;

    response->onSubscribe(std::make_shared<ClientSubscription>(response, 1));
  }

  void handleFireAndForgetRequest(
      Payload request,
      StreamId streamId) noexcept override {
    LOG(INFO) << "ServerRequestHandler.handleFireAndForgetRequest " << request;
  }

  void handleMetadataPush(
      std::unique_ptr<folly::IOBuf> request) noexcept override {
    LOG(INFO) << "ServerRequestHandler.handleMetadataPush "
              << request->moveToFbString();
  }

  std::shared_ptr<StreamState> handleSetupPayload(
      ReactiveSocket&,
      ConnectionSetupPayload request) noexcept override {
    LOG(INFO) << "ServerRequestHandler.handleSetupPayload " << request;
    return std::make_shared<StreamState>(Stats::noop());
  }
};

int main(int argc, char* argv[]) {
  FLAGS_logtostderr = true;
  FLAGS_minloglevel = 0;

  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  ScopedEventBaseThread eventBaseThread;

  std::unique_ptr<StandardReactiveSocket> reactiveSocket;
  Callback callback;
  StatsPrinter stats;

  eventBaseThread.getEventBase()->runInEventBaseThreadAndWait(
      [&callback, &reactiveSocket, &eventBaseThread, &stats]() {
        folly::AsyncSocket::UniquePtr socket(
            new folly::AsyncSocket(eventBaseThread.getEventBase()));

        folly::SocketAddress addr(FLAGS_host, FLAGS_port, true);

        socket->connect(&callback, addr);

        std::cout << "attempting connection to " << addr.describe()
                  << std::endl;

        std::unique_ptr<DuplexConnection> connection =
            std::make_unique<TcpDuplexConnection>(
                std::move(socket), inlineExecutor(), stats);
        std::unique_ptr<DuplexConnection> framedConnection =
            std::make_unique<FramedDuplexConnection>(
                std::move(connection), inlineExecutor());
        std::unique_ptr<RequestHandler> requestHandler =
            std::make_unique<ClientRequestHandler>();

        reactiveSocket = StandardReactiveSocket::fromClientConnection(
            *eventBaseThread.getEventBase(),
            std::move(framedConnection),
            std::move(requestHandler),
            ConnectionSetupPayload(
                "text/plain", "text/plain", Payload("meta", "data")),
            stats,
            std::make_unique<FollyKeepaliveTimer>(
                *eventBaseThread.getEventBase(),
                std::chrono::milliseconds(5000)));

        // reactiveSocket->requestSubscription(
        //     Payload("from client"), std::make_shared<PrintSubscriber>());
      });

  std::string name;
  std::getline(std::cin, name);

  eventBaseThread.getEventBase()->runInEventBaseThreadAndWait(
      [&reactiveSocket]() { reactiveSocket.reset(nullptr); });

  return 0;
}
