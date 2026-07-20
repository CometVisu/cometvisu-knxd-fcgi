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
 * The handler mirrors the logic of the reference eibread-cgi.c but with
 * several modernizations:
 *   - cache_last_updates_2 (32-bit) instead of cache_last_updates (16-bit)
 *   - No local cache — delegates to knxd's built-in cache
 *   - Does not send GroupValueRead on cache miss (avoids tunnel flooding)
 *   - Deduplication via already_written set
 *   - Belt-and-suspenders group telegram drain after cache poll
 *
 * The poll loop is the heart of COMET long-poll.  It uses cache_last_updates_2
 * to block until a telegram arrives or the timeout expires.  This is zero-CPU
 * because the kernel puts the process to sleep on the knxd socket fd.
 */

#include "read_handler.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "knxd/knxd_client.h"
#include "knxd/knxd_protocol.h"
#include "state/session_store.h"
#include "util/hex.h"
#include "util/json_builder.h"
#include "util/query_string.h"

namespace cvknxd {

ReadHandler::ReadHandler(KnxdClientInterface& knxd, SessionStore& sessions,
                         int longpoll_timeout_sec)
    : knxd_(knxd), sessions_(sessions), longpoll_timeout_sec_(longpoll_timeout_sec) {}

std::optional<int> ReadHandler::parse_timeout(std::string_view t_str) {
  // Parses the `t` (timeout) and `i` (index/position) parameters.
  // Both are simple non-negative integers in the CometVisu protocol.
  // Rejects trailing garbage (e.g. "5abc" is invalid).
  if (t_str.empty()) {
    return std::nullopt;
  }
  int val = 0;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto [ptr, ec] = std::from_chars(t_str.data(), t_str.data() + t_str.size(), val);
  if (ec != std::errc{}) {
    return std::nullopt;
  }
  // Check for trailing garbage
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (ptr != t_str.data() + t_str.size()) {
    return std::nullopt;
  }
  return val;
}

ReadResult ReadHandler::handle(std::string_view query_string) {
  const QueryString params{query_string};
  ReadResult result;

  // ---- Get addresses first (before anything else, so 400 takes priority) ----
  const auto addresses = params.get_all("a");
  if (addresses.empty()) {
    result.http_status = 400;
    result.body = R"({"error":"missing address"})";
    return result;
  }

  // ---- Session validation ----
  // Sessions are optional for reads (anonymous access), but if provided,
  // they must be valid.  The reference eibread-cgi did not have sessions;
  // this is an extension for authenticated CometVisu installations.
  if (auto s_opt = params.get("s")) {
    if (!sessions_.is_valid(*s_opt)) {
      result.http_status = 401;
      result.body = R"({"error":"invalid session"})";
      return result;
    }
  }

  // ---- Parse timeout (t parameter) ----
  // Semantics from the reference eibread-cgi:
  //   t is the timeout in seconds for the poll loop.
  //   If t is not specified, use the configured default (LONGPOLL_TIMEOUT_SEC).
  //   If t == 0: force an initial read (lastpos=0) with a 1-second timeout.
  int timeout_sec = longpoll_timeout_sec_;
  if (auto t_opt = params.get("t")) {
    const auto parsed = parse_timeout(*t_opt);
    if (!parsed.has_value()) {
      result.http_status = 400;
      result.body = R"({"error":"invalid timeout"})";
      return result;
    }
    timeout_sec = *parsed;
  }

  // ---- Parse position (i parameter) ----
  // `i` is the last known position from knxd, equivalent to `lastpos` in
  // eibread-cgi.  0 means the client has no prior state — we do an initial
  // cache read for all requested addresses.
  //
  // Invalid `i` values are silently ignored (treated as 0), matching the
  // reference behaviour.
  uint32_t lastpos = 0;
  if (auto i_opt = params.get("i")) {
    const auto parsed = parse_timeout(*i_opt);  // reuse int parser
    if (parsed.has_value() && *parsed >= 0) {
      lastpos = static_cast<uint32_t>(*parsed);
    }
    // Invalid i is ignored (not an error)
  }

  // ---- t=0 special handling: force initial read ----
  if (timeout_sec == 0) {
    lastpos = 0;
    timeout_sec = 1;
  }

  // ---- Collect EIB addresses and build lookup set ----
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

  // Helper: build the key for an address in the JSON response.
  auto addr_key = [](uint16_t eib_addr) -> std::string {
    return KnxAddress{.ns = std::string{KnxAddress::get_default_namespace()},
                      .group = KnxGroupAddress::from_eibaddr(eib_addr)}
        .to_cometvisu();
  };

  JsonBuilder json;
  json.start_object();
  json.add_key("d");
  json.start_object();
  bool written = false;

  // Track which addresses we've already included (deduplication).
  std::set<uint16_t> already_written;

  // ---- Initial read (if lastpos == 0) ----
  // Reads ALL requested addresses from the knxd cache synchronously.
  // This matches eibread-cgi's initial loop: for each subscribed address,
  // read the cached value.  Addresses not in cache are silently omitted —
  // the poll loop below will catch them when a telegram arrives.
  //
  // We intentionally do NOT send GroupValueRead for cache misses.
  // eibread-cgi did this (sending a read request to trigger a response),
  // but with the fork-based worker pool, multiple workers doing
  // simultaneous initial reads would flood knxd's IP tunnel with read
  // requests, causing retry exhaustion and fatal "Link down, terminating".
  if (lastpos == 0) {
    for (auto addr : eib_addrs) {
      const auto data = knxd_.cache_read(addr, true);  // nowait (cache_read filters out Read APDUs)
      if (data) {
        json.add_string(addr_key(addr), hex_encode(data->data(), data->size()));
        already_written.insert(addr);
        written = true;
      }
    }
  }

  // ---- Poll loop (COMET/long-poll) ----
  // This is the equivalent of eibread-cgi's cache_last_updates loop.
  // We call cache_last_updates_2 which blocks until a telegram arrives
  // or the timeout expires, then returns all changed addresses with the
  // authoritative new_position.
  //
  // The loop continues while: (a) nothing was written yet OR (b) the
  // position is still 0 (initial read didn't find anything), AND (c)
  // there is time remaining.
  auto tstart = std::chrono::steady_clock::now();
  int timeout_retries = 0;
  static constexpr int kMaxTimeoutRetries = 3;

  while ((!written || lastpos < 1) && timeout_sec > 0) {
    // Calculate remaining time
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - tstart)
            .count();
    int remaining = timeout_sec - static_cast<int>(elapsed);
    if (remaining <= 0) {
      break;
    }

    // ---- Poll knxd's cache for position updates. ----
    // cache_last_updates_2 blocks until a telegram arrives or the
    // timeout expires, then returns ALL changed addresses with the
    // authoritative position (new_position).  This single call
    // satisfies all three KNX rules:
    //   Rule 1 (no duplicates): position comes from the same response
    //           as the changed address list, so the client's next
    //           request won't re-fetch the same data.
    //   Rule 2 (immediate delivery): knxd responds as soon as a
    //           telegram arrives — zero-CPU polling.
    //   Rule 3 (authoritative index): new_position comes from knxd,
    //           never fabricated locally.
    const auto updates = knxd_.cache_last_updates_2(lastpos, remaining);
    if (!updates.has_value()) {
      // Cache poll failed (connection loss, timeout, etc.)
      if (!knxd_.is_connected()) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed_sec =
            std::chrono::duration_cast<std::chrono::seconds>(now - tstart).count();
        if (elapsed_sec < timeout_sec && (timeout_sec - elapsed_sec) > 1) {
          (void)knxd_.reconnect();
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          continue;
        }
      }
      // Transient failure — brief sleep and retry.
      if (remaining > 0 && timeout_retries < kMaxTimeoutRetries) {
        timeout_retries++;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      break;
    }

    // Successful call — position from knxd (authoritative).
    timeout_retries = 0;
    lastpos = updates->new_position;

    // ---- Process changed addresses from knxd's cache. ----
    for (const auto changed_addr : updates->changed_addresses) {
      if (!eib_addrs.contains(changed_addr)) {
        continue;
      }
      if (already_written.contains(changed_addr)) {
        continue;
      }
      const auto data = knxd_.cache_read(changed_addr, true);  // nowait
      if (data) {
        json.add_string(addr_key(changed_addr), hex_encode(data->data(), data->size()));
        already_written.insert(changed_addr);
        written = true;
      }
    }

    // ---- Belt-and-suspenders: drain group telegrams that arrived
    // during the cache poll, after knxd reported its position. ----
    {
      uint16_t telegram_addr = 0;
      std::vector<uint8_t> telegram_apdu;
      while (knxd_.poll_group_telegram(telegram_addr, telegram_apdu)) {
        if (eib_addrs.contains(telegram_addr) && !already_written.contains(telegram_addr)) {
          ApduType apdu_type{};
          std::vector<uint8_t> value_data;
          if (parse_apdu(telegram_apdu, apdu_type, value_data) && apdu_type != ApduType::Read) {
            json.add_string(addr_key(telegram_addr),
                            hex_encode(value_data.data(), value_data.size()));
            already_written.insert(telegram_addr);
            written = true;
          }
        }
      }
    }

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
