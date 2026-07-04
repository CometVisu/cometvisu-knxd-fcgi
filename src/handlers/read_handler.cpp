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

#include "read_handler.h"

#include <set>
#include <thread>

#include "../knxd/knxd_client.h"
#include "../knxd/knxd_protocol.h"
#include "../state/address_cache.h"
#include "../state/long_poll.h"
#include "../util/hex.h"
#include "../util/json_builder.h"
#include "../util/query_string.h"

namespace cvknxd {

ReadHandler::ReadHandler(KnxdClientInterface& knxd, AddressCache& cache, LongPollManager& long_poll)
    : knxd_(knxd), cache_(cache), long_poll_(long_poll) {}

std::string ReadHandler::generate_index() {
  return std::to_string(index_counter_++);
}

ReadResult ReadHandler::handle(std::string_view query_string) {
  QueryString params{query_string};
  ReadResult result;

  // Get addresses to read
  auto addresses = params.get_all("a");
  if (addresses.empty()) {
    result.http_status = 400;
    result.body = "{}";
    return result;
  }

  // Parse timeout
  int timeout = -999;  // sentinel: not provided → long-poll
  if (auto t_opt = params.get("t")) {
    timeout = std::stoi(std::string{*t_opt});
  }

  // Collect EIB addresses to query
  std::set<uint16_t> eib_addrs;
  for (auto addr_str : addresses) {
    auto parsed = KnxAddress::from_cometvisu(addr_str);
    if (parsed) {
      eib_addrs.insert(parsed->group.to_eibaddr());
    }
    // Unknown addresses are ignored (protocol doesn't specify error per-address)
  }

  if (eib_addrs.empty()) {
    result.http_status = 404;
    result.body = "{}";
    return result;
  }

  // Long-poll mode: no timeout parameter provided
  if (timeout == -999) {
    // In single-threaded FCGI, long-poll is cooperative.
    // We poll the knxd socket for new data for a configurable duration.
    // This is a simplified blocking implementation.
    constexpr int kLongPollMaxSeconds = 60;

    // Check cache first for any recent values
    JsonBuilder json;
    json.start_object();
    json.add_key("d");
    json.start_object();

    bool has_data = false;
    for (auto addr : eib_addrs) {
      auto cached = cache_.get(addr, 5);  // 5 second freshness
      if (cached) {
        auto cv_addr = KnxAddress{"KNX", KnxGroupAddress::from_eibaddr(addr)};
        json.add_string(cv_addr.to_cometvisu(), hex_encode(cached->data(), cached->size()));
        has_data = true;
      }
    }
    json.end_object();

    if (has_data) {
      json.add_string("i", generate_index());
      json.end_object();
      result.body = json.take();
      return result;
    }

    // Poll knxd for new data (blocking, with timeout)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kLongPollMaxSeconds);

    while (std::chrono::steady_clock::now() < deadline) {
      uint16_t recv_addr;
      std::vector<uint8_t> apdu_data;

      if (knxd_.poll_group_telegram(recv_addr, apdu_data)) {
        // Update cache
        cache_.update(recv_addr, apdu_data);

        // Check if this is an address we're waiting for
        if (eib_addrs.find(recv_addr) != eib_addrs.end()) {
          JsonBuilder resp;
          resp.start_object();
          resp.add_key("d");
          resp.start_object();
          auto cv_addr = KnxAddress{"KNX", KnxGroupAddress::from_eibaddr(recv_addr)};
          resp.add_string(cv_addr.to_cometvisu(), hex_encode(apdu_data.data(), apdu_data.size()));
          resp.end_object();
          resp.add_string("i", generate_index());
          resp.end_object();
          result.body = resp.take();
          return result;
        }

        // Also notify long-poll manager for other waiters
        long_poll_.notify(recv_addr, hex_encode(apdu_data.data(), apdu_data.size()));
      }

      // Small sleep to avoid busy-waiting
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Timeout: return empty response
    JsonBuilder resp;
    resp.start_object();
    resp.add_key("d");
    resp.start_object();
    resp.end_object();
    resp.add_string("i", generate_index());
    resp.end_object();
    result.body = resp.take();
    return result;
  }

  // Non-long-poll: timeout provided
  JsonBuilder json;
  json.start_object();
  json.add_key("d");
  json.start_object();

  bool any_found = false;

  for (auto addr : eib_addrs) {
    std::optional<std::vector<uint8_t>> data;

    if (timeout > 0) {
      // Try cache first
      data = cache_.get(addr, timeout);
      if (!data) {
        // Read from knxd cache
        data = knxd_.cache_read(addr, true);
        if (data) {
          cache_.update(addr, *data);
        }
      }
    } else if (timeout == 0) {
      // Read from bus directly (knxd cache, blocking)
      data = knxd_.cache_read(addr, false);
      if (data) {
        cache_.update(addr, *data);
      }
    } else {
      // timeout < 0: cache only
      data = cache_.get_any(addr);
    }

    if (data) {
      auto cv_addr = KnxAddress{"KNX", KnxGroupAddress::from_eibaddr(addr)};
      json.add_string(cv_addr.to_cometvisu(), hex_encode(data->data(), data->size()));
      any_found = true;
    }
  }

  json.end_object();
  json.add_string("i", generate_index());
  json.end_object();

  if (!any_found) {
    result.http_status = 404;
  }

  result.body = json.take();
  return result;
}

}  // namespace cvknxd
