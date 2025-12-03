# POSIX TCP Group Chat Fuzzer

A multi-client **TCP group chat server** and **fuzzing client** implemented in C.  
The server relays messages between clients, and each client generates randomized hex messages, logs everything, and participates in a simple termination protocol.

This project demonstrates **POSIX sockets**, **multi-threaded servers with pthreads**, custom **binary message protocols**, and robust network I/O.

---

## 🚀 Features

- TCP group chat server that:
  - Accepts multiple simultaneous clients
  - Broadcasts every message to **all** connected clients (including the sender)
  - Preserves **message ordering** across all clients
  - Tracks clients and manages a coordinated shutdown using a type-1 control message

- Fuzzing client that:
  - Connects to the server over IPv4
  - Generates random bytes using `getentropy()`
  - Encodes data as printable hex strings (via `hex_string.c`)
  - Sends a configurable number of messages
  - Logs all received messages to a file

- Custom message protocol:
  - Type-tagged messages (`0` = chat, `1` = termination)
  - Server attaches sender IP + port to each broadcast
  - Messages delimited by `'\n'` and buffered correctly

- Concurrency & synchronization:
  - One thread per client on the server
  - Mutex-protected shared structures (client list, termination counter)
  - Client uses a background thread to print and log server messages while sending

---

## 🛠 Tech Stack

- **Language:** C
- **Networking:** POSIX sockets (`AF_INET`, `SOCK_STREAM`)
- **Concurrency:** POSIX threads (`pthread`)
- **Randomness:** `getentropy()` for fuzzed payloads
- **Build System:** CMake
- **Platform:** Linux / POSIX-compliant systems
- **Development Environment:**
  - Shell: `zsh`
  - Editor: Neovim (`nvim`)

---

# 🧵 Messaging Protocol

All communication between server and clients uses a simple binary framing protocol over TCP:

- **Maximum message size:** 1024 bytes
- **First byte:** message type (`uint8_t`)
  - `0` → regular chat message
  - `1` → termination / end-of-execution message
- **Message terminator:** a single newline character `\n` marks the end of a message.

### Server → Client (type 0)

When the server receives a type-0 message from a client, it:

1. Determines the sender’s IP (`uint32_t`) and port (`uint16_t`)
2. Broadcasts the following layout to all clients:

```text
[0]          : uint8_t   type = 0
[1..4]       : uint32_t  sender IP (network byte order)
[5..6]       : uint16_t  sender port (network byte order)
[7..N-1]     : bytes of original message payload (up to '\n')
[N]          : '\n' terminator
```

### Client → Server (type 0)

Clients send type-0 messages as:

```text
[0]      : uint8_t type = 0
[1..N-1] : ASCII hex payload (generated fuzz data)
[N]      : '\n'
```

### Termination (type 1)

After sending its configured number of messages, each client:

- Sends a type-1 message: `[0] = 1`, followed by `\n`.

The server counts type-1 messages from clients:

- Once it has received type-1 from **all** expected clients, it:
  1. Broadcasts a type-1 message to all clients
  2. Prints a termination message
  3. Exits.

Each client:
- Terminates after receiving a type-1 message from the server.

---

## ⚙️ Build with CMake

From the project root:

```bash
cmake -S . -B build
cmake --build build
```

This produces two executables inside `build/`:
- `server`
- `client`

---

## ▶️ Run the Server

Start the server with:

```bash
./build/server <port> <#clients>
```

- `<port>` — TCP port to listen on (e.g. 8000)
- `<#clients>` — expected number of clients that will eventually connect

**Example:**

```bash
./build/server 8000 3
```

---

## 💬 Run a Fuzzing Client

Start a client with:

```bash
./build/client <IP address> <port> <#messages> <log file path>
```

- `<IP address>` — server address (e.g. 127.0.0.1)
- `<port>` — server port (must match the server)
- `<#messages>` — number of random messages to send
- `<log file path>` — where to store all messages received from the server

**Example:**

```bash
./build/client 127.0.0.1 8000 100 client0.log
```

Each client:
- Spawns a background thread to receive and print messages
- Generates random bytes via `getentropy()`
- Converts them to hex using `convert()` from `hex_string.c`
- Sends type-0 messages followed by a final type-1 control message
- Logs each incoming message to the specified log file

---

## 📌 Message Logging Format (Client)

For every type-0 message received from the server, the client prints and logs:

```c
printf("%-15s%-10u%s", ip_str, port, message);
```

- `ip_str` — dotted IPv4 string of the original sender
- `port` — sender’s port
- `message` — hex string payload (includes `\n` at the end)

**Sample line (conceptual):**

```text
192.168.0.10   9000      9391DE3E275ADB19637D   
```