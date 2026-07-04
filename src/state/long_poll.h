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

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace cvknxd {

/// Result of a long-poll wait operation.
struct LongPollResult {
  /// Map of eibaddr → hex value string.
  std::unordered_map<uint16_t, std::string> values;
  /// Index for the next read request (monotonically incrementing).
  std::string index;
};

/// Callback to send a long-poll response to a waiting client.
/// The callback receives the LongPollResult.
using LongPollCallback = std::function<void(const LongPollResult&)>;

/// Manages COMET-style long-polling for /r requests without timeout.
/// When a client requests values for certain addresses without a timeout,
/// they are parked here until new data arrives from knxd.
class LongPollManager {
public:
  /// Register a long-poll waiter.
  /// @param addresses Set of EIB group addresses the client is watching.
  /// @param callback Called when data arrives or timeout occurs.
  /// @param timeout_seconds Max wait time before sending empty response.
  void wait(const std::set<uint16_t>& addresses, LongPollCallback callback, int timeout_seconds);

  /// Notify waiters about new data for a group address.
  /// @param eibaddr The group address that received new data.
  /// @param hex_value The value as hex string (CometVisu format).
  void notify(uint16_t eibaddr, const std::string& hex_value);

  /// Shut down the manager, waking all waiters.
  void shutdown();

private:
  struct Waiter {
    std::set<uint16_t> addresses;
    LongPollCallback callback;
  };

  std::mutex mutex_;
  std::vector<Waiter> waiters_;
  uint64_t next_index_ = 1;
  bool shutting_down_ = false;
};

}  // namespace cvknxd
