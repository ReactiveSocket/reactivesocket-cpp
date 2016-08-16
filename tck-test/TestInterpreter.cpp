// Copyright 2004-present Facebook. All Rights Reserved.

#include "TestInterpreter.h"

#include <folly/io/async/EventBase.h>
#include <glog/logging.h>
#include <src/ReactiveSocket.h>
#include <src/mixins/MemoryMixin.h>
#include "TypedCommands.h"

using namespace folly;

namespace reactivesocket {
namespace tck {

TestInterpreter::TestInterpreter(const Test& test, ReactiveSocket& reactiveSocket, EventBase& rsEventBase) : reactiveSocket_(&reactiveSocket),
                                                                                     test_(test),
                                                                                                             rsEventBase_(&rsEventBase){
  DCHECK(!test.empty());
}

void TestInterpreter::run() {
  LOG(INFO) << "executing test " << test_.name() << "...";

  try {
    for (const auto& command : test_.commands()) {
      if (command.name() == "subscribe") {
        auto subscribe = command.as<SubscribeCommand>();
        handleSubscribe(subscribe);
      } else if (command.name() == "request") {
        auto request = command.as<RequestCommand>();
        handleRequest(request);
      } else if (command.name() == "await") {
        auto await = command.as<AwaitCommand>();
        handleAwait(await);
      } else if (command.name() == "cancel") {
        auto cancel = command.as<CancelCommand>();
        handleCancel(cancel);
      } else if (command.name() == "assert") {
        auto assert = command.as<AssertCommand>();
        handleAssert(assert);
      } else {
        LOG(ERROR) << "unknown command " << command.name();
        throw std::runtime_error("unknown command");
      }
    }
    LOG(INFO) << "test " << test_.name() << " done";
  } catch(const std::exception& ex) {
    LOG(ERROR) << "test " << test_.name() << " failed: " << ex.what();
  }
}

static Payload createPayload(const std::string& payloadData, const std::string& /*payloadMetadata*/) {
  //TODO: use metadata
  return folly::IOBuf::copyBuffer(payloadData);
}

void TestInterpreter::handleSubscribe(const SubscribeCommand& command) {
  interactionIdToType_[command.id()] = command.type();
  if (command.isRequestResponseType()) {
    // TODO
    LOG(ERROR) << "request response not implemented";
  } else if (command.isRequestStreamType()) {
    auto& testSubscriber = createTestSubscriber(command.id());
    rsEventBase_->runInEventBaseThreadAndWait([&](){
      reactiveSocket_->requestStream(createPayload(command.payloadData(), command.payloadMetadata()),
                                     testSubscriber);
    });
  } else {
    throw std::runtime_error("unsupported interaction type");
  }
}

void TestInterpreter::handleRequest(const RequestCommand& command) {
  getSubscriber(command.id()).request(command.n());
}

void TestInterpreter::handleCancel(const CancelCommand& command) {
  getSubscriber(command.id()).cancel();
}

void TestInterpreter::handleAwait(const AwaitCommand& command) {
  if (command.isTerminalType()) {
    getSubscriber(command.id()).awaitTerminalEvent();
  } else if (command.isAtLeastType()) {
    getSubscriber(command.id()).awaitAtLeast(command.numElements());
  } else if (command.isNoEventsType()) {
    getSubscriber(command.id()).awaitNoEvents(command.numElements());
  } else {
    throw std::runtime_error("unsupported await type");
  }
}

void TestInterpreter::handleAssert(const AssertCommand& command) {
  if (command.isNoErrorAssert()) {
  } else if (command.isNoErrorAssert()) {
    getSubscriber(command.id()).assertNoErrors();
  } else if (command.isErrorAssert()) {
    getSubscriber(command.id()).assertError();
  } else if (command.isReceivedAssert()) {
    getSubscriber(command.id()).assertValues(command.values());
  } else if (command.isReceivedNAssert()) {
    getSubscriber(command.id()).assertValueCount(command.valueCount());
  } else if (command.isReceivedAtLeastAssert()) {
    getSubscriber(command.id()).assertReceivedAtLeast(command.valueCount());
  } else if (command.isCompletedAssert()) {
    getSubscriber(command.id()).assertCompleted();
  } else if (command.isNotCompletedAssert()) {
    getSubscriber(command.id()).assertNotCompleted();
  } else if (command.isCanceledAssert()) {
    getSubscriber(command.id()).assertCanceled();
  } else {
    throw std::runtime_error("unsupported assert type");
  }
}

TestSubscriber& TestInterpreter::createTestSubscriber(const std::string& id) {
  if (testSubscribers_.find(id) != testSubscribers_.end()) {
    throw std::runtime_error("test subscriber with the same id already exists");
  }

  auto& testSubscriber = createManagedInstance<TestSubscriber>(*rsEventBase_);
  testSubscribers_[id] = &testSubscriber;
  return testSubscriber;
}

TestSubscriber& TestInterpreter::getSubscriber(const std::string& id) {
  auto found = testSubscribers_.find(id);
  if (found == testSubscribers_.end()) {
    throw std::runtime_error("unable to find test subscriber with provided id");
  }

  found->second->waitForInitialization();
  return *found->second;
}

} // tck
} // reactive socket
