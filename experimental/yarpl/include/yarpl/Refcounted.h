#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace yarpl {

/// Base of refcounted objects.  The intention is the same as that
/// of boost::intrusive_ptr<>, except that we have virtual methods
/// anyway, and want to avoid argument-dependent lookup.
///
/// NOTE: only derive using "virtual public" inheritance.
class Refcounted {
 public:
#if !defined(NDEBUG)
  Refcounted();
  virtual ~Refcounted();

  // Return the number of live refcounted objects.  For testing.
  static std::size_t objects();

  // Return the current count.  For testing.
  std::size_t count() const {
    return refcount_;
  }
#else /* NDEBUG */
  virtual ~Refcounted() = default;
#endif /* NDEBUG */

 private:
  template <typename T>
  friend class Reference;

  void incRef() {
    refcount_.fetch_add(1, std::memory_order_relaxed);
  }

  void decRef() {
    if (refcount_.fetch_sub(1, std::memory_order_relaxed) == 1) {
      std::atomic_thread_fence(std::memory_order_acquire);
      delete this;
    }
  }

  mutable std::atomic_size_t refcount_{0};

#if !defined(NDEBUG)
  static std::atomic_size_t objects_;
#endif /* NDEBUG */
};

/// RAII-enabling smart pointer for refcounted objects.  Each reference
/// constructed against a target refcounted object increases its count
/// by 1 during its lifetime.
template <typename T>
class Reference {
 public:
  template <typename U>
  friend class Reference;

  Reference() {}
  explicit Reference(std::nullptr_t) {}

  explicit Reference(T* pointer) : pointer_(pointer) {
    inc();
  }

  ~Reference() {
    dec();
  }

  //////////////////////////////////////////////////////////////////////////////

  Reference(const Reference& other): pointer_(other.pointer_) {
    inc();
  }

  Reference(Reference&& other): pointer_(other.pointer_) {
    other.pointer_ = nullptr;
  }

  template <typename U>
  Reference(const Reference<U>& other) : pointer_(other.pointer_) {
    inc();
  }

  template <typename U>
  Reference(Reference<U>&& other) : pointer_(other.pointer_) {
    other.pointer_ = nullptr;
  }

  //////////////////////////////////////////////////////////////////////////////

  Reference& operator=(const Reference& other) {
    new (this) Reference(other);
    return *this;
  }

  Reference& operator=(Reference&& other) {
    new (this) Reference(std::move(other));
    return *this;
  }

  template <typename U>
  Reference& operator=(const Reference<U>& other) {
    new (this) Reference<T>(other);
    return *this;
  }

  template <typename U>
  Reference& operator=(Reference<U>&& other) {
    new (this) Reference<T>(std::move(other));
    return *this;
  }

  //////////////////////////////////////////////////////////////////////////////

  T* get() const {
    return pointer_;
  }

  T& operator*() const {
    return *pointer_;
  }

  T* operator->() const {
    return pointer_;
  }

  void reset() {
    Reference{}.swap(*this);
  }

  explicit operator bool() const {
    return pointer_;
  }

 private:
  void inc() {
    if (pointer_) {
      pointer_->incRef();
    }
  }

  void dec() {
    if (pointer_) {
      pointer_->decRef();
    }
  }

  void swap(Reference<T>& other) {
    std::swap(pointer_, other.pointer_);
  }

  T* pointer_{nullptr};
};

} // yarpl
