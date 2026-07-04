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

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "fcgi/fcgi_server.h"

using namespace cvknxd;

/// Unit tests for FcgiServer, focusing on the direct socket support (listen mode).
///
/// NOTE: Unix socket creation requires unsandboxed execution. When running in
/// the VS Code sandbox, FCGX_OpenSocket fails with EACCES. Tests that exercise
/// successful listen() are in the integration test suite which requests
/// unsandboxed access.

TEST(FcgiServerTest, NotListeningByDefault) {
  FcgiServer server;
  EXPECT_FALSE(server.is_listening());
}

TEST(FcgiServerTest, ListenRejectsEmptyPath) {
  FcgiServer server;
  EXPECT_FALSE(server.listen(""));
  EXPECT_FALSE(server.is_listening());
}

TEST(FcgiServerTest, ListenRejectsInvalidPath) {
  FcgiServer server;
  // A path in a non-existent directory should fail
  EXPECT_FALSE(server.listen("/nonexistent/path/fcgi.sock"));
  EXPECT_FALSE(server.is_listening());
}

TEST(FcgiServerTest, ListenStateResetAfterFailedCall) {
  FcgiServer server;
  EXPECT_FALSE(server.listen(""));
  // A subsequent valid attempt should still work (no corrupted state)
  // We can't actually test FCGX_OpenSocket in the sandbox, but we can
  // verify the state machine is correct: after a failed call the server
  // is still in non-listening mode.
  EXPECT_FALSE(server.is_listening());
}
