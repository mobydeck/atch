# TODO: Security Review Fixes (2026-06-25-1)

Tracking document for security review findings from SECURITYREVIEW-2026-06-25-1.md

## Critical Priority (Fix Before Release)

None - no critical findings in this review.

## High Priority (Fix This Sprint)

None - no high findings in this review.

## Medium Priority (Should Fix)

- [ ] **2026-06-25-1-1**: Predictable `/tmp/.atch-<uid>` fallback + symlink-following log/socket creation
  - Files: `atch.c` (get_session_dir:37-57, expand_sockname:258-280), `master.c` (open_log:82-93)
  - Issue: When `$HOME` is unset, sessions fall back to the fixed world-known path `/tmp/.atch-<uid>`; `mkdir` results are unchecked and the log is opened `O_RDWR|O_CREAT` without `O_NOFOLLOW`, so a local attacker can pre-plant a symlink and make the victim truncate/redirect an arbitrary victim-writable file.
  - OWASP / WSTG / API / NIST CSF: A01/A04 / WSTG-CONF-09 / — / PR.AA-05, PR.DS-01
  - Action: Prefer `$XDG_RUNTIME_DIR`; verify the session dir is a non-symlink directory owned by the caller with mode `0700` (bail otherwise); open the log with `O_NOFOLLOW` (and use `openat` from a verified dir fd). Refuse to operate on session files that are symlinks.

