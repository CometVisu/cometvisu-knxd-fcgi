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

#include "session_store.h"

#include <random>

namespace cvknxd {

std::string SessionStore::create_session(bool anonymous) {
  if (anonymous)
    return "0";

  std::string id = generate_id();
  sessions_[id] = Session{id, std::chrono::steady_clock::now()};
  return id;
}

bool SessionStore::is_valid(std::string_view session_id) const {
  if (session_id == "0")
    return true;  // anonymous always valid
  std::string key{session_id};
  return sessions_.find(key) != sessions_.end();
}

void SessionStore::remove(std::string_view session_id) {
  if (session_id == "0")
    return;
  std::string key{session_id};
  sessions_.erase(key);
}

std::string SessionStore::generate_id() {
  // Simple: hex counter. More sophisticated randomness could be used.
  std::string id;
  id.reserve(16);
  auto num = next_id_++;
  while (num > 0) {
    static constexpr char kHex[] = "0123456789abcdef";
    id.insert(id.begin(), kHex[num & 0xF]);
    num >>= 4;
  }
  if (id.empty())
    id = "0";
  return id;
}

}  // namespace cvknxd
