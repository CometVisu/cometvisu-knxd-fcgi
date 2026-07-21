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

#include <utility>

namespace cvknxd {

void GroupCache::update(uint16_t addr, std::vector<uint8_t> value) {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_[addr] = std::move(value);
}

std::optional<std::vector<uint8_t>> GroupCache::get(uint16_t addr) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(addr);
  if (it != entries_.end()) {
    return it->second;
  }
  return std::nullopt;
}

void GroupCache::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
}

size_t GroupCache::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

bool GroupCache::contains(uint16_t addr) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.contains(addr);
}

}  // namespace cvknxd
