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
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "knxd/knxd_client.h"

using namespace cvknxd;

namespace {

/// Generate a unique socket path for this test run.
std::string make_temp_socket_path() {
  const char* tmpdir = getenv("TMPDIR");
  if (tmpdir == nullptr || *tmpdir == '\0') {
    tmpdir = "/tmp";
  }
  std::string tmpl = std::string(tmpdir) + "/knxd-concurrency-test-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  int fd = mkstemp(buf.data());
  if (fd < 0) {
    return "";
  }
  close(fd);
  unlink(buf.data());
  return std::string(buf.data()) + ".sock";
}

/// Minimal fake knxd server that speaks enough of the eibd protocol
/// to support OPEN_GROUPCON, GROUP_PACKET, and CACHE_LAST_UPDATES_2.
/// Used to verify concurrency: a write on the main connection must not
/// be blocked by a long-poll (cache_last_updates_2) on the cache connection.
class FakeKnxdServer {
public:
  /// Record of a received group packet.
  struct ReceivedGroupPacket {
    uint16_t group_addr;
    std::vector<uint8_t> apdu;
  };

  /// Inject a stale CACHE_LAST_UPDATES_2 response directly into the cache
  /// socket's kernel buffer.  Simulates a response from a previous call
  /// that was never consumed because the combined poll detected group data.
  void inject_stale_cache_response(uint32_t position) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (cache_fd_ < 0)
      return;
    std::array<uint8_t, 8> resp = {{
        0x00,
        0x06,
        static_cast<uint8_t>((EIB_CACHE_LAST_UPDATES_2 >> 8) & 0xFF),
        static_cast<uint8_t>(EIB_CACHE_LAST_UPDATES_2 & 0xFF),
        static_cast<uint8_t>((position >> 24) & 0xFF),
        static_cast<uint8_t>((position >> 16) & 0xFF),
        static_cast<uint8_t>((position >> 8) & 0xFF),
        static_cast<uint8_t>(position & 0xFF),
    }};
    write_all(cache_fd_, resp.data(), resp.size());
  }

  /// Configure the next CACHE_LAST_UPDATES_2 response.
  void set_cache_last_updates_response(uint32_t position, bool inject_group_data = false) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    cache_response_position_ = position;
    cache_inject_group_data_ = inject_group_data;
  }

  FakeKnxdServer() {
    socket_path_ = make_temp_socket_path();
    if (socket_path_.empty()) {
      return;
    }

    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return;
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    size_t path_len = socket_path_.copy(addr.sun_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[path_len] = '\0';

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }

    if (::listen(listen_fd_, 2) < 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }

    accept_thread_ = std::thread(&FakeKnxdServer::accept_loop, this);
  }

  FakeKnxdServer(const FakeKnxdServer&) = delete;
  FakeKnxdServer& operator=(const FakeKnxdServer&) = delete;
  FakeKnxdServer(FakeKnxdServer&&) = delete;
  FakeKnxdServer& operator=(FakeKnxdServer&&) = delete;

  ~FakeKnxdServer() {
    shutdown_.store(true);
    // Connect to ourselves to unblock accept()
    if (!socket_path_.empty()) {
      int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd >= 0) {
        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        size_t path_len = socket_path_.copy(addr.sun_path, sizeof(addr.sun_path) - 1);
        addr.sun_path[path_len] = '\0';
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        int conn_ret = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        (void)conn_ret;
        ::close(fd);
      }
    }
    if (accept_thread_.joinable()) {
      accept_thread_.join();
    }
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
    }
    if (!socket_path_.empty()) {
      ::unlink(socket_path_.c_str());
    }
  }

  [[nodiscard]] bool is_ready() const { return listen_fd_ >= 0; }
  [[nodiscard]] const std::string& path() const { return socket_path_; }

  /// Get all group packets received by the fake server.
  [[nodiscard]] std::vector<ReceivedGroupPacket> received_packets() const {
    std::lock_guard<std::mutex> lock(recv_mutex_);
    return received_packets_;
  }

