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
 * @brief CometVisu /r endpoint — ring-cache based, single knxd connection.
 *
 * Group telegrams are drained from the group socket, pushed into
 * GroupCache (which owns the authoritative position), and delivered
 * to clients.  The `i` value always comes from cache_.position() —
 * never fabricated or manipulated.  wait_for_activity() blocks on
 * the group socket fd with zero CPU via kernel poll().
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
#include "state/group_cache.h"
#include "state/session_store.h"
#include "util/hex.h"
#include "util/json_builder.h"
#include "util/query_string.h"

namespace cvknxd {

ReadHandler::ReadHandler(KnxdClientInterface& knxd, GroupCache& cache,
                         SessionStore& sessions, int longpoll_timeout_sec)
    : knxd_(knxd),
      cache_(cache),
      sessions_(sessions),
      longpoll_timeout_sec_(longpoll_timeout_sec) {}

std::optional<int> ReadHandler::parse_timeout(std::string_view t_str) {
  if (t_str.empty()) { return std::nullopt; }
  int val = 0;
  const auto [ptr, ec] =
      std::from_chars(t_str.data(), t_str.data() + t_str.size(), val);
  if (ec != std::errc{}) { return std::nullopt; }
  if (ptr != t_str.data() + t_str.size()) { return std::nullopt; }
  return val;
}

/// Drain all pending group telegrams from the socket into GroupCache.
/// The cache position advances automatically on each push().
static void drain_into_cache(KnxdClientInterface& knxd, GroupCache& cache) {
  uint16_t addr = 0;
  std::vector<uint8_t> apdu;
  while (knxd.poll_group_telegram(addr, apdu)) {
    ApduType apdu_type{};
    std::vector<uint8_t> value_data;
    if (!parse_apdu(apdu, apdu_type, value_data) ||
        apdu_type == ApduType::Read) {
      continue;
    }
    cache.push(addr, value_data);
  }
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
  int timeout_sec = longpoll_timeout_sec_;
  if (auto t_opt = params.get("t")) {
    const auto parsed = parse_timeout(*t_opt);
    if (!parsed) {
      result.http_status = 400;
      result.body = R"({"error":"invalid timeout"})";
      return result;
    }
    timeout_sec = *parsed;
  }
  uint32_t lastpos = 0;
  if (auto i_opt = params.get("i")) {
    const auto parsed = parse_timeout(*i_opt);
    if (parsed && *parsed >= 0) { lastpos = static_cast<uint32_t>(*parsed); }
  }
  if (timeout_sec == 0) {
    lastpos = 0;
    timeout_sec = 1;
  }

  // ---- Build EIB address set ----
  std::set<uint16_t> eib_addrs;
  for (const auto& addr_str : addresses) {
    const auto parsed = KnxAddress::from_cometvisu(addr_str);
    if (parsed) { eib_addrs.insert(parsed->group.to_eibaddr()); }
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

  // ---- Initial read: latest values from GroupCache ----
  // The cache position is authoritative — we use it as i= directly.
  if (lastpos == 0) {
    for (auto addr : eib_addrs) {
      auto cached = cache_.get(addr, timeout_sec);
      if (cached) {
        json.add_string(addr_key(addr),
                        hex_encode(cached->data(), cached->size()));
        already_written.insert(addr);
      }
    }
    lastpos = cache_.position();
  }

  // ---- Poll loop — wait, drain, query cache delta ----
  auto tstart = std::chrono::steady_clock::now();
  bool written = !already_written.empty();

  while (timeout_sec > 0) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - tstart)
                             .count();
    int remaining = timeout_sec - static_cast<int>(elapsed);
    if (remaining <= 0) { break; }

    // Block on the group socket fd — kernel puts process to sleep.
    // Wakes immediately when a group telegram (including /w echo) arrives.
    auto activity = knxd_.wait_for_activity(remaining * 1000);
    if (activity == KnxdClientInterface::WaitResult::Timeout) { break; }

    // Push all pending telegrams into the cache (position auto-advances).
    drain_into_cache(knxd_, cache_);

    // Query cache for entries newer than the client's last-known position.
    auto delta = cache_.get_delta(lastpos, eib_addrs, timeout_sec);

    for (const auto& [addr, val] : delta.values) {
      if (!already_written.contains(addr)) {
        json.add_string(addr_key(addr),
                        hex_encode(val.data(), val.size()));
        already_written.insert(addr);
        written = true;
      }
    }

    // Position comes from the cache — the single source of truth.
    // Never fabricated, never manipulated.
    lastpos = delta.position;

    if (written) { break; }
  }

  json.end_object();  // d
  json.add_number("i", lastpos);
  json.end_object();  // root

  result.body = json.take();
  return result;
}

}  // namespace cvknxd