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

#include <cstdint>
#include <string>
#include <string_view>

namespace cvknxd {

class KnxdClientInterface;
class AddressCache;
class LongPollManager;

/// Result of a read operation.
struct ReadResult {
  /// HTTP status code (200, 401, 403, 404).
  int http_status = 200;
  /// JSON response body.
  std::string body;
  /// New index for the client to pass to next read.
  std::string index;
};

/// Handles CometVisu read requests: GET /r?s=SESSION&a=ADDRESS&t=TIMEOUT&i=INDEX
/// This is the most complex handler due to long-poll (COMET) support.
class ReadHandler {
public:
  ReadHandler(KnxdClientInterface& knxd, AddressCache& cache, LongPollManager& long_poll);

  /// Process a read request.
  /// @param query_string Raw QUERY_STRING from FCGI.
  /// @return ReadResult with status code and JSON body.
  [[nodiscard]] ReadResult handle(std::string_view query_string);

private:
  KnxdClientInterface& knxd_;
  AddressCache& cache_;
  LongPollManager& long_poll_;
  uint64_t index_counter_ = 1;

  [[nodiscard]] std::string generate_index();
};

}  // namespace cvknxd
