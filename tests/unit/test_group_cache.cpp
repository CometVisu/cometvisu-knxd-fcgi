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
  EXPECT_EQ(cache_.size(), 0);
  EXPECT_FALSE(cache_.contains(0x0A03));
}

TEST_F(GroupCacheTest, UpdateAndGet) {
  cache_.update(0x0A03, {0x42});
  EXPECT_EQ(cache_.size(), 1);
  EXPECT_TRUE(cache_.contains(0x0A03));

  auto val = cache_.get(0x0A03);
  ASSERT_TRUE(val.has_value());
  ASSERT_EQ(val->size(), 1);
  EXPECT_EQ((*val)[0], 0x42);
}

TEST_F(GroupCacheTest, GetNonexistentReturnsNullopt) {
  auto val = cache_.get(0x0A03);
  EXPECT_FALSE(val.has_value());
}

TEST_F(GroupCacheTest, UpdateOverwrites) {
  cache_.update(0x0A03, {0x42});
  cache_.update(0x0A03, {0x0C, 0x6F});

  auto val = cache_.get(0x0A03);
  ASSERT_TRUE(val.has_value());
  ASSERT_EQ(val->size(), 2);
  EXPECT_EQ((*val)[0], 0x0C);
  EXPECT_EQ((*val)[1], 0x6F);
  EXPECT_EQ(cache_.size(), 1);  // still one entry
}

TEST_F(GroupCacheTest, MultipleAddresses) {
  cache_.update(0x0A03, {0x42});
  cache_.update(0x0B04, {0x0C, 0x6F});
  cache_.update(0x0C05, {0x01});

  EXPECT_EQ(cache_.size(), 3);
  EXPECT_TRUE(cache_.contains(0x0A03));
  EXPECT_TRUE(cache_.contains(0x0B04));
  EXPECT_TRUE(cache_.contains(0x0C05));
  EXPECT_FALSE(cache_.contains(0x0D06));

  // Check values independently
  auto v1 = cache_.get(0x0A03);
  ASSERT_TRUE(v1.has_value());
  ASSERT_EQ(v1->size(), 1);
  EXPECT_EQ((*v1)[0], 0x42);

  auto v2 = cache_.get(0x0B04);
  ASSERT_TRUE(v2.has_value());
  ASSERT_EQ(v2->size(), 2);
  EXPECT_EQ((*v2)[0], 0x0C);
  EXPECT_EQ((*v2)[1], 0x6F);
}

TEST_F(GroupCacheTest, ClearRemovesAll) {
  cache_.update(0x0A03, {0x42});
  cache_.update(0x0B04, {0x01});
  EXPECT_EQ(cache_.size(), 2);

  cache_.clear();
  EXPECT_EQ(cache_.size(), 0);
  EXPECT_FALSE(cache_.contains(0x0A03));
  EXPECT_FALSE(cache_.contains(0x0B04));
}

TEST_F(GroupCacheTest, CopyPreservesValues) {
  cache_.update(0x0A03, {0x42});
  auto val = cache_.get(0x0A03);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ((*val)[0], 0x42);

  // Modify the original vector — should not affect cache
  (*val)[0] = 0x99;

  auto val2 = cache_.get(0x0A03);
  ASSERT_TRUE(val2.has_value());
  EXPECT_EQ((*val2)[0], 0x42);  // original value preserved
}
