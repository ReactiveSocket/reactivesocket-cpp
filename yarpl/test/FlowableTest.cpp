// Copyright 2004-present Facebook. All Rights Reserved.

#include <type_traits>
#include <vector>
#include <gtest/gtest.h>
#include "yarpl/Flowable.h"

namespace yarpl {
namespace flowable {
namespace {

void unreachable() {
  EXPECT_TRUE(false);
}

template <typename T>
class CollectingSubscriber : public Subscriber<T> {
 public:
  explicit CollectingSubscriber(int64_t requestCount = 100)
      : requestCount_(requestCount) {}

  void onSubscribe(Reference<Subscription> subscription) override {
    Subscriber<T>::onSubscribe(subscription);
    subscription->request(requestCount_);
  }

  void onNext(T next) override {
    values_.push_back(std::move(next));
  }

  void onComplete() override {
    Subscriber<T>::onComplete();
    complete_ = true;
  }

  void onError(const std::exception_ptr ex) override {
    Subscriber<T>::onError(ex);
    error_ = true;

    try {
      std::rethrow_exception(ex);
    } catch (const std::exception& e) {
      errorMsg_ = e.what();
    }
  }

  std::vector<T>& values() {
    return values_;
  }

  bool complete() const {
    return complete_;
  }

  bool error() const {
    return error_;
  }

  const std::string& errorMsg() const {
    return errorMsg_;
  }

  void cancelSubscription() {
    Subscriber<T>::subscription()->cancel();
  }

