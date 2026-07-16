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

#include <cstdlib>
#include <string>

#include "handlers/login_handler.h"
#include "state/session_store.h"

using namespace cvknxd;

namespace {

/// Helper: set an environment variable for the duration of a test case.
class ScopedEnvVar {
public:
  ScopedEnvVar(const char* name, const char* value) : name_(name) {
    if (value != nullptr) {
      setenv(name, value, 1);
    } else {
      unsetenv(name);
    }
  }
  ~ScopedEnvVar() { unsetenv(name_); }
  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;
  ScopedEnvVar(ScopedEnvVar&&) = delete;
  ScopedEnvVar& operator=(ScopedEnvVar&&) = delete;

private:
  const char* name_;
};

}  // namespace

TEST(LoginHandlerTest, AnonymousLogin) {
  SessionStore sessions;
  LoginHandler handler(sessions);

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"v\":\"0.0.2\""), std::string::npos);
  EXPECT_NE(response.find("\"s\":\"0\""), std::string::npos);
}

TEST(LoginHandlerTest, AuthenticatedLogin) {
  SessionStore sessions;
  LoginHandler handler(sessions);

  std::string response = handler.handle("u=admin&p=secret");
  EXPECT_NE(response.find("\"v\":\"0.0.2\""), std::string::npos);
  // Session ID must NOT be "0"
  EXPECT_EQ(response.find("\"s\":\"0\""), std::string::npos);
  // Session ID should be a non-empty hex string
  auto spos = response.find("\"s\":\"");
  ASSERT_NE(spos, std::string::npos);
  auto epos = response.find('"', spos + 5);
  ASSERT_NE(epos, std::string::npos);
  std::string sid = response.substr(spos + 5, epos - spos - 5);
  EXPECT_FALSE(sid.empty());
  EXPECT_NE(sid, "0");
}

TEST(LoginHandlerTest, AnonymousWhenNoUserOrPassword) {
  SessionStore sessions;
  LoginHandler handler(sessions);

  // No u or p params → anonymous
  std::string response = handler.handle("d=mydevice");
  EXPECT_NE(response.find("\"s\":\"0\""), std::string::npos);
}

TEST(LoginHandlerTest, ValidJsonStructure) {
  SessionStore sessions;
  LoginHandler handler(sessions);

  std::string response = handler.handle("u=test&p=test");
  EXPECT_EQ(response.front(), '{');
  EXPECT_EQ(response.back(), '}');
  // Must have exactly v and s keys
  EXPECT_NE(response.find("\"v\":\"0.0.2\""), std::string::npos);
  EXPECT_NE(response.find("\"s\":"), std::string::npos);
}

TEST(LoginHandlerTest, DifferentSessionsAreUnique) {
  SessionStore sessions;
  LoginHandler handler(sessions);

  std::string r1 = handler.handle("u=a&p=b");
  std::string r2 = handler.handle("u=c&p=d");

  auto extract_sid = [](const std::string& json) -> std::string {
    auto s = json.find("\"s\":\"");
    if (s == std::string::npos) {
      return "";
    }
    auto e = json.find('"', s + 5);
    if (e == std::string::npos) {
      return "";
    }
    return json.substr(s + 5, e - s - 5);
  };

  std::string sid1 = extract_sid(r1);
  std::string sid2 = extract_sid(r2);
  EXPECT_NE(sid1, "0");
  EXPECT_NE(sid2, "0");
  EXPECT_NE(sid1, sid2);
}

TEST(LoginHandlerTest, ConfigBlockAbsentWhenNoUrlPath) {
  // The "c" block now always appears (version info), but "baseURL" is absent
  // when no base_url was given.
  SessionStore sessions;
  LoginHandler handler(sessions);  // default base_url = ""

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"c\""), std::string::npos);
  EXPECT_EQ(response.find("\"baseURL\""), std::string::npos);
}

TEST(LoginHandlerTest, ConfigBlockPresentWhenUrlPathSet) {
  SessionStore sessions;
  LoginHandler handler(sessions, "/proxy/visu");

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"c\""), std::string::npos);
  EXPECT_NE(response.find("\"baseURL\""), std::string::npos);
  EXPECT_NE(response.find("\"/proxy/visu\""), std::string::npos);
}

