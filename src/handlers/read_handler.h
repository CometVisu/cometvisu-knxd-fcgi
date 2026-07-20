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
 * @file read_handler.h
 * @brief CometVisu read endpoint handler — cache reads and COMET long-poll.
 *
 * This is the most complex handler.  It implements the equivalent of the
 * reference eibread-cgi.c's main loop: subscribe to addresses, read cached
 * values on first request, then poll for changes via EIB_CACHE_LAST_UPDATES_2.
 *
 * Key differences from eibread-cgi:
 *   - Uses cache_last_updates_2 (32-bit position) instead of the older
 *     cache_last_updates (16-bit).  This avoids position wrap-around on
 *     busy installations.
 *   - No local cache — delegates entirely to knxd.  eibread-cgi maintained
 *     its own cache and updated it from group telegrams; we read from knxd's
 *     cache on demand via cache_read().
 *   - Belt-and-suspenders telegram drain: after cache_last_updates_2 returns,
 *     we drain the group socket for telegrams that arrived between knxd's
 *     position report and our next cache_read().  The original didn't need
 *     this because it maintained a local cache.
 *   - Does NOT send GroupValueRead on cache miss — flooding knxd's tunnel
 *     with read requests causes "Link down, terminating" on busy systems.
 */

#ifndef COMETVISU_KNXD_FCGI_READ_HANDLER_H_
#define COMETVISU_KNXD_FCGI_READ_HANDLER_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cvknxd {

class KnxdClientInterface;
class SessionStore;

/**
 * @brief Result of a read operation (GET /r).
 */
struct ReadResult {
  /// HTTP status code (200, 400, 401, 403, 404).
  int http_status = 200;
  /// @brief JSON response body (e.g. {"d":{"1/2/3":"42"},"i":"141"}).
  std::string body;
  /// @brief New index (position) for the client to pass as `i` in the next
  ///        read request.  Always sourced from knxd's authoritative position.
  std::string index;
};

/**
 * @brief Handles CometVisu read requests: `GET /r?s=SESSION&a=ADDRESS&t=TIMEOUT&i=INDEX`
 *
 * This is the most complex handler due to COMET (long-poll) support.
 * The request can block for up to @p longpoll_timeout_sec seconds waiting
 * for a value change, burning zero CPU via poll()-based sleep on the knxd
 * socket.
 *
 * Uses knxd's built-in group cache via cache_read() and cache_last_updates_2()
 * — no local cache duplication, unlike the reference eibread-cgi.
 *
 * Three KNX semantic correctness rules are enforced (see AGENTS.md):
 *   -# No duplicate delivery — values are deduplicated within a response.
 *   -# Immediate delivery — group telegrams wake the handler immediately.
 *   -# Authoritative index — the `i` field always comes from knxd.
 */
class ReadHandler {
public:
  ReadHandler(KnxdClientInterface& knxd, SessionStore& sessions, int longpoll_timeout_sec = 300);

  ~ReadHandler() = default;

  // Reference members prevent copy/move.
  ReadHandler(const ReadHandler&) = delete;
  ReadHandler& operator=(const ReadHandler&) = delete;
  ReadHandler(ReadHandler&&) = delete;
  ReadHandler& operator=(ReadHandler&&) = delete;

  /// @brief Process a read request.
  /// @param query_string Raw QUERY_STRING from FCGI (e.g. "a=1/2/3&t=5&i=140").
  /// @return ReadResult with HTTP status code and JSON body.
  [[nodiscard]] ReadResult handle(std::string_view query_string);

private:
  KnxdClientInterface& knxd_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
  SessionStore& sessions_;     // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
  int longpoll_timeout_sec_;

  /// @brief Parse the `t` (timeout) parameter from a query string value.
  /// @return The parsed integer, or std::nullopt if the string is not a
  ///         valid non-negative integer.
  [[nodiscard]] static std::optional<int> parse_timeout(std::string_view t_str);
};

}  // namespace cvknxd

#endif  // COMETVISU_KNXD_FCGI_READ_HANDLER_H_