private:
  /// Read exactly N bytes from fd. Returns false on EOF/error.
  static bool read_exact(int fd, uint8_t* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      ssize_t n = ::read(fd, &buf[off], len - off);
      if (n <= 0) {
        return false;
      }
      off += static_cast<size_t>(n);
    }
    return true;
  }

  /// Write all bytes to fd. Returns false on error.
  static bool write_all(int fd, const uint8_t* data, size_t len) {
    size_t off = 0;
    while (off < len) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      ssize_t n = ::write(fd, &data[off], len - off);
      if (n <= 0) {
        return false;
      }
      off += static_cast<size_t>(n);
    }
    return true;
  }

  /// Handle a single client connection.
  /// This is intentionally minimal — it only supports the messages
  /// needed for the concurrency test.
  void handle_client(int fd) {
    // Read the first message to determine if this is a main or cache connection
    while (!shutdown_.load()) {
      // Read 2-byte length prefix
      std::array<uint8_t, 2> len_buf{};
      if (!read_exact(fd, len_buf.data(), 2)) {
        break;
      }
      uint16_t payload_len = static_cast<uint16_t>((len_buf[0] << 8) | len_buf[1]);

      // Read payload
      std::vector<uint8_t> payload(payload_len);
      if (payload_len > 0 && !read_exact(fd, payload.data(), payload_len)) {
        break;
      }

      if (payload_len < 2) {
        continue;
      }

      uint16_t msg_type = static_cast<uint16_t>((payload[0] << 8) | payload[1]);

      if (msg_type == EIB_OPEN_GROUPCON) {
        // Track this fd as the group socket connection.
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          group_fd_ = fd;
        }
        // Respond with success: same type, empty payload.
        // Wire format: [len:2][type:2].  len=2, payload is the 2-byte type.
        std::array<uint8_t, 4> resp = {0x00, 0x02,  // payload length = 2
                                       static_cast<uint8_t>((msg_type >> 8) & 0xFF),
                                       static_cast<uint8_t>(msg_type & 0xFF)};
        write_all(fd, resp.data(), resp.size());
        // After OPEN_GROUPCON, the main connection stays open for GROUP_PACKET.
        continue;
      }

      if (msg_type == EIB_GROUP_PACKET && payload_len >= 4) {
        // Group packet: [dst_addr:2][apdu:N]
        uint16_t dst_addr = static_cast<uint16_t>((payload[2] << 8) | payload[3]);
        std::vector<uint8_t> apdu(payload.begin() + 4, payload.end());
        {
          std::lock_guard<std::mutex> lock(recv_mutex_);
          received_packets_.push_back({dst_addr, std::move(apdu)});
        }
        // send_group_packet() returns after write_all() succeeds, so we don't
        // need to send a response. Just continue.
        continue;
      }

      if (msg_type == EIB_CACHE_READ || msg_type == EIB_CACHE_READ_NOWAIT) {
        // Cache read request: [addr:2]
        // Response for cache miss: [type:2][src:2][dst:2] = 6 bytes payload
        if (payload_len >= 2) {
          std::array<uint8_t, 8> resp = {0x00,
                                         0x06,  // payload length = 6
                                         static_cast<uint8_t>((msg_type >> 8) & 0xFF),
                                         static_cast<uint8_t>(msg_type & 0xFF),
                                         payload[0],   // src hi (echo)
                                         payload[1],   // src lo
                                         payload[0],   // dst hi
                                         payload[1]};  // dst lo
          write_all(fd, resp.data(), resp.size());
        }
        continue;
      }

      if (msg_type == EIB_CACHE_LAST_UPDATES_2) {
        // Track this fd as the cache connection.
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          cache_fd_ = fd;
        }
        // Check if a response is configured.
        uint32_t resp_pos = 0;
        bool inject = false;
        int gfd = -1;
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          resp_pos = cache_response_position_;
          inject = cache_inject_group_data_;
          gfd = group_fd_;
          cache_response_position_ = 0;
          cache_inject_group_data_ = false;
        }

        if (resp_pos != 0) {
          // If configured, inject an APDU_PACKET on the group socket FIRST,
          // before sending the cache response.  This ensures the combined
          // poll detects group data and returns nullopt, leaving the cache
          // response (with the potentially stale position) in the kernel
          // buffer for the next ensure_cache_connection() drain test.
          if (inject && gfd >= 0) {
            std::array<uint8_t, 11> group_msg = {{
                0x00, 0x09,  // payload length = 9 (type:2 + src:2 + dst:2 + apdu:3)
                static_cast<uint8_t>((EIB_APDU_PACKET >> 8) & 0xFF),
                static_cast<uint8_t>(EIB_APDU_PACKET & 0xFF), 0x11, 0x01,  // src pa
                0x0A, 0x03,       // dst ga = 0x0A03 (1/2/3)
                0x00, 0x80, 0x42  // APDU = A_GroupValue_Write, value 0x42
            }};
            write_all(gfd, group_msg.data(), group_msg.size());
          }

          // Now send the CACHE_LAST_UPDATES_2 response: [len:2][type:2][end_pos:4]
          // No changed addresses.
          std::array<uint8_t, 8> resp = {{
              0x00,
              0x06,  // payload length = 6 (type:2 + end_pos:4)
              static_cast<uint8_t>((EIB_CACHE_LAST_UPDATES_2 >> 8) & 0xFF),
              static_cast<uint8_t>(EIB_CACHE_LAST_UPDATES_2 & 0xFF),
              static_cast<uint8_t>((resp_pos >> 24) & 0xFF),
              static_cast<uint8_t>((resp_pos >> 16) & 0xFF),
              static_cast<uint8_t>((resp_pos >> 8) & 0xFF),
              static_cast<uint8_t>(resp_pos & 0xFF),
          }};
          write_all(fd, resp.data(), resp.size());
          continue;
        }

        // Default: hold the connection open (long-poll simulation)
        while (!shutdown_.load()) {
          std::array<uint8_t, 2> dummy{};
          if (!read_exact(fd, dummy.data(), 2)) {
            break;
          }
          uint16_t pl = static_cast<uint16_t>((dummy[0] << 8) | dummy[1]);
          std::vector<uint8_t> p(pl);
          if (pl > 0 && !read_exact(fd, p.data(), pl)) {
            break;
          }
        }
        break;
      }

      // Unknown message — close
      break;
    }

    ::close(fd);
  }

  void accept_loop() {
    while (!shutdown_.load()) {
      int client = ::accept(listen_fd_, nullptr, nullptr);
      if (client < 0) {
        if (shutdown_.load()) {
          break;
        }
        continue;
      }
      // Handle each client in a separate thread (detached)
      std::thread(&FakeKnxdServer::handle_client, this, client).detach();
    }
  }

  int listen_fd_ = -1;
  std::string socket_path_;
  std::atomic<bool> shutdown_{false};
  std::thread accept_thread_;
  mutable std::mutex recv_mutex_;
  std::vector<ReceivedGroupPacket> received_packets_;

  // Thread-safe state shared between connection handlers.
  mutable std::mutex state_mutex_;
  int group_fd_ = -1;  // fd of the group-socket connection
  int cache_fd_ = -1;  // fd of the cache connection
  uint32_t cache_response_position_ = 0;
  bool cache_inject_group_data_ = false;
};

}  // namespace

