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
 * @file read_handler.cpp
 * @brief CometVisu /r endpoint — shared-memory cache based.
 *
 * The shared cache (SharedGroupCache) is populated by a dedicated cache
 * reader process.  This handler queries the cache directly.  Long-poll
 * blocks on the shared condition variable — zero CPU, immediate wake
 * on new data.
 *
 * The `i` value always comes from the shared cache's position counter,
 * which is monotonically increasing across all worker processes.
 */

#include "read_handler.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "knxd/knxd_client.h"
#include "knxd/knxd_protocol.h"
#include "state/session_store.h"
#include "state/shared_group_cache.h"
#include "util/hex.h"
#include "util/json_builder.h"
#include "util/query_string.h"

namespace cvknxd {

ReadHandler::ReadHandler(SharedGroupCache& cache, KnxdClientInterface& knxd, SessionStore& sessions,
                         int longpoll_timeout_sec)
    : cache_(cache),
      knxd_(knxd),
      sessions_(sessions),
      longpoll_timeout_sec_(longpoll_timeout_sec) {}

std::optional<int> ReadHandler::parse_timeout(std::string_view t_str) {
  if (t_str.empty()) {
    return std::nullopt;
  }
  int val = 0;
  const auto [ptr, ec] = std::from_chars(t_str.data(), t_str.data() + t_str.size(), val);
  if (ec != std::errc{}) {
    return std::nullopt;
  }
  if (ptr != t_str.data() + t_str.size()) {
    return std::nullopt;
  }
  return val;
}

ReadResult ReadHandler::handle(std::string_view query_string) {
  const QueryString params{query_string};
  ReadResult result;

  // ---- Parse parameters ----
  const auto addresses = params.get_all("a");
  if (addresses.empty()) {
    result.http_status = 400;
    result.body = R"({"error":"missing address"})";
    return result;
  }
  if (auto s_opt = params.get("s")) {
    if (!sessions_.is_valid(*s_opt)) {
      result.http_status = 401;
      result.body = R"({"error":"invalid session"})";
      return result;
    }
  }
  // ---- Parse t= parameter (poll timeout AND age filter) ----
  // Per spec (https://github.com/CometVisu/CometVisu/wiki/Protokoll):
  //   t not provided → long-poll, no age filter
  //   t = 0         → read from bus (we approximate: force initial cache read)
  //   t > 0         → cached data ≤ t seconds old, then poll for t seconds
  //   t < 0         → cache only, no bus read (no poll, no age filter)
  int timeout_sec = longpoll_timeout_sec_;
  bool t_provided = false;
  int t_original = 0;
  if (auto t_opt = params.get("t")) {
    const auto parsed = parse_timeout(*t_opt);
    if (!parsed) {
      result.http_status = 400;
      result.body = R"({"error":"invalid timeout"})";
      return result;
    }
    t_provided = true;
    t_original = *parsed;
    timeout_sec = *parsed;
  }

  // Age filter: only active when t > 0 (spec: "maximal TIMEOUT Sekunden alt").
  int max_age_sec = -1;  // -1 = no age filter
  if (t_provided && t_original > 0) {
    max_age_sec = t_original;
  }

  uint32_t lastpos = 0;
  if (auto i_opt = params.get("i")) {
    const auto parsed = parse_timeout(*i_opt);
    if (parsed && *parsed >= 0) {
      lastpos = static_cast<uint32_t>(*parsed);
    }
  }

  // t=0: force initial read (spec: "versucht das Backend die Daten zu lesen").
  // t<0: force initial read + no poll (spec: "nur aus dem Cache gelesen").
  if (t_provided && t_original == 0) {
    lastpos = 0;
    timeout_sec = 1;  // short poll for immediate return after initial read
  } else if (t_provided && t_original < 0) {
    lastpos = 0;
    timeout_sec = 0;  // no poll — return immediately
  }

  // ---- Build EIB address set ----
  std::set<uint16_t> eib_addrs;
  for (const auto& addr_str : addresses) {
    const auto parsed = KnxAddress::from_cometvisu(addr_str);
    if (parsed) {
      eib_addrs.insert(parsed->group.to_eibaddr());
    }
  }
  if (eib_addrs.empty()) {
    result.http_status = 404;
    result.body = R"({"error":"no valid addresses"})";
    return result;
  }

  auto addr_key = [](uint16_t a) -> std::string {
    return KnxAddress{.ns = std::string{KnxAddress::get_default_namespace()},
                      .group = KnxGroupAddress::from_eibaddr(a)}
        .to_cometvisu();
  };

  JsonBuilder json;
  json.start_object();
  json.add_key("d");
  json.start_object();
  std::set<uint16_t> already_written;

  // ---- Initial read: latest values from shared cache ----
  // max_age_sec follows the spec:
  //   t not provided → -1 (no age filter, long-poll initial read)
  //   t = 0         → -1 (no age filter, "read from bus")
  //   t > 0         → t  (spec: "maximal TIMEOUT Sekunden alt")
  //   t < 0         → -1 (no age filter, "nur aus dem Cache")
  if (lastpos == 0) {
    for (auto addr : eib_addrs) {
      auto cached = cache_.get(addr, max_age_sec);
      if (cached) {
        json.add_string(addr_key(addr), hex_encode(cached->data(), cached->size()));
        already_written.insert(addr);
      }
    }
    lastpos = cache_.position();
  }

  // ---- t=0: send GroupValueRead for uncached addresses ----
  // Per spec, t=0 means "read from bus NOW" — for any address not in the cache,
  // issue a GroupValueRead telegram.  The response arrives asynchronously and is
  // handled like a normal bus write by the cache reader process.  The response
  // to THIS request is returned immediately (non-blocking).
  if (t_provided && t_original == 0) {
    // Build GroupValueRead APDU: [0x00, 0x00] = A_GroupValue_Read
    const std::vector<uint8_t> read_apdu = {0x00, 0x00};
    for (auto addr : eib_addrs) {
      if (!already_written.contains(addr)) {
        knxd_.send_group_packet(addr, read_apdu);
      }
    }
  }

  // ---- Poll loop — wait on shared condition variable, query delta ----
  auto tstart = std::chrono::steady_clock::now();
  bool written = !already_written.empty();

  while (timeout_sec > 0) {
    // If the initial read already delivered data, skip the poll loop.
    if (written) {
      break;
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - tstart)
            .count();
    int remaining = timeout_sec - static_cast<int>(elapsed);
    if (remaining <= 0) {
      break;
    }

    // Block on the shared condition variable — zero CPU.
    // Wakes immediately when the cache reader pushes new data.
    bool new_data = cache_.wait_for_new_data(remaining * 1000);
    if (!new_data) {
      break;
    }

    // Query cache for entries newer than the client's last-known position.
    // max_age_sec follows the spec:
    //   t > 0 → entries ≤ t seconds old; t=0/t<0/no-t → no age filter.
    // The position-based filter (pushed_at > lastpos) guarantees we only
    // return new data.  Age filtering is an additional constraint per spec.
    auto delta = cache_.get_delta(lastpos, eib_addrs, max_age_sec);

    for (const auto& [addr, val] : delta.values) {
      if (!already_written.contains(addr)) {
        json.add_string(addr_key(addr), hex_encode(val.data(), val.size()));
        already_written.insert(addr);
        written = true;
      }
    }

    // Position comes from the shared cache — single source of truth,
    // monotonically increasing across all workers.
    lastpos = delta.position;

    if (written) {
      break;
    }
  }

  json.end_object();  // d
  json.add_number("i", lastpos);
  json.end_object();  // root

  result.body = json.take();
  return result;
}

}  // namespace cvknxd