- [ ] **2026-06-25-1-11**: Unbounded clients drive `FD_SET` past `FD_SETSIZE` → master crash/corruption
  - File: `master.c` (control_activity:467-496, main-loop FD_SET:654-657, pty_activity FD_SET:417-422)
  - Issue: `accept` has no `MAX_CLIENTS` cap and no `fd >= FD_SETSIZE` rejection; idle accepted clients are never pruned. A same-uid process can hold open many connections; once an accepted fd reaches `FD_SETSIZE` (~1023), `FD_SET(p->fd, …)` writes out of bounds — UB, in practice stack corruption/crash of the long-lived master. (Crash trigger requires the master's `RLIMIT_NOFILE` soft limit > `FD_SETSIZE`, common on modern systemd/container defaults.)
  - OWASP / WSTG / API / NIST CSF: A04 / WSTG-BUSL-03 / API4 / PR.IR-03, DE.CM-09, RS.MI-02
  - Action: Immediate — reject and `close` accepted fds `>= FD_SETSIZE`. Durable — enforce a hard `MAX_CLIENTS` cap + idle-connection timeout and migrate the `select`/`fd_set` loops to `poll`/`ppoll`/`epoll` (do this together with the 1-2 per-client queues).

- [ ] **2026-06-25-1-2**: Sole/all-clients-stalled freezes the session and stalls the child (head-of-line DoS)
  - File: `master.c` (pty_activity:368-464)
  - Issue: When the sole attached client (or all attached clients) stop reading (full socket buffer, e.g. Ctrl-S/slow link), `pty_activity` reaches `nclients == 0` and the `goto top` re-blocks the master in its inner `select` with no timeout — stalling pty reads, the child process, and detach handling. (Reconciled: if a co-attached client is still draining, the function returns and only the stuck client misses that chunk — the "freezes everyone with one stuck client among several" framing is overstated.)
  - OWASP / WSTG / API / NIST CSF: A04 / WSTG-BUSL-03 / API4 / PR.IR-03
  - Action: Give each client a bounded outbound queue; never block pty reading on a single client; flush per-client in the main `select` loop and drop a client whose queue exceeds the cap.

## Low Priority (Nice to Have)

- [ ] **2026-06-25-1-3**: Control socket has no message origin check (same-uid input/signal injection)
  - File: `master.c` (client_activity:498-576, control_activity:467-496)
  - Issue: `MSG_PUSH`/`MSG_KILL` are honored from any process that can connect; with `0600` perms that means any same-uid process can inject keystrokes and signals into any live session.
  - OWASP / WSTG / API / NIST CSF: A01/A07 / WSTG-ATHZ-02 / API2 / PR.AA-05
  - Action: Document the same-uid trust boundary in the README; add a `SO_PEERCRED` uid==owner check on `accept` (defense-in-depth, Linux).

- [ ] **2026-06-25-1-4**: Short read disconnects a valid client (stream-framing assumption)
  - File: `master.c` (client_activity:499-517)
  - Issue: A single `read` is assumed to return exactly one 10-byte packet; any short read closes the client. Not guaranteed for `SOCK_STREAM` under fragmentation/signal interruption.
  - OWASP / WSTG / API / NIST CSF: A04 / WSTG-BUSL-09 / — / PR.IR-03
  - Action: Reassemble into a per-client buffer until a full `struct packet` is available; close only on EOF/hard error.

- [ ] **2026-06-25-1-5**: Async-signal-unsafe `printf`/`exit`/`system` in signal handlers
  - Files: `attach.c` (die:153-166, restore_term:85-95), `master.c` (master_die:115-126)
  - Issue: Handlers call `printf`/`exit` (which runs `atexit`→`restore_term`→`printf`/`system`); none are async-signal-safe → possible deadlock/corruption on signal delivery.
  - OWASP / WSTG / API / NIST CSF: A04 / WSTG-BUSL / — / PR.IR-03
  - Action: Set a `volatile sig_atomic_t` flag in handlers and do printing/exit in the main loop; use `_exit`/`write(2)` only inside handlers.

- [ ] **2026-06-25-1-6**: Session log fd leaked to executed program (no `FD_CLOEXEC`)
  - File: `master.c` (open_log:82-93)
  - Issue: `log_fd` is opened without `O_CLOEXEC`/`FD_CLOEXEC` (unlike the control socket) and is inherited by the exec'd child, which can read/`ftruncate` its own on-disk history.
  - OWASP / WSTG / API / NIST CSF: A05/A08 / WSTG-CONF-09 / API8 / PR.PS-01
  - Action: Add `O_CLOEXEC` to the `open` in `open_log` (one line); audit other long-lived fds.

- [ ] **2026-06-25-1-7**: `parse_size` integer overflow + unchecked malloc / path truncation
  - File: `atch.c` (parse_size:111-134, expand_sockname:276-279)
  - Issue: `-C` size multiply (`*1024`/`*1024*1024`) has no overflow check → silently tiny/zero log cap; `expand_sockname` malloc unchecked; fixed `*.log`/dir buffers truncate over-long session paths, causing socket/log path mismatch.
  - OWASP / WSTG / API / NIST CSF: A04/A05 / WSTG-INPV-11 / — / PR.PS-01
  - Action: Overflow-check the multiply and reject; check malloc; detect `snprintf` truncation and error out; centralize `<path>.log` building.

- [ ] **2026-06-25-1-8**: Scrollback ring overwritten mid-replay → corrupted/out-of-order history
  - File: `master.c` (scrollback_append:294-317, replay_start/replay_drain:319-364, main loop:686-700)
  - Issue: Replay streams from the live ring across `select` iterations; an `EAGAIN` pause lets `scrollback_append` overwrite unread bytes, garbling replayed history for slow clients on busy sessions.
  - OWASP / WSTG / API / NIST CSF: A04/A08 / WSTG-BUSL-07 / — / PR.DS-01
  - Action: Snapshot the replay region into a per-client buffer at `replay_start`; or track a wrap generation and restart/fall back to the on-disk log if the ring wrapped past `replay_head`.

- [ ] **2026-06-25-1-9**: No security/concurrency tests, no CI/SAST, no CHANGELOG (documentation + tests)
  - Files: `tests/test.sh`, `makefile`
  - Issue: The suite covers CLI surface but has no permission/symlink/fd-leak/concurrency/signal tests; no CI workflow, no Semgrep/ASan/UBSan, no `CHANGELOG.md` for WONTFIX/ONHOLD tracking.
  - OWASP / WSTG / API / NIST CSF: A04/A09 / WSTG-CONF-09 / — / GV.SC-09, DE.CM-09
  - Action: Add a `security` test group (mode assertions, symlink refusal, fd-leak, multi-client/slow-reader, mid-replay) and a CI workflow running test.sh + Semgrep + ASan/UBSan; add `CHANGELOG.md`.

- [ ] **2026-06-25-1-10**: Verbatim log replay is a terminal-escape injection vector
  - Files: `attach.c` (replay_session_log:228-257), `atch.c` (cmd_tail:580-660)
  - Issue: Cold attach / `tail` `write(1, …)` raw log bytes with no filtering; an attacker who can author a log (see 1-1 or a planted log) can inject escape sequences (OSC 52 clipboard, title/hyperlink abuse) into the victim's terminal.
  - OWASP / WSTG / API / NIST CSF: A03 / WSTG-CLNT-04 / — / PR.DS-02
  - Action: Close the authoring vector via 1-1; add an opt-in `--sanitize` mode for cold replay/`tail` that strips C1/`ESC` control bytes; document residual risk (inherent to raw passthrough).

## Won't Fix (From This Review)

None - all issues have actionable fixes. (2026-06-25-1-3 and 2026-06-25-1-10 are partly inherent to the same-uid / raw-passthrough design; the actions above are defense-in-depth rather than full eliminations.)

## Summary

| Priority | Count | Status |
|----------|-------|--------|
| Critical | 0 | — |
| High | 0 | — |
| Medium | 3 | Pending |
| Low | 8 | Pending |
| **Total** | **11** | **0 Fixed** |

---

## Implementation Order (Suggested)

1. **Critical first**: none.
2. **High priority batch**: none.
3. **Quick wins (low-risk, one-liners)**: 2026-06-25-1-6 (`O_CLOEXEC`), 2026-06-25-1-7 (overflow/malloc checks), 2026-06-25-1-11 stop-gap (reject `fd >= FD_SETSIZE` at `accept`).
4. **Medium batch**: 2026-06-25-1-1 (safe dir + `O_NOFOLLOW`), 2026-06-25-1-2 + 2026-06-25-1-11 durable fix (per-client queue + slow-client drop + `MAX_CLIENTS` cap + `poll`/`epoll` migration, also resolves 2026-06-25-1-4).
5. **Hardening batch**: 2026-06-25-1-5 (signal safety), 2026-06-25-1-8 (replay snapshot), 2026-06-25-1-3 (`SO_PEERCRED` + docs), 2026-06-25-1-10 (sanitize mode + docs).
6. **Assurance**: 2026-06-25-1-9 (security tests + CI/SAST + CHANGELOG) — land alongside the fixes so each is regression-guarded.

Group fixes by control family: transport/IPC & permissions (1-1, 1-3, 1-6), resilience/backpressure (1-2, 1-4, 1-5, 1-8, 1-11), input validation (1-7, 1-10), assurance/CI (1-9).

---

## Change Log

| Date | Action |
|------|--------|
| 2026-06-25 | Initial TODO created from security review (Agent A / Claude) |
| 2026-06-25 | Added 2026-06-25-1-11 (Agent B / Codex finding, verified VALID by Agent A); reconciled 2026-06-25-1-2 to PARTIALLY_VALID; refreshed counts (Medium 2→3, Low 7→8, Total 9→11) |
