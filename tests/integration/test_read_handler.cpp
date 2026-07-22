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
  cache_.push(0x0A03, {0x42});  // 1/2/3
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
  cache_.push(0x0A03, {0x42});        // 1/2/3
  cache_.push(0x0B04, {0x0C, 0x6F});  // 1/3/4
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4&t=30");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  EXPECT_NE(r.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(r.body.find("0c6f"), std::string::npos);
}

TEST_F(ReadHandlerTest, InitialReadMixedCachedAndUncached) {
  cache_.push(0x0A03, {0x42});  // only 1/2/3 cached
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4&t=0");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  EXPECT_EQ(r.body.find("1/3/4"), std::string::npos);
}

TEST_F(ReadHandlerTest, TZeroForcesInitialRead) {
  cache_.push(0x0A03, {0x42});
  cache_.push(0x0B04, {0x0C, 0x6F});
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
  auto r = handler.handle("a=1/2/3&i=0&t=5");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  // i = cache position after push
  EXPECT_NE(r.body.find("\"i\":1"), std::string::npos);
}

TEST_F(ReadHandlerTest, WriteWakesBlockedReadNonMatchingIgnored) {
  ReadHandler handler(knxd_, cache_, sessions_);
  knxd_.enqueue_telegram(0x0B04, {0x00, 0x80, 0x0C, 0x6F});
  auto r = handler.handle("a=1/2/3&i=0&t=5");
  EXPECT_EQ(r.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(r.body.find("\"d\":{}"), std::string::npos);
  // no data delivered → i stays at request_i
  EXPECT_NE(r.body.find("\"i\":1"), std::string::npos);
}

TEST_F(ReadHandlerTest, WriteWakesReadIndexAdvances) {
  ReadHandler handler(knxd_, cache_, sessions_);
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x99});
  auto r = handler.handle("a=1/2/3&i=0&t=5");
  EXPECT_NE(r.body.find("99"), std::string::npos);
  // same address deduplicated → 1 delivery → i = cache position after 2 pushes
  EXPECT_NE(r.body.find("\"i\":2"), std::string::npos);
}

// ================================================================
// Position (i) correctness
// ================================================================

TEST_F(ReadHandlerTest, IndexAdvancesPerUniqueAddress) {
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  knxd_.enqueue_telegram(0x0B04, {0x00, 0x80, 0x0C, 0x6F});
  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4&t=5");
  // 2 unique addresses → i = 0 + 2 = 2
  EXPECT_NE(r.body.find("\"i\":2"), std::string::npos);
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

// ================================================================
// Multi-iteration poll loop: handles busy-bus correctly
// ================================================================

/// Non-matching telegrams arrive first, then matching — handler continues polling.
TEST_F(ReadHandlerTest, BusyBusSkipsNonMatchingThenFindsMatch) {
  ReadHandler handler(knxd_, cache_, sessions_);
  // First poll: non-matching telegram only
  knxd_.enqueue_telegram(0x0B04, {0x00, 0x80, 0x01});  // 1/3/4 — not subscribed
  // Second poll: matching telegram arrives
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});  // 1/2/3

  auto r = handler.handle("a=1/2/3&t=5");
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  EXPECT_EQ(r.body.find("1/3/4"), std::string::npos);
}

/// All buffered telegrams are drained, even when none match.
TEST_F(ReadHandlerTest, DrainsAllBufferedWhenNoMatch) {
  ReadHandler handler(knxd_, cache_, sessions_);
  knxd_.enqueue_telegram(0x0B04, {0x00, 0x80, 0x01});  // 1/3/4 — not subscribed
  knxd_.enqueue_telegram(0x0C05, {0x00, 0x80, 0x02});  // 1/4/5 — not subscribed

  auto r = handler.handle("a=1/2/3&t=1");
  EXPECT_NE(r.body.find("\"d\":{}"), std::string::npos);
  // Both non-matching telegrams consumed, no data delivered
  EXPECT_EQ(cache_.position(), 2);  // both pushed to cache
}

/// Timeout after multiple empty polls returns empty.
TEST_F(ReadHandlerTest, TimeoutAfterEmptyPollsReturnsEmpty) {
  ReadHandler handler(knxd_, cache_, sessions_);
  // No telegrams — timeout after t=1
  auto r = handler.handle("a=1/2/3&t=1");
  EXPECT_NE(r.body.find("\"d\":{}"), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":0"), std::string::npos);
}

