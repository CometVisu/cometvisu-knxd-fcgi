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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "fcgi/fcgi_server.h"

using namespace cvknxd;

namespace {

/// Generate a unique socket path in the writable temp directory.
/// Returns empty string on failure.
std::string make_unique_socket_path() {
  const char* tmpdir = getenv("TMPDIR");
  if (tmpdir == nullptr || tmpdir[0] == '\0') {
    tmpdir = "/tmp";
  }
  std::string tmpl = std::string(tmpdir) + "/fcgi-sock-integration-XXXXXX";
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

/// Check if we can create Unix sockets. The VS Code sandbox may block
/// AF_UNIX socket creation, in which case we skip these tests.
bool sockets_available() {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  close(fd);

  // Also verify we can bind
  std::string path = make_unique_socket_path();
  if (path.empty()) {
    return false;
  }
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0) {
    return false;
  }
  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  size_t path_len = path.copy(addr.sun_path, sizeof(addr.sun_path) - 1);
  addr.sun_path[path_len] = '\0';
  int rc = bind(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
  close(s);
  unlink(path.c_str());
  return rc == 0;
}

}  // namespace

// ============================================================
// Test suite 1: Socket connectivity (runs on ALL platforms)
// ============================================================
//
// These tests verify that listen() creates a working listening socket.
// They do NOT start FCGI_Accept() — they just connect a plain Unix socket
// to verify the kernel-level socket is alive. This is safe on CI.
//
class FcgiServerSocketTest : public ::testing::Test {
protected:
  void SetUp() override {
    if (!sockets_available()) {
      GTEST_SKIP() << "Unix socket creation is blocked by sandbox; "
                    << "run with unsandboxed execution to test FCGI direct socket";
    }
    socket_path_ = make_unique_socket_path();
    ASSERT_FALSE(socket_path_.empty()) << "Failed to create temp socket path";
  }

  void TearDown() override {
    unlink(socket_path_.c_str());
  }

  [[nodiscard]] int connect_to_server() const {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      return -1;
    }
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    size_t path_len = socket_path_.copy(addr.sun_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[path_len] = '\0';
    int rc = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc < 0) {
      close(fd);
      return -1;
    }
    return fd;
  }

  std::string socket_path_;
};

TEST_F(FcgiServerSocketTest, ListenOpensSocket) {
  FcgiServer server;
  EXPECT_TRUE(server.listen(socket_path_));
  EXPECT_TRUE(server.is_listening());
}

TEST_F(FcgiServerSocketTest, CanConnectToListeningSocket) {
  FcgiServer server;
  ASSERT_TRUE(server.listen(socket_path_));

  int fd = connect_to_server();
  ASSERT_GE(fd, 0) << "Failed to connect to listening socket";
  close(fd);
}

TEST_F(FcgiServerSocketTest, SocketFileExistsAfterListen) {
  FcgiServer server;
  ASSERT_TRUE(server.listen(socket_path_));
  EXPECT_EQ(access(socket_path_.c_str(), F_OK), 0);
}

TEST_F(FcgiServerSocketTest, IsListeningStateAfterSuccessfulListen) {
  FcgiServer server;
  EXPECT_FALSE(server.is_listening());
  ASSERT_TRUE(server.listen(socket_path_));
  EXPECT_TRUE(server.is_listening());
}

// ============================================================
// Test suite 2: Full FCGI protocol exchange (manual test only)
// ============================================================
//
// The full FCGI protocol cycle (FCGI_Accept over a direct socket) cannot
// be automated in a test harness because:
//
//   1. FCGI_Accept() uses global process-level state (stdin/stdout
//      redirection, internal fd arrays) that cannot be shared across
//      threads — std::thread causes hangs on CI.
//
//   2. fork() doesn't help: FCGX_Accept_r() sets up FCGI streams in the
//      child, but the process-wide stdout redirection doesn't work
//      correctly in test environments — response data goes to the
//      terminal instead of the socket.
//
// The full request/response cycle IS tested by the e2e tests
// (test_full_flow.cpp) using MockKnxdClient. The socket creation itself
// is verified by the FcgiServerSocketTest suite above.
//
// To manually test the full FCGI protocol over a real socket:
//
//   # Terminal 1: start the server with a direct socket
//   FCGI_SOCKET=/tmp/test-fcgi.sock ./build/src/cometvisu-knxd-fcgi
//
//   # Terminal 2: send an FCGI request (using cgi-fcgi or a custom client)
//   cgi-fcgi -connect /tmp/test-fcgi.sock /l
//
// Or use the built-in socket mode with a web server:
//   FCGI_SOCKET=:9000 ./build/src/cometvisu-knxd-fcgi

