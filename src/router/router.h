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
 * @file router.h
 * @brief URL router — dispatches FastCGI requests to the appropriate handler.
 *
 * Maps CometVisu protocol endpoints to their handlers:
 *   - `/l` → LoginHandler (session creation)
 *   - `/r` → ReadHandler (cache read + COMET long-poll)
 *   - `/w` → WriteHandler (group telegram write)
 *
 * The router itself has minimal logic — it simply extracts the path from the
 * request and delegates to the correct handler.  Unknown endpoints receive a
 * 404 response.
 */

#pragma once

#include "fcgi/fcgi_request.h"
#include "handlers/login_handler.h"
#include "handlers/read_handler.h"
#include "handlers/write_handler.h"

namespace cvknxd {

/**
 * @brief URL router: dispatches FCGI requests to the appropriate handler.
 *
 * Constructed with references to the knxd client and session store, which
 * are forwarded to the individual handlers.
 */
class Router {
public:
  Router(KnxdClientInterface& knxd, SessionStore& sessions, int longpoll_timeout_sec = 300,
         std::string base_url = "");

  /// @brief Dispatch a request and return the response.
  /// @param request The parsed FCGI request.
  /// @return FcgiResponse with status code, content type, and body.
  [[nodiscard]] FcgiResponse route(const FcgiRequest& request);

private:
  LoginHandler login_handler_;
  ReadHandler read_handler_;
  WriteHandler write_handler_;
};

}  // namespace cvknxd
