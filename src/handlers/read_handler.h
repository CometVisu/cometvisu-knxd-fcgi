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
 * @brief CometVisu read endpoint handler — shared cache and COMET long-poll.
 *
 * Implements the equivalent of the reference eibread-cgi.c's main loop:
 * subscribes to addresses, reads cached values on first request, then
 * blocks waiting for new data via the shared cache's condition variable.
 *
 * The shared cache (SharedGroupCache) is populated by a dedicated cache
 * reader process that drains the single knxd group socket.  Workers query
 * the cache directly — no per-worker knxd group socket needed.  This
 * ensures a single, authoritative position counter (`i`) across all
 * workers, preventing duplicate delivery and non-monotonic index bugs.
 *
 * Key differences from eibread-cgi:
 *   - Uses a shared-memory cache with process-shared mutex/condvar
 *     instead of per-process caches or knxd's built-in cache.
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
class SharedGroupCache;

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
 * position (`i`) always comes from the shared cache's position counter,
 * which is monotonically increasing across all worker processes.
 *
 * Three KNX semantic correctness rules are enforced (see AGENTS.md):
 *   -# No duplicate delivery — values are deduplicated within a response.
 *   -# Immediate delivery — group telegrams wake the handler immediately.
 *   -# Authoritative index — the `i` field always comes from knxd.
 */
class ReadHandler {
public:
  ReadHandler(SharedGroupCache& cache, SessionStore& sessions, int longpoll_timeout_sec = 300);

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
  SharedGroupCache& cache_;  // NOLINT — shared cache for group telegrams
  SessionStore& sessions_;   // NOLINT
  int longpoll_timeout_sec_;

  [[nodiscard]] static std::optional<int> parse_timeout(std::string_view t_str);
};

}  // namespace cvknxd

#endif  // COMETVISU_KNXD_FCGI_READ_HANDLER_H_
