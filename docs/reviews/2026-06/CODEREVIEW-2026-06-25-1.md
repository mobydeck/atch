# Code Review: atch

**Reviewers:** Claude (Agent A), Codex (Agent B)
**Date:** 2026-06-25
**Review ID:** 2026-06-25-1
**Codebase Version:** dev (HEAD 15e0d3a "Add `rm` command to remove stale and exited sessions")
**Previous Reviews:** None (first review of this project)

---

## How to Conduct the Review

See `docs/reviews/CODEREVIEW-prompt.md` and `docs/reviews/CODEREVIEW-template.md`. This is a 5-pass review (Bug Hunt, Logic & Edge Cases, Reliability & Performance, Maintainability, Dead/Obsolete Code) with dual-agent verification.

---

## Executive Summary

This review identified **20 issues** across 5 passes (16 from Agent A, 4 appended by Agent B). `atch` is a compact C terminal attach/detach tool (a dtach-style fork) with added persistent on-disk session logs, scrollback replay, session listing/removal, and a tail command. The most impactful findings are correctness issues in the live data-fanout path (`pty_activity` drops or duplicates output to slow clients) and in scrollback replay (the in-memory ring can be overwritten while a slow client is mid-replay), plus reliability gaps in `tail -f` (breaks on log rotation, never exits on a closed pipe). No memory-safety overflow or remote-exploitable issue was found; buffer copies into `sun_path` are length-guarded. There are also several maintainability cleanups and dead `config.h`/`atch.h` defines.

