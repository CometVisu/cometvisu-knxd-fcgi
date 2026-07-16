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

#include "state/session_store.h"

using namespace cvknxd;

TEST(SessionStoreTest, CreateAnonymousSession) {
  SessionStore store;
  std::string id = store.create_session(true);
  EXPECT_EQ(id, "0");
}

TEST(SessionStoreTest, CreateNormalSession) {
  SessionStore store;
  std::string id = store.create_session(false);
  EXPECT_NE(id, "0");
  EXPECT_FALSE(id.empty());
}

TEST(SessionStoreTest, ValidateSession) {
  SessionStore store;
  std::string id = store.create_session(false);
  EXPECT_TRUE(store.is_valid(id));
}

TEST(SessionStoreTest, AnonymousAlwaysValid) {
  SessionStore store;
  EXPECT_TRUE(store.is_valid("0"));
}

TEST(SessionStoreTest, InvalidSession) {
  SessionStore store;
  EXPECT_FALSE(store.is_valid("nonexistent"));
}

TEST(SessionStoreTest, RemoveSession) {
  SessionStore store;
  std::string id = store.create_session(false);
  EXPECT_TRUE(store.is_valid(id));
  store.remove(id);
  EXPECT_FALSE(store.is_valid(id));
}

TEST(SessionStoreTest, RemoveAnonymousNoop) {
  SessionStore store;
  (void)store.create_session(true);
  store.remove("0");
  EXPECT_TRUE(store.is_valid("0"));  // anonymous always valid
}

TEST(SessionStoreTest, MultipleSessions) {
  SessionStore store;
  auto id1 = store.create_session(false);
  auto id2 = store.create_session(false);
  EXPECT_NE(id1, id2);
  EXPECT_TRUE(store.is_valid(id1));
  EXPECT_TRUE(store.is_valid(id2));
  EXPECT_EQ(store.count(), 2);
}

TEST(SessionStoreTest, SessionIdsAreRandom) {
  SessionStore store;
  auto id1 = store.create_session(false);
  auto id2 = store.create_session(false);
  // IDs should be 16 hex chars (64-bit random, not sequential)
  EXPECT_EQ(id1.size(), 16);
  EXPECT_EQ(id2.size(), 16);
  EXPECT_NE(id1, id2);
  EXPECT_NE(id2, "1");  // not sequential
}

TEST(SessionStoreTest, Count) {
  SessionStore store;
  EXPECT_EQ(store.count(), 0);
  (void)store.create_session(false);
  EXPECT_EQ(store.count(), 1);
  (void)store.create_session(true);  // anonymous not counted
  EXPECT_EQ(store.count(), 1);
  (void)store.create_session(false);
  EXPECT_EQ(store.count(), 2);
}

TEST(SessionStoreTest, ExpiredSessionsCleanedOnIsValid) {
  // Use a very short TTL so sessions expire quickly
  SessionStore store(0);  // 0-second TTL — sessions expire immediately

  std::string id = store.create_session(false);
  EXPECT_EQ(store.count(), 1);

  // is_valid should detect expiration AND clean up
  EXPECT_FALSE(store.is_valid(id));
  EXPECT_EQ(store.count(), 0);  // expired session removed
}

TEST(SessionStoreTest, MaxSessionsEnforced) {
  SessionStore store(1800, 2);  // 30-min TTL, max 2 sessions

  auto id1 = store.create_session(false);
  auto id2 = store.create_session(false);
  EXPECT_EQ(store.count(), 2);
  EXPECT_TRUE(store.is_valid(id1));
  EXPECT_TRUE(store.is_valid(id2));

  // Third session should evict the oldest
  auto id3 = store.create_session(false);
  EXPECT_EQ(store.count(), 2);
  EXPECT_TRUE(store.is_valid(id3));
  EXPECT_TRUE(store.is_valid(id2));   // newer session preserved
  EXPECT_FALSE(store.is_valid(id1));  // oldest evicted
}

TEST(SessionStoreTest, MaxSessionsPreservesAnonymous) {
  SessionStore store(1800, 1);  // max 1 session

  auto id1 = store.create_session(false);
  EXPECT_EQ(store.count(), 1);

  // Anonymous session doesn't count toward limit
  auto anon = store.create_session(true);
  EXPECT_EQ(anon, "0");
  EXPECT_EQ(store.count(), 1);  // anonymous not stored in map
  EXPECT_TRUE(store.is_valid("0"));
  EXPECT_TRUE(store.is_valid(id1));
}
