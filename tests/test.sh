#!/bin/sh
# Integration tests for atch.
# Usage: sh tests/test.sh <path-to-atch-binary>
# Runs entirely in /tmp so it never touches real session state.

ATCH="${1:-./atch}"

# ── test framework ──────────────────────────────────────────────────────────

PASS=0
FAIL=0
T=0

ok() {
    T=$((T + 1)); PASS=$((PASS + 1))
    printf "ok %d - %s\n" "$T" "$1"
}

fail() {
    T=$((T + 1)); FAIL=$((FAIL + 1))
    printf "not ok %d - %s\n" "$T" "$1"
    [ -n "$2" ] && printf "  # expected : %s\n  # got      : %s\n" "$2" "$3"
}

assert_eq() {
    if [ "$2" = "$3" ]; then ok "$1"; else fail "$1" "$2" "$3"; fi
}

assert_contains() {
    case "$3" in *"$2"*) ok "$1" ;; *) fail "$1" "(contains '$2')" "$3" ;; esac
}

assert_not_contains() {
    case "$3" in *"$2"*) fail "$1" "(not contain '$2')" "$3" ;; *) ok "$1" ;; esac
}

assert_exit() {
    if [ "$2" = "$3" ]; then ok "$1"; else fail "$1" "exit $2" "exit $3"; fi
}

run() { out=$("$@" 2>&1); rc=$?; }

# ── isolation ────────────────────────────────────────────────────────────────

TESTDIR=$(mktemp -d)
export HOME="$TESTDIR"
trap 'rm -rf "$TESTDIR"' EXIT

printf "TAP version 13\n"

# ── helpers ──────────────────────────────────────────────────────────────────

# Wait until the session socket appears (up to 1 s).
wait_socket() {
    local i=0
    while [ $i -lt 20 ]; do
        [ -S "$HOME/.cache/atch/$1" ] && return 0
        sleep 0.05
        i=$((i + 1))
    done
    return 1
}

# Kill a named session and wait for its socket to vanish.
tidy() { "$ATCH" kill "$1" >/dev/null 2>&1; sleep 0.05; }

# ── 1. help / version ────────────────────────────────────────────────────────

run "$ATCH" --help
assert_exit   "help: --help exits 0"      0 "$rc"
assert_contains "help: --help shows usage"  "Usage:" "$out"

run "$ATCH" -h
assert_exit   "help: -h exits 0"          0 "$rc"
assert_contains "help: -h shows usage"    "Usage:" "$out"

run "$ATCH" "?"
assert_exit   "help: ? exits 0"           0 "$rc"
assert_contains "help: ? shows usage"     "Usage:" "$out"

run "$ATCH" --version
assert_exit     "version: exits 0"        0 "$rc"
assert_contains "version: shows version"  "version" "$out"

# ── 2. start command ─────────────────────────────────────────────────────────

run "$ATCH" start
assert_exit     "start: no session → exit 1"        1 "$rc"
assert_contains "start: no session → message"       "No session was specified" "$out"

run "$ATCH" start s-basic sleep 999
assert_exit     "start: exits 0"                    0 "$rc"
assert_contains "start: prints confirmation"        "session 's-basic' started" "$out"
tidy s-basic

# quiet flag after session name
run "$ATCH" start s-quiet -q sleep 999
assert_exit     "start -q: exits 0"                 0 "$rc"
assert_eq       "start -q: no output"               "" "$out"
tidy s-quiet

# quiet flag before session name (new-style option position)
run "$ATCH" start -q s-qpre sleep 999
assert_exit     "start -q (pre-session): exits 0"   0 "$rc"
assert_eq       "start -q (pre-session): no output" "" "$out"
tidy s-qpre

# already running
run "$ATCH" start s-dup sleep 999
run "$ATCH" start s-dup sleep 999
assert_exit     "start: already running → exit 1"   1 "$rc"
assert_contains "start: already running message"    "already running" "$out"
tidy s-dup

# bad command
run "$ATCH" start s-badcmd __atch_no_such_cmd__
assert_exit     "start: bad command → exit 1"       1 "$rc"
assert_contains "start: bad command message"        "could not execute" "$out"

# short alias 's'
run "$ATCH" s s-alias sleep 999
assert_exit     "start alias 's': exits 0"          0 "$rc"
assert_contains "start alias 's': confirmation"     "session 's-alias' started" "$out"
tidy s-alias

# legacy -n
run "$ATCH" -n s-legacy sleep 999
assert_exit     "legacy -n: exits 0"                0 "$rc"
tidy s-legacy

# ── 3. list command ───────────────────────────────────────────────────────────

