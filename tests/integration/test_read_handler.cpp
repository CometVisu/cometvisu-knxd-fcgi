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

#include "handlers/read_handler.h"
#include "mock_knxd_socket.h"
#include "state/group_cache.h"
#include "state/session_store.h"

using namespace cvknxd;

class ReadHandlerTest : public ::testing::Test {
public:
  void SetUp() override {
    (void)knxd_.connect("/run/knx");
    (void)knxd_.open_group_socket(false);
  }

  MockKnxdClient knxd_;
  GroupCache cache_;
  SessionStore sessions_;
};

// ================================================================
// Parameter handling & error cases
// ================================================================

TEST_F(ReadHandlerTest, NoAddressesReturns400) {
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("s=abc&t=0");
  EXPECT_EQ(r.http_status, 400);
  EXPECT_NE(r.body.find("\"error\":\"missing address\""), std::string::npos);
}

TEST_F(ReadHandlerTest, InvalidTimeoutReturns400) {
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&t=abc");
  EXPECT_EQ(r.http_status, 400);
}

TEST_F(ReadHandlerTest, SessionInvalidReturns401) {
  (void)sessions_.create_session(false);
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&t=30&s=nonexistent");
  EXPECT_EQ(r.http_status, 401);
}

TEST_F(ReadHandlerTest, AnonymousSessionOk) {
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&t=30&s=0");
  EXPECT_EQ(r.http_status, 200);
}

TEST_F(ReadHandlerTest, ValidSessionProceeds) {
  auto sid = sessions_.create_session(false);
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&t=30&s=" + sid);
  EXPECT_EQ(r.http_status, 200);
}

TEST_F(ReadHandlerTest, NoValidAddressesReturns404) {
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=invalid");
  EXPECT_EQ(r.http_status, 404);
}

// ================================================================
// Initial read: GroupCache lookup (lastpos == 0)
// ================================================================

TEST_F(ReadHandlerTest, InitialReadFromGroupCache) {
  cache_.update(0x0A03, {0x42});  // 1/2/3
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&t=30");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("\"42\""), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":"), std::string::npos);
}

TEST_F(ReadHandlerTest, InitialReadCacheMissReturnsEmpty) {
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&t=30");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("\"d\":{}"), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":0"), std::string::npos);
}

TEST_F(ReadHandlerTest, InitialReadMultipleAddresses) {
  cache_.update(0x0A03, {0x42});        // 1/2/3
  cache_.update(0x0B04, {0x0C, 0x6F});  // 1/3/4
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4&t=30");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  EXPECT_NE(r.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(r.body.find("0c6f"), std::string::npos);
}

TEST_F(ReadHandlerTest, InitialReadMixedCachedAndUncached) {
  cache_.update(0x0A03, {0x42});  // only 1/2/3 cached
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4&t=0");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  EXPECT_EQ(r.body.find("1/3/4"), std::string::npos);
}

TEST_F(ReadHandlerTest, TZeroForcesInitialRead) {
  cache_.update(0x0A03, {0x42});
  cache_.update(0x0B04, {0x0C, 0x6F});
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4&t=0");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":"), std::string::npos);
}

// ================================================================
// Poll loop: wait_for_activity + drain_and_cache
// ================================================================

TEST_F(ReadHandlerTest, LongPollReceivesGroupTelegram) {
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":"), std::string::npos);
}

TEST_F(ReadHandlerTest, LongPollSkipsNonMatchingTelegram) {
  knxd_.enqueue_telegram(0x0B04, {0x00, 0x80, 0x0C, 0x6F});  // 1/3/4
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&t=1");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_EQ(r.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(r.body.find("\"d\":{}"), std::string::npos);
}

TEST_F(ReadHandlerTest, LongPollMultipleTelegrams) {
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  knxd_.enqueue_telegram(0x0B04, {0x00, 0x80, 0x0C, 0x6F});
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  EXPECT_NE(r.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(r.body.find("0c6f"), std::string::npos);
}

TEST_F(ReadHandlerTest, LongPollTimeoutReturnsEmpty) {
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&t=1");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("\"d\":{}"), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":0"), std::string::npos);
}

// ================================================================
// Write-wakes-read
// ================================================================

TEST_F(ReadHandlerTest, WriteWakesBlockedRead) {
  ReadHandler handler(knxd_, cache_, sessions_);
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  auto r = handler.handle("a=1/2/3&i=42&t=5");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":1"), std::string::npos);
}

TEST_F(ReadHandlerTest, WriteWakesBlockedReadNonMatchingIgnored) {
  ReadHandler handler(knxd_, cache_, sessions_);
  knxd_.enqueue_telegram(0x0B04, {0x00, 0x80, 0x0C, 0x6F});
  auto r = handler.handle("a=1/2/3&i=42&t=5");
  EXPECT_EQ(r.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(r.body.find("\"d\":{}"), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":1"), std::string::npos);
}

TEST_F(ReadHandlerTest, WriteWakesReadIndexAdvances) {
  ReadHandler handler(knxd_, cache_, sessions_);
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x99});
  auto r = handler.handle("a=1/2/3&i=42&t=5");
  EXPECT_NE(r.body.find("99"), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":2"), std::string::npos);
}

// ================================================================
// Position (i) correctness
// ================================================================

TEST_F(ReadHandlerTest, IndexIsTelegramCount) {
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x99});
  knxd_.enqueue_telegram(0x0B04, {0x00, 0x80, 0x01});
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&t=5");
  EXPECT_NE(r.body.find("\"i\":3"), std::string::npos);
}

TEST_F(ReadHandlerTest, IndexNeverGoesBackward) {
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  ReadHandler h1(knxd_, cache_, sessions_);
  auto r1 = h1.handle("a=1/2/3&i=0");
  EXPECT_NE(r1.body.find("\"i\":1"), std::string::npos);

  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x99});
  ReadHandler h2(knxd_, cache_, sessions_);
  auto r2 = h2.handle("a=1/2/3&i=1");
  EXPECT_NE(r2.body.find("\"i\":2"), std::string::npos);
}

// ================================================================
// Deduplication
// ================================================================

TEST_F(ReadHandlerTest, DeduplicatesSameAddress) {
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x99});
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3");
  size_t first = r.body.find("1/2/3");
  EXPECT_NE(first, std::string::npos);
  size_t second = r.body.find("1/2/3", first + 1);
  EXPECT_EQ(second, std::string::npos);
}

// ================================================================
// GroupCache persistence
// ================================================================

TEST_F(ReadHandlerTest, GroupCachePersistsAcrossRequests) {
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  {
    ReadHandler h(knxd_, cache_, sessions_);
    (void)h.handle("a=1/2/3&t=1");
  }
  ReadHandler h2(knxd_, cache_, sessions_);
  auto r = h2.handle("a=1/2/3&i=0&t=1");
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
}

// ================================================================
// Read-APDU filtering
// ================================================================

TEST_F(ReadHandlerTest, FiltersOutReadApdu) {
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x00, 0x00});  // A_GroupValue_Read
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&t=1");
  EXPECT_NE(r.body.find("\"d\":{}"), std::string::npos);
}
