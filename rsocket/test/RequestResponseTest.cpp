// Copyright 2004-present Facebook. All Rights Reserved.

#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/synchronization/Baton.h>
#include <gtest/gtest.h>
#include <thread>

#include "RSocketTests.h"
#include "rsocket/test/test_utils/GenericRequestResponseHandler.h"
#include "yarpl/Single.h"
#include "yarpl/single/SingleTestObserver.h"

using namespace yarpl;
using namespace yarpl::single;
using namespace rsocket;
using namespace rsocket::tests;
using namespace rsocket::tests::client_server;

namespace {
class TestHandlerCancel : public rsocket::RSocketResponder {
 public:
  TestHandlerCancel(
      std::shared_ptr<folly::Baton<>> onCancel,
      std::shared_ptr<folly::Baton<>> onSubscribe)
      : onCancel_(std::move(onCancel)), onSubscribe_(std::move(onSubscribe)) {}
  std::shared_ptr<Single<Payload>> handleRequestResponse(Payload request, StreamId)
      override {
    // used to signal to the client when the subscribe is received
    onSubscribe_->post();
    // used to block this responder thread until a cancel is sent from client
    // over network
    auto cancelFromClient = std::make_shared<folly::Baton<>>();
    // used to signal to the client once we receive a cancel
    auto onCancel = onCancel_;
    auto requestString = request.moveDataToString();
    return Single<Payload>::create(
        [ name = std::move(requestString), cancelFromClient, onCancel ](
            auto subscriber) mutable {
          std::thread([
            subscriber = std::move(subscriber),
            name = std::move(name),
            cancelFromClient,
            onCancel
          ]() {
            auto subscription = SingleSubscriptions::create(
                [cancelFromClient] { cancelFromClient->post(); });
            subscriber->onSubscribe(subscription);
            // simulate slow processing or IO being done
            // and block this current background thread
            // until we are cancelled
            cancelFromClient->wait();
            if (subscription->isCancelled()) {
              //  this is used by the unit test to assert the cancel was
              //  received
              onCancel->post();
            } else {
              // if not cancelled would do work and emit here
            }
          })
              .detach();
        });
  }

 private:
  std::shared_ptr<folly::Baton<>> onCancel_;
  std::shared_ptr<folly::Baton<>> onSubscribe_;
};
} // namespace

TEST(RequestResponseTest, Cancel) {
  folly::ScopedEventBaseThread worker;
  auto onCancel = std::make_shared<folly::Baton<>>();
  auto onSubscribe = std::make_shared<folly::Baton<>>();
  auto server =
      makeServer(std::make_shared<TestHandlerCancel>(onCancel, onSubscribe));
  auto client = makeClient(worker.getEventBase(), *server->listeningPort());
  auto requester = client->getRequester();

  auto to = SingleTestObserver<std::string>::create();
  requester->requestResponse(Payload("Jane"))
      ->map([](auto p) { return p.moveDataToString(); })
      ->subscribe(to);
  // NOTE: wait for server to receive request/subscribe
  // otherwise the cancellation will all happen locally
  onSubscribe->wait();
  // now cancel the local subscription
  to->cancel();
  // wait for cancel to propagate to server
  onCancel->wait();
  // assert no signals received on client
  to->assertNoTerminalEvent();
}

// response creation usage
TEST(RequestResponseTest, CanCtorTypes) {
  Response r1 = payload_response("foo", "bar");
  Response r2 = error_response(std::runtime_error("whew!"));
}

TEST(RequestResponseTest, Hello) {
  folly::ScopedEventBaseThread worker;
  auto server = makeServer(std::make_shared<GenericRequestResponseHandler>(
      [](StringPair const& request) {
        return payload_response(
            "Hello, " + request.first + " " + request.second + "!", ":)");
      }));

  auto client = makeClient(worker.getEventBase(), *server->listeningPort());
  auto requester = client->getRequester();

  auto to = SingleTestObserver<StringPair>::create();
  requester->requestResponse(Payload("Jane", "Doe"))
      ->map(payload_to_stringpair)
      ->subscribe(to);
  to->awaitTerminalEvent();
  to->assertOnSuccessValue({"Hello, Jane Doe!", ":)"});
}

TEST(RequestResponseTest, FailureInResponse) {
  folly::ScopedEventBaseThread worker;
  auto server = makeServer(std::make_shared<GenericRequestResponseHandler>(
      [](StringPair const& request) {
        EXPECT_EQ(request.first, "foo");
        EXPECT_EQ(request.second, "bar");
        return error_response(std::runtime_error("whew!"));
      }));

  auto client = makeClient(worker.getEventBase(), *server->listeningPort());
  auto requester = client->getRequester();

  auto to = SingleTestObserver<StringPair>::create();
  requester->requestResponse(Payload("foo", "bar"))
      ->map(payload_to_stringpair)
      ->subscribe(to);
  to->awaitTerminalEvent();
  to->assertOnErrorMessage("whew!");
}

TEST(RequestResponseTest, RequestOnDisconnectedClient) {
  folly::ScopedEventBaseThread worker;
  auto client = makeDisconnectedClient(worker.getEventBase());

  auto requester = client->getRequester();
  bool did_call_on_error = false;
  folly::Baton<> wait_for_on_error;
  requester->requestResponse(Payload("foo", "bar"))
      ->subscribe(
          [](auto) {
            // should not call onSuccess
            FAIL();
          },
          [&](folly::exception_wrapper) {
            did_call_on_error = true;
            wait_for_on_error.post();
          });

  wait_for_on_error.timed_wait(std::chrono::milliseconds(100));
  ASSERT(did_call_on_error);
}