/// FIXTURE: sets up a FakeKnxdServer and a KnxdClient connected to it.
class KnxdClientConcurrencyTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Skip if socket creation failed (e.g., no /tmp access)
    if (!fake_knxd_.is_ready()) {
      GTEST_SKIP() << "Cannot create fake knxd socket";
    }

    // Ignore SIGPIPE so that write() on a closed socket returns EPIPE
    // instead of killing the process.  Production code does this via
    // FCGX_Init(), but the test binary doesn't call that.
    ::signal(SIGPIPE, SIG_IGN);

    ASSERT_TRUE(client_.connect(fake_knxd_.path()));
    ASSERT_TRUE(client_.open_group_socket(false));
    client_.set_nonblocking(true);
  }

  void TearDown() override { client_.disconnect(); }

  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
  FakeKnxdServer fake_knxd_;
  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
  KnxdClient client_;
};

/// Verify that send_group_packet() is not blocked by a concurrent
/// cache_last_updates_2() call in another thread.
///
/// Without the fix, cache_last_updates_2() holds the global mutex
/// while blocking in poll(), so send_group_packet() on another thread
/// is forced to wait for the full long-poll timeout — making write
/// operations unresponsive during any active long-poll read.
TEST_F(KnxdClientConcurrencyTest, SendGroupPacketNotBlockedByLongPoll) {
  std::atomic<bool> longpoll_started{false};
  std::atomic<bool> longpoll_done{false};

  // Thread A: start a long-poll that will block (no response from fake knxd)
  std::thread longpoll_thread([&]() {
    longpoll_started.store(true);
    auto result = client_.cache_last_updates_2(0, 1);  // 1 second timeout
    // Expected: times out because fake knxd never responds
    EXPECT_FALSE(result.has_value());
    longpoll_done.store(true);
  });

  // Wait for the long-poll to start blocking
  while (!longpoll_started.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Give it a moment to enter the poll() call
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Thread B (main): try to send a group packet while the long-poll is active
  auto write_start = std::chrono::steady_clock::now();
  std::vector<uint8_t> apdu = {0x00, 0x80, 0x42};
  bool write_result = client_.send_group_packet(0x0A03, apdu);
  auto write_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - write_start)
                           .count();

  EXPECT_TRUE(write_result);

  // Critical assertion: the write must complete quickly, not be blocked
  // by the concurrent long-poll. Without the mutex fix, this would take
  // ~5 seconds (the full long-poll timeout).
  EXPECT_LT(write_elapsed, 1000) << "send_group_packet was blocked for " << write_elapsed
                                 << "ms by concurrent long-poll";

  longpoll_thread.join();
  EXPECT_TRUE(longpoll_done.load());
}

