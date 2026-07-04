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

#include "hex.h"

#include <cctype>
#include <stdexcept>

namespace cvknxd {

std::vector<uint8_t> hex_decode(std::string_view hex) {
  if (hex.size() % 2 != 0) {
    return {};  // invalid: odd length
  }
  std::vector<uint8_t> result;
  result.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    char hi = hex[i];
    char lo = hex[i + 1];
    if (!std::isxdigit(static_cast<unsigned char>(hi)) ||
        !std::isxdigit(static_cast<unsigned char>(lo))) {
      return {};
    }
    auto nibble = [](char c) -> uint8_t {
      if (c >= '0' && c <= '9')
        return static_cast<uint8_t>(c - '0');
      if (c >= 'a' && c <= 'f')
        return static_cast<uint8_t>(c - 'a' + 10);
      if (c >= 'A' && c <= 'F')
        return static_cast<uint8_t>(c - 'A' + 10);
      return 0;
    };
    result.push_back(static_cast<uint8_t>((nibble(hi) << 4) | nibble(lo)));
  }
  return result;
}

std::string hex_encode(const uint8_t* data, size_t len) {
  static constexpr char kHexChars[] = "0123456789abcdef";
  std::string result;
  result.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    result.push_back(kHexChars[(data[i] >> 4) & 0x0F]);
    result.push_back(kHexChars[data[i] & 0x0F]);
  }
  return result;
}

std::string hex_byte(uint8_t byte) {
  return hex_encode(&byte, 1);
}

}  // namespace cvknxd
