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

#include "util/query_string.h"

using namespace cvknxd;

TEST(QueryStringTest, EmptyInput) {
  QueryString qs("");
  EXPECT_FALSE(qs.has("anything"));
  EXPECT_EQ(qs.size(), 0);
}

TEST(QueryStringTest, SingleParameter) {
  QueryString qs("a=1/2/3");
  EXPECT_TRUE(qs.has("a"));
  auto val = qs.get("a");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, "1/2/3");
}

TEST(QueryStringTest, MultipleParameters) {
  QueryString qs("a=1/2/3&t=5&a=4/5/6");
  EXPECT_TRUE(qs.has("a"));
  EXPECT_TRUE(qs.has("t"));

  auto first = qs.get("a");
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, "1/2/3");

  auto all = qs.get_all("a");
  ASSERT_EQ(all.size(), 2);
  EXPECT_EQ(all[0], "1/2/3");
  EXPECT_EQ(all[1], "4/5/6");

  auto t_val = qs.get("t");
  ASSERT_TRUE(t_val.has_value());
  EXPECT_EQ(*t_val, "5");
}

TEST(QueryStringTest, MissingParameter) {
  QueryString qs("a=foo");
  EXPECT_FALSE(qs.has("b"));
  auto val = qs.get("b");
  EXPECT_FALSE(val.has_value());
  EXPECT_TRUE(qs.get_all("b").empty());
}

TEST(QueryStringTest, CometVisuLoginParams) {
  QueryString qs("u=admin&p=secret&d=visu-living");
  EXPECT_EQ(*qs.get("u"), "admin");
  EXPECT_EQ(*qs.get("p"), "secret");
  EXPECT_EQ(*qs.get("d"), "visu-living");
}

TEST(QueryStringTest, CometVisuReadParams) {
  QueryString qs("s=abc123&a=KNX:1/2/3&a=KNX:4/5/6&t=0&i=42");
  EXPECT_EQ(*qs.get("s"), "abc123");
  auto addrs = qs.get_all("a");
  ASSERT_EQ(addrs.size(), 2);
  EXPECT_EQ(addrs[0], "KNX:1/2/3");
  EXPECT_EQ(addrs[1], "KNX:4/5/6");
  EXPECT_EQ(*qs.get("t"), "0");
  EXPECT_EQ(*qs.get("i"), "42");
}

TEST(QueryStringTest, URLDecoding) {
  QueryString qs("key=hello%20world&path=%2Ftmp%2Ftest");
  EXPECT_EQ(*qs.get("key"), "hello world");
  EXPECT_EQ(*qs.get("path"), "/tmp/test");
}

TEST(QueryStringTest, ValueWithEquals) {
  QueryString qs("a=foo=bar");
  auto val = qs.get("a");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, "foo=bar");
}

TEST(QueryStringTest, ParameterCountLimit) {
  // Build a query string with many unique parameters
  std::string q;
  for (int i = 0; i < 150; ++i) {
    if (i > 0) q += '&';
    q += "p" + std::to_string(i) + "=v" + std::to_string(i);
  }
  QueryString qs(q);
  // Should not exceed the limit — stops at kMaxQueryParams (100)
  EXPECT_LE(qs.size(), 100);
  // First parameter should still be present
  EXPECT_TRUE(qs.has("p0"));
  EXPECT_EQ(*qs.get("p0"), "v0");
  // Parameter beyond limit should be ignored
  EXPECT_FALSE(qs.has("p140"));
}

TEST(QueryStringTest, ValueCountPerKeyLimit) {
  // Many values for a single key
  std::string q;
  for (int i = 0; i < 150; ++i) {
    if (i > 0) q += '&';
    q += "a=v" + std::to_string(i);
  }
  QueryString qs(q);
  auto all = qs.get_all("a");
  // Should cap at kMaxValuesPerKey (128)
  EXPECT_LE(all.size(), 128);
  EXPECT_EQ(all[0], "v0");
}

TEST(QueryStringTest, TotalParamCountLimit) {
  // Many identical params — tests the total pair count limit
  std::string q;
  for (int i = 0; i < 200; ++i) {
    if (i > 0) q += '&';
    q += "a=v" + std::to_string(i);
  }
  QueryString qs(q);
  // Should still parse without crash, capping at kMaxTotalPairs (200)
  EXPECT_EQ(qs.size(), 1);  // single key "a"
  auto all = qs.get_all("a");
  EXPECT_LE(all.size(), 128);  // capped per key
}