/// Verify that send_group_packet() actually sends a correctly formatted
/// group packet that the knxd server receives.  This is a regression test
/// for the "halve knxd connections" change where cache_read was moved to
/// share the main connection fd.
TEST_F(KnxdClientConcurrencyTest, SendGroupPacketReachesServer) {
  std::vector<uint8_t> apdu = {0x00, 0x80, 0x42};
  EXPECT_TRUE(client_.send_group_packet(0x0A03, apdu));

  // Give the fake server thread time to process
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto packets = fake_knxd_.received_packets();
  ASSERT_EQ(packets.size(), 1);
  EXPECT_EQ(packets[0].group_addr, 0x0A03);
  EXPECT_EQ(packets[0].apdu, apdu);
}

/// Verify that send_group_packet() works correctly even after cache_read
/// has been called.  The two operations share the main connection
/// in the "halve connections" design, so we need to ensure the cache
/// read buffer is not interfering with writes.
TEST_F(KnxdClientConcurrencyTest, SendGroupPacketAfterCacheRead) {
  // First, do a cache_read to exercise the cache connection path
  auto cache_result = client_.cache_read(0x0A03, true);
  // Fake server responds with cache miss — expected
  EXPECT_FALSE(cache_result.has_value());

  // Now send a group packet
  std::vector<uint8_t> apdu = {0x00, 0x80, 0x0C, 0x6F};
  EXPECT_TRUE(client_.send_group_packet(0x0B04, apdu));

  // Give the fake server time to process
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto packets = fake_knxd_.received_packets();
  ASSERT_EQ(packets.size(), 1);
  EXPECT_EQ(packets[0].group_addr, 0x0B04);
  EXPECT_EQ(packets[0].apdu, apdu);
}

