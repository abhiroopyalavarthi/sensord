#include "pipeline/hub.hpp"

#include <algorithm>

namespace sensord::pipeline {

Subscription::Subscription(std::vector<uint32_t> device_ids, std::size_t capacity)
    : filter_(device_ids.begin(), device_ids.end()), q_(capacity) {}

bool Subscription::wants(uint32_t device_id) const {
  return filter_.empty() || filter_.count(device_id) > 0;
}

void Subscription::deliver(const BatchPtr& b) {
  if (auto dropped = q_.push_drop_oldest(b); dropped && *dropped > 0)
    dropped_.fetch_add(*dropped, std::memory_order_relaxed);
}

Hub::Hub(BoundedQueue<BatchPtr>& in) : in_(in) {}

Hub::~Hub() { stop(); }

void Hub::start() {
  thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

void Hub::stop() {
  if (thread_.joinable()) {
    thread_.request_stop();
    thread_.join();
  }
  std::shared_ptr<const SubList> subs;
  {
    std::lock_guard lk(mu_);
    stopped_ = true;
    subs = subs_;
    subs_ = std::make_shared<const SubList>();
  }
  for (auto& s : *subs) s->close();
}

std::shared_ptr<Subscription> Hub::subscribe(std::vector<uint32_t> device_ids,
                                             std::size_t capacity) {
  auto sub = std::make_shared<Subscription>(std::move(device_ids), capacity);
  std::lock_guard lk(mu_);
  if (stopped_) {
    sub->close();
    return sub;
  }
  auto next = std::make_shared<SubList>(*subs_);
  next->push_back(sub);
  subs_ = std::move(next);
  return sub;
}

void Hub::unsubscribe(const std::shared_ptr<Subscription>& sub) {
  {
    std::lock_guard lk(mu_);
    auto next = std::make_shared<SubList>(*subs_);
    next->erase(std::remove(next->begin(), next->end(), sub), next->end());
    subs_ = std::move(next);
  }
  sub->close();
}

std::size_t Hub::subscriber_count() const {
  std::lock_guard lk(mu_);
  return subs_->size();
}

void Hub::run(std::stop_token st) {
  while (auto b = in_.pop(st)) {
    batches_in_.fetch_add(1, std::memory_order_relaxed);
    std::shared_ptr<const SubList> subs;
    {
      std::lock_guard lk(mu_);
      subs = subs_;
    }
    for (const auto& s : *subs)
      if (s->wants((*b)->device_id)) s->deliver(*b);
  }
}

}  // namespace sensord::pipeline
