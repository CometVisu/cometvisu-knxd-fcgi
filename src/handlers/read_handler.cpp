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
 * @brief Implementation of the CometVisu /r (read) endpoint handler.
 *
 * Like the reference eibread-cgi.c, maintains a local GroupCache updated
 * from APDU_PACKET telegrams on the group socket.  This avoids all knxd
 * cache round-trips — data comes from GroupCache, position (`i`) from
 * the group telegram counter.  cache_last_updates_2 is not used.
 *
 * The poll loop blocks via wait_for_activity() on the group socket fd,
 * burning zero CPU because the kernel puts the process to sleep.
 */

#include "read_handler.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "knxd/knxd_client.h"
#include "knxd/knxd_protocol.h"
#include "state/group_cache.h"
#include "state/session_store.h"
#include "util/hex.h"
#include "util/json_builder.h"
#include "util/query_string.h"

namespace cvknxd {

ReadHandler::ReadHandler(KnxdClientInterface& knxd, GroupCache& cache, SessionStore& sessions,
                         int longpoll_timeout_sec)
    : knxd_(knxd),
      cache_(cache),
      sessions_(sessions),
      longpoll_timeout_sec_(longpoll_timeout_sec) {}

std::optional<int> ReadHandler::parse_timeout(std::string_view t_str) {
  if (t_str.empty())
    return std::nullopt;
  int val = 0;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto [ptr, ec] = std::from_chars(t_str.data(), t_str.data() + t_str.size(), val);
  if (ec != std::errc{})
    return std::nullopt;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (ptr != t_str.data() + t_str.size())
    return std::nullopt;
  return val;
}

/// Drain all pending group telegrams: parse APDU, update GroupCache,
/// and collect hex-encoded values for subscribed addresses.
static void drain_and_cache(KnxdClientInterface& knxd, GroupCache& cache,
                            const std::set<uint16_t>& eib_addrs,
                            std::unordered_map<uint16_t, std::string>& out_matches) {
  uint16_t addr = 0;
  std::vector<uint8_t> apdu;
  while (knxd.poll_group_telegram(addr, apdu)) {
    ApduType apdu_type{};
    std::vector<uint8_t> value_data;
    if (!parse_apdu(apdu, apdu_type, value_data) || apdu_type == ApduType::Read)
      continue;
    cache.update(addr, value_data);
    if (eib_addrs.contains(addr)) {
      out_matches[addr] = hex_encode(value_data.data(), value_data.size());
    }
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
    if (parsed && *parsed >= 0)
      lastpos = static_cast<uint32_t>(*parsed);
  }
  if (timeout_sec == 0) {
    lastpos = 0;
    timeout_sec = 1;
  }

  // ---- Build EIB address set ----
  std::set<uint16_t> eib_addrs;
  for (const auto& addr_str : addresses) {
    const auto parsed = KnxAddress::from_cometvisu(addr_str);
    if (parsed)
      eib_addrs.insert(parsed->group.to_eibaddr());
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
  bool written = false;
  std::set<uint16_t> already_written;

  // ---- Initial read from GroupCache (lastpos == 0) ----
  if (lastpos == 0) {
    for (auto addr : eib_addrs) {
      auto cached = cache_.get(addr);
      if (cached) {
        json.add_string(addr_key(addr), hex_encode(cached->data(), cached->size()));
        already_written.insert(addr);
        written = true;
      }
    }
  }

  // ---- Poll loop — wait_for_activity, drain, deliver ----
  auto tstart = std::chrono::steady_clock::now();

  while ((!written || lastpos < 1) && timeout_sec > 0) {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - tstart)
            .count();
    int remaining = timeout_sec - static_cast<int>(elapsed);
    if (remaining <= 0)
      break;

    // Block on both the group socket and cache connection.  Either waking
    // indicates activity (a group telegram or cache update).  Zero CPU.
    auto activity = knxd_.wait_for_activity(remaining * 1000);
    if (activity == KnxdClientInterface::WaitResult::Timeout)
      break;

    // Drain group telegrams into GroupCache, collect matches.
    std::unordered_map<uint16_t, std::string> matches;
    drain_and_cache(knxd_, cache_, eib_addrs, matches);

    for (const auto& [addr, hex_val] : matches) {
      if (!already_written.contains(addr)) {
        json.add_string(addr_key(addr), hex_val);
        already_written.insert(addr);
        written = true;
      }
    }

    // Position: group-telegram counter, monotonically increasing.
    // Every APDU_PACKET advances it — no duplicate i values possible.
    lastpos = static_cast<uint32_t>(knxd_.get_telegram_count());

    if (written)
      break;
  }

  json.end_object();  // d
  json.add_number("i", lastpos);
  json.end_object();  // root

  result.body = json.take();
  return result;
}

}  // namespace cvknxd
