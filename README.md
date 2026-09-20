# POSIX TCP Group Chat Fuzzer

A multi-client TCP group chat server and fuzzing client implemented in C.
The server relays messages between clients, and each client generates
randomized hex messages, logs everything, and participates in a simple
termination protocol.

This project demonstrates POSIX sockets, multi-threaded servers with
pthreads, and a custom binary message protocol.

## Features

TCP group chat server:
- Accepts multiple simultaneous clients
- Broadcasts every message to all connected clients, including the sender
- Preserves message ordering across all clients
- Tracks clients and manages a coordinated shutdown using a type-1 control message

Fuzzing client:
- Connects to the server over IPv4
- Generates random bytes using `getentropy()`
- Encodes data as printable hex strings (via `hex_string.c`)
- Sends a configurable number of messages
- Logs all received messages to a file

Custom message protocol:
- Type-tagged messages (`0` = chat, `1` = termination)
- Server attaches sender IP and port to each broadcast
- Length-prefixed framing (see below)

Concurrency and synchronization:
- One thread per client on the server
- Mutex-protected shared structures (client list, termination counter)
- Client uses a background thread to print and log server messages while sending

## Tech stack

- Language: C
- Networking: POSIX sockets (`AF_INET`, `SOCK_STREAM`)
- Concurrency: POSIX threads (`pthread`)
- Randomness: `getentropy()` for fuzzed payloads
- Build system: CMake
- Platform: Linux / POSIX-compliant systems

## Project structure

```bash
.
├── CMakeLists.txt
├── .gitignore
├── include
│   └── hex_string.h
├── src
│   ├── client.c
│   ├── hex_string.c
│   └── server.c
└── README.md
```

## Messaging protocol

All communication between server and clients is length-prefixed framing
over TCP:

```text
[0..3] : uint32_t frame length N (network byte order), not counting itself
[4..N+3] : N bytes of frame body
```

Each frame body starts with a type byte (`uint8_t`):
- `0` → regular chat message
- `1` → termination / end-of-execution message

