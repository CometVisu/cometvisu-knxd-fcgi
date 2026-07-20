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
 * @file write_handler.h
 * @brief CometVisu write endpoint handler — sends group telegrams to knxd.
 *
 * Equivalent to the reference eibwrite-cgi.c: parses the address and hex
 * value from the query string, builds an APDU, and sends it via the group
 * socket.  Key differences:
 *   - Multi-address writes: the `a` parameter can appear multiple times.
 *   - Rate limiting: enforces a minimum 50 ms interval between writes to
 *     prevent flooding knxd's IP tunnel (eibwrite-cgi had no such limit).
 *   - APDU validation: rejects values whose first byte lacks the Write APCI
 *     bit (0x80), matching eibwrite-cgi's check.
 *   - Error reporting: returns HTTP 503 if all valid writes fail, unlike
 *     eibwrite-cgi which silently ignored failures.
 */

#ifndef COMETVISU_KNXD_FCGI_WRITE_HANDLER_H_
#define COMETVISU_KNXD_FCGI_WRITE_HANDLER_H_

#include <string>
#include <string_view>

namespace cvknxd {

class KnxdClientInterface;
class SessionStore;

/**
 * @brief Result of a write operation (POST /w).
 */
struct WriteResult {
  /// HTTP status code (200, 400, 401, 403, 404).
  int http_status = 200;
  /// Response body (always empty for write).
  std::string body;
};

/**
 * @brief Handles CometVisu write requests: `POST /w?a=ADDRESS&v=HEXVALUE&s=SESSION`
 *
 * The `v` parameter contains the hex-encoded APDU value with the APCI byte
 * already set (e.g. `v=8042` for a 1-byte write of 0x42).  This matches the
 * eibwrite-cgi convention.
 */
class WriteHandler {
public:
  WriteHandler(KnxdClientInterface& knxd, SessionStore& sessions);

  ~WriteHandler() = default;

  // Reference members prevent copy/move.
  WriteHandler(const WriteHandler&) = delete;
  WriteHandler& operator=(const WriteHandler&) = delete;
  WriteHandler(WriteHandler&&) = delete;
  WriteHandler& operator=(WriteHandler&&) = delete;

  /// @brief Process a write request.
  /// @param query_string Raw QUERY_STRING from FCGI (e.g. "a=1/2/3&v=8042").
  /// @return WriteResult with HTTP status code (200, 401, 404, 503).
  [[nodiscard]] WriteResult handle(std::string_view query_string);

private:
  KnxdClientInterface& knxd_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
  SessionStore& sessions_;     // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
};

}  // namespace cvknxd

#endif  // COMETVISU_KNXD_FCGI_WRITE_HANDLER_H_
