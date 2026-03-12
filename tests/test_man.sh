#!/bin/sh
# Man page tests for atch.
# Usage: sh tests/test_man.sh [path-to-atch.1]
# Verifies structure, mandatory sections, and content of the man page.

MAN_PAGE="${1:-./atch.1}"

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

assert_contains() {
    case "$3" in *"$2"*) ok "$1" ;; *) fail "$1" "(contains '$2')" "$3" ;; esac
}

assert_exit() {
    if [ "$2" = "$3" ]; then ok "$1"; else fail "$1" "exit $2" "exit $3"; fi
}

printf "TAP version 13\n"

# ── 1. file exists ────────────────────────────────────────────────────────────

if [ -f "$MAN_PAGE" ]; then
    ok "man page file exists"
else
    fail "man page file exists" "file" "not found at $MAN_PAGE"
    printf "\n1..%d\n" "$T"
    printf "# %d passed, %d failed\n" "$PASS" "$FAIL"
    exit 1
fi

CONTENT=$(cat "$MAN_PAGE")

# ── 2. mandatory roff sections ────────────────────────────────────────────────

assert_contains "section NAME present"        ".SH NAME"        "$CONTENT"
assert_contains "section SYNOPSIS present"    ".SH SYNOPSIS"    "$CONTENT"
assert_contains "section DESCRIPTION present" ".SH DESCRIPTION" "$CONTENT"
assert_contains "section COMMANDS present"    ".SH COMMANDS"    "$CONTENT"
assert_contains "section OPTIONS present"     ".SH OPTIONS"     "$CONTENT"
assert_contains "section FILES present"       ".SH FILES"       "$CONTENT"
assert_contains "section ENVIRONMENT present" ".SH ENVIRONMENT" "$CONTENT"
assert_contains "section EXIT STATUS present" ".SH EXIT STATUS" "$CONTENT"
assert_contains "section EXAMPLES present"    ".SH EXAMPLES"    "$CONTENT"
assert_contains "section SEE ALSO present"    ".SH SEE ALSO"    "$CONTENT"
assert_contains "section AUTHORS present"     ".SH AUTHORS"     "$CONTENT"

# ── 3. TH macro (title header) ───────────────────────────────────────────────

assert_contains "TH macro section 1"          ".TH ATCH 1"      "$CONTENT"

# ── 4. commands documented ───────────────────────────────────────────────────

assert_contains "command 'attach' documented"  "attach"   "$CONTENT"
assert_contains "command 'new' documented"     "new"      "$CONTENT"
assert_contains "command 'start' documented"   "start"    "$CONTENT"
assert_contains "command 'run' documented"     "run"      "$CONTENT"
assert_contains "command 'push' documented"    "push"     "$CONTENT"
assert_contains "command 'kill' documented"    "kill"     "$CONTENT"
assert_contains "command 'clear' documented"   "clear"    "$CONTENT"
assert_contains "command 'list' documented"    "list"     "$CONTENT"
assert_contains "command 'current' documented" "current"  "$CONTENT"

# ── 5. options documented ─────────────────────────────────────────────────────

assert_contains "option -e documented"   "\\-e"  "$CONTENT"
assert_contains "option -E documented"   "\\-E"  "$CONTENT"
assert_contains "option -r documented"   "\\-r"  "$CONTENT"
assert_contains "option -R documented"   "\\-R"  "$CONTENT"
assert_contains "option -z documented"   "\\-z"  "$CONTENT"
assert_contains "option -q documented"   "\\-q"  "$CONTENT"
assert_contains "option -t documented"   "\\-t"  "$CONTENT"
assert_contains "option -C documented"   "\\-C"  "$CONTENT"
assert_contains "option -f for kill documented" "\\-f"  "$CONTENT"

# ── 6. environment variable documented ───────────────────────────────────────

assert_contains "ATCH_SESSION documented"     "ATCH_SESSION"   "$CONTENT"

# ── 7. man renders without error ─────────────────────────────────────────────

mandoc "$MAN_PAGE" > /dev/null 2>&1
assert_exit "man renders without error (mandoc)" 0 "$?"

# ── summary ──────────────────────────────────────────────────────────────────

printf "\n1..%d\n" "$T"
printf "# %d passed, %d failed\n" "$PASS" "$FAIL"

[ "$FAIL" -eq 0 ]
