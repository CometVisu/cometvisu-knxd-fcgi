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

#include "long_poll.h"

#include <thread>

namespace cvknxd {

void LongPollManager::wait(const std::set<uint16_t>& addresses, LongPollCallback callback,
                           int timeout_seconds) {
  // In a single-threaded FCGI model, we need cooperative scheduling.
  // For now: we block the calling thread and poll for data.
  // This will be refined when integrated with the event loop.

  // Simple implementation: block for timeout_seconds, then call
  // callback with whatever data we can collect.
  // In practice, this will be driven by the event loop checking
  // the knxd socket and match incoming data against waiters.

  std::unique_lock<std::mutex> lock(mutex_);

  // For a cooperative single-threaded implementation, we just
  // store the waiter and let the event loop handle notification.
  waiters_.push_back({addresses, std::move(callback)});

  // Note: In a real implementation, we would release the lock and
  // wait on a condition variable signaled by notify().
  // For the initial implementation, the caller uses cooperative
  // polling via check_waiters().
}

void LongPollManager::notify(uint16_t eibaddr, const std::string& hex_value) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = waiters_.begin();
  while (it != waiters_.end()) {
    if (it->addresses.find(eibaddr) != it->addresses.end()) {
      // This waiter is interested in this address
      LongPollResult result;
      result.values[eibaddr] = hex_value;
      result.index = std::to_string(next_index_++);
      it->callback(result);
      it = waiters_.erase(it);
    } else {
      ++it;
    }
  }
}

void LongPollManager::shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  shutting_down_ = true;
  // Wake all waiters with empty results
  for (auto& waiter : waiters_) {
    LongPollResult result;
    result.index = std::to_string(next_index_++);
    waiter.callback(result);
  }
  waiters_.clear();
}

}  // namespace cvknxd
