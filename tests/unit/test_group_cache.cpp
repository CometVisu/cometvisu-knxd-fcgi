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
#include "state/group_cache.h"

using namespace cvknxd;

class GroupCacheTest : public ::testing::Test {
protected:
  GroupCache cache_;
};

TEST_F(GroupCacheTest, InitiallyEmpty) {
  EXPECT_EQ(cache_.position(), 0);
  EXPECT_FALSE(cache_.get(0x0A03).has_value());
}

TEST_F(GroupCacheTest, PushAndGet) {
  cache_.push(0x0A03, {0x42});
  EXPECT_EQ(cache_.position(), 1);
  auto val = cache_.get(0x0A03);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ((*val)[0], 0x42);
}

TEST_F(GroupCacheTest, PushOverwrites) {
  cache_.push(0x0A03, {0x42});
  cache_.push(0x0A03, {0x0C, 0x6F});
  EXPECT_EQ(cache_.position(), 2);
  auto val = cache_.get(0x0A03);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ((*val)[0], 0x0C);
  EXPECT_EQ((*val)[1], 0x6F);
}

TEST_F(GroupCacheTest, MultipleAddresses) {
  cache_.push(0x0A03, {0x42});
  cache_.push(0x0B04, {0x01});
  EXPECT_EQ(cache_.position(), 2);
  EXPECT_TRUE(cache_.get(0x0A03).has_value());
  EXPECT_TRUE(cache_.get(0x0B04).has_value());
  EXPECT_FALSE(cache_.get(0x0C05).has_value());
}

TEST_F(GroupCacheTest, GetDeltaSubscribedOnly) {
  cache_.push(0x0A03, {0x42});
  cache_.push(0x0B04, {0x01});
  cache_.push(0x0A03, {0x99});
  std::set<uint16_t> sub = {0x0A03};
  auto delta = cache_.get_delta(0, sub);
  EXPECT_EQ(delta.position, 3);
  ASSERT_EQ(delta.values.size(), 1);
  EXPECT_TRUE(delta.values.contains(0x0A03));
  EXPECT_EQ(delta.values[0x0A03][0], 0x99);
}

TEST_F(GroupCacheTest, GetDeltaEmptyWhenPositionUnchanged) {
  cache_.push(0x0A03, {0x42});  // pos=1
  std::set<uint16_t> sub = {0x0A03};
  auto delta = cache_.get_delta(1, sub);  // since_pos == current pos
  EXPECT_EQ(delta.position, 1);
  EXPECT_TRUE(delta.values.empty());
}

TEST_F(GroupCacheTest, GetDeltaEmptyWhenNotSubscribed) {
  cache_.push(0x0B04, {0x01});
  std::set<uint16_t> sub = {0x0A03};
  auto delta = cache_.get_delta(0, sub);
  EXPECT_EQ(delta.position, 1);
  EXPECT_TRUE(delta.values.empty());
}

TEST_F(GroupCacheTest, Clear) {
  cache_.push(0x0A03, {0x42});
  cache_.clear();
  EXPECT_EQ(cache_.position(), 0);
  EXPECT_FALSE(cache_.get(0x0A03).has_value());
}

TEST_F(GroupCacheTest, PositionMonotonic) {
  for (int i = 0; i < 1000; ++i) cache_.push(0x0A03, {static_cast<uint8_t>(i)});
  EXPECT_EQ(cache_.position(), 1000);  // never wraps in practice (uint32_t)
}

// ================================================================
// Age filtering (t= parameter defines max allowable entry age)
// ================================================================

TEST_F(GroupCacheTest, GetWithAgeLimitReturnsFreshEntry) {
  cache_.push(0x0A03, {0x42});  // just pushed → age ~0
  auto val = cache_.get(0x0A03, 30);  // max 30s old
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ((*val)[0], 0x42);
}

TEST_F(GroupCacheTest, GetWithZeroAgeLimitRejectsAll) {
  cache_.push(0x0A03, {0x42});
  // max_age=0 means no tolerance — entry might be a few microseconds old
  auto val = cache_.get(0x0A03, 0);
  EXPECT_FALSE(val.has_value());
}

TEST_F(GroupCacheTest, GetDeltaFiltersByAge) {
  cache_.push(0x0A03, {0x42});  // fresh
  std::set<uint16_t> sub = {0x0A03};
  auto delta = cache_.get_delta(0, sub, 30);
  EXPECT_EQ(delta.position, 1);
  ASSERT_EQ(delta.values.size(), 1);
}

TEST_F(GroupCacheTest, GetDeltaRejectsStaleEntriesWhenZeroAge) {
  cache_.push(0x0A03, {0x42});
  std::set<uint16_t> sub = {0x0A03};
  auto delta = cache_.get_delta(0, sub, 0);  // max_age=0 rejects all
  EXPECT_EQ(delta.position, 1);
  EXPECT_TRUE(delta.values.empty());  // too old (microseconds)
}

TEST_F(GroupCacheTest, GetNoAgeLimitReturnsEverything) {
  cache_.push(0x0A03, {0x42});
  auto val = cache_.get(0x0A03);  // no age limit
  ASSERT_TRUE(val.has_value());
}