/// Verify that cache_read followed by send_group_packet works when
/// multiple cache reads are interleaved with writes.
TEST_F(KnxdClientConcurrencyTest, SendGroupPacketAfterCacheReadTwice) {
  // First cache_read
  EXPECT_FALSE(client_.cache_read(0x0A03, true).has_value());

  // Send a packet
  std::vector<uint8_t> apdu1 = {0x00, 0x80, 0x42};
  EXPECT_TRUE(client_.send_group_packet(0x0A03, apdu1));

  // Second cache_read
  EXPECT_FALSE(client_.cache_read(0x0B04, true).has_value());

  // Send another packet
  std::vector<uint8_t> apdu2 = {0x00, 0x80, 0x0C, 0x6F};
  EXPECT_TRUE(client_.send_group_packet(0x0C05, apdu2));

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto packets = fake_knxd_.received_packets();
  ASSERT_EQ(packets.size(), 2);
  EXPECT_EQ(packets[0].group_addr, 0x0A03);
  EXPECT_EQ(packets[0].apdu, apdu1);
  EXPECT_EQ(packets[1].group_addr, 0x0C05);
  EXPECT_EQ(packets[1].apdu, apdu2);
}

/// Verify that rapid consecutive send_group_packet() calls are rate-limited.
/// The inter-write delay (50 ms) prevents the application from flooding knxd's
/// IP tunnel, which would otherwise cause retry exhaustion and fatal
/// "Link down, terminating" errors.
TEST_F(KnxdClientConcurrencyTest, RapidWritesAreRateLimited) {
  constexpr int kWrites = 5;
  constexpr auto kMinInterval = std::chrono::milliseconds(50);

  std::vector<uint8_t> apdu = {0x00, 0x80, 0x42};

  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < kWrites; ++i) {
    EXPECT_TRUE(client_.send_group_packet(0x0A03, apdu));
  }
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  // With 50 ms minimum interval, 5 writes should take at least
  // 4 × 50 ms = 200 ms.  Allow 20% tolerance for scheduler jitter.
  EXPECT_GE(elapsed.count(), (kWrites - 1) * kMinInterval.count() * 80 / 100)
      << "Expected at least " << ((kWrites - 1) * kMinInterval.count() * 80 / 100) << " ms for "
      << kWrites << " writes, but took " << elapsed.count() << " ms";

  // All packets must have been received by the fake server.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto packets = fake_knxd_.received_packets();
  EXPECT_EQ(packets.size(), kWrites);
}

/// Verify that stale CACHE_LAST_UPDATES_2 responses in the kernel buffer
/// are drained before the cache connection is reused.  This prevents the
/// lastpos (i=) index from jumping backwards when a response from a previous
/// (interrupted-by-group-data) call is consumed as the response to a new call.
TEST_F(KnxdClientConcurrencyTest, StaleCacheResponseDrainedBeforeReuse) {
  // Establish the cache connection by doing a cache operation.
  // The fake server tracks cache_fd_ from this.
  EXPECT_FALSE(client_.cache_read(0x0A03, true).has_value());

  // Inject a stale CACHE_LAST_UPDATES_2 response (position 140) directly
  // into the cache socket.  This simulates a response that was left in
  // the kernel buffer because the combined poll was interrupted by group
  // socket data.
  fake_knxd_.inject_stale_cache_response(140);

  // Give the kernel time to deliver the data.
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Now call cache_last_updates_2 with a higher start position.
  // ensure_cache_connection() reuses the cache connection and MUST drain
  // the stale position-140 response from the kernel buffer before sending
  // the new request.  The fake server is configured to respond with
  // position 141.  Without the drain, the function would consume the
  // stale response and return position 140 — a backward jump.
  fake_knxd_.set_cache_last_updates_response(141, false);
  auto result = client_.cache_last_updates_2(141, 1);
  ASSERT_TRUE(result.has_value()) << "Expected a fresh response, got nullopt";
  EXPECT_EQ(result->new_position, 141u)
      << "Got stale position " << result->new_position << " instead of 141";
}
