#pragma once

#include "../Refcounted.h"

namespace yarpl {
namespace flowable {

class Subscription : public virtual Refcounted {
 public:
  virtual ~Subscription() = default;
  virtual void request(int64_t n) = 0;
  virtual void cancel() = 0;

 protected:
  Subscription() : reference_(this) {}

  // Drop the reference we're holding on the subscription (handle).
  void release() {
    reference_.reset();
  }

 private:
  // TODO(lehecka): we can save some bytes by removing reference_ and replace
  // it with addRef/release calls
  //
  // We expect to be heap-allocated; until this subscription finishes
  // (is canceled; completes; error's out), hold a reference so we are
  // not deallocated (by the subscriber).
  Reference<Refcounted> reference_;
};

} // flowable
} // yarpl
