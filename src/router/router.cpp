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

#include "router.h"

#include "../util/query_string.h"

namespace cvknxd {

Router::Router(KnxdClientInterface& knxd, SessionStore& sessions, AddressCache& cache,
               LongPollManager& long_poll, int longpoll_timeout_sec)
    : login_handler_(sessions),
      read_handler_(knxd, cache, long_poll, sessions, longpoll_timeout_sec),
      write_handler_(knxd, cache, sessions) {}

FcgiResponse Router::route(const FcgiRequest& request) {
  // Determine the endpoint from the path
  // CometVisu uses: /l (login), /r (read), /w (write), /f (filter)
  // The path_info or script_name typically contains the endpoint
  // e.g. SCRIPT_NAME=/cgi-bin/visu, PATH_INFO=/l
  std::string endpoint;
  if (!request.path_info.empty()) {
    endpoint = request.path_info;
  } else {
    // Try to extract from request_uri
    endpoint = request.request_uri;
    // Strip query string
    auto qpos = endpoint.find('?');
    if (qpos != std::string::npos) {
      endpoint = endpoint.substr(0, qpos);
    }
  }

  FcgiResponse response;

  // Simple path dispatch
  if (endpoint == "/l") {
    std::string body = login_handler_.handle(request.query_string);
    response.body = std::move(body);
  } else if (endpoint == "/r") {
    auto result = read_handler_.handle(request.query_string);
    response.status_code = result.http_status;
    response.body = std::move(result.body);
  } else if (endpoint == "/w") {
    auto result = write_handler_.handle(request.query_string);
    response.status_code = result.http_status;
  } else {
    // Unknown endpoint
    response.status_code = 404;
    response.body = "{}";
  }

  return response;
}

}  // namespace cvknxd
