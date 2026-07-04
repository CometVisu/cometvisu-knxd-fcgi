# cometvisu-knxd-fcgi

FastCGI backend in modern C++ that implements the
[CometVisu Protocol](https://github.com/CometVisu/CometVisu/wiki/Protocol)
and connects to a local [knxd](https://github.com/knxd/knxd) daemon.

## Quick Start

```bash
# Prerequisites
sudo apt install libfcgi-dev cmake g++

# Build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)

# Run tests
ctest --test-dir build --output-on-failure

# Run (via spawn-fcgi or your web server's FastCGI support)
spawn-fcgi -p 9000 -n ./build/src/cometvisu-knxd-fcgi
```

## Architecture

See [PLAN.md](PLAN.md) for the full architecture and design document.

## License

See [LICENSE](LICENSE).