// ================================================================
// Position (i) correctness — black-box
// ================================================================

/// i reflects cache position, not telegram count.
TEST_F(ReadHandlerTest, IndexComesFromCachePosition) {
  // Pre-populate cache via direct pushes — simulates previous telegrams
  for (int i = 0; i < 42; ++i) cache_.push(0x0A03, {0x42});

  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&i=0");
  EXPECT_NE(r.body.find("\"i\":42"), std::string::npos);
}

/// i never goes backward across multiple requests.
TEST_F(ReadHandlerTest, IndexNeverGoesBackwardAcrossRequests) {
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  ReadHandler h1(knxd_, cache_, sessions_);
  auto r1 = h1.handle("a=1/2/3&i=0");
  uint32_t i1 = [&]() {
    auto p = r1.body.find("\"i\":");
    return static_cast<uint32_t>(std::stoul(r1.body.substr(p + 4)));
  }();

  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x99});
  ReadHandler h2(knxd_, cache_, sessions_);
  auto r2 = h2.handle("a=1/2/3&i=" + std::to_string(i1));
  uint32_t i2 = [&]() {
    auto p = r2.body.find("\"i\":");
    return static_cast<uint32_t>(std::stoul(r2.body.substr(p + 4)));
  }();
  EXPECT_GT(i2, i1) << "i must advance: " << i1 << " → " << i2;
}

// ================================================================
// Concurrent / black-box E2E tests
// ================================================================

/// Two readers sharing one cache: both receive the same update.
TEST_F(ReadHandlerTest, TwoReadersReceiveSameUpdate) {
  // Both readers share the same GroupCache
  ReadHandler reader_a(knxd_, cache_, sessions_);
  ReadHandler reader_b(knxd_, cache_, sessions_);

  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});  // duplicate for second poll

  auto ra = reader_a.handle("a=1/2/3&t=5");
  auto rb = reader_b.handle("a=1/2/3&t=5");

  EXPECT_NE(ra.body.find("42"), std::string::npos);
  EXPECT_NE(rb.body.find("42"), std::string::npos);
}

/// Writer updates cache, multiple readers see it concurrently.
TEST_F(ReadHandlerTest, WriteUpdatesVisibleToMultipleReaders) {
  // Simulate: /w pushes value, then two /r requests see it
  cache_.push(0x0A03, {0x42});  // write

  ReadHandler r1(knxd_, cache_, sessions_);
  ReadHandler r2(knxd_, cache_, sessions_);

  auto resp1 = r1.handle("a=1/2/3&i=0&t=1");
  auto resp2 = r2.handle("a=1/2/3&i=0&t=1");

  EXPECT_NE(resp1.body.find("42"), std::string::npos);
  EXPECT_NE(resp2.body.find("42"), std::string::npos);
  // Both see the same position
  EXPECT_EQ(resp1.body.find("\"i\":"), resp2.body.find("\"i\":"));
}

/// Delta query only returns entries newer than the client's i.
TEST_F(ReadHandlerTest, DeltaOnlyReturnsNewerThanLastIndex) {
  // Pre-populate cache, then enqueue new telegrams
  cache_.push(0x0A03, {0x01});  // pos 1 (pre-populate)
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x02});  // new telegram at pos 3
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x03});  // new telegram at pos 3

  ReadHandler handler(knxd_, cache_, sessions_);

  // Client last saw position 1 — gets new entries from positions 2 and 3
  auto r = handler.handle("a=1/2/3&i=2&t=30");
  EXPECT_NE(r.body.find("\"03\""), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":3"), std::string::npos);
}

/// Unchanged address is NOT re-transmitted when another address changes.
TEST_F(ReadHandlerTest, UnchangedAddressNotRetransmittedWhenOtherChanges) {
  // This is the exact bug reproduction: A and B in cache, only B changes.
  // A must NOT appear in the response.
  cache_.push(0x0A03, {0x42});  // A at pos 1
  cache_.push(0x0B04, {0x01});  // B at pos 2

  ReadHandler handler(knxd_, cache_, sessions_);

  // Client has already seen position 1 (which includes A's value)
  // Now knxd reports activity — but only B changed
  knxd_.enqueue_telegram(0x0B04, {0x00, 0x80, 0x01});  // B again (same value, pos 3)

  // Client polls with i=1 — should ONLY get B (which changed at pos 2 and again at pos 3)
  // A must NOT be returned because it hasn't changed since pos 1
  auto r = handler.handle("a=1/2/3&a=1/3/4&i=1&t=5");
  EXPECT_EQ(r.body.find("1/2/3"), std::string::npos) << "A must not be retransmitted";
  EXPECT_NE(r.body.find("1/3/4"), std::string::npos) << "B should be delivered";
}

