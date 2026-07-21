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
 * Implements the equivalent of the reference eibread-cgi.c's main loop:
 * subscribes to addresses, reads cached values on first request, then
 * polls for changes via EIB_CACHE_LAST_UPDATES_2.
 *
 * Like the reference, maintains a local GroupCache updated from group
 * socket APDU_PACKET telegrams.  This avoids round-trips to knxd's
 * built-in cache for every value lookup, and ensures /w writes are
 * immediately visible to blocked /r readers — the group socket echo
 * updates GroupCache before the cache_last_updates_2 position arrives.
 *
 * Key differences from eibread-cgi:
 *   - Uses cache_last_updates_2 (32-bit position) instead of the older
 *     cache_last_updates (16-bit).  This avoids position wrap-around on
 *     busy installations.
 *   - Does NOT send GroupValueRead on cache miss — flooding knxd's tunnel
 *     with read requests causes "Link down, terminating" on busy systems.
 */

#ifndef COMETVISU_KNXD_FCGI_READ_HANDLER_H_
#define COMETVISU_KNXD_FCGI_READ_HANDLER_H_

#include <optional>
#include <string>
#include <string_view>

namespace cvknxd {

class KnxdClientInterface;
class SessionStore;
class GroupCache;

/**
 * @brief Result of a read operation (GET /r).
 */
struct ReadResult {
  /// HTTP status code (200, 400, 401, 403, 404).
  int http_status = 200;
  /// @brief JSON response body (e.g. {"d":{"1/2/3":"42"},"i":"141"}).
  std::string body;
};

/**
 * @brief Handles CometVisu read requests: `GET /r?s=SESSION&a=ADDRESS&t=TIMEOUT&i=INDEX`
 *
 * This is the most complex handler due to COMET (long-poll) support.
 * The request can block for up to @p longpoll_timeout_sec seconds waiting
 * for a value change, burning zero CPU via poll()-based sleep on the knxd
 * socket.
 *
 * Maintains a local GroupCache (like the reference eibread-cgi) updated
 * from APDU_PACKET telegrams on the group socket.  The authoritative
 * position (`i`) always comes from knxd's cache_last_updates_2.
 *
 * Three KNX semantic correctness rules are enforced (see AGENTS.md):
 *   -# No duplicate delivery — values are deduplicated within a response.
 *   -# Immediate delivery — group telegrams wake the handler immediately.
 *   -# Authoritative index — the `i` field always comes from knxd.
 */
class ReadHandler {
public:
  ReadHandler(KnxdClientInterface& knxd, GroupCache& cache, SessionStore& sessions,
              int longpoll_timeout_sec = 300);

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
  KnxdClientInterface& knxd_;  // NOLINT
  GroupCache& cache_;          // NOLINT — local cache for group telegrams
  SessionStore& sessions_;     // NOLINT
  int longpoll_timeout_sec_;

  [[nodiscard]] static std::optional<int> parse_timeout(std::string_view t_str);
};

}  // namespace cvknxd

#endif  // COMETVISU_KNXD_FCGI_READ_HANDLER_H_
