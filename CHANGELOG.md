# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] — 2026-07-05

### Added

- Initial release of cometvisu-knxd-fcgi.
- FastCGI backend implementing the CometVisu Protocol.
- `/l` (login) endpoint with session management.
- `/r` (read) endpoint with long-poll support via `poll()`.
- `/w` (write) endpoint for sending KNX telegrams.
- knxd Unix socket client with eibd binary protocol support.
- Address cache integration with knxd's built-in cache.
- Configurable knxd socket path (`KNXD_SOCKET` env var).
- Configurable long-poll timeout (`LONGPOLL_TIMEOUT_SEC` env var).
- Debug logging via `DEBUG_BACKEND` environment variable.
- JSON response builder (no external JSON library dependency).
- Query string parser.
- Comprehensive test suite: unit, integration, and end-to-end tests.
- CMake build system with Google Test integration.
- GPLv3 licensed.
