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

/**
 * @file knxd_client.h
 * @brief knxd Unix socket client — the interface to the KNX bus daemon.
 *
 * Communicates with knxd over a Unix domain socket using the eibd client
 * protocol.  Provides both a virtual interface (KnxdClientInterface) for
 * test mocking and a real implementation (KnxdClient) using POSIX sockets.
 *
 * Design differences from the reference eibread-cgi / eibwrite-cgi:
 *   - Persistent connections: each worker opens its own knxd connection at
 *     startup and keeps it open.  The reference opened a new connection per
 *     request (connect → operate → disconnect), which is simpler but burns
 *     syscalls and knxd connection slots under load.
 *   - Separate cache connection: cache operations use a dedicated connection
 *     to avoid head-of-line blocking from group telegrams.
 *   - Non-blocking I/O: sockets are set to O_NONBLOCK for efficient poll()
 *     integration in the long-poll handler.
 *   - Reconnect resilience: transparently reconnects after knxd restarts,
 *     with exponential backoff retry.
 */

#ifndef COMETVISU_KNXD_FCGI_KNXD_CLIENT_H_
#define COMETVISU_KNXD_FCGI_KNXD_CLIENT_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "knxd_protocol.h"

namespace cvknxd {

/**
 * @brief Abstract interface for knxd communication.
 *
 * All knxd-dependent code accepts a KnxdClientInterface& so that tests can
 * inject a mock.  The real implementation is KnxdClient.
 */
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
class KnxdClientInterface {
public:
  virtual ~KnxdClientInterface() = default;

  /// @brief Connect to knxd via Unix domain socket.
  /// @param socket_path Filesystem path to the knxd socket (e.g. "/run/knx").
  /// @return true on success.
  [[nodiscard]] virtual bool connect(std::string_view socket_path) = 0;

  /// @brief Disconnect from knxd, closing all sockets.
  virtual void disconnect() = 0;

  /// @brief Attempt reconnection after a disconnect.
  ///
  /// Uses the last known socket path and group socket settings.
  /// Re-opens the group socket after reconnecting.
  /// @return true if both reconnection and group socket re-opening succeeded.
  [[nodiscard]] virtual bool reconnect() = 0;

  /// Check if connected.
  [[nodiscard]] virtual bool is_connected() const = 0;

  /// @brief Open a group socket for sending and receiving group telegrams.
  ///
  /// knxd opens a "Group Socket" that delivers APDU_PACKET messages for all
  /// group telegrams on the bus.  This is separate from a T_Group tunnel
  /// which is per-address.
  ///
  /// @param write_only If true, the socket will only send telegrams
  ///                   (no incoming APDU_PACKET delivery).
  /// @return true on success.
  [[nodiscard]] virtual bool open_group_socket(bool write_only) = 0;

  /// @brief Send a group telegram (GroupValue_Write, GroupValue_Read, or
  ///        GroupValue_Response).
  ///
  /// Rate-limited internally to 50 ms between writes to prevent flooding
  /// knxd's IP tunnel.
  ///
  /// @param group_addr 16-bit EIB group address.
  /// @param apdu Encoded APDU bytes including the 2-byte APDU header
  ///            ([0x00, APCI|value...]).
  /// @return true on success (data sent to knxd).
  [[nodiscard]] virtual bool send_group_packet(uint16_t group_addr,
                                               const std::vector<uint8_t>& apdu) = 0;

  /// @brief Read a group value from knxd's built-in cache.
  ///
  /// Uses a separate cache connection (not the group socket) to avoid
  /// head-of-line blocking.  Retries once on connection errors.
  ///
  /// @param group_addr 16-bit EIB group address.
  /// @param nowait If true, return immediately even if no value is cached
  ///               (EIB_CACHE_READ_NOWAIT).  If false, knxd may block briefly.
  /// @return Cached value bytes (APDU payload after header), or std::nullopt
  ///         if the address is not cached or the operation failed.
  [[nodiscard]] virtual std::optional<std::vector<uint8_t>> cache_read(uint16_t group_addr,
                                                                       bool nowait) = 0;

