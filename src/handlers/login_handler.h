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
 * @file login_handler.h
 * @brief CometVisu login endpoint handler — session creation.
 *
 * Equivalent to the login handler in the reference PHP/Perl backends.
 * Creates a session, returns the protocol version and session ID.
 * Extensions beyond the reference:
 *   - Anonymous sessions (id "0") when no credentials are provided
 *   - Config block with baseURL, version info, and arbitrary extra config
 *   - Session TTL and max-session enforcement
 */

#ifndef COMETVISU_KNXD_FCGI_LOGIN_HANDLER_H_
#define COMETVISU_KNXD_FCGI_LOGIN_HANDLER_H_

#include <string>
#include <string_view>

namespace cvknxd {

class SessionStore;

/**
 * @brief Handles CometVisu login requests: `GET /l?u=USER&p=PASSWORD&d=DEVICE`
 *
 * Returns a JSON response with the protocol version, session ID, and
 * optionally a configuration block ("c") with baseURL, version info,
 * and arbitrary extra configuration from the LOGIN_EXTRA_CONFIG env var.
 */
class LoginHandler {
public:
  /// @brief Construct a login handler.
  /// @param sessions Session store for creating and managing sessions.
  /// @param base_url Base URL prefix advertised in the login response "c"
  ///                 block (e.g. "/proxy/visu").  Empty = omit "c" block.
  explicit LoginHandler(SessionStore& sessions, std::string base_url = "");

  ~LoginHandler() = default;

  // Reference/const members prevent copy/move.
  LoginHandler(const LoginHandler&) = delete;
  LoginHandler& operator=(const LoginHandler&) = delete;
  LoginHandler(LoginHandler&&) = delete;
  LoginHandler& operator=(LoginHandler&&) = delete;

  /// @brief Process a login request.
  /// @param query_string Raw QUERY_STRING from FCGI (e.g. "u=admin&p=secret&d=web").
  /// @return JSON response body: `{"v":"0.0.2","s":"SESSION_ID"}` with optional
  ///         `"c":{...}` configuration block.
  [[nodiscard]] std::string handle(std::string_view query_string);

private:
  SessionStore& sessions_;      // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
  const std::string base_url_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
};

}  // namespace cvknxd

#endif  // COMETVISU_KNXD_FCGI_LOGIN_HANDLER_H_
