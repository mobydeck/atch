# TODO: Code Review Fixes (2026-06-25-1)

Tracking document for code review findings from CODEREVIEW-2026-06-25-1.md

## Critical Priority (Fix Before Release)

None.

## High Priority (Fix Soon)

None.

## Medium Priority (Should Fix)

- [ ] **2026-06-25-1-2**: `pty_activity` drops/duplicates live output to slow clients
  - File: `master.c:438`
  - Issue: Non-blocking client fds have no per-client live-write backlog; on partial/EAGAIN writes the remaining bytes are lost (when another client completes) or re-sent from offset 0 (single slow client), corrupting output.
  - Action: Give `struct client` a pending live-write buffer/offset (mirroring `replay_head`/`replay_remaining`) and drain it from the `writefds` branch; drop the client only if the backlog exceeds a bound. May be tracked as a deliberate design item if matching upstream dtach is acceptable.

- [ ] **2026-06-25-1-3**: Scrollback ring overwritten during slow replay
  - File: `master.c:354`
  - Issue: `replay_start` snapshots absolute ring indices, but `scrollback_append` keeps overwriting the same ring during the replay, corrupting replayed bytes and dropping output produced during the replay window.
  - Action: On overwrite, clamp/re-snapshot the replaying client's `replay_head`/`replay_remaining` to the new oldest valid byte, or fall back to disk-log replay.

- [ ] **2026-06-25-1-7**: `tail -f` stops after log rotation
  - File: `atch.c:649`
  - Issue: Follow loop never revalidates its offset; after `rotate_log` truncates the file the reader sits past EOF and shows nothing further.
  - Action: Detect truncation (compare file size to last read offset) and seek back to start when the file shrinks.

- [ ] **2026-06-25-1-11**: Signal handlers call non-async-signal-safe functions
  - Files: `attach.c:153`, `master.c:115`, `attach.c:93`
  - Issue: `die`/`master_die` call `printf`/`snprintf`/`exit` (and via atexit, `system("tput init")`) from signal context.
  - Action: Use `_exit` and/or a `volatile sig_atomic_t` flag handled in the main loop; restrict handlers to async-signal-safe calls.

- [ ] **2026-06-25-1-1**: `expand_sockname` unchecked `malloc` (NULL deref)
  - File: `atch.c:277`
  - Issue: `malloc` result passed to `snprintf` without a NULL check; reachable on nearly every command.
  - Action: Check `malloc`; print "out of memory" and exit(1) on failure.

- [ ] **2026-06-25-1-17**: Control protocol assumes `SOCK_STREAM` packets are atomic (Agent B) — *DISAGREEMENT*
  - File: `master.c:505`
  - Issue: Master reads one `struct packet` with a single non-blocking `read` and drops the client on any short read; the write side (`write_packet_or_fail`, `push_main`, `send_kill`) assumes whole-packet writes. Stream sockets don't preserve message boundaries. Agent B rated MEDIUM; Agent A rated practical severity LOW because `struct packet` is only ~12 bytes (`1 + 1 + sizeof(winsize)`), so fragmentation over AF_UNIX is effectively impossible. Verdicts: Codex VALID, Claude PARTIALLY_VALID — needs human review.
  - Action: Add length-prefixed/offset-tracking framing (per-client `inbuf`/`in_used` read loop; offset-tracking write loop on all senders), or document/accept the small-packet atomicity assumption.

## Low Priority (Nice to Have)

- [ ] **2026-06-25-1-4**: Legacy single-dash dispatch ignores clustered flag letters
  - File: `atch.c:817`
  - Issue: `mode = argv[0][1]` ignores any further characters, so `atch -nq ...` silently drops `q`.
  - Action: Reject legacy flags longer than two chars, or document that legacy mode letters cannot be clustered (documentation only acceptable).

- [ ] **2026-06-25-1-5**: `parse_size` multiplier overflow
  - File: `atch.c:113`
  - Issue: `v *= 1024`/`v *= 1024*1024` and `strtoul` ERANGE unchecked; a huge `-C` value wraps to a small cap.
  - Action: Check `errno == ERANGE` and bound `v` before multiplying.

- [ ] **2026-06-25-1-6**: High-bit detach character never matches (signedness)
  - File: `atch.c:177`
  - Issue: `detach_char = (*argv)[0][0]` can be negative on signed-char platforms; compared against `unsigned char` keystroke it never matches.
  - Action: Cast to `(unsigned char)` when storing.

