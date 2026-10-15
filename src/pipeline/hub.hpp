#pragma once
// Fan-out: one hub thread takes batches from the shared input queue and hands
// a pointer to each subscriber's own bounded queue. A full subscriber queue
// drops its OLDEST batch and counts the drop, so a slow client can never stall
// the devices or the other clients.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "pipeline/batch.hpp"
#include "pipeline/bounded_queue.hpp"

namespace sensord::pipeline {

class Subscription {
 public:
  Subscription(std::vector<uint32_t> device_ids, std::size_t capacity);

  // Waits up to `timeout` for the next batch.
  template <class Rep, class Period>
  std::optional<BatchPtr> next(std::chrono::duration<Rep, Period> timeout) {
    return q_.pop_for(timeout);
  }
  std::optional<BatchPtr> next(std::stop_token st) { return q_.pop(st); }

  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
  bool closed() const { return q_.closed(); }
  bool wants(uint32_t device_id) const;
  void close() { q_.close(); }

 private:
  friend class Hub;
  void deliver(const BatchPtr& b);

  std::unordered_set<uint32_t> filter_;  // empty = everything; immutable after construction
  BoundedQueue<BatchPtr> q_;
  std::atomic<uint64_t> dropped_{0};
};

class Hub {
 public:
  explicit Hub(BoundedQueue<BatchPtr>& in);
  ~Hub();

  void start();
  void stop();  // joins the hub thread and closes every subscription

  std::shared_ptr<Subscription> subscribe(std::vector<uint32_t> device_ids,
                                          std::size_t capacity = 256);
  void unsubscribe(const std::shared_ptr<Subscription>& sub);
  std::size_t subscriber_count() const;
  uint64_t batches_in() const { return batches_in_.load(); }

 private:
  using SubList = std::vector<std::shared_ptr<Subscription>>;
  void run(std::stop_token st);

  BoundedQueue<BatchPtr>& in_;
  mutable std::mutex mu_;
  // Copy-on-write list: the hub thread grabs the current pointer under the
  // lock and then walks it without holding the lock.
  std::shared_ptr<const SubList> subs_ = std::make_shared<const SubList>();
  bool stopped_ = false;
  std::atomic<uint64_t> batches_in_{0};
  std::jthread thread_;
};

}  // namespace sensord::pipeline
