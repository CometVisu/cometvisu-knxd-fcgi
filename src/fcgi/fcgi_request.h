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

#include <string>
#include <string_view>

/**
 * @file fcgi_request.h
 * @brief Parsed FastCGI request and response data types.
 *
 * These simple POD-like structs carry the parsed FCGI parameters from the web
 * server into the handler chain.  They are deliberately kept free of any I/O
 * or protocol logic — that lives in FcgiServer.
 */

namespace cvknxd {

/**
 * @brief Parsed FastCGI request parameters.
 *
 * Holds the FCGI environment variables and (for POST/PUT) the request body.
 * The web server (Apache, nginx, lighttpd) sets these via the FCGI protocol;
 * FcgiServer::read_request() extracts them into this struct.
 */
struct FcgiRequest {
  std::string request_method;  // GET, POST, etc.
  std::string request_uri;     // Full request URI
  std::string query_string;    // QUERY_STRING
  std::string content_type;
  std::string content;
  std::string script_name;      // SCRIPT_NAME
  std::string path_info;        // PATH_INFO
  std::string server_protocol;  // e.g. "HTTP/1.1"

  /// @brief Extract the endpoint path from the URI, sans query string.
  ///
  /// Resolves the path using standard CGI precedence:
  ///   1. PATH_INFO (set by the web server for script-relative paths)
  ///   2. request_uri minus query string, with SCRIPT_NAME prefix stripped
  ///
  /// @return The endpoint path (e.g. "/l", "/r", "/w"), or empty if
  ///         neither PATH_INFO nor request_uri is available.
  [[nodiscard]] std::string_view path() const;
};

/**
 * @brief FastCGI response to be sent back to the client.
 *
 * Contains the HTTP status code, content type, and body.  FcgiServer
 * serializes this into a valid FCGI response frame.
 */
struct FcgiResponse {
  int status_code = 200;
  std::string content_type = "application/json";
  std::string body;
};

}  // namespace cvknxd
