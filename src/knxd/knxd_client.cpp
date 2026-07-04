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

#include "knxd_client.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "knxd_protocol.h"

namespace cvknxd {

struct KnxdClient::Impl {
  int fd = -1;
  GroupTelegramCallback telegram_callback;
  bool group_socket_open = false;

  ~Impl() {
    if (fd >= 0) {
      ::close(fd);
    }
  }
};

KnxdClient::KnxdClient() : impl_(std::make_unique<Impl>()) {}

KnxdClient::~KnxdClient() = default;

KnxdClient::KnxdClient(KnxdClient&&) noexcept = default;
KnxdClient& KnxdClient::operator=(KnxdClient&&) noexcept = default;

bool KnxdClient::connect(std::string_view socket_path) {
  if (impl_->fd >= 0) {
    disconnect();
  }

  impl_->fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (impl_->fd < 0)
    return false;

  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  // Copy path safely
  if (socket_path.size() >= sizeof(addr.sun_path))
    return false;
  std::memcpy(addr.sun_path, socket_path.data(), socket_path.size());

  if (::connect(impl_->fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(impl_->fd);
    impl_->fd = -1;
    return false;
  }

  return true;
}

void KnxdClient::disconnect() {
  if (impl_->fd >= 0) {
    ::close(impl_->fd);
    impl_->fd = -1;
  }
  impl_->group_socket_open = false;
}

bool KnxdClient::is_connected() const {
  return impl_->fd >= 0;
}

namespace {

/// Write all bytes to socket. Returns true if all bytes were written.
bool write_all(int fd, const uint8_t* data, size_t len) {
  while (len > 0) {
    ssize_t written = ::write(fd, data, len);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    data += written;
    len -= static_cast<size_t>(written);
  }
  return true;
}

/// Read exactly len bytes. Returns true on success.
bool read_exact(int fd, uint8_t* buf, size_t len) {
  while (len > 0) {
    ssize_t n = ::read(fd, buf, len);
    if (n <= 0) {
      if (n < 0 && errno == EINTR)
        continue;
      return false;
    }
    buf += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

/// Read a complete eibd message (length-prefixed).
std::optional<std::vector<uint8_t>> read_message(int fd) {
  // Read 2-byte length header
  uint8_t len_buf[2];
  if (!read_exact(fd, len_buf, 2))
    return std::nullopt;

  uint16_t payload_len = static_cast<uint16_t>((len_buf[0] << 8) | len_buf[1]);
  std::vector<uint8_t> full_msg(2 + payload_len);
  full_msg[0] = len_buf[0];
  full_msg[1] = len_buf[1];

  if (payload_len > 0) {
    if (!read_exact(fd, full_msg.data() + 2, payload_len))
      return std::nullopt;
  }

  return full_msg;
}

}  // namespace

bool KnxdClient::open_group_socket(bool write_only) {
  if (!is_connected())
    return false;

  uint8_t wo_byte = write_only ? 0xFF : 0x00;
  std::vector<uint8_t> data = {wo_byte};
  auto msg = build_eibd_message(EibMessageType::OPEN_GROUPCON, data);

  if (!write_all(impl_->fd, msg.data(), msg.size()))
    return false;

  // Read response
  auto resp = read_message(impl_->fd);
  if (!resp)
    return false;

  uint16_t resp_type;
  std::vector<uint8_t> resp_data;
  if (!parse_eibd_message(*resp, resp_type, resp_data))
    return false;

  // Success: response is OPEN_GROUPCON with no error
  impl_->group_socket_open = (resp_type == EibMessageType::OPEN_GROUPCON);
  return impl_->group_socket_open;
}

bool KnxdClient::send_group_packet(uint16_t group_addr, const std::vector<uint8_t>& apdu) {
  if (!is_connected() || !impl_->group_socket_open)
    return false;

  std::vector<uint8_t> data;
  data.reserve(2 + apdu.size());
  data.push_back(static_cast<uint8_t>((group_addr >> 8) & 0xFF));
  data.push_back(static_cast<uint8_t>(group_addr & 0xFF));
  data.insert(data.end(), apdu.begin(), apdu.end());

  auto msg = build_eibd_message(EibMessageType::GROUP_PACKET, data);
  return write_all(impl_->fd, msg.data(), msg.size());
}

std::optional<std::vector<uint8_t>> KnxdClient::cache_read(uint16_t group_addr, bool nowait) {
  if (!is_connected())
    return std::nullopt;

  uint16_t msg_type = nowait ? EibMessageType::CACHE_READ_NOWAIT : EibMessageType::CACHE_READ;
  std::vector<uint8_t> data = {static_cast<uint8_t>((group_addr >> 8) & 0xFF),
                               static_cast<uint8_t>(group_addr & 0xFF)};

  auto msg = build_eibd_message(msg_type, data);
  if (!write_all(impl_->fd, msg.data(), msg.size()))
    return std::nullopt;

  auto resp = read_message(impl_->fd);
  if (!resp)
    return std::nullopt;

  uint16_t resp_type;
  std::vector<uint8_t> resp_data;
  if (!parse_eibd_message(*resp, resp_type, resp_data))
    return std::nullopt;

  if (resp_type == msg_type && resp_data.size() >= 6) {
    // Response format: src(2) + dst(2) + apdu_data...
    return std::vector<uint8_t>(resp_data.begin() + 4, resp_data.end());
  }

  return std::nullopt;
}

bool KnxdClient::poll_group_telegram(uint16_t& out_group_addr, std::vector<uint8_t>& out_apdu) {
  if (!is_connected())
    return false;

  // Try non-blocking read of message
  auto msg = read_message(impl_->fd);
  if (!msg)
    return false;

  uint16_t msg_type;
  std::vector<uint8_t> msg_data;
  if (!parse_eibd_message(*msg, msg_type, msg_data))
    return false;

  if (msg_type == EibMessageType::APDU_PACKET && msg_data.size() >= 4) {
    // Format: src_addr(2) + apdu...
    out_group_addr = static_cast<uint16_t>((msg_data[0] << 8) | msg_data[1]);
    out_apdu.assign(msg_data.begin() + 2, msg_data.end());
    return true;
  }

  return false;
}

void KnxdClient::set_telegram_callback(GroupTelegramCallback callback) {
  impl_->telegram_callback = std::move(callback);
}

}  // namespace cvknxd
