#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "pipeline/bounded_queue.hpp"

using sensord::pipeline::BoundedQueue;
using namespace std::chrono_literals;

TEST(BoundedQueue, FifoOrder) {
  BoundedQueue<int> q(4);
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
  for (int i = 0; i < 4; ++i) EXPECT_EQ(q.pop(), i);
}

TEST(BoundedQueue, FullRejectsTryPush) {
  BoundedQueue<int> q(2);
  EXPECT_TRUE(q.try_push(1));
  EXPECT_TRUE(q.try_push(2));
  EXPECT_FALSE(q.try_push(3));
  EXPECT_EQ(q.size(), 2u);
}

TEST(BoundedQueue, EmptyPopTimesOut) {
  BoundedQueue<int> q(2);
  auto t0 = std::chrono::steady_clock::now();
  EXPECT_FALSE(q.pop_for(30ms));
  EXPECT_GE(std::chrono::steady_clock::now() - t0, 25ms);
  EXPECT_FALSE(q.try_pop());
}

TEST(BoundedQueue, PushBlocksUntilSpace) {
  BoundedQueue<int> q(1);
  ASSERT_TRUE(q.try_push(1));
  std::atomic<bool> pushed{false};
  std::jthread producer([&] {
    q.push(2);
    pushed = true;
  });
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(pushed.load()) << "push should block while full";
  EXPECT_EQ(q.pop(), 1);
  producer.join();
  EXPECT_TRUE(pushed.load());
  EXPECT_EQ(q.pop(), 2);
}

TEST(BoundedQueue, DropOldestWhenFull) {
  BoundedQueue<int> q(3);
  for (int i = 0; i < 3; ++i) EXPECT_EQ(q.push_drop_oldest(i), 0u);
  EXPECT_EQ(q.push_drop_oldest(3), 1u);
  EXPECT_EQ(q.push_drop_oldest(4), 1u);
  EXPECT_EQ(q.pop(), 2);
  EXPECT_EQ(q.pop(), 3);
  EXPECT_EQ(q.pop(), 4);
}

TEST(BoundedQueue, CloseWakesBlockedPop) {
  BoundedQueue<int> q(2);
  std::optional<int> got = 99;
  std::jthread consumer([&] { got = q.pop(); });
  std::this_thread::sleep_for(30ms);
  q.close();
  consumer.join();
  EXPECT_FALSE(got.has_value());
}

TEST(BoundedQueue, CloseWakesBlockedPush) {
  BoundedQueue<int> q(1);
  ASSERT_TRUE(q.try_push(1));
  bool ok = true;
  std::jthread producer([&] { ok = q.push(2); });
  std::this_thread::sleep_for(30ms);
  q.close();
  producer.join();
  EXPECT_FALSE(ok);
}

TEST(BoundedQueue, CloseStillDrainsRemainingItems) {
  BoundedQueue<int> q(4);
  q.try_push(1);
  q.try_push(2);
  q.close();
  EXPECT_FALSE(q.try_push(3));
  EXPECT_EQ(q.pop(), 1);
  EXPECT_EQ(q.pop(), 2);
  EXPECT_FALSE(q.pop());
}

TEST(BoundedQueue, StopTokenWakesBlockedPop) {
  BoundedQueue<int> q(2);
  std::optional<int> got = 99;
  std::jthread consumer([&](std::stop_token st) { got = q.pop(st); });
  std::this_thread::sleep_for(30ms);
  consumer.request_stop();
  consumer.join();
  EXPECT_FALSE(got.has_value());
}

TEST(BoundedQueue, StopTokenWakesBlockedPush) {
  BoundedQueue<int> q(1);
  q.try_push(1);
  bool ok = true;
  std::jthread producer([&](std::stop_token st) { ok = q.push(2, st); });
  std::this_thread::sleep_for(30ms);
  producer.request_stop();
  producer.join();
  EXPECT_FALSE(ok);
  EXPECT_EQ(q.size(), 1u);
}

TEST(BoundedQueue, ManyProducersManyConsumers) {
  BoundedQueue<int> q(8);
  constexpr int kProducers = 4, kPerProducer = 5000;
  std::atomic<long long> sum{0};
  std::atomic<int> count{0};
  {
    std::vector<std::jthread> consumers;
    for (int c = 0; c < 3; ++c)
      consumers.emplace_back([&] {
        while (auto v = q.pop()) {
          sum += *v;
          ++count;
        }
      });
    {
      std::vector<std::jthread> producers;
      for (int p = 0; p < kProducers; ++p)
        producers.emplace_back([&, p] {
          for (int i = 0; i < kPerProducer; ++i) q.push(p * kPerProducer + i);
        });
    }  // producers joined
    q.close();
  }  // consumers drain and exit
  const long long n = kProducers * kPerProducer;
  EXPECT_EQ(count.load(), n);
  EXPECT_EQ(sum.load(), n * (n - 1) / 2);
}
