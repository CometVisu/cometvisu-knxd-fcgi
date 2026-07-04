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

namespace cvknxd {

/// Parsed FastCGI request parameters.
struct FcgiRequest {
  std::string request_method;  // GET, POST, etc.
  std::string request_uri;     // Full request URI
  std::string query_string;    // QUERY_STRING
  std::string content_type;
  std::string content;
  std::string script_name;      // SCRIPT_NAME
  std::string path_info;        // PATH_INFO
  std::string server_protocol;  // e.g. "HTTP/1.1"

  /// Extract the path (without query string) from the URI.
  [[nodiscard]] std::string_view path() const;
};

/// FastCGI response.
struct FcgiResponse {
  int status_code = 200;
  std::string content_type = "application/json";
  std::string body;
};

}  // namespace cvknxd