> The protocol originally delimited messages with a trailing `\n` instead
> of a length prefix. Broadcast frames embed the sender's raw IP/port bytes
> in the body, and those binary bytes can coincidentally equal `0x0A`
> (`\n`). The parser would then split the frame at the wrong offset and
> desync the stream, corrupting later messages on that connection. A length
> prefix removes the ambiguity. See [Known limitations](#known-limitations)
> for how this was found.

### Server → client (type 0)

When the server receives a type-0 frame from a client, it:

1. Determines the sender's IP (`uint32_t`) and port (`uint16_t`)
2. Broadcasts the following body to all clients, in one length-prefixed frame:

```text
[0]      : uint8_t   type = 0
[1..4]   : uint32_t  sender IP (network byte order)
[5..6]   : uint16_t  sender port (network byte order)
[7..N-1] : bytes of the original message payload
```

### Client → server (type 0)

Clients send type-0 frames with a body of:

```text
[0]      : uint8_t type = 0
[1..N-1] : ASCII hex payload (generated fuzz data)
```

### Termination (type 1)

After sending its configured number of messages, each client sends a
type-1 frame with a 2-byte body: `[0] = 1`.

The server counts type-1 messages from clients. Once it has received
type-1 from all expected clients, it broadcasts a type-1 message to all
clients, prints a termination message, and exits.

Each client terminates after receiving a type-1 message from the server.

## Build with CMake

From the project root:

```bash
cmake -S . -B build
cmake --build build
```

This produces two executables inside `build/`: `server` and `client`.

By default this builds without a sanitizer. To build with AddressSanitizer
(plus UndefinedBehaviorSanitizer) or ThreadSanitizer instead, pass `-DSANITIZER`:

```bash
cmake -S . -B build-asan -DSANITIZER=address
cmake --build build-asan

cmake -S . -B build-tsan -DSANITIZER=thread
cmake --build build-tsan
```

ASan and TSan cannot be linked into the same binary, so build separate
directories for each, as above.

## Testing

There is no unit-test suite. The protocol is a handful of tight network I/O
loops, not units that separate cleanly. Instead there's a deterministic
integration test that builds the project, starts the server, runs several
fuzzing clients against it concurrently, and checks that every client
received every broadcast message plus a clean termination, and that the
server exited on its own:

```bash
scripts/run_multi_client_test.sh [none|address|thread]
```

`NUM_CLIENTS` and `NUM_MESSAGES` environment variables override the defaults
(4 clients, 25 messages each). Example run:

```text
$ scripts/run_multi_client_test.sh none
== Configuring (SANITIZER=none) ==
== Building ==
== Starting server on port 20733 for 3 clients ==
== Launching 3 clients, 25 messages each ==
== Verifying client logs ==
== PASS: 3 clients x 25 messages, all logs consistent ==
```

This script is what CI runs, once per sanitizer configuration
(`none`, `address`, `thread`); see
[`.github/workflows/ci.yml`](.github/workflows/ci.yml). It runs on Linux
(`ubuntu-latest`), since the code targets POSIX sockets and pthreads.

## Run the server

Start the server with:

```bash
./build/server <port> <#clients>
```

- `<port>`: TCP port to listen on (e.g. 8000)
- `<#clients>`: expected number of clients that will eventually connect

Example:

```bash
./build/server 8000 3
```

## Run a fuzzing client

Start a client with:

```bash
./build/client <IP address> <port> <#messages> <log file path>
```

- `<IP address>`: server address (e.g. 127.0.0.1)
- `<port>`: server port (must match the server)
- `<#messages>`: number of random messages to send
- `<log file path>`: where to store all messages received from the server

Example:

```bash
./build/client 127.0.0.1 8000 100 client0.log
```

Each client spawns a background thread to receive and print messages,
generates random bytes via `getentropy()`, converts them to hex using
`convert()` from `hex_string.c`, sends type-0 messages followed by a final
type-1 control message, and logs each incoming message to the specified
log file.

## Message logging format (client)

For every type-0 message received from the server, the client prints and logs:

```c
printf("%-15s%-10u%s", ip_str, port, message);
```

- `ip_str`: dotted IPv4 string of the original sender
- `port`: sender's port
- `message`: hex string payload (includes `\n` at the end)

Sample line (conceptual):

```text
192.168.0.10   9000      9391DE3E275ADB19637D   
```

## Known limitations

Writing [`scripts/run_multi_client_test.sh`](scripts/run_multi_client_test.sh)
turned up several real bugs that code review alone had missed. They're
fixed now, but worth recording since they shaped the current design.

The server used to broadcast a message only to clients already registered
at the moment it arrived. A client connecting even slightly after the
others would silently miss everything sent before it joined; there was no
backlog. It now blocks each client's message loop behind a barrier until
exactly `<#clients>` connections have been accepted, so no traffic moves
until the whole group is present. That fixes the common case (everyone
starts together), but it isn't real message history: a client that
disconnects and reconnects mid-run still loses whatever went by while it
was gone.

The newline-delimited framing could desync (see the protocol note above),
fixed by switching to length-prefixed frames. `write()` and `read()`
weren't retried on short transfers, so a partial write could quietly drop
part of a message; both now go through retry loops (`write_all`/`read_all`).
And the server never actually exited on its own: after broadcasting
termination, the main thread stayed blocked in `accept()` forever, so the
documented "prints a termination message and exits" behavior only happened
if something else killed the process. The finishing thread now calls
`exit(0)` directly.

What the test still doesn't cover:

- No replay/backlog and no reconnect support, as above. This is a fuzzing
  harness, not a durable chat server.
- `<#clients>` is fixed at server startup, and a connecting client past
  that count crashes the server (`Exceeding_Max_client`) instead of being
  rejected gracefully.
- No authentication, encryption, or resource limits. This is a local
  testing tool, not something to expose on an untrusted network.
- No fuzzing of malformed or adversarial input. The client only ever sends
  well-formed frames, so the server's own parsing (frame-length handling,
  type byte validation) isn't exercised against malicious input.
- CI covers Linux only (`ubuntu-latest`), matching the POSIX-only
  sockets/pthreads code. There's no Windows or macOS coverage.