 private:
  std::vector<T> values_;
  std::string errorMsg_;
  bool complete_{false};
  bool error_{false};
  int64_t requestCount_;
};

/// Construct a pipeline with a collecting subscriber against the supplied
/// flowable.  Return the items that were sent to the subscriber.  If some
/// exception was sent, the exception is thrown.
template <typename T>
std::vector<T> run(
    Reference<Flowable<T>> flowable,
    uint64_t requestCount = 100) {
  auto collector = make_ref<CollectingSubscriber<T>>(requestCount);
  flowable->subscribe(collector);
  return std::move(collector->values());
}

} // namespace

TEST(FlowableTest, SingleFlowable) {
  auto flowable = Flowables::just(10);
  flowable.reset();
}

TEST(FlowableTest, SingleMovableFlowable) {
  auto value = std::make_unique<int>(123456);

  auto flowable = Flowables::justOnce(std::move(value));
  EXPECT_EQ(std::size_t{1}, flowable->count());

  auto values = run(std::move(flowable));
  EXPECT_EQ(
      values.size(),
      size_t(1));

  EXPECT_EQ(
      *values[0],
      123456);
}

TEST(FlowableTest, JustFlowable) {
  EXPECT_EQ(run(Flowables::just(22)), std::vector<int>{22});
  EXPECT_EQ(
      run(Flowables::justN({12, 34, 56, 98})),
      std::vector<int>({12, 34, 56, 98}));
  EXPECT_EQ(
      run(Flowables::justN({"ab", "pq", "yz"})),
      std::vector<const char*>({"ab", "pq", "yz"}));
}

TEST(FlowableTest, JustIncomplete) {
  auto flowable = Flowables::justN<std::string>({"a", "b", "c"})->take(2);
  EXPECT_EQ(run(std::move(flowable)), std::vector<std::string>({"a", "b"}));

  flowable = Flowables::justN<std::string>({"a", "b", "c"})->take(2)->take(1);
  EXPECT_EQ(run(std::move(flowable)), std::vector<std::string>({"a"}));
  flowable.reset();

  flowable = Flowables::justN<std::string>(
                 {"a", "b", "c", "d", "e", "f", "g", "h", "i"})
                 ->map([](std::string s) {
                   s[0] = ::toupper(s[0]);
                   return s;
                 })
                 ->take(5);

  EXPECT_EQ(
      run(std::move(flowable)),
      std::vector<std::string>({"A", "B", "C", "D", "E"}));
  flowable.reset();
}

TEST(FlowableTest, Range) {
  EXPECT_EQ(
      run(Flowables::range(10, 5)),
      std::vector<int64_t>({10, 11, 12, 13, 14}));
}

TEST(FlowableTest, RangeWithMap) {
  auto flowable = Flowables::range(1, 3)
                      ->map([](int64_t v) { return v * v; })
                      ->map([](int64_t v) { return v * v; })
                      ->map([](int64_t v) { return std::to_string(v); });
  EXPECT_EQ(
      run(std::move(flowable)), std::vector<std::string>({"1", "16", "81"}));
}

TEST(FlowableTest, RangeWithReduceMoreItems) {
  auto flowable = Flowables::range(0, 10)
      ->reduce([](int64_t acc, int64_t v) { return acc + v; });
  EXPECT_EQ(
      run(std::move(flowable)), std::vector<int64_t>({45}));
}

TEST(FlowableTest, RangeWithReduceByMultiplication) {
  auto flowable = Flowables::range(0, 10)
      ->reduce([](int64_t acc, int64_t v) { return acc * v; });
  EXPECT_EQ(
      run(std::move(flowable)), std::vector<int64_t>({0}));

  flowable = Flowables::range(1, 10)
      ->reduce([](int64_t acc, int64_t v) { return acc * v; });
  EXPECT_EQ(
      run(std::move(flowable)), std::vector<int64_t>({2*3*4*5*6*7*8*9*10}));
}

TEST(FlowableTest, RangeWithReduceLessItems) {
  auto flowable = Flowables::range(0, 10)
      ->reduce([](int64_t acc, int64_t v) { return acc + v; });
  // Even if we ask for 1 item only, it will reduce all the items
  EXPECT_EQ(
      run(std::move(flowable), 5), std::vector<int64_t>({45}));
}

TEST(FlowableTest, RangeWithReduceOneItem) {
  auto flowable = Flowables::range(5, 1)
      ->reduce([](int64_t acc, int64_t v) { return acc + v; });
  EXPECT_EQ(
      run(std::move(flowable)), std::vector<int64_t>({5}));
}

TEST(FlowableTest, RangeWithReduceNoItem) {
  auto flowable = Flowables::range(0, 0)
    ->reduce([](int64_t acc, int64_t v) { return acc + v; });
  auto collector = make_ref<CollectingSubscriber<int64_t>>(100);
  flowable->subscribe(collector);
  EXPECT_EQ(collector->error(), false);
  EXPECT_EQ(
    collector->values(), std::vector<int64_t>({}));
}

TEST(FlowableTest, RangeWithFilterAndReduce) {
  auto flowable = Flowables::range(0, 10)
      ->filter([](int64_t v) { return v % 2 != 0; })
      ->reduce([](int64_t acc, int64_t v) { return acc + v; });
  EXPECT_EQ(
      run(std::move(flowable)), std::vector<int64_t>({1+3+5+7+9}));
}

TEST(FlowableTest, RangeWithReduceToBiggerType) {
  auto flowable = Flowables::range(5, 1)
      ->map([](int64_t v){ return (char)(v + 10); })
      ->reduce([](int64_t acc, char v) { return acc + v; });
  EXPECT_EQ(
      run(std::move(flowable)), std::vector<int64_t>({15}));
}

TEST(FlowableTest, StringReduce) {
  auto flowable = Flowables::justN<std::string>(
    {"a", "b", "c", "d", "e", "f", "g", "h", "i"})
    ->reduce([](std::string acc, std::string v) {
      return acc + v;
    });
  EXPECT_EQ(
    run(std::move(flowable)), std::vector<std::string>({"abcdefghi"}));
}

TEST(FlowableTest, RangeWithFilterRequestMoreItems) {
  auto flowable =
      Flowables::range(0, 10)->filter([](int64_t v) { return v % 2 != 0; });
  EXPECT_EQ(run(std::move(flowable)), std::vector<int64_t>({1, 3, 5, 7, 9}));
}

TEST(FlowableTest, RangeWithFilterRequestLessItems) {
  auto flowable =
      Flowables::range(0, 10)->filter([](int64_t v) { return v % 2 != 0; });
  EXPECT_EQ(run(std::move(flowable), 5), std::vector<int64_t>({1, 3, 5, 7, 9}));
}

TEST(FlowableTest, RangeWithFilterAndMap) {
  auto flowable = Flowables::range(0, 10)
      ->filter([](int64_t v) { return v % 2 != 0; })
      ->map([](int64_t v){ return v + 10; });
  EXPECT_EQ(run(std::move(flowable)), std::vector<int64_t>({11, 13, 15, 17, 19}));
}

TEST(FlowableTest, RangeWithMapAndFilter) {
  auto flowable = Flowables::range(0, 10)
      ->map([](int64_t v){ return (char)(v + 10); })
      ->filter([](char v) { return v % 2 != 0; });
  EXPECT_EQ(run(std::move(flowable)), std::vector<char>({11, 13, 15, 17, 19}));
}

TEST(FlowableTest, SimpleTake) {
  EXPECT_EQ(
      run(Flowables::range(0, 100)->take(3)), std::vector<int64_t>({0, 1, 2}));
  EXPECT_EQ(
      run(Flowables::range(10, 5)),
      std::vector<int64_t>({10, 11, 12, 13, 14}));
}

TEST(FlowableTest, SimpleSkip) {
  EXPECT_EQ(run(Flowables::range(0, 10)->skip(8)), std::vector<int64_t>({8, 9}));
}

TEST(FlowableTest, OverflowSkip) {
  EXPECT_EQ(run(Flowables::range(0, 10)->skip(12)), std::vector<int64_t>({}));
}

TEST(FlowableTest, SkipPartial) {
  auto collector = make_ref<CollectingSubscriber<int64_t>>(2);
  auto flowable = Flowables::range(0, 10)->skip(5);
  flowable->subscribe(collector);

  EXPECT_EQ(collector->values(), std::vector<int64_t>({5, 6}));
  collector->cancelSubscription();
}

TEST(FlowableTest, IgnoreElements) {
  auto flowable = Flowables::range(0, 100)
      ->ignoreElements()
      ->map([](int64_t v) { return v * v; });
  EXPECT_EQ(run(flowable), std::vector<int64_t>({}));
}

TEST(FlowableTest, IgnoreElementsPartial) {
  auto collector = make_ref<CollectingSubscriber<int64_t>>(5);
  auto flowable = Flowables::range(0, 10)->ignoreElements();
  flowable->subscribe(collector);

  EXPECT_EQ(
      collector->values(),
      std::vector<int64_t>({}));
  EXPECT_EQ(collector->complete(), false);
  collector->cancelSubscription();

  flowable.reset();
  collector.reset();
}

TEST(FlowableTest, IgnoreElementsError) {
  auto collector = make_ref<CollectingSubscriber<int>>();
  auto flowable = Flowables::error<int>(std::runtime_error("Failure"));
  flowable->subscribe(collector);
  EXPECT_EQ(collector->error(), true);
  EXPECT_EQ(collector->errorMsg(), "Failure");
}

TEST(FlowableTest, FlowableError) {
  auto flowable = Flowables::error<int>(std::runtime_error("something broke!"));
  auto collector = make_ref<CollectingSubscriber<int>>();
  flowable->subscribe(collector);

  EXPECT_EQ(collector->complete(), false);
  EXPECT_EQ(collector->error(), true);
  EXPECT_EQ(collector->errorMsg(), "something broke!");
}

TEST(FlowableTest, FlowableErrorPtr) {
  auto flowable = Flowables::error<int>(
      std::make_exception_ptr(std::runtime_error("something broke!")));
  auto collector = make_ref<CollectingSubscriber<int>>();
  flowable->subscribe(collector);

  EXPECT_EQ(collector->complete(), false);
  EXPECT_EQ(collector->error(), true);
  EXPECT_EQ(collector->errorMsg(), "something broke!");
}

TEST(FlowableTest, FlowableEmpty) {
  auto flowable = Flowables::empty<int>();
  auto collector = make_ref<CollectingSubscriber<int>>();
  flowable->subscribe(collector);

  EXPECT_EQ(collector->complete(), true);
  EXPECT_EQ(collector->error(), false);
}

TEST(FlowableTest, FlowableFromGenerator) {
  auto flowable = Flowables::fromGenerator<std::unique_ptr<int>>(
      [] {return std::unique_ptr<int>();}
  );

  auto collector = make_ref<CollectingSubscriber<std::unique_ptr<int>>>(10);
  flowable->subscribe(collector);

  EXPECT_EQ(collector->complete(), false);
  EXPECT_EQ(collector->error(), false);
  EXPECT_EQ(std::size_t{10}, collector->values().size());

  collector->cancelSubscription();
}

TEST(FlowableTest, FlowableFromGeneratorException) {
  int count = 5;
  auto flowable = Flowables::fromGenerator<std::unique_ptr<int>>(
  [&] {
    while (count--) { return std::unique_ptr<int>(); }
    throw std::runtime_error("error from generator");
  });

  auto collector = make_ref<CollectingSubscriber<std::unique_ptr<int>>>(10);
  flowable->subscribe(collector);

  EXPECT_EQ(collector->complete(), false);
  EXPECT_EQ(collector->error(), true);
  EXPECT_EQ(std::size_t{5}, collector->values().size());
}

TEST(FlowableTest, SubscribersComplete) {
  auto flowable = Flowables::empty<int>();

  bool completed = false;

  auto subscriber = Subscribers::create<int>(
      [](int) { unreachable(); },
      [](std::exception_ptr) { unreachable(); },
      [&] { completed = true; });

  flowable->subscribe(std::move(subscriber));
  EXPECT_TRUE(completed);
}

TEST(FlowableTest, SubscribersError) {
  auto flowable = Flowables::error<int>(std::runtime_error("Whoops"));
  bool errored = false;

  auto subscriber = Subscribers::create<int>(
      [](int) { unreachable(); },
      [&](std::exception_ptr) { errored = true; },
      [] { unreachable(); });

  flowable->subscribe(std::move(subscriber));
  EXPECT_TRUE(errored);
}

TEST(FlowableTest, FlowableCompleteInTheMiddle) {
  auto flowable = Flowable<int>::create(
      [](Subscriber<int> & subscriber, int64_t requested) {
        EXPECT_GT(requested, 1);
        subscriber.onNext(123);
        subscriber.onComplete();
        return std::make_tuple(int64_t(1), true);
      })->map([](int v) { return std::to_string(v); });

  auto collector = make_ref<CollectingSubscriber<std::string>>(10);
  flowable->subscribe(collector);

  EXPECT_EQ(collector->complete(), true);
  EXPECT_EQ(collector->error(), false);
  EXPECT_EQ(std::size_t{1}, collector->values().size());
}

} // flowable
} // yarpl
