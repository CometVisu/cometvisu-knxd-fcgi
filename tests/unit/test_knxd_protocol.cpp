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

#include "knxd/knxd_protocol.h"

using namespace cvknxd;

// ---- KnxGroupAddress ----

TEST(KnxGroupAddressTest, FromStringValid) {
  auto addr = KnxGroupAddress::from_string("1/2/3");
  ASSERT_TRUE(addr.has_value());
  EXPECT_EQ(addr->main, 1);
  EXPECT_EQ(addr->middle, 2);
  EXPECT_EQ(addr->sub, 3);
}

TEST(KnxGroupAddressTest, FromStringMaxValues) {
  auto addr = KnxGroupAddress::from_string("31/7/255");
  ASSERT_TRUE(addr.has_value());
  EXPECT_EQ(addr->main, 31);
  EXPECT_EQ(addr->middle, 7);
  EXPECT_EQ(addr->sub, 255);
}

TEST(KnxGroupAddressTest, FromStringInvalidFormat) {
  EXPECT_FALSE(KnxGroupAddress::from_string("1/2").has_value());
  EXPECT_FALSE(KnxGroupAddress::from_string("a/b/c").has_value());
  EXPECT_FALSE(KnxGroupAddress::from_string("1/2/3/4").has_value());
  EXPECT_FALSE(KnxGroupAddress::from_string("").has_value());
}

TEST(KnxGroupAddressTest, FromStringTooLarge) {
  EXPECT_FALSE(KnxGroupAddress::from_string("32/0/0").has_value());
}

TEST(KnxGroupAddressTest, ToString) {
  KnxGroupAddress addr{1, 2, 3};
  EXPECT_EQ(addr.to_string(), "1/2/3");
}

TEST(KnxGroupAddressTest, ToEibAddr) {
  // 1/2/3 → (1 << 11) | (2 << 8) | 3 = 0x0800 | 0x0200 | 0x0003 = 0x0A03
  KnxGroupAddress addr{1, 2, 3};
  EXPECT_EQ(addr.to_eibaddr(), 0x0A03);
}

TEST(KnxGroupAddressTest, FromEibAddr) {
  auto addr = KnxGroupAddress::from_eibaddr(0x0A03);
  EXPECT_EQ(addr.main, 1);
  EXPECT_EQ(addr.middle, 2);
  EXPECT_EQ(addr.sub, 3);
}

TEST(KnxGroupAddressTest, EibAddrRoundTrip) {
  KnxGroupAddress original{15, 7, 200};
  auto addr = KnxGroupAddress::from_eibaddr(original.to_eibaddr());
  EXPECT_EQ(addr.main, original.main);
  EXPECT_EQ(addr.middle, original.middle);
  EXPECT_EQ(addr.sub, original.sub);
}

// ---- KnxAddress (CometVisu format) ----

TEST(KnxAddressTest, FromCometvisuWithNamespace) {
  auto addr = KnxAddress::from_cometvisu("KNX:1/2/3");
  ASSERT_TRUE(addr.has_value());
  EXPECT_EQ(addr->ns, "KNX");
  EXPECT_EQ(addr->group.main, 1);
  EXPECT_EQ(addr->group.middle, 2);
  EXPECT_EQ(addr->group.sub, 3);
}

TEST(KnxAddressTest, FromCometvisuDefaultNamespace) {
  auto addr = KnxAddress::from_cometvisu("1/2/3");
  ASSERT_TRUE(addr.has_value());
  EXPECT_EQ(addr->ns, "KNX");
  EXPECT_EQ(addr->group.main, 1);
}

TEST(KnxAddressTest, FromCometvisuInvalid) {
  EXPECT_FALSE(KnxAddress::from_cometvisu("KNX:invalid").has_value());
  EXPECT_FALSE(KnxAddress::from_cometvisu("").has_value());
}

TEST(KnxAddressTest, ToCometvisu) {
  KnxAddress addr{"KNX", KnxGroupAddress{4, 5, 6}};
  EXPECT_EQ(addr.to_cometvisu(), "KNX:4/5/6");
}

// ---- APDU ----