- [ ] **2026-06-25-1-8**: `tail -f` never exits on closed output pipe
  - File: `atch.c:650`
  - Issue: SIGPIPE ignored and `write(1,...)` result discarded, so `tail -f | head` loops forever as an orphan.
  - Action: Check `write` result and exit on EPIPE (or don't ignore SIGPIPE in follow mode).

- [ ] **2026-06-25-1-9**: On-disk log can reach ~2× the configured cap
  - File: `master.c:393`
  - Issue: Rotation triggers on session-local `log_written`, ignoring the existing file size at startup, so the file oscillates between ~1× and ~2× the cap — inconsistent with the README's "capped at 1 MB". (Severity LOW per the report header; sits on the Medium/Low boundary.)
  - Action: Track actual file size for the trigger (seed `log_written` from post-rotation size, or compare `lseek(SEEK_CUR)` to the cap), or document the overshoot.

- [ ] **2026-06-25-1-10**: `scrollback_append` byte-by-byte copy (perf)
  - File: `master.c:296`
  - Issue: Per-byte copy with per-byte masking/branching on the master's main output path.
  - Action: Replace with up to two `memcpy` calls (power-of-two ring), adjusting len/head once.

- [ ] **2026-06-25-1-12**: Duplicated write-failed reporting block
  - File: `attach.c:42`
  - Issue: Identical ~12-line block in `write_buf_or_fail` and `write_packet_or_fail`.
  - Action: Extract a `static void write_failed(void)` helper.

- [ ] **2026-06-25-1-13**: Duplicated "invalid arguments / Try --help" blocks
  - File: `atch.c:405`
  - Issue: Same two printf lines repeated in ~8 command handlers.
  - Action: Add a `usage_error()` helper returning 1.

- [ ] **2026-06-25-1-14**: Scattered magic-number path buffers (PARTIALLY_VALID — both agents)
  - File: `attach.c:737`
  - Issue: Inconsistent hard-coded path sizes (600/512/768/800) that must be kept mutually consistent by hand, and `snprintf` truncation is generally unchecked. Note: the original `path[768]` "off-by-one" claim is NOT real — `dir[512]` holds at most a 511-char string, so 511 + '/' + NAME_MAX(255) + NUL = 768 fits exactly. Only the broader maintainability concern stands.
  - Action: Centralize a `SESSION_PATH_MAX` (or derive from `PATH_MAX`); optionally check `snprintf` truncation on path-building. No buffer resize is actually required for correctness.

- [ ] **2026-06-25-1-15**: Dead `config.h` defines (documentation/cleanup)
  - File: `config.h`
  - Issue: `HAVE_SYS_TIME_H` and `HAVE_LIBUTIL` are defined but never referenced (`<sys/time.h>` is included unconditionally; the real guard is `HAVE_LIBUTIL_H`).
  - Action: Remove the unused defines (or actually guard `<sys/time.h>` with `HAVE_SYS_TIME_H`).

- [ ] **2026-06-25-1-16**: Unreachable `return 0` after `master_process`
  - File: `master.c:757`
  - Issue: `master_process` never returns; the two trailing `return 0;` (master.c:757, 771) are dead.
  - Action: Mark `master_process` `noreturn` and drop the dead returns, or add a clarifying comment.

- [ ] **2026-06-25-1-18**: `tail -n` accepts nonnumeric counts as `1` (Agent B)
  - File: `atch.c:603`
  - Issue: `cmd_tail` parses `-n N` / `-nN` with `atoi`; `atoi("foo")==0` and the `nlines < 1` clamp turns it into 1, so `tail -nfoo` is silently accepted instead of erroring like other invalid options.
  - Action: Parse with a strict `parse_positive_int` (`strtol` + `ERANGE` + `*end=='\0'` + `INT_MAX` bound) and reject malformed counts; drop or document the `<1` clamp.

- [ ] **2026-06-25-1-19**: `create_socket` leaves a bound socket pathname behind on post-bind failures (Agent B)
  - File: `master.c:260`
  - Issue: After `bind` creates the socket file, the `listen`/`setnonblocking`/`chmod` failure branches `close(s); return -1;` without `unlink(name)`, leaving a stale socket that can confuse future session creation.
  - Action: Add `unlink(name)` to each post-bind failure branch, ideally via a single `goto fail_bound;` cleanup label.

- [ ] **2026-06-25-1-20**: Dead `S_ISREG` fallback macro (Agent B)
  - File: `atch.h:64`
  - Issue: `atch.h:64-65` defines a fallback `S_ISREG`, but no code references `S_ISREG` (only `S_ISSOCK` is used). Inherited dead compatibility scaffolding.
  - Action: Remove the unused `#ifndef S_ISREG` / `#define S_ISREG` block.

## Won't Fix (From This Review)

None - all issues have actionable fixes (a few may be accepted as design trade-offs after human review, notably 2026-06-25-1-2).

## Summary

| Priority | Count | Status |
|----------|-------|--------|
| Critical | 0 | - |
| High | 0 | - |
| Medium | 6 | Pending |
| Low | 14 | Pending |
| **Total** | **20** | **0 Fixed** |

Two issues carry an unresolved Agent A/Agent B verification disagreement and need human review: **2026-06-25-1-14** (off-by-one not real; broader concern PARTIALLY_VALID by both) and **2026-06-25-1-17** (Codex VALID / Claude PARTIALLY_VALID on severity).

---

## Implementation Order (Suggested)

1. **Critical first**: none
2. **High priority batch**: none
3. **Medium priority batch**: 2026-06-25-1-1 (quick NULL guard), 2026-06-25-1-7, 2026-06-25-1-3, 2026-06-25-1-11, 2026-06-25-1-2 (largest change); 2026-06-25-1-17 after human review of severity
4. **Low priority batch**: 2026-06-25-1-5, -6, -8, -9, -15, -16, -19, -20 (quick), then -4, -10, -12, -13, -14, -18

---

## Change Log

| Date | Action |
|------|--------|
| 2026-06-25 | Initial TODO created from code review CODEREVIEW-2026-06-25-1 |
| 2026-06-25 | Added Agent B findings 2026-06-25-1-17..-20; Agent A cross-verified them and reconciled issues -14 (VALID→PARTIALLY_VALID) and -17; counts updated to 20 total (6 Medium / 14 Low) |
| 2026-06-25 | Step 2.7 refresh: verified Executive Summary / per-pass counts (no change needed); moved -9 (final severity LOW) from the Medium to the Low section so TODO sections match header severities and the 6 Medium / 14 Low summary |

---
