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
  if (auto s_opt = params.get("s")) {
    if (!sessions_.is_valid(*s_opt)) {
      result.http_status = 401;
      result.body = R"({"error":"invalid session"})";
      return result;
    }
  }

  // ---- Parse timeout (t parameter) ----
  // Original semantics: t is a simple timeout in seconds for the poll loop.
  // If t is not specified, use the default longpoll timeout.
  // If t == 0: force initial read (lastpos=0) and set timeout to 1 second.
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
  // i is the last known position (like "lastpos" in the original).
  // 0 means the client has no prior state.
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
  // This matches the original: for (i = 0; i < UINT16; i++) { if (subscribed) { ... } }
  // Addresses NOT found in cache are simply omitted from the response —
  // the poll loop below will catch them when the device sends its value,
  // or when our own write triggers an APDU_PACKET on the group socket.
  // We intentionally do NOT send GroupValueRead telegrams for cache misses:
  // doing so would flood knxd's IP tunnel with read requests when many
  // workers process initial reads simultaneously, causing knxd's tunnel
  // retry mechanism to exhaust and fatally disconnect ("Link down, terminating").
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

    // ---- Step 1: Poll knxd's cache for position updates. ----
    // This is now the FIRST step in the poll loop.  Group telegrams
    // are drained in the nullopt handler (if the cache poll fails due
    // to group data) or in Step 3 (belt-and-suspenders after a
    // successful cache poll).  This ensures ALL group-telegram data
    // goes through the pending-data + position-confirmation pattern.
    //
    // cache_last_updates_2 does a combined poll on both the cache
    // connection AND the group socket.  When group data is available,
    // it returns nullopt — the handler then drains group telegrams
    // with the pending-data pattern without the cache round-trip.
    //
    // This returns knxd's AUTHORITATIVE position — we never fabricate i=.
    const auto updates = knxd_.cache_last_updates_2(lastpos, remaining);
    if (!updates.has_value()) {
      if (written) {
        // Data was found during the initial read (lastpos==0) but the
        // cache poll failed. Try to get authoritative position.
        // KNX Rule 3: i must come from knxd, never fabricated.
        if (!knxd_.is_connected()) {
          (void)knxd_.reconnect();
        }
        auto pos = knxd_.cache_last_updates_2(lastpos, 0);
        if (pos.has_value() && pos->new_position > lastpos) {
          lastpos = pos->new_position;
          break;
        }
        // Position not confirmed — retry up to kMaxTimeoutRetries times,
        // then break.  This is the edge case where knxd is persistently
        // unresponsive; delivering cached data with a stale position is
        // better than losing the data entirely.  (KNX Rule 1: this may
        // cause re-delivery on the next poll, but knxd being down is the
        // bigger problem.)
        if (timeout_retries < kMaxTimeoutRetries) {
          timeout_retries++;
          continue;
        }
        break;
      }

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

      // Drain group telegrams into a pending list BEFORE adding to JSON.
      // cache_last_updates_2 now does a combined poll (group socket + cache
      // connection) and returns nullopt when group data is available.
      //
      // KNX Rule 2 (immediate delivery): we drain group telegrams here
      // without waiting for a cache round-trip.
      //
      // KNX Rule 1 (no duplicates): we only deliver after confirming the
      // authoritative position (Rule 3).  If position hasn't advanced,
      // we discard pending data — the cache poll on the next iteration
      // will re-deliver it with the correct position.
      {
        struct PendingMatch {
          uint16_t addr;
          std::vector<uint8_t> value_data;
        };
        std::vector<PendingMatch> pending;

        uint16_t telegram_addr = 0;
        std::vector<uint8_t> telegram_apdu;
        while (knxd_.poll_group_telegram(telegram_addr, telegram_apdu)) {
          if (eib_addrs.contains(telegram_addr) && !already_written.contains(telegram_addr)) {
            ApduType apdu_type{};
            std::vector<uint8_t> value_data;
            if (parse_apdu(telegram_apdu, apdu_type, value_data) && apdu_type != ApduType::Read) {
              pending.push_back({telegram_addr, std::move(value_data)});
              already_written.insert(telegram_addr);
            }
          }
        }

        if (!pending.empty()) {
          // Confirm authoritative position (KNX Rule 3).
          // Retry up to 3 times with 10ms waits — knxd may not have
          // processed the cache update yet (rare race).
          bool position_ok = false;
          for (int retry = 0; retry < 3 && !position_ok; retry++) {
            if (!knxd_.is_connected()) {
              (void)knxd_.reconnect();
            }
            auto pos = knxd_.cache_last_updates_2(lastpos, 0);
            if (pos.has_value() && pos->new_position > lastpos) {
              lastpos = pos->new_position;
              position_ok = true;
            } else if (retry < 2) {
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
          }

          if (position_ok) {
            // Position confirmed — deliver immediately (KNX Rule 2).
            for (const auto& m : pending) {
              json.add_string(addr_key(m.addr),
                              hex_encode(m.value_data.data(), m.value_data.size()));
            }
            written = true;
            break;
          }

          // Position not confirmed after retries — DISCARD pending data.
          // KNX Rule 1: never deliver with unconfirmed position.
          // The telegrams were consumed from the group socket, but knxd
          // has updated its cache — the next cache_last_updates_2 call
          // will report the changed addresses with authoritative position.
          for (const auto& m : pending) {
            already_written.erase(m.addr);
          }
        }
      }

      // Transient failure with retries remaining — brief sleep and retry.
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

    // ---- Step 2: Process changed addresses from knxd's cache. ----
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

    // ---- Step 3: Belt-and-suspenders drain (group telegrams that arrived
    // during the cache poll, after knxd reported its position). ----
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