After cross-verification and reconciliation both agents agree on 19 of the 20 issues. **2026-06-25-1-14** is reconciled (both agents PARTIALLY_VALID: the specific `path[768]` off-by-one is not real, only the broader maintainability concern is). One issue carries a residual, severity-only disagreement left for human review: **2026-06-25-1-17** (Agent A PARTIALLY_VALID vs Agent B VALID — both agree the stream-socket framing reliance is real, but with the fixed 10-byte `struct packet` written on a blocking client socket the MEDIUM impact does not materialize, so Agent A's reconciled stance is that practical severity is LOW; there is no dispute about the code itself). See the Step 2.6 Reconciliation section at the end of this report.

| Pass | Focus | Issues Found |
|------|-------|--------------|
| 1 | Bug Hunt | 3 issues |
| 2 | Logic & Edge Cases | 5 issues |
| 3 | Reliability & Performance | 6 issues |
| 4 | Maintainability | 3 issues |
| 5 | Dead / Obsolete Code | 3 issues |

**Critical:** 0
**High:** 0
**Medium:** 6
**Low:** 14
**Total:** 20
**FIXED:** 0
**WONTFIX:** 0
**ONHOLD:** 0

---

## Pass 1: Bug Hunt

*Act as a senior engineer doing a pre-release bug hunt. Look for obvious bugs, crashes, data corruption, security issues.*

### 2026-06-25-1-1: `expand_sockname` does not check `malloc` result before use

**Status:** OPEN
**Severity:** MEDIUM
**Pass:** Bug Hunt (Pass 1)
**File:** [atch.c:277](../../../atch.c#L277)

`expand_sockname()` allocates the full socket path with `malloc` and immediately writes to it with `snprintf` and assigns it to the global `sockname`, without checking for `NULL`. Under memory pressure `malloc` returns `NULL`, and `snprintf(NULL, ...)` is undefined behavior (typically a SIGSEGV). Every primary command path (`cmd_open`, `cmd_new`, `cmd_attach`, etc.) routes through `expand_sockname` via `consume_session`/`cmd_open`, so this is the first thing that runs for nearly every invocation.

**Evidence:**
```c
-- atch.c:276-279
	fulllen = strlen(dir) + 1 + strlen(sockname);
	full = malloc(fulllen + 1);
	snprintf(full, fulllen + 1, "%s/%s", dir, sockname);
	sockname = full;
```

**Impact:** Crash (NULL dereference) on allocation failure instead of a clean error message. Low probability but trivially avoidable, and the program is in a known-bad state if it happens silently.

**Fix:**
```diff
--- a/atch.c
+++ b/atch.c
@@ expand_sockname
 	fulllen = strlen(dir) + 1 + strlen(sockname);
 	full = malloc(fulllen + 1);
+	if (!full) {
+		printf("%s: out of memory\n", progname);
+		exit(1);
+	}
 	snprintf(full, fulllen + 1, "%s/%s", dir, sockname);
 	sockname = full;
```

**Claude Issue Verification:** VALID - atch.c:277, `full` used by snprintf with no NULL check; reachable on every command via consume_session/cmd_open.
**Codex Issue Verification:** VALID - atch.c:277 allocates with `malloc` and atch.c:278 immediately passes the result to `snprintf` with no NULL check; the path is reached by bare session names through `consume_session` and `cmd_open`.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### 2026-06-25-1-2: `pty_activity` drops (or duplicates) live output to a slow/backpressured client

**Status:** OPEN
**Severity:** MEDIUM
**Pass:** Bug Hunt (Pass 1)
**File:** [master.c:438](../../../master.c#L438)

Client sockets are non-blocking (`setnonblocking` in `control_activity`). In `pty_activity`, each chunk of pty output is written to every writable client with a bounded inner loop that gives up on `EAGAIN`. There is no per-client pending-write buffer for live data (unlike the scrollback `replay_remaining` mechanism, which does handle backpressure). Two failure modes result:

1. Data loss (multi-client): if a fast client completes its write, `nclients` becomes non-zero, so the `goto top` re-send is skipped. Any slow client that only got a partial write (broke out on `EAGAIN`, so it is *not* dropped because `errno == EAGAIN`) silently loses the remaining `len - written` bytes — the next `pty_activity` call overwrites `buf`.
2. Data duplication (single slow client): if no client completes (`nclients == 0`), the code `goto top` and re-sends the same `buf` from `written = 0` for every writable client. A client that already received a partial write receives those bytes a second time.

**Evidence:**
```c
-- master.c:431-463
	for (p = clients, nclients = 0; p; p = next) {
		...
		written = 0;
		while (written < len) {
			ssize_t n = write(p->fd, buf + written, len - written);
			if (n > 0) { written += n; continue; }
			else if (n < 0 && errno == EINTR) continue;
			break;
		}
		if (written == len) {
			nclients++;
		} else if (errno != EAGAIN) {
			/* Write error: drop this client */
			...
		}
	}
	/* Try again if nothing happened. */
	if (!FD_ISSET(s, &readfds) && nclients == 0)
		goto top;
```

**Impact:** Corrupted/garbled terminal output for a client that cannot keep up (e.g. a small/slow terminal during a burst of output), either as missing bytes (multi-client) or duplicated bytes (single client). This is the same architectural limitation as upstream dtach; the persistent-log and scrollback-replay paths in this fork already model the correct pattern (per-client pending state).

**Fix:** The robust fix is to give each `struct client` a pending live-write buffer/offset analogous to `replay_head`/`replay_remaining`, and drain it from the `writefds` branch in `master_process`, rather than discarding or re-sending from offset 0. Sketch:
```diff
--- a/master.c
+++ b/master.c
@@ struct client
 	size_t replay_head;
 	size_t replay_remaining;
+	/* Pending live-output bytes not yet accepted by this client. */
+	unsigned char *pending;
+	size_t pending_len;
+	size_t pending_off;
```
On a partial/`EAGAIN` write in `pty_activity`, copy the unsent tail into `pending` and register the fd in `writefds`; drain it before delivering the next chunk. (Bounded by a max buffer; drop the client if the backlog exceeds the bound.) Given the complexity, this may be tracked as a deliberate design item if matching upstream behavior is acceptable.

**Claude Issue Verification:** VALID - master.c:439-463, non-blocking client fds with no per-client live backlog; partial writes lost when nclients>0, re-sent from 0 when nclients==0.
**Codex Issue Verification:** VALID - master.c:438-463 writes the current pty buffer directly to non-blocking client sockets with no per-client offset/backlog, so partial EAGAIN writes are either abandoned when another client completed or retried from byte 0 when no client completed.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

## Pass 2: Logic & Edge Cases

*Check logic correctness and edge cases. Look for off-by-one errors, null handling, race conditions, boundary conditions.*

### 2026-06-25-1-3: Scrollback ring can be overwritten while a slow client is mid-replay

**Status:** OPEN
**Severity:** MEDIUM
**Pass:** Logic & Edge Cases (Pass 2)
**File:** [master.c:354](../../../master.c#L354)

`replay_start` snapshots the replay window as `p->replay_head = scrollback_head; p->replay_remaining = scrollback_len;` and then drains it incrementally across many `master_process` iterations (the client is non-blocking, so replay yields on `EAGAIN`). Meanwhile `pty_activity` continues to call `scrollback_append`, which advances `scrollback_head` and overwrites the oldest bytes of the same fixed-size ring. Because `replay_head` is an absolute ring index, the bytes it still points to can be overwritten by new pty output before the replaying client has drained them. The client then transmits whichever (newer) bytes now occupy those slots, and the live output produced during the replay window is never delivered (the replay window was a fixed snapshot of `scrollback_len`).

**Evidence:**
```c
-- master.c:354-364 (snapshot, never revalidated)
static void replay_start(struct client *p)
{
	if (scrollback_len == 0) { p->replay_remaining = 0; p->attached = 1; return; }
	p->replay_head = scrollback_head;
	p->replay_remaining = scrollback_len;
	replay_drain(p);
}
-- master.c:306-316 (concurrent overwrite of the same ring during replay)
	for (i = 0; i < len; i++) {
		size_t wp = (scrollback_head + scrollback_len) & (SCROLLBACK_SIZE - 1);
		scrollback_buf[wp] = buf[i];
		if (scrollback_len < SCROLLBACK_SIZE) scrollback_len++;
		else scrollback_head = (scrollback_head + 1) & (SCROLLBACK_SIZE - 1);
	}
```

**Impact:** Corrupted scrollback replay (and a gap in delivered output) when the ring (default 128 KiB) wraps during a slow attach — plausible with a large scrollback and a client that applies backpressure while a burst of output is produced. Note this path is normally used only when the on-disk log is disabled (`-C 0`); with logging on, `attach_main` replays the disk log and sets `pkt.len=1` so the ring is skipped.

**Fix:** Detect overwrite of an in-flight replay window and either re-snapshot to the current `scrollback_head`/oldest valid byte, or freeze the affected client to disk-log replay. Minimal guard: when `scrollback_append` would advance `scrollback_head` past a client's `replay_head`, clamp that client's `replay_head`/`replay_remaining` forward to the new oldest byte (accepting that the very oldest replayed bytes are dropped rather than corrupted).

**Claude Issue Verification:** VALID - master.c:354-364 snapshots absolute ring indices; master.c:306-316 mutates the same ring with no coordination against in-flight replay clients.
**Codex Issue Verification:** VALID - replay state stores only absolute ring indices at master.c:361-362, while scrollback_append mutates and advances the same ring at master.c:306-315 without protecting in-flight replay windows.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### 2026-06-25-1-4: Legacy single-dash dispatch only reads the second character; trailing letters are silently ignored

**Status:** OPEN
**Severity:** LOW
**Pass:** Logic & Edge Cases (Pass 2)
**File:** [atch.c:817](../../../atch.c#L817)

The legacy backward-compat dispatcher takes `mode = (*argv)[1]` (the second character of the flag) and never inspects the rest of the string. A combined legacy invocation such as `atch -nq session cmd` is parsed as mode `n` with the `q` silently discarded (quiet is never set). The new command dispatcher and the global-option pre-pass handle clustering correctly, but the legacy branch does not.

**Evidence:**
```c
-- atch.c:815-820
	if (argc >= 1 && argv[0][0] == '-' && argv[0][1] != '\0' &&
	    argv[0][1] != '-') {
		int mode = (*argv)[1];
		++argv;
		--argc;
```

**Impact:** Silent option drop for clustered legacy flags. Minor: legacy syntax is documented as backward-compat, and global options can be passed separately before the mode letter.

**Fix:** Either reject a legacy flag longer than two characters with an "Invalid mode" error, or document that legacy mode letters must not be clustered. (Documentation-only fix is acceptable.)

**Claude Issue Verification:** VALID - atch.c:817 reads only index [1]; remaining characters of argv[0] are never examined in the legacy branch.
**Codex Issue Verification:** VALID - atch.c:817 reads only `argv[0][1]` for legacy mode dispatch and the rest of the single-dash token is never inspected before the branch consumes that argument.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### 2026-06-25-1-5: `parse_size` can integer-overflow on the k/m multiplier

**Status:** OPEN
**Severity:** LOW
**Pass:** Logic & Edge Cases (Pass 2)
**File:** [atch.c:113](../../../atch.c#L113)

`parse_size` parses the `-C` log cap with `strtoul` then multiplies by 1024 or 1024*1024 with no overflow check. A value like `-C 99999999999999m` wraps `unsigned long` and yields a small or arbitrary `log_max_size`, silently contradicting the user's intent. `strtoul` overflow (`ERANGE`/`ULONG_MAX`) is also not checked.

**Evidence:**
```c
-- atch.c:120-132
	v = strtoul(s, &end, 10);
	if (end == s) return 1;
	if (*end == 'k' || *end == 'K') { v *= 1024; end++; }
	else if (*end == 'm' || *end == 'M') { v *= 1024 * 1024; end++; }
	if (*end != '\0') return 1;
	*out = (size_t)v;
```

**Impact:** A nonsensical/overflowed cap is accepted silently instead of rejected. Low severity (operator-supplied input, no memory safety impact).

**Fix:**
```diff
--- a/atch.c
+++ b/atch.c
@@ parse_size
-	v = strtoul(s, &end, 10);
-	if (end == s)
-		return 1;
-	if (*end == 'k' || *end == 'K') {
-		v *= 1024;
-		end++;
-	} else if (*end == 'm' || *end == 'M') {
-		v *= 1024 * 1024;
-		end++;
-	}
+	errno = 0;
+	v = strtoul(s, &end, 10);
+	if (end == s || errno == ERANGE)
+		return 1;
+	if (*end == 'k' || *end == 'K') {
+		if (v > ULONG_MAX / 1024) return 1;
+		v *= 1024; end++;
+	} else if (*end == 'm' || *end == 'M') {
+		if (v > ULONG_MAX / (1024 * 1024)) return 1;
+		v *= 1024 * 1024; end++;
+	}
```

**Claude Issue Verification:** VALID - atch.c:123-129 multiplies without bound check; strtoul ERANGE not handled.
**Codex Issue Verification:** VALID - atch.c:120 does not reset/check `errno` for `strtoul`, and atch.c:123-128 multiplies by the suffix scale without checking overflow before storing into `size_t`.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### 2026-06-25-1-6: Detach character with the high bit set can never match (signedness)

**Status:** OPEN
**Severity:** LOW
**Pass:** Logic & Edge Cases (Pass 2)
**File:** [atch.c:177](../../../atch.c#L177)

When `-e` is given a literal non-caret character, `detach_char = (*argv)[0][0];`. On platforms where `char` is signed, a byte ≥ 0x80 becomes a negative `int`. The comparison in `process_kbd` is `pkt->u.buf[0] == detach_char`, where `pkt->u.buf[0]` is `unsigned char` (promoted to 0–255). A negative `detach_char` can therefore never equal any keystroke, so a high-bit detach character is silently inert.

**Evidence:**
```c
-- atch.c:170-177
	if ((*argv)[0][0] == '^' && (*argv)[0][1]) {
		...
	} else
		detach_char = (*argv)[0][0];
-- attach.c:203
	else if (pkt->u.buf[0] == detach_char) {
```

**Impact:** A user-chosen 8-bit detach character is ignored with no diagnostic. Suspicion-level/edge: only affects non-ASCII detach chars; ASCII and the default `^\` are unaffected. Additional evidence to confirm: build with a signed-`char` target and set `-e $'\200'`.

**Fix:**
```diff
--- a/atch.c
+++ b/atch.c
-		detach_char = (*argv)[0][0];
+		detach_char = (unsigned char)(*argv)[0][0];
```

**Claude Issue Verification:** VALID - atch.c:177 stores a possibly-signed char into int detach_char; attach.c:203 compares against unsigned char[0]. Marked LOW/suspicion pending a signed-char build test.
**Codex Issue Verification:** VALID - atch.c:177 assigns a plain `char` into the integer `detach_char`, while attach.c:203 compares it against an `unsigned char`; this is a target-dependent signed-char portability bug for high-bit literal detach bytes.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

## Pass 3: Reliability & Performance

*Check timeouts, retries, memory leaks, resource exhaustion, connection limits, error recovery.*

### 2026-06-25-1-7: `tail -f` stops showing output after a log rotation

**Status:** OPEN
**Severity:** MEDIUM
**Pass:** Reliability & Performance (Pass 3)
**File:** [atch.c:649](../../../atch.c#L649)

`cmd_tail` follow mode keeps reading from a fixed fd/offset. When the master rotates the log (`rotate_log` does `ftruncate(log_fd, 0)` then rewrites the trimmed tail), the reader's current offset is now far past the new end-of-file, so `read` returns 0 forever and no further output is shown. The README explicitly advertises `tail -f` for "monitoring a running session," and rotation happens automatically once the log passes the cap (default 1 MB) — so a long-lived monitored session will silently stop updating.

**Evidence:**
```c
-- atch.c:649-656
	if (follow) {
		signal(SIGPIPE, SIG_IGN);
		for (;;) {
			usleep(250000);
			while ((n = read(fd, rbuf, sizeof(rbuf))) > 0)
				write(1, rbuf, (size_t)n);
		}
	}
-- master.c:67-70 (rotation truncates the file the reader is tracking)
			ftruncate(log_fd, 0);
			lseek(log_fd, 0, SEEK_SET);
			write(log_fd, buf, (size_t)n);
```

**Impact:** `tail -f` silently goes dead after the first rotation of a busy session — a correctness/monitoring failure for the documented use case.

**Fix:** In the follow loop, detect truncation by comparing the current file size (`fstat`/`lseek(fd,0,SEEK_END)`) against the last read offset; if the file shrank, `lseek(fd, 0, SEEK_SET)` (and ideally re-tail). Sketch:
```diff
+		off_t last = lseek(fd, 0, SEEK_CUR);
 		for (;;) {
 			usleep(250000);
+			off_t cur = lseek(fd, 0, SEEK_END);
+			if (cur < last)
+				last = lseek(fd, 0, SEEK_SET);  /* rotated */
+			else
+				lseek(fd, last, SEEK_SET);
 			while ((n = read(fd, rbuf, sizeof(rbuf))) > 0)
 				write(1, rbuf, (size_t)n);
+			last = lseek(fd, 0, SEEK_CUR);
 		}
```

**Claude Issue Verification:** VALID - atch.c:649-656 never revalidates the offset; master.c:68 truncates the same file. Reproducible with a small `-C` cap.
**Codex Issue Verification:** VALID - atch.c:649-655 follows a fixed fd and current offset forever, while master.c:67-70 truncates and rewrites the same log file during rotation; the follower does not detect that its offset is past the new EOF.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### 2026-06-25-1-8: `tail -f` never exits when its output pipe is closed

**Status:** OPEN
**Severity:** LOW
**Pass:** Reliability & Performance (Pass 3)
**File:** [atch.c:650](../../../atch.c#L650)

Follow mode does `signal(SIGPIPE, SIG_IGN)` and then ignores the return value of `write(1, ...)`. When the consumer closes the pipe (e.g. `atch tail -f s | head`), `write` returns `-1`/`EPIPE` but the loop never notices and runs forever (a 250 ms-paced idle loop), leaving an orphan process.

**Evidence:**
```c
-- atch.c:650-655
		signal(SIGPIPE, SIG_IGN);
		for (;;) {
			usleep(250000);
			while ((n = read(fd, rbuf, sizeof(rbuf))) > 0)
				write(1, rbuf, (size_t)n);
		}
```

**Impact:** Orphaned `atch tail -f` processes accumulate when piped into a consumer that exits early.

**Fix:** Check the `write` result and exit on `EPIPE` (or simply do not ignore `SIGPIPE` in follow mode so the default disposition terminates the process). Apply to the non-follow drain at atch.c:645-646 as well for consistency.
```diff
-			while ((n = read(fd, rbuf, sizeof(rbuf))) > 0)
-				write(1, rbuf, (size_t)n);
+			while ((n = read(fd, rbuf, sizeof(rbuf))) > 0)
+				if (write(1, rbuf, (size_t)n) < 0)
+					return 1;
```

**Claude Issue Verification:** VALID - atch.c:650-655 ignores write() result with SIGPIPE ignored; loop has no exit condition.
**Codex Issue Verification:** VALID - atch.c:650 ignores SIGPIPE and atch.c:653-654 ignores `write(1, ...)` errors in an infinite follow loop, so a closed output pipe is not an exit condition once follow mode is running.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### 2026-06-25-1-9: On-disk log can grow to ~2× the configured cap before rotating

**Status:** OPEN
**Severity:** LOW
**Pass:** Reliability & Performance (Pass 3)
**File:** [master.c:393](../../../master.c#L393)

The rotation trigger uses `log_written` (bytes written *this session*), which is reset to 0 after `open_log`/`rotate_log` even though the existing on-disk log may already be near the cap. So after startup the file can grow by another full `log_max_size` before `log_written >= log_max_size` triggers `rotate_log` — i.e. the file oscillates between roughly `log_max_size` and `2 × log_max_size`. The README states "The log is capped at 1 MB ... once it exceeds that, only the most recent 1 MB is kept," which a user could reasonably read as a hard cap.

**Evidence:**
```c
-- master.c:390-396
		write(log_fd, buf, (size_t)len);
		log_written += (size_t)len;
		if (log_written >= log_max_size) {
			rotate_log();
			log_written = 0;
		}
-- master.c:82-92 (open_log calls rotate_log but leaves log_written at 0)
	log_fd = fd;
	rotate_log();
	return fd;
```

**Impact:** On-disk usage up to ~2× the documented cap; mild surprise for tight `-C` settings. Not a leak (bounded), but inconsistent with docs.

**Fix:** Track the actual file size for the trigger instead of session-only bytes — e.g. set `log_written` to the post-rotation file size in `open_log`, or compare `lseek(log_fd, 0, SEEK_CUR)` against `log_max_size`. Alternatively, document the ~2× overshoot.

**Claude Issue Verification:** VALID - master.c:393 triggers on session-local log_written; open_log leaves it at 0 regardless of existing file size.
**Codex Issue Verification:** VALID - master.c:391-395 triggers rotation from session-local `log_written`, and open_log at master.c:90-92 rotates existing logs but leaves `log_written` at 0 even when the fd is already positioned near the cap.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### 2026-06-25-1-10: `scrollback_append` copies byte-by-byte instead of using `memcpy`

**Status:** OPEN
**Severity:** LOW
**Pass:** Reliability & Performance (Pass 3)
**File:** [master.c:296](../../../master.c#L296)

The hot path for every byte of pty output copies into the ring one byte at a time, recomputing the masked write position and branching per byte. For bursty output (up to `BUFSIZE` = 4096 bytes per read) this is needlessly expensive; the ring is a power of two so the copy can be done with one or two `memcpy` calls.

**Evidence:**
```c
-- master.c:306-316
	for (i = 0; i < len; i++) {
		size_t wp = (scrollback_head + scrollback_len) & (SCROLLBACK_SIZE - 1);
		scrollback_buf[wp] = buf[i];
		if (scrollback_len < SCROLLBACK_SIZE) scrollback_len++;
		else scrollback_head = (scrollback_head + 1) & (SCROLLBACK_SIZE - 1);
	}
```

**Impact:** Minor CPU overhead in the master's central data path. Correctness is fine.

**Fix:** Replace the loop with up to two `memcpy` calls (head→end, then wrap), then adjust `scrollback_len`/`scrollback_head` once. Low priority; verify carefully against the wrap accounting.

**Claude Issue Verification:** VALID - master.c:306-316 per-byte loop on the master's main output path; memcpy-able since SCROLLBACK_SIZE is a power of two.
**Codex Issue Verification:** VALID - master.c:306-316 performs one masked index calculation, branch, and byte copy per byte on the pty-output hot path; the ring size is a power of two and this can be implemented with bounded bulk copies.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### 2026-06-25-1-11: Signal handlers call non-async-signal-safe functions

**Status:** OPEN
**Severity:** MEDIUM
**Pass:** Reliability & Performance (Pass 3)
**File:** [attach.c:153](../../../attach.c#L153)

The attacher's `die` handler (installed for SIGHUP/SIGTERM/SIGINT/SIGQUIT) calls `session_age` (which calls `time`/`snprintf`), `printf`, and `exit` — none of which are async-signal-safe. `exit` runs `atexit` handlers, so `restore_term` (and, with `-t`, `system("tput init")`) execute in signal context. The master's `master_die` calls `exit(1)` for SIGINT/SIGTERM, running `atexit(cleanup_session)` which does `snprintf`/`write`/`unlink` from signal context. If a signal arrives while the main thread is inside `printf`/`malloc`, the handler can deadlock or corrupt state.

**Evidence:**
```c
-- attach.c:153-166
static RETSIGTYPE die(int sig)
{
	char age[32];
	session_age(age, sizeof(age));
	if (sig == SIGHUP || sig == SIGINT)
		printf("%s[%s: session '%s' detached after %s]\r\n", ...);
	else
		printf("...got signal %d - exiting after %s]\r\n", ...);
	exit(1);
}
-- attach.c:93-94 (reached via exit() -> atexit(restore_term))
	if (no_ansiterm)
		(void)system("tput init 2>/dev/null");
```

**Impact:** Rare deadlock/undefined behavior on signal delivery during stdio/heap activity; `system()` from a signal handler is especially fraught. In practice this tool's main loops spend most time blocked in `select`, which lowers the odds, but it remains undefined.

**Fix:** Use `_exit` from the handlers, or set a `volatile sig_atomic_t` flag and act on it from the main loop; restrict handler bodies to async-signal-safe calls (`write` instead of `printf`). This is a broader refactor; flagged for awareness and prioritization.

**Claude Issue Verification:** VALID - attach.c:153-166 and master.c:115-126 invoke printf/exit/atexit chains (incl. system()) from signal context.
**Codex Issue Verification:** VALID - attach.c:153-165 calls `time`/`snprintf` through `session_age`, `printf`, and `exit` from signal context, and master.c:115-125 calls `exit` from SIGINT/SIGTERM handlers, invoking non-async-signal-safe atexit cleanup.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

## Pass 4: Maintainability

*Check code organization, naming, documentation gaps, refactoring opportunities.*

### 2026-06-25-1-12: Duplicated "write failed" reporting block in `write_buf_or_fail` and `write_packet_or_fail`

**Status:** OPEN
**Severity:** LOW
**Pass:** Maintainability (Pass 4)
**File:** [attach.c:42](../../../attach.c#L42)

The ~12-line "session write failed" reporting block (with/without `session_start`) is duplicated verbatim in `write_buf_or_fail` (attach.c:42-53) and `write_packet_or_fail` (attach.c:69-80). Divergence risk on future edits.

**Evidence:**
```c
-- attach.c:42-53 (identical to attach.c:69-80)
			if (session_start) {
				char age[32];
				session_age(age, sizeof(age));
				printf("%s[%s: session '%s' write failed after %s]\r\n",
				    clear_csi_data(), progname, session_shortname(), age);
			} else {
				printf("%s[%s: write failed]\r\n", clear_csi_data(), progname);
			}
			exit(1);
```

**Impact:** Maintainability only.

**Fix:** Extract a `static void write_failed(void)` helper and call it from both spots.

**Claude Issue Verification:** VALID - attach.c:42-53 and attach.c:69-80 are byte-identical reporting blocks.
**Codex Issue Verification:** VALID - attach.c:42-53 and attach.c:69-80 contain the same write-failure reporting branch, including the session-age formatting and fallback message.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### 2026-06-25-1-13: "Invalid number of arguments" + "Try --help" block repeated across many command handlers

**Status:** OPEN
**Severity:** LOW
**Pass:** Maintainability (Pass 4)
**File:** [atch.c:405](../../../atch.c#L405)

The two-line "Invalid number of arguments." / "Try '%s --help'..." pattern (and the analogous "No session was specified." pair) is copy-pasted in at least eight places (atch.c:405-408, 476-478, 503-506, 533-536, 623-626, 673-677, 684-688, 847-852, 857-863, 879-885). A small helper would shrink the file and keep wording consistent.

**Evidence:**
```c
-- atch.c:405-408 (one of ~8 occurrences)
		printf("%s: Invalid number of arguments.\n", progname);
		printf("Try '%s --help' for more information.\n", progname);
		return 1;
```

**Impact:** Maintainability only; risk of inconsistent wording over time.

**Fix:** Add `static int usage_error(const char *msg)` that prints the message + the "Try --help" line and returns 1; replace the repeated blocks.

**Claude Issue Verification:** VALID - the same two printf lines recur in ~8 handlers across atch.c.
**Codex Issue Verification:** VALID - atch.c repeats the same invalid-argument and "Try --help" reporting pair across many command handlers, including the cited atch.c:405-408, 503-506, 533-536, 623-626, 673-677, 847-852, 857-863, and 879-885 blocks.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### 2026-06-25-1-14: Scattered magic-number path buffers risk silent truncation and drift

**Status:** OPEN
**Severity:** LOW
**Pass:** Maintainability (Pass 4)
**File:** [attach.c:737](../../../attach.c#L737)

Path buffer sizes are hard-coded inconsistently: `log_path[600]` (atch.c:514, 584; attach.c:233, 608), `dir[512]`, `path[768]`, `log_full[800]`, etc. `list_main`'s own comment says `path` needs `sizeof(dir) + '/' + NAME_MAX` = 512 + 1 + 255 = 768, but that totals 769, so a maximal name truncates by one byte. None of these overflow (all use `snprintf`), but a truncated path silently targets the wrong file, and the magic numbers must be kept mutually consistent by hand.

**Evidence:**
```c
-- attach.c:736-737
	char dir[512];
	char path[768];		/* sizeof(dir) + '/' + NAME_MAX */
```

**Impact:** Silent truncation edge case for pathological names; maintainability/readability.

**Fix:** Centralize a `SESSION_PATH_MAX` (or derive from `PATH_MAX`) and size all path buffers from it; bump `path` to ≥ 769 (or `dir + 2 + NAME_MAX`). Consider checking `snprintf` return for truncation on the security-relevant paths.

**Claude Issue Verification:** PARTIALLY_VALID (reconciled, was VALID) - On re-read I agree with Codex: `dir[512]` (attach.c:736) is filled by `get_session_dir` and holds at most a 511-character C string, so the worst case is 511 + '/' + NAME_MAX(255) + NUL = 768 bytes, which fits `path[768]` exactly (attach.c:737, 760). My original "off-by-one / 769" claim was wrong (and arithmetically self-contradictory). The broader point — hard-coded, mutually-dependent magic-number path sizes and unchecked `snprintf` truncation — remains a valid maintainability concern, so PARTIALLY_VALID.
**Codex Issue Verification:** PARTIALLY_VALID - the broader maintainability issue is real because path buffers are hard-coded and `snprintf` truncation is generally unchecked, but the specific `path[768]` off-by-one claim is not correct for a `dir[512]` C string: the maximum stored `dir` length is 511, so 511 + '/' + NAME_MAX(255) + NUL fits in 768 bytes.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

## Pass 5: Dead / Obsolete Code

*Identify code that is no longer used: orphaned call sites, removed feature flags still wired up, unused exports, commented-out blocks, unreachable branches, helper files kept "just in case". Each finding must include evidence the code is genuinely unreferenced.*

### 2026-06-25-1-15: `config.h` defines `HAVE_SYS_TIME_H` and `HAVE_LIBUTIL` that are never referenced

**Status:** OPEN
**Severity:** LOW
**Pass:** Dead / Obsolete Code (Pass 5)
**File:** [config.h:1](../../../config.h#L1)

`config.h` defines `HAVE_LIBUTIL` (line 1) and `HAVE_SYS_TIME_H` (line 10). Neither is tested anywhere: `atch.h` includes `<sys/time.h>` unconditionally (atch.h:24), and the libutil header guard the code actually uses is `HAVE_LIBUTIL_H` (a different macro, atch.h:35). These are leftover autoconf-style defines with no effect.

**Evidence:**
```
$ grep -rn "HAVE_SYS_TIME_H" .
config.h:10:#define HAVE_SYS_TIME_H 1
$ grep -rn "HAVE_LIBUTIL\b" --include=*.c --include=*.h .
config.h:1:#define HAVE_LIBUTIL 1
-- atch.h:24 (sys/time.h pulled in unconditionally, not via HAVE_SYS_TIME_H)
#include <sys/time.h>
```

**Impact:** Dead configuration noise; mildly misleading (suggests a guard exists where none does).

**Fix:** Remove the two unused `#define`s from `config.h` (or actually guard `<sys/time.h>` with `HAVE_SYS_TIME_H` if conditional inclusion is intended).

**Claude Issue Verification:** VALID - grep shows each macro defined only in config.h with zero references; sys/time.h included unconditionally.
**Codex Issue Verification:** VALID - `rg -n "HAVE_SYS_TIME_H|HAVE_LIBUTIL\\b" config.h atch.h atch.c attach.c master.c` shows `HAVE_SYS_TIME_H` and `HAVE_LIBUTIL` are only defined in config.h; atch.h includes `<sys/time.h>` unconditionally and checks `HAVE_LIBUTIL_H`, not `HAVE_LIBUTIL`.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### 2026-06-25-1-16: Unreachable `return 0` after `master_process` in `master_main`

**Status:** OPEN
**Severity:** LOW
**Pass:** Dead / Obsolete Code (Pass 5)
**File:** [master.c:757](../../../master.c#L757)

`master_process` never returns — it is an infinite `while (1)` loop whose only exits are `exit()` calls. Both call sites in `master_main` (`dontfork` branch at master.c:756-757 and the forked-child branch at master.c:770-771) follow the call with `return 0;`, which is unreachable.

**Evidence:**
```c
-- master.c:755-758
	if (dontfork) {
		master_process(s, argv, waitattach, fd[1]);
		return 0;
	}
-- master.c:770-772
		master_process(s, argv, waitattach, fd[1]);
		return 0;
	}
-- master.c:631-702 master_process is `while (1) { ... }` with only exit() paths
```

**Impact:** Dead statements; harmless but misleading (implies a normal return that cannot happen).

**Fix:** Either annotate `master_process` as non-returning (`__attribute__((noreturn))`) and drop the `return 0;` lines, or leave as defensive code with a comment. Low priority.

**Claude Issue Verification:** VALID - master.c:631-702 has no return; the two `return 0;` after master_process (master.c:757, 771) are unreachable.
**Codex Issue Verification:** VALID - master_process is a `while (1)` loop at master.c:631-701 with exit-only failure paths, so the `return 0;` statements immediately after its two master_main call sites at master.c:757 and master.c:771 are not reachable in the current implementation.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

## Summary by Priority

### Critical (Fix Before Release)
None.

### High (Fix Soon)
None.

### Medium (Should Fix)
1. **2026-06-25-1-2**: `pty_activity` drops/duplicates live output to a slow client
2. **2026-06-25-1-3**: Scrollback ring overwritten during slow replay
3. **2026-06-25-1-7**: `tail -f` stops after log rotation
4. **2026-06-25-1-11**: Signal handlers call non-async-signal-safe functions
5. **2026-06-25-1-1**: `expand_sockname` unchecked `malloc` (NULL deref)

### Medium / Low boundary
6. **2026-06-25-1-9**: On-disk log can reach ~2× the cap

### Low (Nice to Have)
7. **2026-06-25-1-4**: Legacy dispatch ignores clustered flag letters
8. **2026-06-25-1-5**: `parse_size` multiplier overflow
9. **2026-06-25-1-6**: High-bit detach char never matches (signedness)
10. **2026-06-25-1-8**: `tail -f` never exits on closed pipe
11. **2026-06-25-1-10**: `scrollback_append` byte-by-byte copy (perf)
12. **2026-06-25-1-12**: Duplicated write-failed reporting block
13. **2026-06-25-1-13**: Duplicated "invalid arguments" blocks
14. **2026-06-25-1-14**: Scattered magic-number path buffers
15. **2026-06-25-1-15**: Dead `config.h` defines
16. **2026-06-25-1-16**: Unreachable `return 0` after `master_process`

---

## Previously Marked WONTFIX/ONHOLD (Unchanged)

None — this is the first review of the project and there is no CHANGELOG.md tracking prior dispositions.

---

## Appendix: Files Changed Summary

| File | Issues |
|------|--------|
| atch.c | 2026-06-25-1-1, -4, -5, -6, -7, -8, -13, -18 |
| master.c | 2026-06-25-1-2, -3, -9, -10, -16, -17, -19 |
| attach.c | 2026-06-25-1-11, -12, -14 |
| config.h | 2026-06-25-1-15 |
| atch.h | 2026-06-25-1-20 |

---

## Agent B Independent 5-Pass Review

Agent B performed a separate 5-pass review after cross-verifying Agent A's findings. Build baseline: `make` succeeds, with warnings for ignored `write`, `ftruncate`, and `system` return values. Test baseline: the first run raced the build and was discarded; the clean `sh tests/test.sh` run finished with 228 passed and 1 failed, where the failure was `current: dash in binary name → underscore in env var name` because the generated `/tmp/.../bin/ssh2incus-atch` symlink target was not found. Additional targeted repros were run for the findings below.

### Agent B Pass 1: Bug Hunt

#### 2026-06-25-1-17: Control protocol treats `SOCK_STREAM` packets as atomic and drops fragmented packets

**Status:** OPEN
**Severity:** MEDIUM
**Pass:** Bug Hunt (Pass 1)
**File:** [master.c:505](../../../master.c#L505)

The client/master control channel is a Unix `SOCK_STREAM`, but the master reads one `struct packet` with a single non-blocking `read` and drops the client whenever that read returns anything other than the full struct size. Stream sockets do not preserve message boundaries, so a valid packet can be delivered in fragments. The client helpers also assume a full packet write: `write_packet_or_fail` exits on a positive short write instead of continuing from the remaining offset, while `push_main` and `send_kill` do one raw `write` and require the full struct length.

**Evidence:**
```c
-- master.c:505-512
	len = read(p->fd, &pkt, sizeof(struct packet));
	if (len < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (len != sizeof(struct packet)) {
		close(p->fd);
		if (p->next)
			p->next->pprev = p->pprev;
```
```c
-- attach.c:62-68
		ssize_t ret = write(fd, pkt, sizeof(struct packet));
		if (ret == sizeof(struct packet))
			return;
		else if (ret < 0 && errno == EINTR)
			continue;
		else {
```
```c
-- attach.c:500-501
		len = write(s, &pkt, sizeof(struct packet));
		if (len != sizeof(struct packet)) {
```

**Impact:** A fragmented attach, push, redraw, or kill control packet is treated as a dead/bad client, causing spurious detach, lost input, or failed kill delivery. This is most likely under unusual socket backpressure or with nonstandard clients, but the current protocol is relying on behavior not guaranteed by stream sockets.

**Fix:**
```diff
--- a/master.c
+++ b/master.c
@@ struct client
 	size_t replay_head;
 	size_t replay_remaining;
+	unsigned char inbuf[sizeof(struct packet)];
+	size_t in_used;
@@ control_activity
 	p->replay_remaining = 0;
+	p->in_used = 0;
@@ client_activity
-	len = read(p->fd, &pkt, sizeof(struct packet));
-	if (len < 0 && (errno == EAGAIN || errno == EINTR))
-		return;
-	if (len != sizeof(struct packet)) {
+	while (p->in_used < sizeof(struct packet)) {
+		len = read(p->fd, p->inbuf + p->in_used, sizeof(struct packet) - p->in_used);
+		if (len > 0) {
+			p->in_used += (size_t)len;
+			continue;
+		}
+		if (len < 0 && (errno == EAGAIN || errno == EINTR))
+			return;
 		close(p->fd);
 		...
 		return;
 	}
+	memcpy(&pkt, p->inbuf, sizeof(pkt));
+	p->in_used = 0;
```

Also update `write_packet_or_fail`, `push_main`, and `send_kill` to use the same offset-tracking write loop as `write_buf_or_fail`.

**Claude Issue Verification:** PARTIALLY_VALID (reconciled, reaffirmed) - The code observation is exactly correct and both agents agree on it: master.c:505-510 does a single non-blocking `read` and drops the client on any `len != sizeof(struct packet)`, and the write side (attach.c:62-68 `write_packet_or_fail` exits on a positive short write; attach.c:500-501 `push_main`) assumes whole-packet writes — none of which is guaranteed by `SOCK_STREAM` in the abstract. The sole point of disagreement with Codex (VALID) is impact/severity. On a fresh re-read I steelmanned the VALID position (stream sockets genuinely make no framing guarantee, so relying on it is a real latent defect that a robust server should not do) but the evidence does not support the MEDIUM real-world impact: (1) `struct packet` (atch.h:108-117) is `1 + 1 + sizeof(struct winsize)` = 10 bytes with `pkt.len` capped at 8; (2) the client socket `s` is created/connected at attach.c:111 and never `setnonblocking`, so it is **blocking** — `write_packet_or_fail`/`push_main`/`send_kill` write the full 10 bytes in one syscall into an essentially-empty AF_UNIX `SOCK_STREAM` buffer, which the kernel delivers as a single contiguous skb; (3) the master then reads that whole skb in one `read`, so a short read cannot occur for the shipped client. Fragmentation is reachable only via a deliberately nonstandard client or a near-full (within <10 bytes) send buffer — exactly the "unusual socket backpressure or nonstandard clients" Codex's own write-up names as the precondition. So the described MEDIUM consequences (spurious detach, lost input, failed kill) do not materialize in normal operation; the accurate practical severity is LOW. I therefore reaffirm PARTIALLY_VALID (correct diagnosis, overstated impact) and propose the common-ground resolution of downgrading Severity MEDIUM → LOW. If Codex holds MEDIUM, the residual is a severity-only disagreement for human review — there is no remaining dispute about the code itself.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### Agent B Pass 2: Logic & Edge Cases

#### 2026-06-25-1-18: `tail -n` accepts nonnumeric counts as `1` instead of rejecting them

**Status:** OPEN
**Severity:** LOW
**Pass:** Logic & Edge Cases (Pass 2)
**File:** [atch.c:603](../../../atch.c#L603)

`cmd_tail` parses both `-n N` and compact `-nN` with `atoi`. `atoi("foo")` returns 0, and the later clamp turns any value below 1 into 1, so `atch tail -nfoo session` is accepted as `tail -n 1` instead of producing the same invalid-option style error used elsewhere. A targeted repro with an existing short-lived logged session returned exit 0 and printed the last line/end marker for `tail -nfoo`.

**Evidence:**
```c
-- atch.c:603-619
			nlines = atoi(argv[1]);
			argc -= 2;
			argv += 2;
		} else if (strncmp(argv[0], "-n", 2) == 0 &&
			   argv[0][2] != '\0') {
			nlines = atoi(argv[0] + 2);
			argc--;
			argv++;
		} else {
...
	if (nlines < 1)
		nlines = 1;
```

**Impact:** Mistyped values like `-nfoo`, `-n1x`, or overflow-sized numbers silently produce surprising output instead of an error. This is low severity, but it makes CLI behavior harder to script reliably.

**Fix:**
```diff
--- a/atch.c
+++ b/atch.c
@@ cmd_tail
-			nlines = atoi(argv[1]);
+			if (parse_positive_int(argv[1], &nlines)) {
+				printf("%s: Invalid line count '%s'.\n", progname, argv[1]);
+				printf("Try '%s --help' for more information.\n", progname);
+				return 1;
+			}
@@ cmd_tail
-			nlines = atoi(argv[0] + 2);
+			if (parse_positive_int(argv[0] + 2, &nlines)) {
+				printf("%s: Invalid line count '%s'.\n", progname, argv[0] + 2);
+				printf("Try '%s --help' for more information.\n", progname);
+				return 1;
+			}
```

Implement `parse_positive_int` with `strtol`, `errno == ERANGE` checks, `*end == '\0'`, and an `INT_MAX` upper bound; then remove the silent `< 1` clamp or keep it only for a deliberate documented `0 => 1` compatibility choice.

**Claude Issue Verification:** VALID - atch.c:603 and atch.c:608 both parse the line count with `atoi`, and atch.c:618-619 clamps `nlines < 1` to 1, so `atoi("foo") == 0` is silently accepted as `-n 1` instead of raising the "Invalid option" error used at atch.c:611-615. Confirmed against source.
**Codex Issue Verification:** VALID - atch.c:603 and atch.c:608 use `atoi`; a targeted `HOME=/tmp/... ./atch tail -nfoo nfoo-session` against an existing log exited 0 instead of rejecting the malformed count.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### Agent B Pass 3: Reliability & Performance

#### 2026-06-25-1-19: `create_socket` leaves a bound socket pathname behind on post-bind setup failures

**Status:** OPEN
**Severity:** LOW
**Pass:** Reliability & Performance (Pass 3)
**File:** [master.c:260](../../../master.c#L260)

After `bind` succeeds, later setup failures close the listening fd but do not unlink the just-created socket pathname. The `listen`, `setnonblocking`, and `chmod` error branches all return `-1` with the filesystem entry still present. A later start for the same session can then fail with `EADDRINUSE`/stale-session behavior even though no usable master was created.

**Evidence:**
```c
-- master.c:256-271
	if (bind(s, (struct sockaddr *)&sockun, sizeof(sockun)) < 0) {
		close(s);
		return -1;
	}
	if (listen(s, 128) < 0) {
		close(s);
		return -1;
	}
	if (setnonblocking(s) < 0) {
		close(s);
		return -1;
	}
	/* chmod it to prevent any surprises */
	if (chmod(name, 0600) < 0) {
		close(s);
		return -1;
```

**Impact:** Rare setup failures can leave stale socket files that block or confuse future session creation until manually removed. The common path is unaffected, but cleanup should be deterministic once `bind` has created a pathname.

**Fix:**
```diff
--- a/master.c
+++ b/master.c
@@ create_socket
 	if (listen(s, 128) < 0) {
 		close(s);
+		unlink(name);
 		return -1;
 	}
 	if (setnonblocking(s) < 0) {
 		close(s);
+		unlink(name);
 		return -1;
 	}
@@
 	if (chmod(name, 0600) < 0) {
 		close(s);
+		unlink(name);
 		return -1;
 	}
```

A `goto fail_bound;` cleanup label would reduce duplication and avoid missing future post-bind branches.

**Claude Issue Verification:** VALID - Confirmed at master.c:256-272: `bind` creates the on-disk socket pathname, and the subsequent `listen` (260), `setnonblocking` (264), and `chmod` (269) failure branches all `close(s); return -1;` without `unlink(name)`, leaving a stale socket file. Rare but real; the suggested `goto`-based cleanup is the clean fix. Severity LOW is appropriate.
**Codex Issue Verification:** VALID - master.c:256 creates the socket pathname with `bind`, and master.c:260-271 closes the fd but does not unlink `name` on later setup errors.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### Agent B Pass 4: Maintainability

No additional confirmed maintainability-only findings beyond Agent A's Pass 4 items. The new Agent B findings above are behavioral/reliability issues with maintainability aspects, but they are tracked in their higher-impact passes.

---

### Agent B Pass 5: Dead / Obsolete Code

#### 2026-06-25-1-20: `S_ISREG` fallback macro is never referenced

**Status:** OPEN
**Severity:** LOW
**Pass:** Dead / Obsolete Code (Pass 5)
**File:** [atch.h:64](../../../atch.h#L64)

`atch.h` carries a fallback `S_ISREG` definition, but no code in the repository tests `S_ISREG`. The active filesystem type check is `S_ISSOCK`, which is used by list/remove/session validation paths. This looks like inherited compatibility scaffolding that no longer has a call site.

**Evidence:**
```
$ rg -n "S_ISREG" . --glob '!docs/reviews/2026-06/CODEREVIEW-2026-06-25-1.md' --glob '!docs/reviews/2026-06/TODO-CODEREVIEW-2026-06-25-1.md'
./atch.h:64:#ifndef S_ISREG
./atch.h:65:#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
```

**Impact:** Dead compatibility code adds small header noise and suggests regular-file checks exist when they do not.

**Fix:**
```diff
--- a/atch.h
+++ b/atch.h
@@
-#ifndef S_ISREG
-#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
-#endif
-
 #ifndef S_ISSOCK
 #define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
 #endif
```

**Claude Issue Verification:** VALID - `grep -rn "S_ISREG"` over `*.c *.h` returns only the fallback definition at atch.h:64-65; every filesystem-type check in the code uses `S_ISSOCK` (attach.c:121, 676, 761). The `S_ISREG` macro is genuinely unreferenced dead scaffolding.
**Codex Issue Verification:** VALID - targeted `rg` finds only the fallback definition in atch.h and no source call sites.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### Agent B Summary by Priority

#### Critical (Fix Before Release)
None.

#### High (Fix Soon)
None.

#### Medium (Should Fix)
1. **2026-06-25-1-17**: Control protocol treats stream socket packets as atomic and drops fragmented packets

#### Low (Nice to Have)
2. **2026-06-25-1-18**: `tail -n` accepts nonnumeric counts as `1`
3. **2026-06-25-1-19**: `create_socket` leaves a socket pathname behind on post-bind setup failures
4. **2026-06-25-1-20**: Dead `S_ISREG` fallback macro

---

## Step 2.6: Reconciliation of Disagreements (Agent A)

Agent A re-examined every issue where the two agents' Issue Verification differed, adopting a neutral stance and re-reading the cited source fresh rather than defending the original verdict. After cross-verification the verdicts line up on all but one issue:

| Issue | Claude (A) | Codex (B) | Status |
|-------|-----------|-----------|--------|
| 2026-06-25-1-14 | PARTIALLY_VALID | PARTIALLY_VALID | **Agreed** (no disagreement — both reached PARTIALLY_VALID; the Exec Summary previously mislabeled this as residual and has been corrected) |
| 2026-06-25-1-17 | PARTIALLY_VALID | VALID | **Residual, severity-only** — reaffirmed after fresh review |

All other 18 issues are VALID/VALID and need no reconciliation.

### 2026-06-25-1-14 — resolved, no action

Both agents independently arrived at PARTIALLY_VALID for the same reason: `dir[512]` (attach.c:736) is produced by `get_session_dir` and holds at most a 511-char C string, so the worst case `511 + '/' + NAME_MAX(255) + NUL = 768` fits `path[768]` exactly — the original "769 / off-by-one" claim was wrong — while the broader concern (hard-coded, mutually-dependent magic-number path sizes and unchecked `snprintf` truncation) is a genuine maintainability issue. This is full agreement, not a residual disagreement; only the Executive Summary text needed correcting.

### 2026-06-25-1-17 — residual severity-only disagreement (PARTIALLY_VALID vs VALID)

**Fresh re-read of the source.** Confirmed independently of the original write-up: `struct packet` (atch.h:108-117) is `unsigned char type` + `unsigned char len` + a `union { unsigned char buf[sizeof(struct winsize)]; struct winsize ws; }` = **10 bytes**, with `pkt.len` capped at `sizeof(pkt.u.buf)` = 8 (master.c:521). The master reads it with one non-blocking `read` and drops the client on any short read (master.c:505-517). The client socket `s` is created and `connect`-ed at attach.c:111 and is **never** passed to `setnonblocking` (only the master's accepted client fds are, at master.c:476), so the client side is **blocking**; `write_packet_or_fail` (attach.c:62), `push_main` (attach.c:500), and `send_kill` each issue a single blocking `write` of the full 10-byte struct.

**Steelman of Codex's VALID.** Codex is right on the principle: `SOCK_STREAM` makes no message-boundary guarantee, so any server that requires "one packet per `read`" is relying on behavior the API does not promise. As a defensive/robustness matter the master *should* accumulate into a per-client input buffer until a full `struct packet` is present (the fix sketch in the issue is correct), and the asymmetric write helpers (`write_packet_or_fail`/`push_main`/`send_kill`) should track an offset like `write_buf_or_fail` already does. That latent defect is real and worth fixing.

**Where the evidence lands on impact.** A blocking 10-byte `write` into an essentially-empty AF_UNIX `SOCK_STREAM` buffer is delivered by the kernel as a single contiguous skb, and a subsequent `read(fd, &pkt, 10)` consumes that whole skb — so a short read cannot occur for the shipped client. Fragmentation requires either (a) a deliberately nonstandard client that splits its writes, or (b) the send buffer being filled to within <10 free bytes of capacity (~20k unread packets queued), neither of which arises in normal single-binary operation. Codex's own write-up names exactly these preconditions ("most likely under unusual socket backpressure or with nonstandard clients"). So the described MEDIUM consequences — spurious detach, lost input, failed kill — do not materialize in practice; the accurate practical severity is LOW.

**Decision.** I reaffirm **PARTIALLY_VALID**: the code diagnosis is fully correct (hence not INVALID), but the MEDIUM impact is overstated (hence not VALID). This is not me defending my prior verdict — both agents' *substantive* analysis actually agrees that fragmentation needs nonstandard/extreme conditions; the only gap is the severity label. **Proposed common ground: keep the issue OPEN as a real defensive fix but downgrade Severity MEDIUM → LOW.** Per the workflow, severity itself was left unchanged in the issue header pending the Step 2.7 Executive-Summary/TODO refresh and/or human confirmation; if Codex maintains MEDIUM, this stands as a severity-only residual for human review, with no remaining dispute about the code.

---
