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

#include "util/hex.h"

using namespace cvknxd;

TEST(HexTest, DecodeLowercase) {
  auto bytes = hex_decode("0c6f");
  ASSERT_EQ(bytes.size(), 2);
  EXPECT_EQ(bytes[0], 0x0C);
  EXPECT_EQ(bytes[1], 0x6F);
}

TEST(HexTest, DecodeUppercase) {
  auto bytes = hex_decode("0C6F");
  ASSERT_EQ(bytes.size(), 2);
  EXPECT_EQ(bytes[0], 0x0C);
  EXPECT_EQ(bytes[1], 0x6F);
}

TEST(HexTest, DecodeSingleByte) {
  auto bytes = hex_decode("42");
  ASSERT_EQ(bytes.size(), 1);
  EXPECT_EQ(bytes[0], 0x42);
}

TEST(HexTest, DecodeEmpty) {
  auto bytes = hex_decode("");
  EXPECT_TRUE(bytes.empty());
}

TEST(HexTest, DecodeOddLength) {
  auto bytes = hex_decode("abc");
  EXPECT_TRUE(bytes.empty());
}

TEST(HexTest, DecodeInvalidChars) {
  auto bytes = hex_decode("0g6f");
  EXPECT_TRUE(bytes.empty());
}

TEST(HexTest, EncodeEmpty) {
  auto result = hex_encode(nullptr, 0);
  EXPECT_TRUE(result.empty());
}

TEST(HexTest, EncodeSingleByte) {
  uint8_t data[] = {0x42};
  EXPECT_EQ(hex_encode(data, 1), "42");
}

TEST(HexTest, EncodeMultiByte) {
  uint8_t data[] = {0x0C, 0x6F};
  EXPECT_EQ(hex_encode(data, 2), "0c6f");
}

TEST(HexTest, EncodeFullRange) {
  uint8_t data[] = {0x00, 0xFF, 0xA5};
  EXPECT_EQ(hex_encode(data, 3), "00ffa5");
}

TEST(HexTest, HexByte) {
  EXPECT_EQ(hex_byte(0x00), "00");
  EXPECT_EQ(hex_byte(0xFF), "ff");
  EXPECT_EQ(hex_byte(0x42), "42");
}

TEST(HexTest, RoundTrip) {
  std::string original = "0c6f0815ff00";
  auto bytes = hex_decode(original);
  ASSERT_FALSE(bytes.empty());
  auto result = hex_encode(bytes.data(), bytes.size());
  EXPECT_EQ(result, original);
}