TEST(LoginHandlerTest, ConfigBlockWithCustomUrlPath) {
  SessionStore sessions;
  LoginHandler handler(sessions, "/custom/prefix");

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"baseURL\":\"/custom/prefix\""), std::string::npos);
}

TEST(LoginHandlerTest, ConfigBlockIgnoredWhenUrlPathEmpty) {
  // The "c" block appears (version info), but "baseURL" is absent.
  SessionStore sessions;
  LoginHandler handler(sessions, "");

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"c\""), std::string::npos);
  EXPECT_EQ(response.find("\"baseURL\""), std::string::npos);
}

// ============================================================================
// Regression test: BASE_URL from process environment must NOT be read.
//
// The original bug: LoginHandler used std::getenv("BASE_URL") at request time.
// In fork-based FCGI mode, FCGX_Accept_r() replaces environ with FCGI_PARAMS,
// which may not include BASE_URL.  Result: the "c" / "baseURL" config block
// silently disappeared from the login response even though the variable was
// correctly set in the process environment.
//
// The fix: inject base_url via the constructor at startup (before any FCGI
// accept), so the handler is decoupled from the run-time environ pointer.
// ============================================================================

TEST(LoginHandlerTest, ConfigBlockNotReadFromEnvironment) {
  // BASE_URL is set in the process environment ...
  ScopedEnvVar set("BASE_URL", "/proxy/visu");

  // ... but NOT injected via constructor (simulating FCGI environ swap).
  SessionStore sessions;
  LoginHandler handler(sessions);  // default base_url = ""

  // The "c" block appears (version info), but "baseURL" must NOT be
  // present — handler must NOT read getenv("BASE_URL").
  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"c\""), std::string::npos);
  EXPECT_EQ(response.find("\"baseURL\""), std::string::npos);
}

TEST(LoginHandlerTest, ConfigBlockRequiresExplicitInjectionNotEnvVar) {
  // Even with BASE_URL in the environment ...
  ScopedEnvVar set("BASE_URL", "/secret/prefix");

  // ... only the constructor parameter controls the output.
  SessionStore sessions;
  LoginHandler handler(sessions, "/explicit/prefix");

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"baseURL\":\"/explicit/prefix\""), std::string::npos);
  EXPECT_EQ(response.find("/secret/prefix"), std::string::npos);
}

// ============================================================================
// Version info in the "c" block — always included when available
// ============================================================================

TEST(LoginHandlerTest, ConfigBlockIncludesVersionInfo) {
  // Version info is included even without baseURL.
  // The compile-time values come from the build system; runtime from popen.
  SessionStore sessions;
  LoginHandler handler(sessions);  // no base_url

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"c\""), std::string::npos);
  EXPECT_NE(response.find("\"fcgiVersion\":"), std::string::npos);
  EXPECT_NE(response.find("\"fcgiGitHash\":"), std::string::npos);
}

TEST(LoginHandlerTest, ConfigBlockWithBaseUrlIncludesVersionInfo) {
  // When base_url is set, baseURL and all version keys are present.
  SessionStore sessions;
  LoginHandler handler(sessions, "/proxy/visu");

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"baseURL\":\"/proxy/visu\""), std::string::npos);
  EXPECT_NE(response.find("\"fcgiVersion\":"), std::string::npos);
  EXPECT_NE(response.find("\"fcgiGitHash\":"), std::string::npos);
}

TEST(LoginHandlerTest, KnxdRuntimeVersionIsCached) {
  // The runtime knxd version is queried via popen("knxd --version").
  // Test that subsequent calls produce identical output (cached).
  SessionStore sessions;
  LoginHandler handler(sessions);

  std::string r1 = handler.handle("");
  std::string r2 = handler.handle("");
  EXPECT_EQ(r1, r2);  // identical output (cached)
}

TEST(LoginHandlerTest, KnxdRuntimeVersionFromCustomBinary) {
  // When a custom knxd binary path is provided, its --version output
  // should appear as knxdRuntimeVersion in the login response.
  SessionStore sessions;
  const std::string mock_binary = std::string(TEST_SOURCE_DIR) + "/mock_knxd_version.sh";
  LoginHandler handler(sessions, "", mock_binary);

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"knxdRuntimeVersion\":\"knxd 9.99.99-test\""), std::string::npos);
}
