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

#include "group_cache.h"

namespace cvknxd {

void GroupCache::push(uint16_t addr, const std::vector<uint8_t>& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Assign pushed_at = new position after increment, so get_delta(since_pos)
  // returns entries with pushed_at > since_pos — i.e. entries that arrived
  // strictly after the client's last-seen position.
  uint32_t new_pos = position_.fetch_add(1) + 1;
  entries_[addr] = {value, static_cast<uint32_t>(std::time(nullptr)), new_pos};
}

std::optional<std::vector<uint8_t>> GroupCache::get(uint16_t addr,
                                                    int max_age_sec) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(addr);
  if (it == entries_.end()) {
    return std::nullopt;
  }
  if (max_age_sec >= 0) {
    auto now = static_cast<uint32_t>(std::time(nullptr));
    if (now - it->second.timestamp >= static_cast<uint32_t>(max_age_sec)) {
      return std::nullopt;
    }
  }
  return it->second.value;
}

GroupCache::Delta GroupCache::get_delta(uint32_t since_pos,
                                        const std::set<uint16_t>& subscribed,
                                        int max_age_sec) const {
  std::lock_guard<std::mutex> lock(mutex_);
  Delta delta;
  delta.position = position_.load();

  // Epoch detection: if the client's position exceeds our current position,
  // the counter was reset (worker restart, fresh cache).  Clamp since_pos
  // to 0 so fresh entries are delivered instead of being silently treated
  // as "already seen" in a previous epoch.
  uint32_t effective_since = since_pos;
  if (since_pos > delta.position) {
    effective_since = 0;
  }

  uint32_t now = static_cast<uint32_t>(std::time(nullptr));
  for (auto addr : subscribed) {
    auto it = entries_.find(addr);
    if (it == entries_.end()) {
      continue;
    }
    // Only include entries pushed after the client's known position.
    if (it->second.pushed_at <= effective_since) {
      continue;
    }
    // Age filter: reject entries older than max_age_sec.
    if (max_age_sec >= 0 &&
        now - it->second.timestamp >= static_cast<uint32_t>(max_age_sec)) {
      continue;
    }
    delta.values[addr] = it->second.value;
  }
  return delta;
}

void GroupCache::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
  position_.store(0);
}

}  // namespace cvknxd