  /// @brief Query knxd for group addresses that changed since a given
  ///        position — the COMET/long-poll primitive.
  ///
  /// Blocks until a telegram arrives or the timeout expires, then returns
  /// all changed addresses with the authoritative new position.  Uses the
  /// EIB_CACHE_LAST_UPDATES_2 protocol message with 32-bit counters.
  ///
  /// This is equivalent to eibread-cgi's cache_last_updates() call, but
  /// with 32-bit positions to avoid wrap-around on busy installations.
  ///
  /// @param start The starting position (only updates after this are returned).
  /// @param timeout_sec How long to wait (seconds, 0 = return immediately).
  /// @return LastUpdatesResult with changed addresses and new position, or
  ///         std::nullopt on error (connection loss, timeout, etc.).
  [[nodiscard]] virtual std::optional<LastUpdatesResult> cache_last_updates_2(uint32_t start,
                                                                              int timeout_sec) = 0;

  /// @brief Try to receive a group telegram from the group socket
  ///        (non-blocking).
  ///
  /// On a group socket, knxd delivers APDU_PACKET messages for every
  /// group telegram on the bus.  This method drains one such message
  /// without blocking.
  ///
  /// @param[out] out_group_addr Destination group address of the telegram.
  /// @param[out] out_apdu Raw APDU bytes.
  /// @return true if a telegram was available and consumed.
  [[nodiscard]] virtual bool poll_group_telegram(uint16_t& out_group_addr,
                                                 std::vector<uint8_t>& out_apdu) = 0;

  /// @brief Get the underlying socket file descriptor for poll()/select().
  /// @return The fd, or -1 if not connected.
  ///
  /// @warning The returned fd is only valid while the calling thread holds
  ///          the appropriate lock.  After the lock is released, another
  ///          thread may call disconnect() or reconnect(), invalidating the
  ///          fd.  Do not use the fd after releasing the lock.
  [[nodiscard]] virtual int get_fd() const = 0;

  /// Get the total number of group telegrams received from the knxd bus.
  /// This is used as the "i" (index/state-version) field in CometVisu responses.
  [[nodiscard]] virtual uint64_t get_telegram_count() const = 0;

  /// Set the socket to non-blocking mode.
  virtual void set_nonblocking(bool enable) = 0;

  /// @brief Wait for activity on either the group socket or cache connection.
  ///
  /// Enables the read handler to receive instant write notifications via
  /// the group socket (APDU_PACKET) while also waiting for cache updates.
  ///
  /// @param timeout_ms Maximum time to wait in milliseconds.
  /// @return Which fd has data, or Timeout if the timeout expired.
  enum class WaitResult { Timeout = 0, GroupData = 1, CacheData = 2 };
  [[nodiscard]] virtual WaitResult wait_for_activity(int timeout_ms) = 0;
};

/// Real implementation of KnxdClientInterface using Unix sockets.
class KnxdClient : public KnxdClientInterface {
public:
  KnxdClient();
  ~KnxdClient() override;

  // Move-only
  KnxdClient(KnxdClient&&) noexcept;
  KnxdClient& operator=(KnxdClient&&) noexcept;
  KnxdClient(const KnxdClient&) = delete;
  KnxdClient& operator=(const KnxdClient&) = delete;

  [[nodiscard]] bool connect(std::string_view socket_path) override;
  void disconnect() override;
  [[nodiscard]] bool reconnect() override;
  [[nodiscard]] bool is_connected() const override;
  [[nodiscard]] bool open_group_socket(bool write_only) override;
  [[nodiscard]] bool send_group_packet(uint16_t group_addr,
                                       const std::vector<uint8_t>& apdu) override;
  [[nodiscard]] std::optional<std::vector<uint8_t>> cache_read(uint16_t group_addr,
                                                               bool nowait) override;
  [[nodiscard]] std::optional<LastUpdatesResult> cache_last_updates_2(uint32_t start,
                                                                      int timeout_sec) override;
  [[nodiscard]] bool poll_group_telegram(uint16_t& out_group_addr,
                                         std::vector<uint8_t>& out_apdu) override;
  [[nodiscard]] int get_fd() const override;
  [[nodiscard]] uint64_t get_telegram_count() const override;
  void set_nonblocking(bool enable) override;
  [[nodiscard]] WaitResult wait_for_activity(int timeout_ms) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  /// Ensure the cache connection is open (lazy initialization).
  /// Returns pointer to the cache fd, or nullptr on failure.
  [[nodiscard]] int* ensure_cache_connection();

  /// Close the cache connection so the next cache operation will reconnect.
  void invalidate_cache();
};

}  // namespace cvknxd

#endif  // COMETVISU_KNXD_FCGI_KNXD_CLIENT_H_
