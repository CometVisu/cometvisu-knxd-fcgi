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

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "fcgi/fcgi_server.h"

using namespace cvknxd;

namespace {

/// FCGI record types (from fcgiapp.h / FastCGI spec).
constexpr uint8_t kFcgiBeginRequest = 1;
constexpr uint8_t kFcgiParams = 4;
constexpr uint8_t kFcgiStdin = 5;
constexpr uint8_t kFcgiStdout = 6;
constexpr uint8_t kFcgiEndRequest = 3;

/// FCGI role: RESPONDER
constexpr uint16_t kFcgiResponder = 1;

/// Pack a 16-bit value to big-endian bytes.
std::array<uint8_t, 2> pack_be16(uint16_t val) {
  return {static_cast<uint8_t>((val >> 8) & 0xFF), static_cast<uint8_t>(val & 0xFF)};
}

/// Write an FCGI record to a socket.
void send_fcgi_record(int fd, uint8_t type, uint16_t request_id,
                      const std::vector<uint8_t>& content) {
  // FCGI record header: 8 bytes
  std::vector<uint8_t> header;
  header.push_back(1);  // version
  header.push_back(type);
  auto id_bytes = pack_be16(request_id);
  header.push_back(id_bytes[0]);
  header.push_back(id_bytes[1]);
  auto len_bytes = pack_be16(static_cast<uint16_t>(content.size()));
  header.push_back(len_bytes[0]);
  header.push_back(len_bytes[1]);
  // padding length — align to 8 bytes
  uint8_t padding = static_cast<uint8_t>((8 - (content.size() % 8)) % 8);
  header.push_back(padding);
  header.push_back(0);  // reserved

  // Write header + content + padding
  ::write(fd, header.data(), header.size());
  if (!content.empty()) {
    ::write(fd, content.data(), content.size());
  }
  if (padding > 0) {
    std::vector<uint8_t> pad(padding, 0);
    ::write(fd, pad.data(), pad.size());
  }
}

/// Send FCGI_BEGIN_REQUEST record.
void send_begin_request(int fd, uint16_t request_id) {
  std::vector<uint8_t> body;
  auto role_bytes = pack_be16(kFcgiResponder);
  body.push_back(role_bytes[0]);
  body.push_back(role_bytes[1]);
  body.push_back(0);  // flags (0 = don't keep connection)
  std::vector<uint8_t> reserved(5, 0);
  body.insert(body.end(), reserved.begin(), reserved.end());
  send_fcgi_record(fd, kFcgiBeginRequest, request_id, body);
}

/// Send FCGI_PARAMS record with key-value pairs encoded as name=value.
void send_params(int fd, uint16_t request_id,
                 const std::vector<std::pair<std::string, std::string>>& params) {
  std::vector<uint8_t> body;
  for (const auto& [name, value] : params) {
    // FCGI name-value pairs: length-prefixed
    auto write_len = [&body](size_t len) {
      if (len < 128) {
        body.push_back(static_cast<uint8_t>(len));
      } else {
        body.push_back(static_cast<uint8_t>((len >> 24) | 0x80));
        body.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
        body.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        body.push_back(static_cast<uint8_t>(len & 0xFF));
      }
    };
    write_len(name.size());
    write_len(value.size());
    body.insert(body.end(), name.begin(), name.end());
    body.insert(body.end(), value.begin(), value.end());
  }
  send_fcgi_record(fd, kFcgiParams, request_id, body);
}

/// Send empty FCGI_PARAMS record (end of params).
void send_end_params(int fd, uint16_t request_id) {
  send_fcgi_record(fd, kFcgiParams, request_id, {});
}

/// Send empty FCGI_STDIN record (end of stdin).
void send_end_stdin(int fd, uint16_t request_id) {
  send_fcgi_record(fd, kFcgiStdin, request_id, {});
}

/// Read an FCGI record from a socket. Returns the type, or 0 on error/EOF.
struct FcgiReceivedRecord {
  uint8_t type;
  uint16_t request_id;
  std::vector<uint8_t> content;
};

FcgiReceivedRecord read_fcgi_record(int fd) {
  FcgiReceivedRecord rec{};
  // Read 8-byte header
  uint8_t header[8];
  ssize_t n = ::read(fd, header, 8);
  if (n != 8) {
    return rec;  // type will be 0
  }
  rec.type = header[1];
  rec.request_id = static_cast<uint16_t>((static_cast<uint16_t>(header[2]) << 8) | header[3]);
  uint16_t content_len =
      static_cast<uint16_t>((static_cast<uint16_t>(header[4]) << 8) | header[5]);
  uint8_t padding_len = header[6];

  // Read content
  if (content_len > 0) {
    rec.content.resize(content_len);
    ssize_t total = 0;
    while (total < content_len) {
      ssize_t r = ::read(fd, rec.content.data() + total, content_len - total);
      if (r <= 0) {
        rec.type = 0;
        return rec;
      }
      total += r;
    }
  }

  // Skip padding
  if (padding_len > 0) {
    std::vector<uint8_t> pad(padding_len);
    ::read(fd, pad.data(), padding_len);
  }

  return rec;
}

/// Generate a unique socket path in the writable temp directory.
/// Returns empty string on failure.
std::string make_unique_socket_path() {
  const char* tmpdir = getenv("TMPDIR");
  if (tmpdir == nullptr || tmpdir[0] == '\0') {
    tmpdir = "/tmp";
  }
  // Use the workspace build directory as fallback — it's always writable
  // in the sandbox.
  std::string base = std::string(tmpdir);
  std::string tmpl = base + "/fcgi-sock-integration-XXXXXX";
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

/// Integration test: FcgiServer with a direct Unix socket.
/// Starts the server in listener mode, then connects as an FCGI client,
/// sends a simple request, and verifies the response.
class FcgiServerSocketTest : public ::testing::Test {
protected:
  void SetUp() override {
    if (!sockets_available()) {
      GTEST_SKIP() << "Unix socket creation is blocked by sandbox; "
                    << "run with unsandboxed execution to test FCGI direct socket";
    }
    socket_path_ = make_unique_socket_path();
    ASSERT_FALSE(socket_path_.empty()) << "Failed to create temp socket path";
    server_started_ = false;
  }

  void TearDown() override {
    if (server_thread_.joinable()) {
      server_thread_.detach();
    }
    unlink(socket_path_.c_str());
  }

  /// Start the server in a background thread with the given handler.
  void start_server(RequestHandler handler) {
    server_.set_handler(std::move(handler));

    // Listen on the Unix socket
    ASSERT_TRUE(server_.listen(socket_path_)) << "Failed to listen on " << socket_path_;

    server_started_ = true;

    // Run server in a separate thread
    server_thread_ = std::thread([this]() { server_.run(); });

    // Give the server a moment to start accepting
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  /// Connect to the server's Unix socket.
  [[nodiscard]] int connect_to_server() const {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0) << "socket() failed";

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
  FcgiServer server_;
  std::thread server_thread_;
  bool server_started_ = false;
};

TEST_F(FcgiServerSocketTest, HandlesRequestOverSocket) {
  start_server([](const FcgiRequest& req) -> FcgiResponse {
    FcgiResponse resp;
    resp.status_code = 200;
    resp.content_type = "text/plain";
    resp.body = "Hello from FCGI! path=" + std::string(req.path());
    return resp;
  });

  // Connect to the server
  int fd = connect_to_server();
  ASSERT_GE(fd, 0) << "Failed to connect to server socket";

  // Send FCGI request
  constexpr uint16_t kRequestId = 1;
  send_begin_request(fd, kRequestId);
  send_params(fd, kRequestId, {{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/l"}});
  send_end_params(fd, kRequestId);
  send_end_stdin(fd, kRequestId);

  // Read response
  bool got_end_request = false;
  std::vector<uint8_t> response_body;

  while (!got_end_request) {
    auto rec = read_fcgi_record(fd);
    ASSERT_NE(rec.type, 0) << "EOF or error reading FCGI response";

    switch (rec.type) {
      case kFcgiStdout:
        response_body.insert(response_body.end(), rec.content.begin(), rec.content.end());
        break;
      case kFcgiEndRequest:
        got_end_request = true;
        break;
    }
  }

  close(fd);

  // Verify response
  std::string body_str(reinterpret_cast<const char*>(response_body.data()), response_body.size());
  EXPECT_NE(body_str.find("Hello from FCGI!"), std::string::npos);
  EXPECT_NE(body_str.find("path=/l"), std::string::npos);
}

TEST_F(FcgiServerSocketTest, HandlesMultipleRequests) {
  start_server([](const FcgiRequest& req) -> FcgiResponse {
    FcgiResponse resp;
    resp.status_code = 200;
    resp.content_type = "application/json";
    resp.body = "{\"path\":\"" + std::string(req.path()) + "\"}";
    return resp;
  });

  // Connect once, send multiple requests
  int fd = connect_to_server();
  ASSERT_GE(fd, 0);

  for (uint16_t rid = 1; rid <= 3; ++rid) {
    send_begin_request(fd, rid);
    send_params(fd, rid,
                {{"REQUEST_METHOD", "GET"},
                 {"REQUEST_URI", rid == 1 ? "/l" : (rid == 2 ? "/r" : "/w")}});
    send_end_params(fd, rid);
    send_end_stdin(fd, rid);

    // Read response for this request
    bool got_end = false;
    std::vector<uint8_t> body;
    while (!got_end) {
      auto rec = read_fcgi_record(fd);
      ASSERT_NE(rec.type, 0);
      switch (rec.type) {
        case kFcgiStdout:
          body.insert(body.end(), rec.content.begin(), rec.content.end());
          break;
        case kFcgiEndRequest:
          got_end = true;
          break;
      }
    }

    std::string body_str(reinterpret_cast<const char*>(body.data()), body.size());
    std::string expected = rid == 1 ? "/l" : (rid == 2 ? "/r" : "/w");
    EXPECT_NE(body_str.find(expected), std::string::npos) << "Request " << rid;
  }

  close(fd);
}

TEST_F(FcgiServerSocketTest, Returns404ForUnknownEndpoint) {
  start_server([](const FcgiRequest& /*req*/) -> FcgiResponse {
    FcgiResponse resp;
    resp.status_code = 404;
    resp.content_type = "text/plain";
    resp.body = "Not Found";
    return resp;
  });

  int fd = connect_to_server();
  ASSERT_GE(fd, 0);

  send_begin_request(fd, 1);
  send_params(fd, 1, {{"REQUEST_METHOD", "GET"}, {"REQUEST_URI", "/nonexistent"}});
  send_end_params(fd, 1);
  send_end_stdin(fd, 1);

  bool got_end = false;
  std::vector<uint8_t> body;
  while (!got_end) {
    auto rec = read_fcgi_record(fd);
    ASSERT_NE(rec.type, 0);
    switch (rec.type) {
      case kFcgiStdout:
        body.insert(body.end(), rec.content.begin(), rec.content.end());
        break;
      case kFcgiEndRequest:
        got_end = true;
        break;
    }
  }

  close(fd);

  std::string body_str(reinterpret_cast<const char*>(body.data()), body.size());
  // Check for HTTP status in the response
  EXPECT_NE(body_str.find("404"), std::string::npos);
  EXPECT_NE(body_str.find("Not Found"), std::string::npos);
}

TEST_F(FcgiServerSocketTest, ReceivesQueryStringAndMethod) {
  start_server([](const FcgiRequest& req) -> FcgiResponse {
    FcgiResponse resp;
    resp.status_code = 200;
    resp.content_type = "text/plain";
    resp.body = req.request_method + ":" + req.query_string;
    return resp;
  });

  int fd = connect_to_server();
  ASSERT_GE(fd, 0);

  send_begin_request(fd, 1);
  send_params(fd, 1, {{"REQUEST_METHOD", "GET"},
                       {"REQUEST_URI", "/r?a=KNX:1/2/3&t=30"},
                       {"QUERY_STRING", "a=KNX:1/2/3&t=30"}});
  send_end_params(fd, 1);
  send_end_stdin(fd, 1);

  bool got_end = false;
  std::vector<uint8_t> body;
  while (!got_end) {
    auto rec = read_fcgi_record(fd);
    ASSERT_NE(rec.type, 0);
    switch (rec.type) {
      case kFcgiStdout:
        body.insert(body.end(), rec.content.begin(), rec.content.end());
        break;
      case kFcgiEndRequest:
        got_end = true;
        break;
    }
  }

  close(fd);

  std::string body_str(reinterpret_cast<const char*>(body.data()), body.size());
  EXPECT_EQ(body_str, "GET:a=KNX:1/2/3&t=30");
}
