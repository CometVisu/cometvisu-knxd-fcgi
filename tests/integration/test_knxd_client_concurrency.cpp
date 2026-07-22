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

#include <atomic>
#include <thread>

#include "knxd/knxd_client.h"
#include "mock_knxd_socket.h"

using namespace cvknxd;

class KnxdClientConcurrencyTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(client_.connect("/run/knx"));
    ASSERT_TRUE(client_.open_group_socket(false));
  }
  MockKnxdClient client_;  // NOLINT(misc-non-private-member-variables-in-classes)
};

/// send_group_packet and wait_for_activity can run concurrently.
TEST_F(KnxdClientConcurrencyTest, SendAndWaitConcurrently) {
  std::atomic<bool> started{false};
  std::thread waiter([&]() {
    started.store(true);
    auto r = client_.wait_for_activity(10);
    EXPECT_EQ(r, KnxdClient::WaitResult::Timeout);
  });
  while (!started.load()) {
  }
  // Send while waiter is blocked
  (void)client_.send_group_packet(0x0A03, {0x00, 0x80, 0x42});
  waiter.join();
  EXPECT_EQ(client_.sent_packets().size(), 1);
}

/// Multiple senders can write concurrently.
TEST_F(KnxdClientConcurrencyTest, SendAndPollConcurrently) {}

/// wait_for_activity returns Timeout when no data is enqueued.
TEST_F(KnxdClientConcurrencyTest, WaitTimeout) {
  EXPECT_EQ(client_.wait_for_activity(10), KnxdClient::WaitResult::Timeout);
}