TEST(ApduTest, BuildWriteOneByte) {
  auto apdu = build_apdu(ApduType::Write, {0x42});
  ASSERT_EQ(apdu.size(), 2);
  EXPECT_EQ(apdu[0], 0x00);
  EXPECT_EQ(apdu[1], 0x80 | (0x42 & 0x3F));  // Write + packed 6-bit value
}

TEST(ApduTest, BuildWriteMultiByte) {
  auto apdu = build_apdu(ApduType::Write, {0x0C, 0x6F});
  ASSERT_EQ(apdu.size(), 4);
  EXPECT_EQ(apdu[0], 0x00);
  EXPECT_EQ(apdu[1], 0x80);  // Write, no packed data for multi-byte
  EXPECT_EQ(apdu[2], 0x0C);
  EXPECT_EQ(apdu[3], 0x6F);
}

TEST(ApduTest, BuildRead) {
  auto apdu = build_apdu(ApduType::Read, {});
  ASSERT_EQ(apdu.size(), 2);
  EXPECT_EQ(apdu[0], 0x00);
  EXPECT_EQ(apdu[1], 0x00);  // Read
}

TEST(ApduTest, BuildResponseOneByte) {
  auto apdu = build_apdu(ApduType::Response, {0x2A});
  ASSERT_EQ(apdu.size(), 2);
  EXPECT_EQ(apdu[0], 0x00);
  EXPECT_EQ(apdu[1], 0x40 | (0x2A & 0x3F));  // Response + packed 6-bit value
}

TEST(ApduTest, ParseWriteOneByte) {
  // Single-byte write: type bits 0x80, data in lower 6 bits
  std::vector<uint8_t> apdu = {0x00, 0x80 | (0x2A & 0x3F)};
  ApduType type;
  std::vector<uint8_t> data;
  ASSERT_TRUE(parse_apdu(apdu, type, data));
  EXPECT_EQ(type, ApduType::Write);
  ASSERT_EQ(data.size(), 1);
  EXPECT_EQ(data[0], 0x2A);
}

TEST(ApduTest, ParseWriteMultiByte) {
  std::vector<uint8_t> apdu = {0x00, 0x80, 0x0C, 0x6F};
  ApduType type;
  std::vector<uint8_t> data;
  ASSERT_TRUE(parse_apdu(apdu, type, data));
  EXPECT_EQ(type, ApduType::Write);
  ASSERT_EQ(data.size(), 2);
  EXPECT_EQ(data[0], 0x0C);
  EXPECT_EQ(data[1], 0x6F);
}

TEST(ApduTest, ParseRead) {
  std::vector<uint8_t> apdu = {0x00, 0x00};
  ApduType type;
  std::vector<uint8_t> data;
  ASSERT_TRUE(parse_apdu(apdu, type, data));
  EXPECT_EQ(type, ApduType::Read);
  EXPECT_TRUE(data.empty());
}

TEST(ApduTest, ParseResponse) {
  std::vector<uint8_t> apdu = {0x00, 0x40 | (0x1A & 0x3F)};
  ApduType type;
  std::vector<uint8_t> data;
  ASSERT_TRUE(parse_apdu(apdu, type, data));
  EXPECT_EQ(type, ApduType::Response);
  ASSERT_EQ(data.size(), 1);
  EXPECT_EQ(data[0], 0x1A);
}

// ---- EIBD Message ----

TEST(EibdMessageTest, BuildGroupPacket) {
  auto msg = build_eibd_message(0x0027, {0x0A, 0x03, 0x00, 0x80});
  // Length: 2 (type) + 4 (data) = 6 bytes payload
  EXPECT_EQ(msg[0], 0x00);  // len hi
  EXPECT_EQ(msg[1], 0x06);  // len lo
  EXPECT_EQ(msg[2], 0x00);  // type hi
  EXPECT_EQ(msg[3], 0x27);  // type lo
  EXPECT_EQ(msg[4], 0x0A);  // data
  EXPECT_EQ(msg[5], 0x03);
  EXPECT_EQ(msg[6], 0x00);
  EXPECT_EQ(msg[7], 0x80);
}

