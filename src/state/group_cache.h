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
 * @brief KNX group telegram cache with age tracking.
 *
 * Stores latest value and timestamp (Unix epoch seconds) per group address.
 * A monotonically increasing position counter advances on each push().
 * Age filtering via max_age_sec rejects entries older than the threshold.
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

  void push(uint16_t addr, const std::vector<uint8_t>& value);

  /// @param max_age_sec If >= 0, reject entries older than this (seconds).
  [[nodiscard]] std::optional<std::vector<uint8_t>> get(uint16_t addr,
                                                        int max_age_sec = -1) const;

  /// Check for new data since @p since_pos for @p subscribed addresses.
  struct Delta {
    std::unordered_map<uint16_t, std::vector<uint8_t>> values;
    uint32_t position = 0;
  };
  [[nodiscard]] Delta get_delta(uint32_t since_pos,
                                const std::set<uint16_t>& subscribed,
                                int max_age_sec = -1) const;

  /// Authoritative position (= number of pushes).  Lock-free.
  [[nodiscard]] uint32_t position() const { return position_.load(); }

  void clear();

private:
  struct Entry {
    std::vector<uint8_t> value;
    uint32_t timestamp;  // Unix epoch seconds
  };

  mutable std::mutex mutex_;
  std::unordered_map<uint16_t, Entry> entries_;
  std::atomic<uint32_t> position_{0};
};

}  // namespace cvknxd
#endif