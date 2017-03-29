// Copyright 2004-present Facebook. All Rights Reserved.

#include <benchmark/benchmark.h>
#include <thread>

#include <folly/io/async/ScopedEventBaseThread.h>
#include <iostream>
#include <experimental/rsocket/transports/TcpConnectionAcceptor.h>
#include <src/NullRequestHandler.h>
#include <src/SubscriptionBase.h>
#include "rsocket/RSocket.h"
#include "rsocket/transports/TcpConnectionFactory.h"

using namespace ::reactivesocket;
using namespace ::folly;
using namespace ::rsocket;

#define MESSAGE_LENGTH (32)

DEFINE_string(host, "localhost", "host to connect to");
DEFINE_int32(port, 9898, "host:port to connect to");

class BM_Subscription : public SubscriptionBase {
public:
    explicit BM_Subscription(
        std::shared_ptr<Subscriber<Payload>> subscriber,
        std::string name,
        size_t length)
        : ExecutorBase(defaultExecutor()),
        subscriber_(std::move(subscriber)),
        name_(std::move(name)),
        data_(length, 'a'),
        cancelled_(false)
    {
    }

private:
    void requestImpl(size_t n) noexcept override
    {
        LOG(INFO) << "requested=" << n;

        if (cancelled_) {
            LOG(INFO) << "emission stopped by cancellation";
            return;
        }

        subscriber_->onNext(Payload(data_));
        subscriber_->onComplete();
    }

    void cancelImpl() noexcept override
    {
        LOG(INFO) << "cancellation received";
        cancelled_ = true;
    }

    std::shared_ptr<Subscriber<Payload>> subscriber_;
    std::string name_;
    std::string data_;
    std::atomic_bool cancelled_;
};

class BM_RequestHandler : public DefaultRequestHandler
{
public:
    void handleRequestResponse(
        Payload request, StreamId streamId, const std::shared_ptr<Subscriber<Payload>> &response) noexcept override
    {
        LOG(INFO) << "BM_RequestHandler.handleRequestResponse " << request;

        const char* p = reinterpret_cast<const char*>(request.data->data());
        auto requestString = std::string(p, request.data->length());

        response->onSubscribe(
            std::make_shared<BM_Subscription>(response, requestString, MESSAGE_LENGTH));
    }

    std::shared_ptr<StreamState> handleSetupPayload(
        ReactiveSocket &socket, ConnectionSetupPayload request) noexcept override
    {
        LOG(INFO) << "BM_RequestHandler.handleSetupPayload " << request;
        return nullptr;
    }
};

class BM_Subscriber
    : public reactivesocket::Subscriber<reactivesocket::Payload> {
public:
    ~BM_Subscriber()
    {
        LOG(INFO) << "BM_Subscriber destroy " << this;
    }

    BM_Subscriber()
        : initialRequest_(8),
        thresholdForRequest_(initialRequest_ * 0.75)
    {
        LOG(INFO) << "BM_Subscriber " << this << " created with => "
            << "  Initial Request: " << initialRequest_
            << "  Threshold for re-request: " << thresholdForRequest_;
    }

    void onSubscribe(std::shared_ptr<reactivesocket::Subscription> subscription) noexcept override
    {
        LOG(INFO) << "BM_Subscriber " << this << " onSubscribe";
        subscription_ = std::move(subscription);
        requested_ = initialRequest_;
        subscription_->request(initialRequest_);
    }

    void onNext(reactivesocket::Payload element) noexcept override
    {
        LOG(INFO) << "BM_Subscriber " << this
            << " onNext as string: " << element.moveDataToString();

        if (--requested_ == thresholdForRequest_) {
            int toRequest = (initialRequest_ - thresholdForRequest_);
            LOG(INFO) << "BM_Subscriber " << this << " requesting " << toRequest
                << " more items";
            requested_ += toRequest;
            subscription_->request(toRequest);
        }
    }

    void onComplete() noexcept override
    {
        LOG(INFO) << "BM_Subscriber " << this << " onComplete";
        terminated_ = true;
        completed_ = true;
        terminalEventCV_.notify_all();
    }

    void onError(folly::exception_wrapper ex) noexcept override
    {
        LOG(INFO) << "BM_Subscriber " << this << " onError " << ex.what();
        terminated_ = true;
        terminalEventCV_.notify_all();
    }

    void awaitTerminalEvent()
    {
        LOG(INFO) << "BM_Subscriber " << this << " block thread";
        // now block this thread
        std::unique_lock<std::mutex> lk(m_);
        // if shutdown gets implemented this would then be released by it
        terminalEventCV_.wait(lk, [this] { return terminated_; });
        LOG(INFO) << "BM_Subscriber " << this << " unblocked";
    }

    bool completed()
    {
        return completed_;
    }

private:
    int initialRequest_;
    int thresholdForRequest_;
    int requested_;
    std::shared_ptr<reactivesocket::Subscription> subscription_;
    bool terminated_{false};
    std::mutex m_;
    std::condition_variable terminalEventCV_;
    std::atomic_bool completed_{false};
};

class BM_RsFixture : public benchmark::Fixture
{
public:
    BM_RsFixture() :
        host_(FLAGS_host),
        port_(FLAGS_port),
        serverRs_(RSocket::createServer(TcpConnectionAcceptor::create(port_))),
        handler_(std::make_shared<BM_RequestHandler>())
    {
        FLAGS_v = 0;
        FLAGS_minloglevel = 6;
        serverRs_->start([this](auto r) { return handler_; });
    }

    virtual ~BM_RsFixture()
    {
    }

    virtual void SetUp(benchmark::State &state)
    {
    }

    virtual void TearDown(benchmark::State &state)
    {
    }

    std::string host_;
    int port_;
    std::unique_ptr<RSocketServer> serverRs_;
    std::shared_ptr<BM_RequestHandler> handler_;
};

BENCHMARK_DEFINE_F(BM_RsFixture, BM_RequestResponse_Throughput)(benchmark::State &state)
{
    auto clientRs = RSocket::createClient(TcpConnectionFactory::create(host_, port_));
    int reqs = 0;
    int numSubscribers = state.range(0);
    int mask = numSubscribers - 1;

    std::shared_ptr<BM_Subscriber> subs[numSubscribers];

    auto rs = clientRs->connect().get();

    while (state.KeepRunning())
    {
        int index = reqs & mask;

        if (nullptr != subs[index])
        {
            while (!subs[index]->completed())
            {
                std::this_thread::yield();
            }

            subs[index].reset();
        }

        subs[index] = std::make_shared<BM_Subscriber>();
        rs->requestResponse(Payload("BM_RequestResponse"), subs[index]);
        reqs++;
    }

    for (int i = 0; i < numSubscribers; i++)
    {
        if (subs[i])
        {
            subs[i]->awaitTerminalEvent();
        }
    }

    char label[256];

    std::snprintf(label, sizeof(label), "Max Requests: %d, Message Length: %d", numSubscribers, MESSAGE_LENGTH);
    state.SetLabel(label);

    state.SetItemsProcessed(reqs);
}

BENCHMARK_REGISTER_F(BM_RsFixture, BM_RequestResponse_Throughput)->Arg(1)->Arg(2)->Arg(8)->Arg(16)->Arg(32)->Arg(64);

BENCHMARK_MAIN()