run "$ATCH" list
assert_exit  "list: empty → exits 0"                0 "$rc"
assert_eq    "list: empty → (no sessions)"          "(no sessions)" "$out"

# quiet suppresses the empty message
run "$ATCH" -q list
assert_exit  "list -q: empty → exits 0"             0 "$rc"
assert_eq    "list -q: empty → no output"           "" "$out"

# with a running session
"$ATCH" start s-list sleep 999
run "$ATCH" list
assert_exit     "list: shows running session → exits 0"    0 "$rc"
assert_contains "list: shows session name"                 "s-list" "$out"
assert_not_contains "list: no [stale] for live session"    "[stale]" "$out"

# quiet still shows session data (only suppresses meta-messages)
run "$ATCH" -q list
assert_contains "list -q: still shows session data"        "s-list" "$out"

# short aliases
run "$ATCH" l
assert_contains "list alias 'l': works"   "s-list" "$out"
run "$ATCH" ls
assert_contains "list alias 'ls': works"  "s-list" "$out"

# legacy -l
run "$ATCH" -l
assert_contains "legacy -l: works"        "s-list" "$out"

tidy s-list

run "$ATCH" list
assert_eq "list: back to empty after kill" "(no sessions)" "$out"

# list -a: exited sessions (log without socket) appear with [exited]
# Clear any logs left over from earlier tidy'd sessions so they don't pollute.
rm -f "$HOME/.cache/atch"/*.log 2>/dev/null || true
# Simulate an exited session by planting an orphaned log file (no socket).
mkdir -p "$HOME/.cache/atch"
printf "some output\n" > "$HOME/.cache/atch/s-exited.log"
run "$ATCH" list
assert_not_contains "list: exited session absent without -a" "s-exited" "$out"
run "$ATCH" list -a
assert_exit     "list -a: exits 0"                    0 "$rc"
assert_contains "list -a: shows exited session"       "s-exited" "$out"
assert_contains "list -a: marks as [exited]"          "[exited]" "$out"

# clean up the exited log before the next checks so it doesn't pollute [exited]
rm -f "$HOME/.cache/atch/s-exited.log"

# running session is NOT shown as [exited] under -a
"$ATCH" start s-listrun sleep 999
run "$ATCH" list -a
assert_contains     "list -a: running session still listed"  "s-listrun" "$out"
assert_not_contains "list -a: running session not [exited]"  "[exited]" "$out"
tidy s-listrun
rm -f "$HOME/.cache/atch/s-listrun.log"

# list -a with no sessions and no logs → (no sessions)
run "$ATCH" list -a
assert_exit "list -a: empty → exits 0"           0 "$rc"
assert_eq   "list -a: empty → (no sessions)"     "(no sessions)" "$out"

# ── 4. stale session ─────────────────────────────────────────────────────────
# Use 'run' (dontfork master stays in foreground) so we get the master PID
# directly via $!.  kill -9 leaves the socket file on disk but removes the
# listener → ECONNREFUSED → list shows [stale].

"$ATCH" run s-stale sleep 99999 &
STALE_MASTER_PID=$!
wait_socket s-stale
kill -9 "$STALE_MASTER_PID" 2>/dev/null
sleep 0.1

run "$ATCH" list
assert_contains "list: stale session shows [stale]" "[stale]" "$out"

# clean up orphaned child and leftover socket
for p in $(ls /proc | grep -E '^[0-9]+$'); do
    [ -f "/proc/$p/cmdline" ] || continue
    cmd=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null)
    case "$cmd" in *"sleep 99999"*) kill "$p" 2>/dev/null ;; esac
done
rm -f "$HOME/.cache/atch/s-stale"

# ── 5. kill command ───────────────────────────────────────────────────────────

run "$ATCH" kill
assert_exit     "kill: no session → exit 1"          1 "$rc"
assert_contains "kill: no session → message"         "No session was specified" "$out"

run "$ATCH" kill s-noexist
assert_exit     "kill: nonexistent → exit 1"         1 "$rc"
assert_contains "kill: nonexistent → message"        "does not exist" "$out"

"$ATCH" start s-kill sleep 999
run "$ATCH" kill s-kill
assert_exit     "kill: running → exit 0"             0 "$rc"
assert_contains "kill: prints stopped"               "stopped" "$out"

run "$ATCH" kill s-kill
assert_exit     "kill: already gone → exit 1"        1 "$rc"
assert_contains "kill: already gone → message"       "does not exist" "$out"

# legacy -k
"$ATCH" start s-legkill sleep 999
run "$ATCH" -k s-legkill
assert_exit     "legacy -k: exits 0"                 0 "$rc"
assert_contains "legacy -k: prints stopped"          "stopped" "$out"

# short alias 'k'
"$ATCH" start s-kalias sleep 999
run "$ATCH" k s-kalias
assert_exit     "kill alias 'k': exits 0"            0 "$rc"
assert_contains "kill alias 'k': prints stopped"     "stopped" "$out"

# extra args rejected
"$ATCH" start s-killextra sleep 999
run "$ATCH" kill s-killextra extra
assert_exit     "kill: extra arg → exit 1"           1 "$rc"
assert_contains "kill: extra arg → message"          "Invalid number of arguments" "$out"
tidy s-killextra

# kill -f / --force (skip SIGTERM grace period, send SIGKILL directly)
"$ATCH" start s-killf sleep 999
run "$ATCH" kill -f s-killf
assert_exit     "kill -f: exits 0"                   0 "$rc"
assert_contains "kill -f: prints killed"             "killed" "$out"

"$ATCH" start s-killf2 sleep 999
run "$ATCH" kill --force s-killf2
assert_exit     "kill --force: exits 0"              0 "$rc"
assert_contains "kill --force: prints killed"        "killed" "$out"

# -f after session name
"$ATCH" start s-killf3 sleep 999
run "$ATCH" kill s-killf3 -f
assert_exit     "kill -f (after session): exits 0"   0 "$rc"
assert_contains "kill -f (after session): killed"    "killed" "$out"

# -f on nonexistent session still reports the error
run "$ATCH" kill -f s-noexist-force
assert_exit     "kill -f: nonexistent → exit 1"      1 "$rc"
assert_contains "kill -f: nonexistent → message"     "does not exist" "$out"

# ── 6. clear command ─────────────────────────────────────────────────────────

run "$ATCH" clear
assert_exit     "clear: no session, no env → exit 1"         1 "$rc"
assert_contains "clear: no session, no env → message"        "No session was specified" "$out"

# clear a nonexistent log is silent success
run "$ATCH" clear s-noexist-clear
assert_exit "clear: no log → exits 0"                0 "$rc"
assert_eq   "clear: no log → no output"              "" "$out"

# start a session (creates the log)
"$ATCH" start s-clear sleep 999

run "$ATCH" clear s-clear
assert_exit     "clear: exits 0"                     0 "$rc"
assert_contains "clear: confirmation message"        "log cleared" "$out"

# quiet suppresses confirmation
"$ATCH" start s-clearq sleep 999
run "$ATCH" -q clear s-clearq
assert_exit "clear -q: exits 0"                      0 "$rc"
assert_eq   "clear -q: no output"                    "" "$out"

tidy s-clear
tidy s-clearq

# extra args rejected
run "$ATCH" clear s-noexist-clear extra
assert_exit     "clear: extra arg → exit 1"          1 "$rc"
assert_contains "clear: extra arg → message"         "Invalid number of arguments" "$out"

# atch clear with ATCH_SESSION set uses current session
"$ATCH" start s-clear-cur sleep 999
wait_socket s-clear-cur
run env ATCH_SESSION="$HOME/.cache/atch/s-clear-cur" "$ATCH" clear
assert_exit     "clear: current session → exit 0"    0 "$rc"
assert_contains "clear: current session → message"   "log cleared" "$out"

# atch clear with no arg uses innermost session from nested chain
"$ATCH" start s-clear-inner sleep 999
wait_socket s-clear-inner
run env ATCH_SESSION="$HOME/.cache/atch/s-clear-cur:$HOME/.cache/atch/s-clear-inner" \
        "$ATCH" clear
assert_exit     "clear: nested env no arg → exit 0"  0 "$rc"
assert_contains "clear: nested env no arg → message" "s-clear-inner" "$out"

tidy s-clear-cur
tidy s-clear-inner

# ── 7. current command ───────────────────────────────────────────────────────

run "$ATCH" current
assert_exit "current: outside session → exit 1"      1 "$rc"
assert_eq   "current: outside session → no output"   "" "$out"

# ATCH_SESSION holds the chain; a single session has no colon
run env ATCH_SESSION="$HOME/.cache/atch/mywork" "$ATCH" current
assert_exit     "current: single session → exit 0"         0 "$rc"
assert_eq       "current: single session → basename"       "mywork" "$out"

# deep path
run env ATCH_SESSION="/tmp/deep/path/proj" "$ATCH" current
assert_exit     "current: deep path → exit 0"        0 "$rc"
assert_eq       "current: deep path basename"        "proj" "$out"

# two levels of nesting: ATCH_SESSION = outer:inner
run env ATCH_SESSION="$HOME/.cache/atch/outer:$HOME/.cache/atch/inner" \
        "$ATCH" current
assert_exit     "current: nested 2 levels → exit 0"  0 "$rc"
assert_eq       "current: nested 2 levels → chain"   "outer > inner" "$out"

# three levels of nesting
run env ATCH_SESSION="/s/a:/s/b:/s/c" "$ATCH" current
assert_exit     "current: nested 3 levels → exit 0"  0 "$rc"
assert_eq       "current: nested 3 levels → chain"   "a > b > c" "$out"

# legacy -i
run "$ATCH" -i
assert_exit "legacy -i: outside session → exit 1"    1 "$rc"

# verify that ATCH_SESSION is set in the child and contains the socket path
"$ATCH" start envname-test sh -c \
    'printf "%s\n" "$ATCH_SESSION" > /tmp/atch-envname.txt'
sleep 0.1
run grep -q "envname-test" /tmp/atch-envname.txt
assert_exit "current: ATCH_SESSION set in child and contains socket path" 0 "$rc"
rm -f /tmp/atch-envname.txt

# verify that dashes (and other non-alphanumeric chars) in the binary name
# are replaced with underscores in the env var name:
# binary 'ssh2incus-atch' → env var 'SSH2INCUS_ATCH_SESSION'
DASH_ATCH="$TESTDIR/bin/ssh2incus-atch"
mkdir -p "$TESTDIR/bin"
ln -s "$ATCH" "$DASH_ATCH" 2>/dev/null || cp "$ATCH" "$DASH_ATCH"
"$DASH_ATCH" start envdash-test sh -c \
    'printf "%s\n" "$SSH2INCUS_ATCH_SESSION" > /tmp/atch-envdash.txt'
sleep 0.1
run grep -q "envdash-test" /tmp/atch-envdash.txt
assert_exit "current: dash in binary name → underscore in env var name" 0 "$rc"
"$DASH_ATCH" kill envdash-test >/dev/null 2>&1
rm -f /tmp/atch-envdash.txt

# ── 8. push command ───────────────────────────────────────────────────────────

run "$ATCH" push
assert_exit     "push: no session → exit 1"          1 "$rc"
assert_contains "push: no session → message"         "No session was specified" "$out"

run "$ATCH" push s-noexist-push
assert_exit     "push: nonexistent session → exit 1" 1 "$rc"

# push to a running session (cat echoes input back to pty → appears in log)
"$ATCH" start s-push cat
sleep 0.05
printf 'atch-push-marker\n' | "$ATCH" push s-push
sleep 0.1
run grep -q "atch-push-marker" "$HOME/.cache/atch/s-push.log"
assert_exit "push: data appears in session log"       0 "$rc"
tidy s-push

# extra args rejected
run "$ATCH" push s-noexist extra
assert_exit     "push: extra arg → exit 1"           1 "$rc"
assert_contains "push: extra arg → message"          "Invalid number of arguments" "$out"

# legacy -p (nonexistent session)
run "$ATCH" -p s-noexist-push
assert_exit "legacy -p: nonexistent → exit 1"        1 "$rc"

# ── 9. attach / new / open — TTY requirement ─────────────────────────────────
# All three require a TTY. Running in a non-TTY environment they must fail
# with a clear message (not a crash or silent hang).

run "$ATCH" attach s-notty
assert_exit     "attach: no tty → exit 1"            1 "$rc"
assert_contains "attach: no tty → message"           "requires a terminal" "$out"

run "$ATCH" a s-notty
assert_exit     "attach alias 'a': no tty → exit 1"  1 "$rc"
assert_contains "attach alias 'a': message"          "requires a terminal" "$out"

run "$ATCH" new s-notty
assert_exit     "new: no tty → exit 1"               1 "$rc"
assert_contains "new: no tty → message"              "requires a terminal" "$out"

run "$ATCH" n s-notty
assert_exit     "new alias 'n': no tty → exit 1"     1 "$rc"
assert_contains "new alias 'n': message"             "requires a terminal" "$out"

# default open (atch <session>) with no tty
run "$ATCH" s-notty-open
assert_exit     "open: no tty → exit 1"              1 "$rc"
assert_contains "open: no tty → message"             "requires a terminal" "$out"

# strict attach to nonexistent session still reports tty requirement first
run "$ATCH" attach s-noexist-notty
assert_exit     "attach: nonexistent + no tty → exit 1"     1 "$rc"
assert_contains "attach: nonexistent + no tty → message"    "requires a terminal" "$out"

# legacy -a
run "$ATCH" -a s-notty
assert_exit     "legacy -a: no tty → exit 1"         1 "$rc"
assert_contains "legacy -a: no tty → message"        "requires a terminal" "$out"

# legacy -A
run "$ATCH" -A s-notty
assert_exit     "legacy -A: no tty → exit 1"         1 "$rc"
assert_contains "legacy -A: message"                 "requires a terminal" "$out"

# legacy -c
run "$ATCH" -c s-notty
assert_exit     "legacy -c: no tty → exit 1"         1 "$rc"
assert_contains "legacy -c: message"                 "requires a terminal" "$out"

# ── 10. -q global option (before subcommand) ────────────────────────────────

"$ATCH" start gq-s sleep 999

run "$ATCH" -q list
assert_exit     "-q before list: exits 0"            0 "$rc"
assert_contains "-q before list: still shows data"   "gq-s" "$out"

"$ATCH" kill gq-s > /dev/null
run "$ATCH" -q list
assert_exit     "-q before list: empty → exits 0"    0 "$rc"
assert_eq       "-q before list: empty → no output"  "" "$out"

run "$ATCH" -q start gq-s2 sleep 999
assert_exit     "-q before start: exits 0"           0 "$rc"
assert_eq       "-q before start: no output"         "" "$out"
tidy gq-s2

run "$ATCH" -q clear gq-noexist
assert_exit     "-q before clear: exits 0"           0 "$rc"
assert_eq       "-q before clear: no output"         "" "$out"

# ── 11. -e / -E flag ─────────────────────────────────────────────────────────

# -e with ^X notation
run "$ATCH" start -e "^A" e-test sleep 999
assert_exit     "-e ^A: exits 0"                     0 "$rc"
assert_contains "-e ^A: confirmation"                "started" "$out"
tidy e-test

# -e with literal char
run "$ATCH" start -e "~" e-test2 sleep 999
assert_exit     "-e ~: exits 0"                      0 "$rc"
tidy e-test2

# -e with ^? (DEL)
run "$ATCH" start -e "^?" e-test3 sleep 999
assert_exit     "-e ^?: exits 0"                     0 "$rc"
tidy e-test3

# -e missing argument
run "$ATCH" start -e
assert_exit     "-e missing arg: exit 1"             1 "$rc"
assert_contains "-e missing arg: message"            "No escape character" "$out"

# -E disables detach char
run "$ATCH" start -E e-nodetach sleep 999
assert_exit     "-E: exits 0"                        0 "$rc"
assert_contains "-E: confirmation"                   "started" "$out"
tidy e-nodetach

# ── 12. -r / -R flag ─────────────────────────────────────────────────────────

for method in none ctrl_l winch; do
    run "$ATCH" start -r "$method" r-test sleep 999
    assert_exit     "-r $method: exits 0"            0 "$rc"
    tidy r-test
done

run "$ATCH" start -r badmethod r-test sleep 999
assert_exit     "-r badmethod: exit 1"               1 "$rc"
assert_contains "-r badmethod: message"              "Invalid redraw method" "$out"

run "$ATCH" start -r
assert_exit     "-r missing arg: exit 1"             1 "$rc"
assert_contains "-r missing arg: message"            "No redraw method" "$out"

for method in none move; do
    run "$ATCH" start -R "$method" R-test sleep 999
    assert_exit     "-R $method: exits 0"            0 "$rc"
    tidy R-test
done

run "$ATCH" start -R badmethod R-test sleep 999
assert_exit     "-R badmethod: exit 1"               1 "$rc"
assert_contains "-R badmethod: message"              "Invalid clear method" "$out"

run "$ATCH" start -R
assert_exit     "-R missing arg: exit 1"             1 "$rc"
assert_contains "-R missing arg: message"            "No clear method" "$out"

# ── 13. -z / -t flags ────────────────────────────────────────────────────────

run "$ATCH" start -z z-test sleep 999
assert_exit     "-z: exits 0"                        0 "$rc"
assert_contains "-z: confirmation"                   "started" "$out"
tidy z-test

run "$ATCH" start -t t-test sleep 999
assert_exit     "-t: exits 0"                        0 "$rc"
assert_contains "-t: confirmation"                   "started" "$out"
tidy t-test

# ── 14. combined / stacked flags ─────────────────────────────────────────────

# -qEzt combined in one flag
run "$ATCH" start -qEzt combo-test sleep 999
assert_exit     "combined -qEzt: exits 0"            0 "$rc"
assert_eq       "combined -qEzt: no output"          "" "$out"
tidy combo-test

# multiple separate flags before session
run "$ATCH" start -q -E -z -t combo2 sleep 999
assert_exit     "separate -q -E -z -t: exits 0"     0 "$rc"
assert_eq       "separate -q -E -z -t: no output"   "" "$out"
tidy combo2

# -e and -r together (e before r)
run "$ATCH" start -e "^B" -r none combo3 sleep 999
assert_exit     "-e + -r together: exits 0"          0 "$rc"
tidy combo3

# ── 15. -- separator ─────────────────────────────────────────────────────────

# A command that starts with - must be separated with --
run "$ATCH" start sep-test -- sleep 999
assert_exit     "-- separator: exits 0"              0 "$rc"
assert_contains "-- separator: confirmation"         "started" "$out"
tidy sep-test

# ── 16. invalid option ───────────────────────────────────────────────────────

run "$ATCH" start -x s-badopt sleep 999
assert_exit     "invalid option -x: exit 1"          1 "$rc"
assert_contains "invalid option -x: message"         "Invalid option" "$out"

# ── 17. custom socket path (name with /) ─────────────────────────────────────

CUSTOM_SOCK="$TESTDIR/custom-session"
run "$ATCH" start "$CUSTOM_SOCK" sleep 999
assert_exit     "custom path: exits 0"               0 "$rc"
assert_contains "custom path: confirmation"          "started" "$out"
[ -S "$CUSTOM_SOCK" ] && ok "custom path: socket created at given path" \
                        || fail "custom path: socket created at given path" "socket file" "not found"

run "$ATCH" list
assert_not_contains "custom path: not in default list" "custom-session" "$out"

run "$ATCH" kill "$CUSTOM_SOCK"
assert_exit     "custom path kill: exits 0"          0 "$rc"
assert_contains "custom path kill: stopped"          "stopped" "$out"

# ── 18. ATCH_SESSION env in child process ────────────────────────────────────

"$ATCH" start env-test sh -c 'echo "SESSION=$ATCH_SESSION" > /tmp/atch-env-test.txt'
sleep 0.1
run grep -q "SESSION=" /tmp/atch-env-test.txt
assert_exit "ATCH_SESSION set in child process"      0 "$rc"
rm -f /tmp/atch-env-test.txt

# ── 19. session log written on start ─────────────────────────────────────────

"$ATCH" start log-test sh -c 'printf "hello-log-test\n"; sleep 999'
sleep 0.1
run grep -q "hello-log-test" "$HOME/.cache/atch/log-test.log"
assert_exit "session output written to log"          0 "$rc"
tidy log-test

# ── 20. -C log cap flag ──────────────────────────────────────────────────────

# valid sizes: 0 (disable), k/K suffix, m/M suffix, bare bytes
run "$ATCH" start -C 0 C-test0 sleep 999
assert_exit     "-C 0: exits 0"                      0 "$rc"
assert_contains "-C 0: confirmation"                 "started" "$out"
# log file must NOT exist when logging is disabled
if [ -e "$HOME/.cache/atch/C-test0.log" ]; then
    fail "-C 0: no log file created" "no file" "file exists"
else
    ok "-C 0: no log file created"
fi
tidy C-test0

# -C before the subcommand (global pre-pass position)
run "$ATCH" -C 0 start C-global sleep 999
assert_exit     "-C 0 (global position): exits 0"    0 "$rc"
if [ -e "$HOME/.cache/atch/C-global.log" ]; then
    fail "-C 0 (global position): no log file" "no file" "file exists"
else
    ok "-C 0 (global position): no log file"
fi
tidy C-global

run "$ATCH" start -C 128k C-128k sleep 999
assert_exit     "-C 128k: exits 0"                   0 "$rc"
assert_contains "-C 128k: confirmation"              "started" "$out"
tidy C-128k

run "$ATCH" start -C 4m C-4m sleep 999
assert_exit     "-C 4m: exits 0"                     0 "$rc"
assert_contains "-C 4m: confirmation"                "started" "$out"
tidy C-4m

run "$ATCH" start -C 65536 C-bytes sleep 999
assert_exit     "-C <bytes>: exits 0"                0 "$rc"
tidy C-bytes

# missing argument
run "$ATCH" start -C
assert_exit     "-C missing arg: exit 1"             1 "$rc"
assert_contains "-C missing arg: message"            "No log size" "$out"

# invalid value
run "$ATCH" start -C foo C-bad sleep 999
assert_exit     "-C invalid: exit 1"                 1 "$rc"
assert_contains "-C invalid: message"                "Invalid log size" "$out"

# ── 21. tail command ─────────────────────────────────────────────────────────

# tail with no session
run "$ATCH" tail
assert_exit     "tail: no session → exit 1"          1 "$rc"
assert_contains "tail: no session → message"         "No session was specified" "$out"

# tail on nonexistent session (no log file)
run "$ATCH" tail s-noexist-tail
assert_exit     "tail: no log → exit 1"              1 "$rc"
assert_contains "tail: no log → message"             "no log" "$out"

# tail basic: start session that writes 20 numbered lines then sleeps
# Use zero-padded numbers so e.g. "line01" is not a substring of "line11"
"$ATCH" start s-tail sh -c \
    'i=1; while [ $i -le 20 ]; do printf "line%02d\n" $i; i=$((i+1)); done; sleep 999'
sleep 0.2
# default 10 lines: should see line11..line20 but not line01
run "$ATCH" tail s-tail
assert_exit         "tail: exits 0"                       0 "$rc"
assert_contains     "tail: shows recent line"             "line20" "$out"
assert_not_contains "tail: omits early lines"             "line01" "$out"

# -n 5: should see line16..line20
run "$ATCH" tail -n 5 s-tail
assert_exit         "tail -n 5: exits 0"                  0 "$rc"
assert_contains     "tail -n 5: shows line in range"      "line20" "$out"
assert_not_contains "tail -n 5: omits earlier lines"      "line10" "$out"

# -n with combined flag style (-n5)
run "$ATCH" tail -n5 s-tail
assert_exit         "tail -n5 (compact): exits 0"         0 "$rc"
assert_contains     "tail -n5 (compact): shows line20"    "line20" "$out"

# extra args rejected
run "$ATCH" tail s-tail extra
assert_exit     "tail: extra arg → exit 1"           1 "$rc"
assert_contains "tail: extra arg → message"          "Invalid number of arguments" "$out"

# invalid option
run "$ATCH" tail -x s-tail
assert_exit     "tail: invalid option → exit 1"      1 "$rc"
assert_contains "tail: invalid option → message"     "Invalid option" "$out"

# -n missing argument
run "$ATCH" tail -n
assert_exit     "tail -n missing arg: exit 1"        1 "$rc"
assert_contains "tail -n missing arg: message"       "-n requires an argument" "$out"

tidy s-tail

# ── 22. rm command ───────────────────────────────────────────────────────────

# rm with no session and no -a
run "$ATCH" rm
assert_exit     "rm: no session → exit 1"            1 "$rc"
assert_contains "rm: no session → message"           "No session was specified" "$out"

# rm nonexistent session (no socket, no log)
run "$ATCH" rm s-noexist-rm
assert_exit     "rm: nonexistent → exit 1"           1 "$rc"
assert_contains "rm: nonexistent → message"          "does not exist" "$out"

# rm a running session must be refused
"$ATCH" start s-rm-live sleep 999
run "$ATCH" rm s-rm-live
assert_exit     "rm: running session → exit 1"       1 "$rc"
assert_contains "rm: running session → message"      "is running" "$out"
tidy s-rm-live
rm -f "$HOME/.cache/atch/s-rm-live.log"

# rm a stale session: simulate stale via run+kill-9
"$ATCH" run s-rm-stale sleep 99999 &
RM_STALE_PID=$!
wait_socket s-rm-stale
kill -9 "$RM_STALE_PID" 2>/dev/null
sleep 0.1
printf "stale-output\n" > "$HOME/.cache/atch/s-rm-stale.log"

run "$ATCH" rm s-rm-stale
assert_exit     "rm: stale → exit 0"                 0 "$rc"
assert_contains "rm: stale → message"                "removed" "$out"
# socket and log must be gone
if [ -e "$HOME/.cache/atch/s-rm-stale" ]; then
    fail "rm: stale socket removed"   "gone" "still exists"
else
    ok   "rm: stale socket removed"
fi
if [ -e "$HOME/.cache/atch/s-rm-stale.log" ]; then
    fail "rm: stale log removed"      "gone" "still exists"
else
    ok   "rm: stale log removed"
fi

# rm an exited session (log only, no socket)
printf "exited-output\n" > "$HOME/.cache/atch/s-rm-exited.log"
run "$ATCH" rm s-rm-exited
assert_exit     "rm: exited → exit 0"                0 "$rc"
assert_contains "rm: exited → message"               "removed" "$out"
if [ -e "$HOME/.cache/atch/s-rm-exited.log" ]; then
    fail "rm: exited log removed"     "gone" "still exists"
else
    ok   "rm: exited log removed"
fi

# rm: extra arg rejected
run "$ATCH" rm s-noexist-rm extra
assert_exit     "rm: extra arg → exit 1"             1 "$rc"
assert_contains "rm: extra arg → message"            "Invalid number of arguments" "$out"

# rm -a: nothing to remove (clean state)
rm -f "$HOME/.cache/atch"/*.log 2>/dev/null || true
run "$ATCH" rm -a
assert_exit     "rm -a: nothing → exit 0"            0 "$rc"
assert_contains "rm -a: nothing → message"           "nothing to remove" "$out"

# rm -a: removes stale and exited sessions, leaves live ones alone
"$ATCH" run s-rma-stale sleep 99999 &
RMA_STALE_PID=$!
wait_socket s-rma-stale
kill -9 "$RMA_STALE_PID" 2>/dev/null
sleep 0.1
printf "stale\n" > "$HOME/.cache/atch/s-rma-stale.log"
printf "exited\n" > "$HOME/.cache/atch/s-rma-exited.log"
"$ATCH" start s-rma-live sleep 999

run "$ATCH" rm -a
assert_exit     "rm -a: exits 0"                     0 "$rc"
assert_contains "rm -a: reports removed sessions"    "removed" "$out"
assert_contains "rm -a: removes stale"               "s-rma-stale" "$out"
assert_contains "rm -a: removes exited"              "s-rma-exited" "$out"
assert_not_contains "rm -a: skips live session"      "s-rma-live" "$out"

# live session still present after rm -a
run "$ATCH" list
assert_contains "rm -a: live session still listed"   "s-rma-live" "$out"

tidy s-rma-live
rm -f "$HOME/.cache/atch/s-rma-live.log"

# rm -a with extra arg rejected
run "$ATCH" rm -a extra
assert_exit     "rm -a: extra arg → exit 1"          1 "$rc"
assert_contains "rm -a: extra arg → message"         "Invalid number of arguments" "$out"

# ── 23. no-args → usage ──────────────────────────────────────────────────────

# Invoking with zero arguments calls usage() (exits 0, prints help).
# We already consumed the binary name in main, so argc < 1 → usage().
run "$ATCH"
assert_exit     "no args: exits 0 (usage)"           0 "$rc"
assert_contains "no args: shows Usage:"              "Usage:" "$out"

# tail and rm commands appear in help
run "$ATCH" --help
assert_contains "help: shows tail command"           "tail" "$out"
assert_contains "help: shows rm command"             "rm" "$out"

# ── 23. fault injection: short socket writes are retried ───────────────────
# Force the first packet write to a socket to complete with 1 byte.
# Verifies write_all() retries correctly instead of treating short writes
# as fatal.

TESTS_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
OS_NAME=$(uname -s)

FAULT_LIB=
build_short_write_injector() {
    [ -n "$FAULT_LIB" ] && return 0
    case "$OS_NAME" in
        Darwin)
            FAULT_LIB="$TESTDIR/libshortwrite.dylib"
            cc -dynamiclib -O2 -Wall -o "$FAULT_LIB" \
                "$TESTS_DIR/preload_short_write.c" >/dev/null 2>&1 ;;
        *)
            FAULT_LIB="$TESTDIR/libshortwrite.so"
            cc -shared -fPIC -O2 -Wall -o "$FAULT_LIB" \
                "$TESTS_DIR/preload_short_write.c" -ldl >/dev/null 2>&1 ;;
    esac
}

with_short_socket_write() {
    build_short_write_injector || return 1
    case "$OS_NAME" in
        Darwin)
            env DYLD_INSERT_LIBRARIES="$FAULT_LIB" \
                DYLD_FORCE_FLAT_NAMESPACE=1 \
                ATCH_FAULT_SHORT_WRITE_ONCE=1 "$@" ;;
        *)
            env LD_PRELOAD="$FAULT_LIB" \
                ATCH_FAULT_SHORT_WRITE_ONCE=1 "$@" ;;
    esac
}

"$ATCH" start short-push sh -c 'cat'
wait_socket short-push
out=$(printf 'short-write-marker\n' | with_short_socket_write \
    "$ATCH" push short-push 2>&1)
prc=$?
assert_exit "fault: push retries short socket write" 0 "$prc"
sleep 0.2
assert_contains "fault: push data reaches session after short write" \
    "short-write-marker" "$(cat "$HOME/.cache/atch/short-push.log" 2>/dev/null)"
tidy short-push

"$ATCH" start short-kill sleep 999
wait_socket short-kill
out=$(with_short_socket_write "$ATCH" kill short-kill 2>&1)
krc=$?
assert_exit "fault: kill retries short socket write" 0 "$krc"
run "$ATCH" list
assert_not_contains "fault: session is gone after short-write kill" \
    "short-kill" "$out"
"$ATCH" kill -f short-kill >/dev/null 2>&1 || true

# ── summary ──────────────────────────────────────────────────────────────────

printf "\n1..%d\n" "$T"
printf "# %d passed, %d failed\n" "$PASS" "$FAIL"

[ "$FAIL" -eq 0 ]
