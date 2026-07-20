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
 * @file version.cpp
 * @brief Implementation of version information accessors.
 */

#include "version.h"

namespace cvknxd {

std::string_view version() {
#ifdef APP_VERSION
  return APP_VERSION;
#else
  return "unknown";
#endif
}

std::string_view application_name() {
  return "cometvisu-knxd-fcgi";
}

std::string_view git_hash() {
#ifdef GIT_HASH
  return GIT_HASH;
#else
  return "unknown";
#endif
}

std::string_view knxd_build_version() {
#ifdef KNXD_BUILD_VERSION
  return KNXD_BUILD_VERSION;
#else
  return "";
#endif
}

std::string_view knxd_build_git_hash() {
#ifdef KNXD_BUILD_GIT_HASH
  return KNXD_BUILD_GIT_HASH;
#else
  return "";
#endif
}

}  // namespace cvknxd
