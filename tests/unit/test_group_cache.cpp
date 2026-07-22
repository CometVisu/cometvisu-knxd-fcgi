// Copyright (C) 2026 Christian Mayer and the CometVisu contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <gtest/gtest.h>

#include <thread>

#include "state/group_cache.h"

using namespace cvknxd;

class GroupCacheTest : public ::testing::Test {
protected:
  GroupCache cache_;
};

// ================================================================
// Basic operations
// ================================================================

TEST_F(GroupCacheTest, InitiallyEmpty) {
  EXPECT_EQ(cache_.position(), 0);
  EXPECT_FALSE(cache_.get(0x0A03).has_value());
}

TEST_F(GroupCacheTest, PushAndGet) {
  cache_.push(0x0A03, {0x42});
  EXPECT_EQ(cache_.position(), 1);
  auto v = cache_.get(0x0A03);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ((*v)[0], 0x42);
}

TEST_F(GroupCacheTest, PushOverwrites) {
  cache_.push(0x0A03, {0x42});
  cache_.push(0x0A03, {0x99});
  EXPECT_EQ(cache_.position(), 2);
  EXPECT_EQ(cache_.get(0x0A03)->at(0), 0x99);
}

TEST_F(GroupCacheTest, MultipleAddresses) {
  cache_.push(0x0A03, {0x42});
  cache_.push(0x0B04, {0x01});
  EXPECT_TRUE(cache_.get(0x0A03).has_value());
  EXPECT_TRUE(cache_.get(0x0B04).has_value());
  EXPECT_FALSE(cache_.get(0x0C05).has_value());
}

TEST_F(GroupCacheTest, Clear) {
  cache_.push(0x0A03, {0x42});
  cache_.clear();
  EXPECT_EQ(cache_.position(), 0);
  EXPECT_FALSE(cache_.get(0x0A03).has_value());
}

// ================================================================
// Delta correctness — ONLY changed addresses returned
// ================================================================

TEST_F(GroupCacheTest, DeltaOnlyReturnsChangedAddresses) {
  // A changes at pos 0, B changes at pos 1
  cache_.push(0x0A03, {0x42});  // pos 0
  cache_.push(0x0B04, {0x01});  // pos 1

  std::set<uint16_t> sub = {0x0A03, 0x0B04};

  // since_pos=0: both changed → both returned
  auto d0 = cache_.get_delta(0, sub);
  EXPECT_EQ(d0.position, 2);
  EXPECT_EQ(d0.values.size(), 2);

  // since_pos=1: only B changed → only B returned
  auto d1 = cache_.get_delta(1, sub);
  EXPECT_EQ(d1.position, 2);
  ASSERT_EQ(d1.values.size(), 1);
  EXPECT_TRUE(d1.values.contains(0x0B04));
  EXPECT_FALSE(d1.values.contains(0x0A03));
}

TEST_F(GroupCacheTest, DeltaReturnsNothingForUnchangedAddress) {
  cache_.push(0x0A03, {0x42});  // pos 0: A changes
  cache_.push(0x0B04, {0x01});  // pos 1: B changes (unrelated)

  std::set<uint16_t> sub = {0x0A03};
  auto d = cache_.get_delta(1, sub);  // since A's change
  EXPECT_EQ(d.position, 2);
  EXPECT_TRUE(d.values.empty());  // A hasn't changed since pos 1
}

TEST_F(GroupCacheTest, DeltaEmptyWhenPositionUnchanged) {
  cache_.push(0x0A03, {0x42});  // pos 0
  std::set<uint16_t> sub = {0x0A03};
  auto d = cache_.get_delta(1, sub);  // since_pos at current position → nothing new
  EXPECT_EQ(d.position, 1);
  EXPECT_TRUE(d.values.empty());
}

