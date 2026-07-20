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
 * @file hex.h
 * @brief Hex encoding/decoding utilities for the CometVisu wire protocol.
 *
 * CometVisu transmits KNX data values as lowercase hex strings without
 * spaces or "0x" prefix.  These functions convert between the wire format
 * and raw byte vectors.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cvknxd {

/// Convert a hex string (e.g. "0c6f") to bytes.
/// @param hex Input hex string, lowercase or uppercase, no spaces.
/// @return Vector of bytes, or empty vector on invalid input.
[[nodiscard]] std::vector<uint8_t> hex_decode(std::string_view hex);

/// Convert bytes to a lowercase hex string (e.g. [0x0c, 0x6f] → "0c6f").
[[nodiscard]] std::string hex_encode(const uint8_t* data, size_t len);

/// Convert a byte to a 2-char hex string (e.g. 0x42 → "42").
[[nodiscard]] std::string hex_byte(uint8_t byte);

}  // namespace cvknxd
