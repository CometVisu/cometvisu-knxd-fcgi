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
 * @file group_cache.h
 * @brief KNX group telegram cache with per-entry position and age tracking.
 *
 * Each push stores the value, a Unix epoch timestamp (for age filtering),
 * and the cache position at push time.  get_delta() returns only entries
 * whose push position is strictly greater than since_pos — guaranteeing
 * no duplicate delivery and no stale re-transmission of unchanged GAs.
 */

#ifndef COMETVISU_KNXD_FCGI_GROUP_CACHE_H_
#define COMETVISU_KNXD_FCGI_GROUP_CACHE_H_

#include <atomic>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace cvknxd {

class GroupCache {
public:
  GroupCache() = default;

  /// Push a value.  Records current position and timestamp.
  void push(uint16_t addr, const std::vector<uint8_t>& value);

  /// Get latest value for an address, optionally age-filtered.
  [[nodiscard]] std::optional<std::vector<uint8_t>> get(uint16_t addr,
                                                        int max_age_sec = -1) const;

  /// Entries newer than since_pos for subscribed addresses.
  struct Delta {
    std::unordered_map<uint16_t, std::vector<uint8_t>> values;
    uint32_t position = 0;
  };
  /// Only entries with pushed_at > since_pos are returned.
  [[nodiscard]] Delta get_delta(uint32_t since_pos,
                                const std::set<uint16_t>& subscribed,
                                int max_age_sec = -1) const;

  [[nodiscard]] uint32_t position() const { return position_.load(); }
  void clear();

private:
  struct Entry {
    std::vector<uint8_t> value;
    uint32_t timestamp;   // Unix epoch seconds — for age filtering
    uint32_t pushed_at;   // cache position when pushed — for delta queries
  };

  mutable std::mutex mutex_;
  std::unordered_map<uint16_t, Entry> entries_;
  std::atomic<uint32_t> position_{0};
};

}  // namespace cvknxd
#endif