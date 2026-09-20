#!/usr/bin/env bash
# Deterministic integration test: builds the project, starts the server,
# runs several fuzzing clients against it concurrently, and checks that
# every client received the expected number of broadcast messages plus
# the termination message, and that the server exited cleanly.
#
# Usage: scripts/run_multi_client_test.sh [SANITIZER]
#   SANITIZER: none (default), address, or thread — forwarded to CMake.

set -euo pipefail

SANITIZER="${1:-none}"
NUM_CLIENTS="${NUM_CLIENTS:-4}"
NUM_MESSAGES="${NUM_MESSAGES:-25}"
PORT="${PORT:-$((20000 + RANDOM % 10000))}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-test-${SANITIZER}"
WORK_DIR="$(mktemp -d)"

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "== Configuring (SANITIZER=${SANITIZER}) =="
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DSANITIZER="$SANITIZER" >/dev/null

echo "== Building =="
cmake --build "$BUILD_DIR" -j >/dev/null

SERVER_BIN="${BUILD_DIR}/server"
CLIENT_BIN="${BUILD_DIR}/client"

echo "== Starting server on port ${PORT} for ${NUM_CLIENTS} clients =="
SERVER_LOG="${WORK_DIR}/server.log"
"$SERVER_BIN" "$PORT" "$NUM_CLIENTS" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

# Give the server a moment to bind and listen. We must not actually connect
# here: the server accepts every incoming connection unconditionally and
# counts it toward <#clients>, so a probe connection would steal a client
# slot. Instead, poll the listening socket table.
for _ in $(seq 1 50); do
  if ss -ltn 2>/dev/null | grep -q ":${PORT} "; then
    break
  fi
  sleep 0.1
done

echo "== Launching ${NUM_CLIENTS} clients, ${NUM_MESSAGES} messages each =="
CLIENT_PIDS=()
for i in $(seq 0 $((NUM_CLIENTS - 1))); do
  LOG_FILE="${WORK_DIR}/client${i}.log"
  "$CLIENT_BIN" 127.0.0.1 "$PORT" "$NUM_MESSAGES" "$LOG_FILE" \
    >"${WORK_DIR}/client${i}.stdout" 2>&1 &
  CLIENT_PIDS+=($!)
done

FAIL=0
for pid in "${CLIENT_PIDS[@]}"; do
  if ! wait "$pid"; then
    echo "FAIL: a client process exited non-zero"
    FAIL=1
  fi
done

if ! wait "$SERVER_PID"; then
  echo "FAIL: server exited non-zero"
  FAIL=1
fi
unset SERVER_PID

EXPECTED_LINES=$((NUM_CLIENTS * NUM_MESSAGES))

echo "== Verifying client logs =="
for i in $(seq 0 $((NUM_CLIENTS - 1))); do
  LOG_FILE="${WORK_DIR}/client${i}.log"
  if [[ ! -s "$LOG_FILE" ]]; then
    echo "FAIL: client${i}.log is missing or empty"
    FAIL=1
    continue
  fi
  ACTUAL_LINES=$(wc -l < "$LOG_FILE")
  if [[ "$ACTUAL_LINES" -ne "$EXPECTED_LINES" ]]; then
    echo "FAIL: client${i}.log has ${ACTUAL_LINES} lines, expected ${EXPECTED_LINES}"
    FAIL=1
  fi
  if ! grep -q "Server is ending the chat." "${WORK_DIR}/client${i}.stdout"; then
    echo "FAIL: client${i} never reported server termination"
    FAIL=1
  fi
done

if ! grep -q "Ending connection for all client" "$SERVER_LOG"; then
  echo "FAIL: server did not report a coordinated shutdown"
  FAIL=1
fi

if [[ "$FAIL" -eq 0 ]]; then
  echo "== PASS: ${NUM_CLIENTS} clients x ${NUM_MESSAGES} messages, all logs consistent =="
  rm -rf "$WORK_DIR"
else
  echo "== FAILURES DETECTED — artifacts kept at ${WORK_DIR} =="
  exit 1
fi