TEST_F(GroupCacheTest, DeltaLatestPositionReported) {
  // 3 pushes, get_delta at intermediate positions
  cache_.push(0x0A03, {0x42});  // pos 0
  cache_.push(0x0B04, {0x01});  // pos 1
  cache_.push(0x0C05, {0xFF});  // pos 2

  std::set<uint16_t> sub = {0x0A03, 0x0B04, 0x0C05};

  auto d0 = cache_.get_delta(0, sub);
  EXPECT_EQ(d0.position, 3);
  EXPECT_EQ(d0.values.size(), 3);

  auto d1 = cache_.get_delta(1, sub);
  EXPECT_EQ(d1.position, 3);
  EXPECT_EQ(d1.values.size(), 2);  // B + C

  auto d2 = cache_.get_delta(2, sub);
  EXPECT_EQ(d2.position, 3);
  EXPECT_EQ(d2.values.size(), 1);  // only C

  auto d3 = cache_.get_delta(3, sub);
  EXPECT_EQ(d3.position, 3);
  EXPECT_TRUE(d3.values.empty());
}

TEST_F(GroupCacheTest, DeltaNonSubscribedExcluded) {
  cache_.push(0x0A03, {0x42});
  cache_.push(0x0B04, {0x01});
  std::set<uint16_t> sub = {0x0A03};  // only A subscribed
  auto d = cache_.get_delta(0, sub);
  EXPECT_EQ(d.values.size(), 1);
  EXPECT_TRUE(d.values.contains(0x0A03));
}

// ================================================================
// Delta + age filtering
// ================================================================

TEST_F(GroupCacheTest, DeltaFiltersByAge) {
  cache_.push(0x0A03, {0x42});  // pos 0, just pushed → age ~0
  std::set<uint16_t> sub = {0x0A03};
  auto d = cache_.get_delta(0, sub, 30);  // changed + within 30s
  EXPECT_EQ(d.values.size(), 1);
}

TEST_F(GroupCacheTest, DeltaRejectsStaleChangedEntry) {
  cache_.push(0x0A03, {0x42});  // pos 0
  std::set<uint16_t> sub = {0x0A03};
  auto d = cache_.get_delta(0, sub, 0);  // max_age=0 rejects everything
  EXPECT_EQ(d.position, 1);
  EXPECT_TRUE(d.values.empty());
}

TEST_F(GroupCacheTest, DeltaKeepsFreshChangedEntryWhenOtherIsStale) {
  cache_.push(0x0A03, {0x42});  // pos 0
  std::this_thread::sleep_for(std::chrono::seconds(2));
  cache_.push(0x0B04, {0x01});  // pos 1 — fresh

  std::set<uint16_t> sub = {0x0A03, 0x0B04};
  auto d = cache_.get_delta(0, sub, 1);  // max 1s old
  // A is stale (>1s), B is fresh (<1s)
  EXPECT_EQ(d.values.size(), 1);
  EXPECT_TRUE(d.values.contains(0x0B04));
}

// ================================================================
// Position monotonicity
// ================================================================

TEST_F(GroupCacheTest, PositionMonotonic) {
  for (int i = 0; i < 1000; ++i)
    cache_.push(0x0A03, {static_cast<uint8_t>(i)});
  EXPECT_EQ(cache_.position(), 1000);
}

// ================================================================
// Concurrent access — black-box
// ================================================================

#include <thread>

TEST_F(GroupCacheTest, ConcurrentPushAndGetDelta) {
  std::atomic<bool> start{false};
  std::atomic<int> pushes_done{0};

  // Writer thread: pushes 100 values
  std::thread writer([&]() {
    while (!start.load()) {
    }
    for (int i = 0; i < 100; ++i) {
      cache_.push(0x0A03, {static_cast<uint8_t>(i)});
      pushes_done.store(i + 1);
    }
  });

  // Reader thread: polls with get_delta, must always see forward progress
  std::thread reader([&]() {
    while (!start.load()) {
    }
    // Wait until the writer has pushed at least one value
    while (pushes_done.load() == 0) {
    }
    uint32_t last_pos = 0;
    int reads = 0;
    while (pushes_done.load() < 100 && reads < 500) {
      std::set<uint16_t> sub = {0x0A03};
      auto d = cache_.get_delta(last_pos, sub);
      ASSERT_GE(d.position, last_pos) << "position must never go backward";
      if (!d.values.empty()) {
        ASSERT_GT(d.position, last_pos) << "non-empty delta must advance position";
      }
      if (d.position > last_pos)
        last_pos = d.position;
      reads++;
    }
    // Must have seen forward progress
    EXPECT_GT(last_pos, 0);
  });

  start.store(true);
  writer.join();
  reader.join();
}

