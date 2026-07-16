# Copyright (C) 2026 Christian Mayer and the CometVisu contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.20)

# Read version from the VERSION file (single source of truth).
file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" _version_text)
string(STRIP "${_version_text}" PROJECT_VERSION_STRING)

# ---- Git hash (compile-time) ----
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _git_result
)
if(NOT _git_result EQUAL 0 OR GIT_HASH STREQUAL "")
    set(GIT_HASH "unknown")
endif()

project(cometvisu-knxd-fcgi
    VERSION ${PROJECT_VERSION_STRING}
    DESCRIPTION "FastCGI backend bridging CometVisu Protocol to knxd"
    LANGUAGES CXX
)

# Default to Release if no build type is specified.
# Developers can override with -DCMAKE_BUILD_TYPE=Debug.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Options ----
option(BUILD_TESTING "Build tests" ON)
option(ENABLE_SANITIZERS "Enable address/undefined-behavior sanitizers" OFF)

# Path to a knxd source checkout (e.g. -DKNXD_SOURCE_DIR=/path/to/knxd).
# When set, headers are taken from <KNXD_SOURCE_DIR>/include/ instead of
# the system-installed knxd-dev package.
set(KNXD_SOURCE_DIR "" CACHE PATH "Path to knxd source checkout")

# ---- Dependencies ----
include(FetchContent)

# Google Test
if(BUILD_TESTING)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
    )
    FetchContent_MakeAvailable(googletest)
    enable_testing()
endif()

# Find FastCGI library
find_library(FCGI_LIBRARY fcgi)
if(NOT FCGI_LIBRARY)
    message(FATAL_ERROR "FastCGI library (libfcgi) not found. "
            "Install with: sudo apt install libfcgi-dev")
endif()
find_path(FCGI_INCLUDE_DIR fcgi_stdio.h)
if(NOT FCGI_INCLUDE_DIR)
    message(FATAL_ERROR "FastCGI headers not found. "
            "Install with: sudo apt install libfcgi-dev")
endif()

# Find knxd development headers (eibtypes.h for protocol constants).
# Supports two modes:
#   1. -DKNXD_SOURCE_DIR=/path/to/knxd  → uses headers from a source checkout
#   2. Omitting KNXD_SOURCE_DIR          → uses system-installed knxd-dev package
if(KNXD_SOURCE_DIR)
    # User provided a knxd source checkout — look for eibtypes.h there.
    # knxd keeps its public headers in <source>/include/.
    find_path(KNXD_INCLUDE_DIR eibtypes.h
        HINTS "${KNXD_SOURCE_DIR}/include"
        NO_DEFAULT_PATH
    )
    if(NOT KNXD_INCLUDE_DIR)
        message(FATAL_ERROR "KNXD_SOURCE_DIR=${KNXD_SOURCE_DIR} but "
                "eibtypes.h not found in ${KNXD_SOURCE_DIR}/include/")
    endif()

    # Best-effort: detect knxd version from source checkout.
    # Try VERSION file, then configure.ac, then check for git describe.
    if(EXISTS "${KNXD_SOURCE_DIR}/VERSION")
        file(STRINGS "${KNXD_SOURCE_DIR}/VERSION" _knxd_version_text)
        string(STRIP "${_knxd_version_text}" KNXD_BUILD_VERSION)
    elseif(EXISTS "${KNXD_SOURCE_DIR}/configure.ac")
        file(STRINGS "${KNXD_SOURCE_DIR}/configure.ac" _ac_text REGEX "AC_INIT")
        if(_ac_text MATCHES "AC_INIT\\([^,]*,\\[?([0-9.]+)")
            set(KNXD_BUILD_VERSION "${CMAKE_MATCH_1}")
        endif()
    else()
        set(KNXD_BUILD_VERSION "")
    endif()

    # Best-effort: detect knxd git hash from source checkout.
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY "${KNXD_SOURCE_DIR}"
        OUTPUT_VARIABLE KNXD_BUILD_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _knxd_git_result
    )
    if(NOT _knxd_git_result EQUAL 0 OR KNXD_BUILD_GIT_HASH STREQUAL "")
        set(KNXD_BUILD_GIT_HASH "")
    endif()

    message(STATUS "Using knxd headers from source checkout: ${KNXD_INCLUDE_DIR}")
else()
    # No source dir given — search system include paths (knxd-dev package).
    find_path(KNXD_INCLUDE_DIR eibtypes.h)
    if(NOT KNXD_INCLUDE_DIR)
        message(FATAL_ERROR "knxd development headers (eibtypes.h) not found.\n"
                "  Option A: Install the system package:\n"
                "    sudo apt install knxd-dev\n"
                "  Option B: Point CMake to a knxd source checkout:\n"
                "    cmake -B build -DKNXD_SOURCE_DIR=/path/to/knxd")
    endif()

    # Best-effort: detect knxd version from system package.
    # Try pkg-config first, then dpkg.
    find_program(PKG_CONFIG_EXECUTABLE pkg-config)
    if(PKG_CONFIG_EXECUTABLE)
        execute_process(
            COMMAND ${PKG_CONFIG_EXECUTABLE} --modversion knxd
            OUTPUT_VARIABLE KNXD_BUILD_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _pkg_result
        )
        if(NOT _pkg_result EQUAL 0 OR KNXD_BUILD_VERSION STREQUAL "")
            set(KNXD_BUILD_VERSION "")
        endif()
    else()
        # Fallback: try dpkg
        find_program(DPKG_EXECUTABLE dpkg-query)
        if(DPKG_EXECUTABLE)
            execute_process(
                COMMAND ${DPKG_EXECUTABLE} -W -f='${Version}' knxd-dev
                OUTPUT_VARIABLE _knxd_pkg_version
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE _dpkg_result
            )
            if(_dpkg_result EQUAL 0 AND NOT _knxd_pkg_version STREQUAL "")
                string(REGEX REPLACE "-.*" "" KNXD_BUILD_VERSION "${_knxd_pkg_version}")
            else()
                set(KNXD_BUILD_VERSION "")
            endif()
        else()
            set(KNXD_BUILD_VERSION "")
        endif()
    endif()

    set(KNXD_BUILD_GIT_HASH "")

    message(STATUS "Using knxd headers from system: ${KNXD_INCLUDE_DIR}")
endif()

# ---- Compiler Settings ----
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---- Subdirectories ----
add_subdirectory(src)
if(BUILD_TESTING)
    add_subdirectory(mocks)
    add_subdirectory(tests)
endif()

# ---- Install ----
install(TARGETS cometvisu-knxd-fcgi RUNTIME DESTINATION bin)