// TODO: test that multiple requests on a requestResponse
// fail in a well-defined way (right now it'd nullptr deref)
TEST(RequestResponseTest, MultipleRequestsError) {
  folly::ScopedEventBaseThread worker;
  auto server = makeServer(std::make_shared<GenericRequestResponseHandler>(
      [](StringPair const& request) {
        EXPECT_EQ(request.first, "foo");
        EXPECT_EQ(request.second, "bar");
        return payload_response("baz", "quix");
      }));

  auto client = makeClient(worker.getEventBase(), *server->listeningPort());
  auto requester = client->getRequester();

  auto flowable = requester->requestResponse(Payload("foo", "bar"));
}

TEST(RequestResponseTest, FailureOnRequest) {
  folly::ScopedEventBaseThread worker;
  auto server = makeServer(
      std::make_shared<GenericRequestResponseHandler>([](auto const&) {
        ADD_FAILURE();
        return payload_response("", "");
      }));

  auto client = makeClient(worker.getEventBase(), *server->listeningPort());
  auto requester = client->getRequester();

  VLOG(0) << "Shutting down server so client request fails";
  server->shutdownAndWait();
  server.reset();
  VLOG(0) << "Done";

  auto to = SingleTestObserver<StringPair>::create();
  requester->requestResponse(Payload("foo", "bar"))
      ->map(payload_to_stringpair)
      ->subscribe(to);
  to->awaitTerminalEvent();
  EXPECT_TRUE(to->getError());
}

TEST(RequestResponseTest, TestLargePayload) {
  std::string niceLongData     = "ABCDEFGH";
  std::string niceLongMetadata = "12345678";

  auto const REQUEST_SIZE_MB = 50;

  // extend payload and metadata to be > 50 megabytes
  LOG(INFO) << "Building up large data/metadata, this may take a moment...";
  while (niceLongMetadata.size() < (REQUEST_SIZE_MB * 1024 * 1024)) {
    niceLongMetadata += niceLongMetadata;
  }
  while (niceLongData.size() < (REQUEST_SIZE_MB * 1024 * 1024)) {
    niceLongData += niceLongData;
  }
  LOG(INFO) << "Built meta size: " << niceLongMetadata.size()
            << " data size: " << niceLongData.size();

  // Builds up an IOBuf consisting of chunks with the following sizes, and then
  // the rest tacked on the end in one big iobuf chunk
  auto buildIOBufFromString = [&](std::vector<size_t> const& sizes,
                                  std::string const& from) {
    folly::IOBufQueue bufQueue{folly::IOBufQueue::cacheChainLength()};
    size_t fromCursor = 0;
    size_t remaining = from.size();
    for (auto size : sizes) {
      if (remaining == 0)
        break;
      if (size > remaining) {
        size = remaining;
      }

      bufQueue.append(
          folly::IOBuf::copyBuffer(from.c_str() + fromCursor, size));

      fromCursor += size;
      remaining -= size;
    }

    if (remaining) {
      bufQueue.append(
          folly::IOBuf::copyBuffer(from.c_str() + fromCursor, remaining));
    }

    CHECK_EQ(bufQueue.chainLength(), from.size());

    auto ret = bufQueue.move();
    int numChainElems = 1;
    auto currentChainElem = ret.get()->next();
    while (currentChainElem != ret.get()) {
      numChainElems++;
      currentChainElem = currentChainElem->next();
    }
    CHECK_GE(numChainElems, sizes.size());

    // verify that the returned buffer has identical data
    auto str = ret->cloneAsValue().moveToFbString().toStdString();
    CHECK_EQ(str.size(), from.size());
    CHECK(str == from);

    return ret;
  };

  auto debugStringMismatch =
      [](auto const& got, auto const& expect, auto const& name) {
        CHECK_EQ(got.size(), expect.size())
            << "Got mismatched size " << name << " string (" << got.size()
            << " vs " << expect.size() << ")";
        CHECK(got == expect) << name << " mismatch between got and expected";
      };

  folly::ScopedEventBaseThread worker;
  auto server = makeServer(std::make_shared<GenericRequestResponseHandler>(
      [&](StringPair const& request) {
        debugStringMismatch(
            request.first, niceLongData, "data (recieved on server)");
        debugStringMismatch(
            request.second, niceLongMetadata, "metadata (recieved on server)");
        LOG(INFO) << "Got large payload on server, sending response back";
        return payload_response(request.first, request.second);
      }));

  auto client = makeClient(worker.getEventBase(), *server->listeningPort());
  auto requester = client->getRequester();

  auto checkForSizePattern = [&](std::vector<size_t> const& meta_sizes,
                                 std::vector<size_t> const& data_sizes) {
    auto to = SingleTestObserver<StringPair>::create();

    // all in one big chunk
    requester
        ->requestResponse(Payload(
            buildIOBufFromString(data_sizes, niceLongData),
            buildIOBufFromString(meta_sizes, niceLongMetadata)))
        ->map(payload_to_stringpair)
        ->subscribe(to);
    to->awaitTerminalEvent();
    to->assertSuccess();

    auto lastVal = to->getOnSuccessValue();

    debugStringMismatch(
        lastVal.first, niceLongData, "data (recieved on client)");
    debugStringMismatch(
        lastVal.second, niceLongMetadata, "metadata (recieved on client)");
  };

  // All in one big chunk
  checkForSizePattern({}, {});

  // Small chunk, big chunk, small chunk
  checkForSizePattern(
      {100, 10 * 1024 * 1024, 100}, {100, 10 * 1024 * 1024, 100});
}
