# tty-ws-agent

Bridges a local shell to a remote WebSocket server. Spawns `/bin/bash` in a PTY and forwards all I/O over a WebSocket connection.

## Features

*   Forwards PTY I/O to/from a `wss://` server.
*   Handles `resize` messages to adjust the terminal.
*   Replays buffered output on `dump` requests.
*   Optional `--no-verify` flag for self-signed certs.

## Build & Run

**Build it:**
```bash
g++ -std=c++17 -O2 -DNDEBUG tty-ws-agent.cpp -o tty-ws-agent -lboost_system -lssl -lcrypto -lpthread
```
**Run it:**
```bash
./tty-ws-agent <wss-server-host> <wss-server-port> <wss-server-path> [--no-verify]
```

## Generic TCP version
A version that listens on a UNIX socket is provided as generic-tcp.cpp. 