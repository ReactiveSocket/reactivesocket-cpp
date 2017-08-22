// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include <folly/Baton.h>

#include "yarpl/Refcounted.h"
#include "yarpl/single/SingleObserver.h"
#include "yarpl/single/SingleObservers.h"
#include "yarpl/single/SingleSubscription.h"
#include "yarpl/utils/type_traits.h"

namespace yarpl {
namespace single {

template <typename T>
class Single : public virtual Refcounted {
 public:
  virtual void subscribe(Reference<SingleObserver<T>>) = 0;

  /**
   * Subscribe overload that accepts lambdas.
   */
  template <
      typename Success,
      typename = typename std::enable_if<
          std::is_callable<Success(T), void>::value>::type>
  void subscribe(Success next) {
    subscribe(SingleObservers::create<T>(std::move(next)));
  }

  /**
   * Subscribe overload that accepts lambdas.
   */
  template <
      typename Success,
      typename Error,
      typename = typename std::enable_if<
          std::is_callable<Success(T), void>::value &&
          std::is_callable<Error(folly::exception_wrapper), void>::value>::type>
  void subscribe(Success next, Error error) {
    subscribe(SingleObservers::create<T>(
        std::move(next), std::move(error)));
  }

  /**
   * Blocking subscribe that accepts lambdas.
   *
   * This blocks the current thread waiting on the response.
   */
  template <
      typename Success,
      typename = typename std::enable_if<
          std::is_callable<Success(T), void>::value>::type>
  void subscribeBlocking(Success next) {
    auto waiting_ = std::make_shared<folly::Baton<>>();
    subscribe(SingleObservers::create<T>(
        [ next = std::move(next), waiting_ ](T t) {
          next(std::move(t));
          waiting_->post();
        }));
    // TODO get errors and throw if one is received
    waiting_->wait();
  }

  template <
      typename OnSubscribe,
      typename = typename std::enable_if<std::is_callable<
          OnSubscribe(Reference<SingleObserver<T>>),
          void>::value>::type>
  static auto create(OnSubscribe);

  template <typename Function>
  auto map(Function function);
};

template <>
class Single<void> : public virtual Refcounted {
 public:
  virtual void subscribe(Reference<SingleObserver<void>>) = 0;

  /**
   * Subscribe overload taking lambda for onSuccess that is called upon writing
   * to the network.
   */
  template <
      typename Success,
      typename = typename std::enable_if<
          std::is_callable<Success(), void>::value>::type>
  void subscribe(Success s) {
    class SuccessSingleObserver : public SingleObserver<void> {
     public:
      SuccessSingleObserver(Success s) : success_{std::move(s)} {}

      void onSubscribe(Reference<SingleSubscription> subscription) override {
        SingleObserver<void>::onSubscribe(std::move(subscription));
      }

      void onSuccess() override {
        success_();
        SingleObserver<void>::onSuccess();
      }

      // No further calls to the subscription after this method is invoked.
      void onError(folly::exception_wrapper ex) override {
        SingleObserver<void>::onError(std::move(ex));
      }

     private:
      Success success_;
    };

    subscribe(make_ref<SuccessSingleObserver>(std::move(s)));
  }

  template <
      typename OnSubscribe,
      typename = typename std::enable_if<std::is_callable<
          OnSubscribe(Reference<SingleObserver<void>>),
          void>::value>::type>
  static auto create(OnSubscribe);
};

} // single
} // yarpl

#include "yarpl/single/SingleOperator.h"

namespace yarpl {
namespace single {

template <typename T>
template <typename OnSubscribe, typename>
auto Single<T>::create(OnSubscribe function) {
  return Reference<Single<T>>(new FromPublisherOperator<T, OnSubscribe>(
      std::move(function)));
}

template <typename OnSubscribe, typename>
auto Single<void>::create(OnSubscribe function) {
  return Reference<Single<void>>(
      new SingleVoidFromPublisherOperator<OnSubscribe>(
          std::forward<OnSubscribe>(function)));
}

template <typename T>
template <typename Function>
auto Single<T>::map(Function function) {
  using D = typename std::result_of<Function(T)>::type;
  return Reference<Single<D>>(new MapOperator<T, D, Function>(
      Reference<Single<T>>(this), std::move(function)));
}

} // single
} // yarpl
