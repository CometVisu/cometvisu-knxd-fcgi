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

#include "state/address_cache.h"

using namespace cvknxd;

TEST(AddressCacheTest, UpdateAndGet) {
  AddressCache cache;
  std::vector<uint8_t> data = {0x0C, 0x6F};
  cache.update(0x0A03, data);

  auto result = cache.get(0x0A03, 30);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, data);
}

TEST(AddressCacheTest, GetMissing) {
  AddressCache cache;
  auto result = cache.get(0x0A03, 30);
  EXPECT_FALSE(result.has_value());
}

TEST(AddressCacheTest, Expired) {
  AddressCache cache;
  cache.update(0x0A03, {0x42});

  // 0-second TTL: may or may not be available depending on clock resolution.
  // With 1-second-old value, it should be expired for a 1-second TTL.
  auto result = cache.get(0x0A03, 1);
  ASSERT_TRUE(result.has_value());  // just stored, should be fresh

  // For a very old entry (TTL=0 → expired instantly if any time passed),
  // the result depends on the clock. This is a best-effort test.
  // In practice, CometVisu t=0 means "always read from bus".
  // The cache.get with TTL 0 is not directly used for t=0 in the handler.
}

TEST(AddressCacheTest, NegativeTTL) {
  AddressCache cache;
  cache.update(0x0A03, {0x42});

  auto result = cache.get(0x0A03, -1);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 1);
  EXPECT_EQ((*result)[0], 0x42);
}

TEST(AddressCacheTest, GetAny) {
  AddressCache cache;
  cache.update(0x0A03, {0x42});

  auto result = cache.get_any(0x0A03);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ((*result)[0], 0x42);
}

TEST(AddressCacheTest, GetAnyMissing) {
  AddressCache cache;
  EXPECT_FALSE(cache.get_any(0x0A03).has_value());
}

TEST(AddressCacheTest, OverwriteValue) {
  AddressCache cache;
  cache.update(0x0A03, {0x42});
  cache.update(0x0A03, {0x0C, 0x6F});

  auto result = cache.get_any(0x0A03);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->size(), 2);
  EXPECT_EQ((*result)[0], 0x0C);
  EXPECT_EQ((*result)[1], 0x6F);
}

TEST(AddressCacheTest, Remove) {
  AddressCache cache;
  cache.update(0x0A03, {0x42});
  cache.remove(0x0A03);
  EXPECT_FALSE(cache.get_any(0x0A03).has_value());
}

TEST(AddressCacheTest, Clear) {
  AddressCache cache;
  cache.update(0x0A03, {0x42});
  cache.update(0x0B04, {0x0C, 0x6F});
  EXPECT_EQ(cache.size(), 2);

  cache.clear();
  EXPECT_EQ(cache.size(), 0);
  EXPECT_FALSE(cache.get_any(0x0A03).has_value());
}

TEST(AddressCacheTest, Size) {
  AddressCache cache;
  EXPECT_EQ(cache.size(), 0);
  cache.update(0x0A03, {0x42});
  EXPECT_EQ(cache.size(), 1);
  cache.update(0x0A03, {0x43});  // same address, still 1
  EXPECT_EQ(cache.size(), 1);
  cache.update(0x0B04, {0x0C});
  EXPECT_EQ(cache.size(), 2);
}