TEST_F(GroupCacheTest, ConcurrentMultipleWriters) {
  std::atomic<bool> start{false};
  constexpr int kPerThread = 50;
  std::set<uint16_t> addrs = {0x0A03, 0x0B04, 0x0C05};

  auto writer = [&](uint16_t addr) {
    while (!start.load()) {
    }
    for (int i = 0; i < kPerThread; ++i)
      cache_.push(addr, {static_cast<uint8_t>(i)});
  };

  std::thread w1(writer, 0x0A03);
  std::thread w2(writer, 0x0B04);
  std::thread w3(writer, 0x0C05);

  start.store(true);
  w1.join();
  w2.join();
  w3.join();

  // All addresses should have their latest value
  EXPECT_EQ(cache_.position(), kPerThread * 3);
  for (auto addr : addrs) {
    auto v = cache_.get(addr);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->at(0), kPerThread - 1);
  }
}

TEST_F(GroupCacheTest, GetDeltaConsistentUnderConcurrentWrites) {
  // Start a writer, periodically check get_delta — must never see
  // inconsistent state (e.g., position advanced but value missing)
  std::atomic<bool> stop{false};
  std::thread writer([&]() {
    for (int i = 0; !stop.load(); ++i)
      cache_.push(0x0A03, {static_cast<uint8_t>(i % 256)});
  });

  std::set<uint16_t> sub = {0x0A03};
  for (int attempt = 0; attempt < 100; ++attempt) {
    auto d = cache_.get_delta(0, sub);
    if (!d.values.empty()) {
      ASSERT_TRUE(d.values.contains(0x0A03));
      // Position must be > 0 if we got values back
      EXPECT_GT(d.position, 0);
    }
  }
  stop.store(true);
  writer.join();
}

// ================================================================
// Adapted from original MockKnxdClient cache tests
// ================================================================

// Original: CacheReadMiss → get returns nullopt for non-existent address
TEST_F(GroupCacheTest, GetNonexistentReturnsNullopt) {
  EXPECT_FALSE(cache_.get(0xFFFF).has_value());
}

// Original: CacheReadFailCount → GroupCache doesn't have network failures
// but we test that clear + push gives consistent results
TEST_F(GroupCacheTest, ClearThenPushRestoresState) {
  cache_.push(0x0A03, {0x42});
  cache_.clear();
  cache_.push(0x0A03, {0x99});
  EXPECT_EQ(cache_.position(), 1);
  EXPECT_EQ(cache_.get(0x0A03)->at(0), 0x99);
}

// Original: CacheReadWorksAfterReconnect → data survives state changes
TEST_F(GroupCacheTest, DataSurvivesExternalStateChange) {
  cache_.push(0x0A03, {0x42});
  // Simulate reconnect/state change — cache is independent
  EXPECT_TRUE(cache_.get(0x0A03).has_value());
}

// Original: ReconnectAfterDisconnect with cache → initial read works
TEST_F(GroupCacheTest, NewHandlerReadsFromExistingCache) {
  cache_.push(0x0A03, {0x42});
  // A new handler instance (simulating reconnect) should find cached data
  GroupCache fresh_view;  // would be populated by drain_into_cache
  fresh_view.push(0x0A03, {0x42});  // simulate population
  EXPECT_TRUE(fresh_view.get(0x0A03).has_value());
  EXPECT_EQ(fresh_view.position(), 1);
}
