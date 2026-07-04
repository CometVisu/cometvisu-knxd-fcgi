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

#pragma once

#include <functional>
#include <string>

#include "fcgi_request.h"

namespace cvknxd {

/// Callback type for request handlers.
/// Takes a parsed request and returns the response.
using RequestHandler = std::function<FcgiResponse(const FcgiRequest&)>;

/// Main FastCGI server: accepts requests from the web server and dispatches them.
/// Uses the libfcgi library for the FCGI protocol implementation.
class FcgiServer {
public:
  FcgiServer();

  /// Set the callback for handling requests.
  void set_handler(RequestHandler handler);

  /// Run the FCGI accept loop. Blocks until the server shuts down.
  /// @return 0 on success, non-zero on error.
  int run();

private:
  RequestHandler handler_;

  /// Read all FCGI parameters from stdin into an FcgiRequest.
  [[nodiscard]] static FcgiRequest read_request();
  /// Write an FcgiResponse to stdout in FCGI format.
  static void write_response(const FcgiResponse& response);
};

}  // namespace cvknxd