/// Value oscillating (00→01→00) — each change delivered, no stale re-transmission.
TEST_F(ReadHandlerTest, OscillatingValueNoStaleRetransmission) {
  // Reproduces: 00 → 01 → 00 without stale intermediate values
  cache_.push(0x0A03, {0x00});  // pos 1

  ReadHandler h1(knxd_, cache_, sessions_);
  auto r1 = h1.handle("a=1/2/3&i=0&t=30");
  EXPECT_NE(r1.body.find("00"), std::string::npos);
  uint32_t i1 = [&]() {
    auto p = r1.body.find("\"i\":");
    return static_cast<uint32_t>(std::stoul(r1.body.substr(p + 4)));
  }();

  // Value changes to 01
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x01});
  ReadHandler h2(knxd_, cache_, sessions_);
  auto r2 = h2.handle("a=1/2/3&i=" + std::to_string(i1) + "&t=5");
  EXPECT_NE(r2.body.find("01"), std::string::npos);
  uint32_t i2 = [&]() {
    auto p = r2.body.find("\"i\":");
    return static_cast<uint32_t>(std::stoul(r2.body.substr(p + 4)));
  }();

  // Value changes back to 00
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x00});
  ReadHandler h3(knxd_, cache_, sessions_);
  auto r3 = h3.handle("a=1/2/3&i=" + std::to_string(i2) + "&t=5");
  EXPECT_NE(r3.body.find("00"), std::string::npos);
  uint32_t i3 = [&]() {
    auto p = r3.body.find("\"i\":");
    return static_cast<uint32_t>(std::stoul(r3.body.substr(p + 4)));
  }();

  EXPECT_GT(i2, i1);
  EXPECT_GT(i3, i2);
}

// ================================================================
// Latency: matching telegram must be delivered in 1 iteration
// (no unnecessary wake-ups from non-matching bus traffic)
// ================================================================

/// On a busy bus with non-matching telegrams, the handler must still
/// deliver a matching telegram without extra loop iterations.
/// This test enqueues non-matching telegrams first, then a matching one,
/// and verifies the matching data is delivered with advancing position.
TEST_F(ReadHandlerTest, MatchingTelegramDeliveredOnBusyBus) {
  // Simulate a busy bus: non-matching telegrams arrive before the match
  knxd_.enqueue_telegram(0x0B04, {0x00, 0x80, 0x01});  // 1/3/4 — not subscribed
  knxd_.enqueue_telegram(0x0C05, {0x00, 0x80, 0x02});  // 1/4/5 — not subscribed
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});  // 1/2/3 — MATCH

  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&i=42&t=5");

  EXPECT_EQ(r.http_status, 200);
  // Matching address must be delivered
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  // Non-matching addresses must NOT appear
  EXPECT_EQ(r.body.find("1/3/4"), std::string::npos);
  EXPECT_EQ(r.body.find("1/4/5"), std::string::npos);
  // Position must advance (3 telegrams pushed → position = 3)
  EXPECT_NE(r.body.find("\"i\":3"), std::string::npos);
}

/// Multiple matching telegrams for different addresses on a busy bus —
/// all matching addresses delivered, non-matching excluded.
TEST_F(ReadHandlerTest, MultipleMatchingOnBusyBus) {
  knxd_.enqueue_telegram(0x0B04, {0x00, 0x80, 0x01});  // non-match
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});  // match A
  knxd_.enqueue_telegram(0x0C05, {0x00, 0x80, 0x02});  // non-match
  knxd_.enqueue_telegram(0x0D06, {0x00, 0x80, 0x0C, 0x6F});  // match B (1/5/6)

  ReadHandler handler(knxd_, cache_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/5/6&i=0&t=5");

  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  EXPECT_NE(r.body.find("1/5/6"), std::string::npos);
  EXPECT_NE(r.body.find("0c6f"), std::string::npos);
  EXPECT_EQ(r.body.find("1/3/4"), std::string::npos);
  EXPECT_EQ(r.body.find("1/4/5"), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":4"), std::string::npos);  // 4 pushes
}
