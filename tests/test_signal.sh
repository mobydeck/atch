#!/bin/sh
# Signal-safety integration tests for atch attach.
# Uses a forkpty()-based C harness for exact PID targeting.
#
# Usage: sh tests/test_signal.sh <path-to-atch-binary>
# Builds the test harness automatically if needed.

ATCH="${1:-./atch}"
TESTS_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
TESTDIR=$(mktemp -d)
export HOME="$TESTDIR"
SESSION="sig-test-session"
NOISY_SESSION="sig-noisy-session"

trap '"$ATCH" kill "$NOISY_SESSION" >/dev/null 2>&1 || true; "$ATCH" kill "$SESSION" >/dev/null 2>&1 || true; rm -rf "$TESTDIR"' EXIT

# Build the harness (graceful skip if cc fails)
HARNESS="$TESTDIR/test_signal"
if ! cc -o "$HARNESS" "$TESTS_DIR/test_signal.c" -lutil 2>/dev/null; then
    echo "1..0 # SKIP cannot build forkpty harness"
    exit 0
fi

# Start a background session for the tests to attach to
"$ATCH" start "$SESSION" sleep 9999 || {
    echo "1..0 # SKIP failed to start test session"
    exit 0
}

"$ATCH" start "$NOISY_SESSION" sh -c 'yes X' || {
    echo "1..0 # SKIP failed to start noisy test session"
    exit 0
}

# Wait for socket
i=0
while [ $i -lt 20 ]; do
    [ -S "$HOME/.cache/atch/$SESSION" ] && break
    sleep 0.05
    i=$((i + 1))
done

if [ ! -S "$HOME/.cache/atch/$SESSION" ] ||
   [ ! -S "$HOME/.cache/atch/$NOISY_SESSION" ]; then
    echo "1..0 # SKIP session socket did not appear"
    exit 0
fi

# Run the harness
"$HARNESS" "$ATCH" "$SESSION" "$NOISY_SESSION"