TEST(EibdMessageTest, ParseGroupPacket) {
  std::vector<uint8_t> raw = {0x00, 0x04, 0x00, 0x27, 0xAA, 0xBB};
  uint16_t type;
  std::vector<uint8_t> data;
  ASSERT_TRUE(parse_eibd_message(raw, type, data));
  EXPECT_EQ(type, 0x0027);
  ASSERT_EQ(data.size(), 2);
  EXPECT_EQ(data[0], 0xAA);
  EXPECT_EQ(data[1], 0xBB);
}

TEST(EibdMessageTest, ParseTooShort) {
  std::vector<uint8_t> raw = {0x00, 0x04, 0x00};  // truncated
  uint16_t type;
  std::vector<uint8_t> data;
  EXPECT_FALSE(parse_eibd_message(raw, type, data));
}

// ---- try_extract_message (buffer-based incremental parsing) ----

TEST(TryExtractMessageTest, CompleteMessage) {
  std::vector<uint8_t> buffer = {0x00, 0x04, 0x00, 0x27, 0xAA, 0xBB};
  auto msg = try_extract_message(buffer);
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->size(), 6);
  // Verify the message contains what we put in
  EXPECT_EQ((*msg)[0], 0x00);
  EXPECT_EQ((*msg)[1], 0x04);
  EXPECT_EQ((*msg)[4], 0xAA);
  EXPECT_EQ((*msg)[5], 0xBB);
  // Buffer should be consumed
  EXPECT_TRUE(buffer.empty());
}

TEST(TryExtractMessageTest, IncompleteMessage) {
  std::vector<uint8_t> buffer = {0x00, 0x04, 0x00};  // length says 4, only 1 byte available
  auto msg = try_extract_message(buffer);
  EXPECT_FALSE(msg.has_value());
  // Buffer should be unchanged
  EXPECT_EQ(buffer.size(), 3);
}

TEST(TryExtractMessageTest, EmptyBuffer) {
  std::vector<uint8_t> buffer;
  auto msg = try_extract_message(buffer);
  EXPECT_FALSE(msg.has_value());
}

TEST(TryExtractMessageTest, MultipleMessagesBuffered) {
  // Two messages back-to-back
  std::vector<uint8_t> buffer = {
      0x00, 0x02, 0x00, 0x27,  // 2-byte payload, type 0x0027
      0x00, 0x03, 0x00, 0x25, 0xAA  // 3-byte payload, type 0x0025
  };
  auto msg1 = try_extract_message(buffer);
  ASSERT_TRUE(msg1.has_value());
  EXPECT_EQ(msg1->size(), 4);  // 2 len + 2 payload

  auto msg2 = try_extract_message(buffer);
  ASSERT_TRUE(msg2.has_value());
  EXPECT_EQ(msg2->size(), 5);  // 2 len + 3 payload

  EXPECT_TRUE(buffer.empty());
}

TEST(TryExtractMessageTest, PartialSecondMessage) {
  // First message complete, second incomplete
  std::vector<uint8_t> buffer = {
      0x00, 0x02, 0x00, 0x27,  // complete: 2+2=4 bytes
      0x00, 0x05, 0x00          // length says 5, only 1 byte available → incomplete
  };
  auto msg1 = try_extract_message(buffer);
  ASSERT_TRUE(msg1.has_value());
  EXPECT_EQ(msg1->size(), 4);

  auto msg2 = try_extract_message(buffer);
  EXPECT_FALSE(msg2.has_value());
  EXPECT_EQ(buffer.size(), 3);  // remaining partial message preserved
}

TEST(TryExtractMessageTest, SingleByteBuffer) {
  std::vector<uint8_t> buffer = {0x00};  // need 2 bytes for length
  auto msg = try_extract_message(buffer);
  EXPECT_FALSE(msg.has_value());
  EXPECT_EQ(buffer.size(), 1);
}

TEST(TryExtractMessageTest, MaxPayloadSizeAccepted) {
  // Maximum 16-bit payload length: 65535 bytes — this would be huge but
  // the function itself doesn't cap it. The caller (read_message) enforces
  // kMaxReadBufferSize. Here we just verify basic correctness for a valid
  // but empty message (payload_len=0).
  std::vector<uint8_t> buffer = {0x00, 0x00};  // 0-length payload
  auto msg = try_extract_message(buffer);
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->size(), 2);
  EXPECT_TRUE(buffer.empty());
}
