#pragma once
// Bounded MPMC queue built on a mutex and two condition variables.
//
//  - push()            blocks while full (backpressure), wakes on close/stop
//  - push_drop_oldest  never blocks; evicts the oldest item when full
//  - pop()             blocks while empty, wakes on close/stop
//  - close()           wakes everyone; pops still drain what is left
//
// condition_variable_any is used so waits can be interrupted by a
// std::stop_token, which is what makes jthread shutdown clean.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace sensord::pipeline {

template <class T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : cap_(capacity == 0 ? 1 : capacity) {}

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  // Blocks while full. Returns false if the queue closed or stop was requested.
  bool push(T v, std::stop_token st = {}) {
    std::unique_lock lk(mu_);
    if (!not_full_.wait(lk, st, [&] { return closed_ || q_.size() < cap_; })) return false;
    if (closed_) return false;
    q_.push_back(std::move(v));
    lk.unlock();
    not_empty_.notify_one();
    return true;
  }

  bool try_push(T v) {
    std::unique_lock lk(mu_);
    if (closed_ || q_.size() >= cap_) return false;
    q_.push_back(std::move(v));
    lk.unlock();
    not_empty_.notify_one();
    return true;
  }

  // Never blocks. Returns how many items were evicted (0 or 1), or nullopt if closed.
  std::optional<std::size_t> push_drop_oldest(T v) {
    std::unique_lock lk(mu_);
    if (closed_) return std::nullopt;
    std::size_t dropped = 0;
    if (q_.size() >= cap_) {
      q_.pop_front();
      dropped = 1;
    }
    q_.push_back(std::move(v));
    lk.unlock();
    not_empty_.notify_one();
    return dropped;
  }

  // Blocks while empty. nullopt when closed-and-drained or stop requested.
  std::optional<T> pop(std::stop_token st = {}) {
    std::unique_lock lk(mu_);
    if (!not_empty_.wait(lk, st, [&] { return closed_ || !q_.empty(); })) return std::nullopt;
    return take(lk);
  }

  template <class Rep, class Period>
  std::optional<T> pop_for(std::chrono::duration<Rep, Period> timeout) {
    std::unique_lock lk(mu_);
    if (!not_empty_.wait_for(lk, timeout, [&] { return closed_ || !q_.empty(); }))
      return std::nullopt;
    return take(lk);
  }

  std::optional<T> try_pop() {
    std::unique_lock lk(mu_);
    if (q_.empty()) return std::nullopt;
    return take(lk);
  }

  void close() {
    {
      std::lock_guard lk(mu_);
      closed_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  bool closed() const {
    std::lock_guard lk(mu_);
    return closed_;
  }

  std::size_t size() const {
    std::lock_guard lk(mu_);
    return q_.size();
  }

  std::size_t capacity() const { return cap_; }

 private:
  std::optional<T> take(std::unique_lock<std::mutex>& lk) {
    if (q_.empty()) return std::nullopt;  // closed and drained
    T v = std::move(q_.front());
    q_.pop_front();
    lk.unlock();
    not_full_.notify_one();
    return v;
  }

  const std::size_t cap_;
  mutable std::mutex mu_;
  std::condition_variable_any not_empty_;
  std::condition_variable_any not_full_;
  std::deque<T> q_;
  bool closed_ = false;
};

}  // namespace sensord::pipeline
