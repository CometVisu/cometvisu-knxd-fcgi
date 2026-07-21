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
 * @brief Local cache for KNX group telegrams.
 *
 * Mirrors the approach of the reference eibread-cgi: maintains a local
 * cache of group address values, updated from APDU_PACKET telegrams
 * received on the group socket.  This avoids round-trips to knxd's
 * built-in cache (cache_read) for every value lookup, and provides
 * immediate availability of values written via /w — the group socket
 * echo updates this cache before knxd's cache_last_updates_2 position
 * is confirmed.
 *
 * Thread-safe: all public methods are protected by a mutex.
 */

#ifndef COMETVISU_KNXD_FCGI_GROUP_CACHE_H_
#define COMETVISU_KNXD_FCGI_GROUP_CACHE_H_

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace cvknxd {

class GroupCache {
public:
  GroupCache() = default;

  /// @brief Update (or insert) the cached value for a KNX group address.
  /// @param addr 16-bit EIB group address.
  /// @param value Raw APDU value bytes (without APCI header).
  void update(uint16_t addr, std::vector<uint8_t> value);

  /// @brief Get the cached value for a KNX group address.
  /// @param addr 16-bit EIB group address.
  /// @return The cached value bytes, or std::nullopt if not cached.
  [[nodiscard]] std::optional<std::vector<uint8_t>> get(uint16_t addr) const;

  /// @brief Remove all cached entries.
  void clear();

  /// @brief Number of cached entries.
  [[nodiscard]] size_t size() const;

  /// @brief Check if the cache contains an entry for an address.
  [[nodiscard]] bool contains(uint16_t addr) const;

private:
  mutable std::mutex mutex_;
  std::unordered_map<uint16_t, std::vector<uint8_t>> entries_;
};

}  // namespace cvknxd

#endif  // COMETVISU_KNXD_FCGI_GROUP_CACHE_H_
