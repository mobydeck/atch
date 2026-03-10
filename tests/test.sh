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

# ── 21. no-args → usage ──────────────────────────────────────────────────────

# Invoking with zero arguments calls usage() (exits 0, prints help).
# We already consumed the binary name in main, so argc < 1 → usage().
run "$ATCH"
assert_exit     "no args: exits 0 (usage)"           0 "$rc"
assert_contains "no args: shows Usage:"              "Usage:" "$out"

# ── 23. detach-status: S_IXUSR cleared immediately after MSG_DETACH ──────────
#
# Regression test for: when the client detaches (Ctrl+\), it must send
# MSG_DETACH to the master BEFORE calling exit(0).  This ensures the master
# clears the S_IXUSR bit on the socket synchronously (within one select cycle)
# so that `atch list` never races with a stale "[attached]" status.
#
# Without the fix, the client exits without MSG_DETACH; the master only learns
# about the detach when it receives EOF on the closed fd, which can arrive after
# a `list` reads the stale S_IXUSR bit — especially on loaded systems.
#
# Strategy: use Python to simulate the two scenarios:
#   A. MSG_DETACH sent before close  → socket must lose S_IXUSR immediately
#   B. Close without MSG_DETACH      → socket loses S_IXUSR after one master
#                                       select cycle (tolerated, but slower)
#
# The critical invariant tested here is scenario A: after MSG_DETACH is sent
# and acknowledged, `list` must NOT show "[attached]".  This is the exact
# behaviour enforced by the fix in process_kbd.

if command -v python3 >/dev/null 2>&1; then

    # Helper: send MSG_ATTACH, optionally MSG_DETACH, then close.
    # Usage: attach_and_detach <sock_path> <send_detach: 0|1>
    attach_and_detach() {
        python3 - "$1" "$2" << 'PYEOF'
import socket, struct, sys, time

sock_path = sys.argv[1]
send_detach = sys.argv[2] == '1'

MSG_ATTACH = 1
MSG_DETACH = 2

def pkt(msg_type):
    return struct.pack('BB8s', msg_type, 0, b'\x00' * 8)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock_path)
s.sendall(pkt(MSG_ATTACH))
time.sleep(0.05)   # let master process MSG_ATTACH and set S_IXUSR
if send_detach:
    s.sendall(pkt(MSG_DETACH))
    time.sleep(0.05)  # let master process MSG_DETACH and clear S_IXUSR
s.close()
PYEOF
    }

    # --- single session: proper MSG_DETACH flow (scenario A) ---
    "$ATCH" start det-s1 sleep 999
    wait_socket det-s1
    SOCK1="$HOME/.cache/atch/det-s1"

    attach_and_detach "$SOCK1" 1   # send MSG_DETACH before close
    sleep 0.05                     # minimal delay after close

    run "$ATCH" list
    assert_not_contains \
        "detach-status: session not shown as attached after MSG_DETACH" \
        "[attached]" "$out"

    tidy det-s1

    # --- two sessions: reproduce the multi-session attach/detach cycle ---
    # Steps mirror the exact reproduction sequence from the bug report:
    #   create s1, detach, create s2, detach,
    #   attach s1, detach, attach s2, detach → none should show [attached]
    "$ATCH" start det-a sleep 999
    "$ATCH" start det-b sleep 999
    wait_socket det-a
    wait_socket det-b
    SOCKA="$HOME/.cache/atch/det-a"
    SOCKB="$HOME/.cache/atch/det-b"

    attach_and_detach "$SOCKA" 1
    sleep 0.05
    attach_and_detach "$SOCKB" 1
    sleep 0.05
    attach_and_detach "$SOCKA" 1
    sleep 0.05

    run "$ATCH" list
    assert_not_contains \
        "detach-status: det-a not [attached] after second detach cycle" \
        "[attached]" "$out"

    attach_and_detach "$SOCKB" 1
    sleep 0.05

    run "$ATCH" list
    assert_not_contains \
        "detach-status: det-b not [attached] after detach cycle" \
        "[attached]" "$out"

    tidy det-a
    tidy det-b

else
    ok "detach-status: skip (python3 not available)"
    ok "detach-status: skip (python3 not available)"
    ok "detach-status: skip (python3 not available)"
fi

# ── summary ──────────────────────────────────────────────────────────────────

printf "\n1..%d\n" "$T"
printf "# %d passed, %d failed\n" "$PASS" "$FAIL"

[ "$FAIL" -eq 0 ]
