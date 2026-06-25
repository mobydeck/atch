# Security Review: atch

**Reviewers:** Claude (Agent A), Codex (Agent B)
**Date:** 2026-06-25
**Review ID:** 2026-06-25-1
**Codebase Version:** 15e0d3a0912618c08f7a74f85e41cca673b313f0
**Previous Reviews:** None
**Prompt:** [SECURITYREVIEW-prompt.md](../SECURITYREVIEW-prompt.md)

---

## How the Review Was Performed

Deep security-focused review grounded in OWASP Top 10, OWASP API Security Top 10, OWASP WSTG, OWASP Juice-Shop-style anti-patterns, and NIST CSF 2.0 outcomes (GV / ID / PR / DE / RS / RC). Conducted in 5 passes by two AI agents (Claude and Codex) writing into this same file, with dual-agent cross-verification of every finding.

`atch` is a small C utility (a `dtach` fork) that attaches/detaches terminal sessions over a per-user Unix domain socket, runs the target program in a pty held by a daemonized master process, persists all pty output to an on-disk log, and replays scrollback to attaching clients. There is no network listener and no privilege boundary inside a single uid: sockets are created mode `0600` inside a `0700` directory, so the realistic threat model is (a) other local users on a shared host, (b) other processes running under the *same* uid (sandboxed helpers, malicious scripts), and (c) untrusted byte streams (session output / planted logs) reaching a victim's terminal. Findings are framed against that model.

| Pass | Focus | Issues Found |
|------|-------|--------------|
| 1 | Attack Surface & High-Risk Findings | 4 |
| 2 | Logic Correctness & Abuse Cases | 2 |
| 3 | Reliability, Resilience & Operational Safety | 4 |
| 4 | Test Coverage, Maintainability & Refactor Opportunities | 1 |
| 5 | Compliance Mapping & Missing Controls | (mapping only) |

(Pass 3 = 4: 2026-06-25-1-2, 1-4, 1-5 from Agent A plus 2026-06-25-1-11 from Agent B.)

**Critical:** 0
**High:** 0
**Medium:** 3
**Low:** 8
**Total:** 11
**FIXED:** 0
**WONTFIX:** 0
**ONHOLD:** 0

**Agent agreement:** Both agents VALID on 10 findings (1-1, 1-3, 1-4, 1-5, 1-6, 1-7, 1-8, 1-9, 1-10, 1-11); both PARTIALLY_VALID (reconciled) on 1-2. No unresolved disagreements remain.

Findings are grounded in specific file paths and line numbers. Each is labeled Confirmed / Likely / Suspicion and mapped to OWASP / WSTG / API Security / NIST CSF 2.0. Distinction between issues and fixes: this document raises issues — patches are applied in separate commits and tracked via the Fix Verification fields below.

### Semgrep triage (leads dismissed, no findings created)

Two Semgrep results were provided. Both were triaged against the source and **dismissed** as not-real / unreachable; they are recorded here so a future reader does not re-flag them:

- `c.lang.security.insecure-use-string-copy-fn` — **atch.c:25** `strcpy(d, "_SESSION")` in `init_envvar_names`. Dismissed (false positive). The destination is `static char envname[128]`; the preceding loop is bounded by `max = sizeof(envname) - sizeof("_SESSION")` = `128 - 9 = 119`, so `d` is at most `envname+119`, and the 9-byte literal `"_SESSION\0"` lands at indices 119..127, exactly inside the 128-byte buffer. The copy length is a compile-time constant into a provably sufficient remainder — no overflow.
- `c.lang.security.insecure-use-string-copy-fn` — **master.c:833** `strcpy(name, buf)` in the bundled fallback `openpty()`. Dismissed (unreachable). This block is compiled only under `#ifndef HAVE_PTY_H`, and even when compiled the only caller is `forkpty(&the_pty.fd, NULL, …)` (master.c:159/161) which passes `name == NULL`; the `if (name) strcpy(name, buf)` guard therefore never runs. On Linux (the supported/release target) `HAVE_PTY_H` is defined and the libc `forkpty` is used, so this code is not even built. Left as a hardening note in 2026-06-25-1-7 rather than its own finding.

---

## Pass 1 — Attack Surface & High-Risk Security Findings

### Executive summary
- The entire control surface is one Unix domain socket per session (`~/.cache/atch/<name>` or a `/`-containing path). Message handling (`client_activity`) is the only parser and it is small and mostly bounds-checked — the `MSG_PUSH` length guard `pkt.len <= sizeof(pkt.u.buf)` is correct, so there is **no buffer overflow** in the protocol path.
- The highest-impact attack-surface issue is the **predictable `/tmp/.atch-<uid>` fallback** combined with `open(O_RDWR|O_CREAT)` on the log without `O_NOFOLLOW`/`O_EXCL` and unchecked `mkdir`: on a shared host with `$HOME` unset, a local attacker can pre-plant a symlink and make the victim truncate/redirect an arbitrary victim-writable file (2026-06-25-1-1).
- The control socket has **no message authentication or origin check** beyond filesystem permissions, so any same-uid process can inject keystrokes (`MSG_PUSH`) and signals (`MSG_KILL`) into any of the user's live sessions (2026-06-25-1-3). Within-uid this is consistent with the Unix model but worth documenting as an explicit trust assumption.
- The persistent session log (`open_log`) is leaked to the executed child program because it is opened **without `FD_CLOEXEC`**, unlike the listening socket which sets it (2026-06-25-1-6).
- Raw bytes from logs are replayed verbatim to the terminal on cold attach / `tail`, an inherent **terminal-escape-injection** vector for attacker-influenced logs (2026-06-25-1-10, Low — folded into the design notes).

### Prioritized findings

---

### 2026-06-25-1-1: Predictable `/tmp/.atch-<uid>` fallback with symlink-following log/socket creation and unchecked `mkdir`

**Status:** OPEN
**Severity:** MEDIUM
**Confidence:** Likely
**Pass:** Attack Surface (Pass 1)
**File:** [atch.c:37-57](../../../atch.c#L37-L57), [atch.c:258-280](../../../atch.c#L258-L280), [master.c:82-93](../../../master.c#L82-L93)

When `$HOME` is unset/empty and the passwd home is missing or `/`, session storage falls back to the **fixed, world-known** path `/tmp/.atch-<uid>` (`get_session_dir`, atch.c:56). `expand_sockname` then calls `mkdir(dir, 0700)` but **ignores the return value** and never verifies that the resulting directory is owned by the caller, is a real directory, and is not a symlink (atch.c:272-275). The session log is later opened with `open(path, O_RDWR | O_CREAT, 0600)` (master.c:86) — **no `O_NOFOLLOW`, no `O_EXCL`** — and `rotate_log` may `ftruncate` and rewrite that file (master.c:55-76).

On a multi-user host an attacker who knows the victim's uid can pre-create `/tmp/.atch-<uid>` (as a directory they own, or as a symlink into a victim-writable location), and/or pre-plant `<session>.log` as a symlink/hardlink to a victim-owned file. When the victim runs `atch`, `mkdir` fails `EEXIST` (silently ignored) and atch proceeds to create the socket and open/append/trim the log through the attacker-controlled path.

**Category**
- OWASP: A01 Broken Access Control (local file-system trust boundary) / A04 Insecure Design (predictable shared-tmp path)
- WSTG: WSTG-CONF-09 (File Permissions), WSTG-BUSL-09 / local symlink TOCTOU
- API Security: — (no HTTP API)
- NIST CSF 2.0: PR.AA-05 (access enforced), PR.DS-01 (data-at-rest integrity), PR.PS-01 (secure configuration)

**Evidence:**
```c
// atch.c:53-57 — predictable fallback when $HOME is unusable
if (home && *home && strcmp(home, "/") != 0)
    snprintf(buf, size, "%s/.cache/%s", home, base);
else
    snprintf(buf, size, "/tmp/.%s-%d", base, (int)getuid());

// atch.c:272-275 — mkdir results unchecked, no ownership/type verification
mkdir(dir, 0700);          // parent (~/.cache)
... 
mkdir(dir, 0700);          // session dir

// master.c:86 — log opened with no O_NOFOLLOW / O_EXCL; follows symlinks
fd = open(path, O_RDWR | O_CREAT, 0600);
```

**Impact:** A local attacker on a shared host can, when the victim's `$HOME` is unset (cron jobs, some `su`/login-shell and container init contexts), (a) cause the victim's session output — which may include typed secrets and command output — to be written into an attacker-chosen, victim-writable destination, and (b) cause `rotate_log`'s `ftruncate(0)` + rewrite to clobber an existing victim file (e.g. `~/.bashrc`, `~/.ssh/authorized_keys`). Integrity/confidentiality loss with attacker-controlled target selection.

**Attack or failure scenario:**
1. Attacker (uid 2000) learns victim uid is 1000 and that the victim periodically runs `atch` with `$HOME` unset.
2. Attacker runs `ln -s /home/victim /tmp/.atch-1000` (or creates the dir and plants `work.log -> /home/victim/.bashrc`).
3. Victim runs `atch work`; `mkdir` fails `EEXIST` (ignored); `open(".../work.log", O_RDWR|O_CREAT)` follows the symlink and opens the victim's real file.
4. `rotate_log` truncates/rewrites it, or pty output is appended to it — victim file corrupted/redirected.

**Recommendation:** Do not use a fixed world-writable path. Prefer `$XDG_RUNTIME_DIR` (per-user, `0700`, on `tmpfs`) when available; otherwise create the fallback with `mkdir(path, 0700)` and **check the result**, then `lstat`/`fstatat` to confirm it is a directory, owned by `getuid()`, mode `0700`, and not a symlink — bail out otherwise. Open the log and bind the socket with `O_NOFOLLOW` (and `O_EXCL`/`mkstemp`-style creation where possible), or open the parent dir with `O_DIRECTORY|O_NOFOLLOW` and use `openat`. Refuse to operate on any session file that is a symlink.

**Patch:**
```diff
--- a/master.c
+++ b/master.c
@@ open_log
-	fd = open(path, O_RDWR | O_CREAT, 0600);
+	fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
 	if (fd < 0)
 		return -1;
```
```diff
--- a/atch.c
+++ b/atch.c
@@ get_session_dir
-	if (home && *home && strcmp(home, "/") != 0)
-		snprintf(buf, size, "%s/.cache/%s", home, base);
-	else
-		snprintf(buf, size, "/tmp/.%s-%d", base, (int)getuid());
+	const char *xdg = getenv("XDG_RUNTIME_DIR");
+	if (home && *home && strcmp(home, "/") != 0)
+		snprintf(buf, size, "%s/.cache/%s", home, base);
+	else if (xdg && *xdg)
+		snprintf(buf, size, "%s/%s", xdg, base);
+	else
+		snprintf(buf, size, "/tmp/.%s-%d", base, (int)getuid());
@@ expand_sockname  (after creating dir)
+	{
+		struct stat ds;
+		if (lstat(dir, &ds) < 0 || !S_ISDIR(ds.st_mode) ||
+		    ds.st_uid != getuid() || (ds.st_mode & 077)) {
+			fprintf(stderr, "%s: unsafe session dir %s\n", progname, dir);
+			exit(1);
+		}
+	}
```

**Tests to add:**
- security: with `HOME=` unset and a pre-planted symlink `${dir}/s.log -> $PWD/victim`, assert atch refuses to start / does not write through the symlink and `victim` is untouched.
- security: pre-create `${dir}` owned by a different uid (or as a symlink) and assert atch bails instead of using it.
- unit: `get_session_dir` returns `$XDG_RUNTIME_DIR/...` when `HOME` is unset and `XDG_RUNTIME_DIR` is set.

**Residual risk / follow-up:** `O_NOFOLLOW` only blocks a symlink at the final component; the full-path-walk through an attacker-owned parent dir still needs the ownership/type check above (or `openat` from a verified dir fd). Sockets created via `bind()` cannot take `O_NOFOLLOW`; the directory-trust check is the real mitigation there.

**Claude Issue Verification:** VALID - Confirmed by reading atch.c:53-57/272-275 and master.c:86; fallback path is fixed and predictable, `mkdir` return ignored, log open lacks O_NOFOLLOW. Exploit requires HOME unset on a shared host (precondition), hence MEDIUM not HIGH.
**Codex Issue Verification:** VALID - Confirmed by reading `get_session_dir`, `expand_sockname`, and `open_log`: the fallback path is predictable, directory creation errors are ignored, no ownership/type checks are performed, and the log open follows symlinks. The exact attacker pre-planting path depends on a victim-writable attacker-controlled directory (or a system without symlink protections), but the underlying unsafe path handling is real.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### 2026-06-25-1-3: Control socket has no message origin check — any same-uid process can inject input and signals into a session

**Status:** OPEN
**Severity:** LOW
**Confidence:** Confirmed
**Pass:** Attack Surface (Pass 1)
**File:** [master.c:498-576](../../../master.c#L498-L576), [master.c:240-274](../../../master.c#L240-L274)

The master accepts any client that can `connect()` to the session socket and immediately honors `MSG_PUSH` (inject bytes into the pty, master.c:520-523) and `MSG_KILL` (send an arbitrary signal to the child's process group, master.c:572-575) with no authentication beyond filesystem permissions. The socket is `0600` in a `0700` dir, so access is restricted to the owning uid — but **every** process running under that uid (sandboxed helpers, a compromised browser, a malicious `npm` post-install script) can therefore drive any of the user's live `atch` sessions.

**Category**
- OWASP: A04 Insecure Design (no authN on a control channel) / A01 Broken Access Control
- WSTG: WSTG-ATHZ-02 (privilege/authorization), WSTG-SESS (session control)
- API Security: API2 Broken Authentication (analogous: unauthenticated local IPC)
- NIST CSF 2.0: PR.AA-05, PR.AA-01 (identities/credentials), DE.CM-01

**Evidence:**
```c
// master.c:520-523 — inject keystrokes into the pty, no caller check
if (pkt.type == MSG_PUSH) {
    if (pkt.len <= sizeof(pkt.u.buf))
        write_buf_or_fail(the_pty.fd, pkt.u.buf, pkt.len);
}
// master.c:572-575 — deliver an arbitrary signal to the child
else if (pkt.type == MSG_KILL) {
    int sig = pkt.len ? (int)(unsigned char)pkt.len : SIGTERM;
    killpty(&the_pty, sig);
}
```

**Impact:** A same-uid process can type commands into a session that may hold a more sensitive context (a `sudo`/`su` shell, a remote SSH, a DB client), i.e. local lateral movement via keystroke injection, and can kill sessions. This does **not** cross a uid boundary, so it is rated LOW (a same-uid attacker can already `ptrace`/`kill`); the value of recording it is to make the "filesystem-perms == authentication" assumption explicit and to argue against ever loosening the socket mode.

**Attack or failure scenario:** Victim runs `atch root -- sudo -s` and authenticates. A malicious same-uid background process connects to `~/.cache/atch/root` and sends `MSG_PUSH` packets spelling `curl evil|sh\n`, executed inside the root shell.

**Recommendation:** Keep `0600`/`0700` and document this as the security boundary (the README should state "any process running as your user can control your sessions"). For defense-in-depth, consider `SO_PEERCRED`/`getsockopt(SO_PEERCRED)` to assert the connecting peer's uid == owner uid on `accept()`, and reject otherwise (cheap, blocks confused-deputy cases where the socket somehow becomes group/other-accessible).

**Patch:**
```diff
--- a/master.c
+++ b/master.c
@@ control_activity
 	fd = accept(s, NULL, NULL);
 	if (fd < 0)
 		return;
+#ifdef SO_PEERCRED
+	{
+		struct ucred cr; socklen_t crl = sizeof(cr);
+		if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &crl) == 0 &&
+		    cr.uid != getuid()) { close(fd); return; }
+	}
+#endif
 	else if (setnonblocking(fd) < 0) {
```

**Tests to add:**
- security: from a second uid (where testable) assert `connect()` is refused / peer-cred check rejects.
- documentation: assert README "Session storage" section states the same-uid control assumption.

**Residual risk / follow-up:** `SO_PEERCRED` is Linux-specific; on other platforms the `0600` perms remain the only control. This is hardening, not a boundary fix — same-uid trust is inherent.

**Claude Issue Verification:** VALID - Confirmed; no authentication on MSG_PUSH/MSG_KILL beyond socket perms. Severity LOW because it is within-uid by design.
**Codex Issue Verification:** VALID - Confirmed: `client_activity` accepts `MSG_PUSH` and `MSG_KILL` from any process that can connect to the socket, with filesystem permissions as the only boundary. Severity LOW is appropriate because this is same-uid by design.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### 2026-06-25-1-6: Session log fd leaked to the executed program (missing `FD_CLOEXEC`)

**Status:** OPEN
**Severity:** LOW
**Confidence:** Confirmed
**Pass:** Attack Surface (Pass 1)
**File:** [master.c:82-93](../../../master.c#L82-L93), [master.c:726-732](../../../master.c#L726-L732), [master.c:149-198](../../../master.c#L149-L198)

`open_log` opens the persistent session log and stores it in `log_fd` (master.c:86) but never sets `FD_CLOEXEC`. The listening control socket *does* get `FD_CLOEXEC` (master.c:735). Because the log is opened before `init_pty`/`forkpty` execs the user's program, the **child program inherits an open, writable fd to its own session log**.

**Category**
- OWASP: A05 Security Misconfiguration (fd hygiene) / A04 Insecure Design
- WSTG: WSTG-CONF-09 (resource/permission hygiene)
- API Security: —
- NIST CSF 2.0: PR.PS-01 (secure configuration), PR.DS-01

**Evidence:**
```c
// master.c:86 — no FD_CLOEXEC on the log fd
fd = open(path, O_RDWR | O_CREAT, 0600);
...
// master.c:735 — contrast: the control socket IS marked close-on-exec
fcntl(s, F_SETFD, FD_CLOEXEC);
```

**Impact:** The executed program (and anything it spawns) gets a stray writable fd to the on-disk transcript. A buggy/hostile child can read or rewrite its own history (e.g. scrub evidence by `ftruncate`), or accidentally write to a high fd number. Same-uid only, hence LOW, but it is a gratuitous fd leak that breaks the "tamper-resistant on-disk history" property the README advertises.

**Attack or failure scenario:** A program run in a session does `for fd in /proc/self/fd/*; do …; done`, finds the inherited log fd, and `ftruncate`s it to erase the recorded session history that an operator relies on for audit.

**Recommendation:** Set `FD_CLOEXEC` on `log_fd` immediately after `open` (or open with `O_CLOEXEC`). Audit all long-lived fds in the master for the same treatment.

**Patch:**
```diff
--- a/master.c
+++ b/master.c
@@ open_log
-	fd = open(path, O_RDWR | O_CREAT, 0600);
+	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
 	if (fd < 0)
 		return -1;
```

**Tests to add:**
- security/integration: run `atch new s -- sh -c 'ls -l /proc/self/fd'` and assert the session `.log` path is not among the child's inherited fds.

**Residual risk / follow-up:** `O_CLOEXEC` requires a reasonably modern libc/kernel; on the static-musl release target it is available. Also confirm the pty master fd is not leaked (it is closed in the forkpty child) and that error-pipe fds are handled (they are, master.c:740-752).

**Claude Issue Verification:** VALID - Confirmed: master.c:86 lacks O_CLOEXEC/FD_CLOEXEC while the socket at master.c:735 sets it; log_fd is opened pre-fork/exec and thus inherited by the child.
**Codex Issue Verification:** VALID - Confirmed: `open_log` opens the transcript before `forkpty` and does not set `O_CLOEXEC`/`FD_CLOEXEC`, while the control socket and status pipe are explicitly marked close-on-exec. The executed program can inherit the writable log fd.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### 2026-06-25-1-10: Verbatim replay of on-disk logs is a terminal-escape-injection vector (cold attach / `tail`)

**Status:** OPEN
**Severity:** LOW
**Confidence:** Likely
**Pass:** Attack Surface (Pass 1)
**File:** [attach.c:228-257](../../../attach.c#L228-L257), [atch.c:580-660](../../../atch.c#L580-L660)

`replay_session_log` and `cmd_tail` read the raw `.log` file and `write(1, …)` it directly to the user's terminal with no sanitization (attach.c:243-244, atch.c:645-646/653-654). This is by design (the whole product premise is raw passthrough). The security-relevant consequence: if an attacker can influence a log's bytes — via the symlink/redirection path of 2026-06-25-1-1, by planting a `<session>.log` file, or by feeding hostile output into a session — then later `atch <session>`, `atch tail`, or `atch list -a`→attach replays attacker-chosen escape sequences (e.g. clipboard writes via OSC 52, title-setting + command-prompt injection, DECRQSS query/response loops) into the victim's terminal.

**Category**
- OWASP: A03 Injection (terminal/control-sequence injection) / A04 Insecure Design
- WSTG: WSTG-CLNT-04 (client-side / output handling), WSTG-INPV (output not neutralized)
- API Security: —
- NIST CSF 2.0: PR.DS-02 (data-in-transit/display integrity), PR.PS-06

**Evidence:**
```c
// attach.c:243-244 — raw log bytes streamed to stdout, no filtering
while ((n = read(logfd, rbuf, sizeof(rbuf))) > 0)
    write(1, rbuf, (size_t)n);
// atch.c:645-646 — same for `tail`
while ((n = read(fd, rbuf, sizeof(rbuf))) > 0)
    write(1, rbuf, (size_t)n);
```

**Impact:** Depends entirely on the victim's terminal emulator; modern terminals have curtailed the worst (no raw "answerback → command") behaviors, but OSC 52 clipboard hijack, window-title abuse, and hyperlink spoofing remain realistic. Same-uid / planted-file precondition makes this LOW, but it compounds 2026-06-25-1-1.

**Attack or failure scenario:** Attacker plants `~/.cache/atch/notes.log` containing OSC 52 sequences that overwrite the victim's clipboard with a malicious command; victim runs `atch tail notes` and later pastes.

**Recommendation:** This is inherent to a raw-passthrough tool and a full fix conflicts with the product goal. Mitigate by: (a) treating cold log replay as lower-trust than live pty data — offer a `--sanitize`/`-S` mode for `tail`/cold-replay that filters C1/`ESC` control bytes; (b) closing the *injection entry point* via 2026-06-25-1-1 so attackers cannot author logs they do not own. Document the residual risk in the README.

**Patch:** (design note — no minimal diff; gate behind an opt-in flag)
```text
Add a filter for tail/cold-replay that strips non-printable control bytes
except CR/LF/TAB when --sanitize is set; keep raw replay as the default for
running-session attach where the bytes are the user's own program output.
```

**Tests to add:**
- security: `atch tail` with `--sanitize` on a log containing `\x1b]52;c;…\x07` emits the bytes neutralized; without the flag, document that raw replay is intentional.

**Residual risk / follow-up:** Cannot be fully eliminated without breaking transparency (mouse/sixel/truecolor). Best handled as defense-in-depth plus fixing the authoring vector (1-1).

**Claude Issue Verification:** VALID - Confirmed raw `write(1,...)` of log bytes at attach.c:243 and atch.c:645/653; rated LOW and partly inherent, but a genuine vector when combined with 2026-06-25-1-1.
**Codex Issue Verification:** VALID - Confirmed raw replay paths write log bytes directly to stdout in `replay_session_log` and `cmd_tail`. This is mostly inherent to the product's raw-terminal model, but it is a real terminal control-sequence exposure when logs are attacker-influenced.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### Coverage and blind spots (Pass 1)
- **Reviewed:** all entry points (`main` dispatch, legacy flag mode, every subcommand), the socket protocol parser (`client_activity`), socket/log creation and permission handling, env-var propagation, fd hygiene, signal setup.
- **Not verified:** runtime behavior on a true multi-user host with `$HOME` unset (the 1-1 exploit is reasoned, not executed); macOS (`HAVE_UTIL_H`) build path; terminal-emulator-specific escape behavior for 1-10.
- **Artifacts that would raise confidence:** a strace of a session showing inherited fds; a shared-host reproduction of the symlink attack; CI matrix output for Linux/macOS builds.

---

## Pass 2 — Logic Correctness, Abuse Cases & Edge Conditions

### Executive summary
- `parse_size` can integer-overflow on the `k`/`m` multiply, silently yielding a tiny or zero log cap (2026-06-25-1-7), which also subsumes a path-truncation robustness note for over-long session names.
- The in-memory scrollback ring can be overwritten by fresh pty output *during* a multi-iteration replay, so a slow client can receive garbled / out-of-order history (2026-06-25-1-8).

### Prioritized findings

---

### 2026-06-25-1-7: `parse_size` integer overflow and unchecked `malloc`/path-truncation around session paths

**Status:** OPEN
**Severity:** LOW
**Confidence:** Confirmed
**Pass:** Logic Correctness (Pass 2)
**File:** [atch.c:111-134](../../../atch.c#L111-L134), [atch.c:276-279](../../../atch.c#L276-L279), [master.c:728-731](../../../master.c#L728-L731)

`parse_size` parses the `-C` log cap via `strtoul` and then does `v *= 1024` / `v *= 1024 * 1024` with **no overflow check** (atch.c:124-128); the result feeds `log_max_size` (a `size_t`) and `rotate_log`'s `malloc(log_max_size)`. A value like `-C 18014398509482m` wraps to a small or zero cap, silently defeating the intended "keep last N bytes" bound or disabling logging unexpectedly. Relatedly, session paths flow into fixed `snprintf` buffers (`log_path[600]`, `dir[512]`, atch.c:514/584/629, master.c:730) where an over-long `/`-style session name is **truncated**, so the master can bind the socket at the full path while opening/clearing/tailing a *different* (truncated) `.log` path — a silent mismatch. `expand_sockname`'s `malloc` return is also unchecked (atch.c:277), so OOM yields `snprintf(NULL, …)` → crash.

**Category**
- OWASP: A04 Insecure Design (unsafe integer/size handling) / A05 Security Misconfiguration
- WSTG: WSTG-INPV-11 (numeric input handling)
- API Security: —
- NIST CSF 2.0: PR.PS-01, PR.DS-01

**Evidence:**
```c
// atch.c:123-128 — multiply with no overflow guard
if (*end == 'k' || *end == 'K') { v *= 1024; end++; }
else if (*end == 'm' || *end == 'M') { v *= 1024 * 1024; end++; }
...
*out = (size_t)v;
// atch.c:276-279 — malloc not checked
full = malloc(fulllen + 1);
snprintf(full, fulllen + 1, "%s/%s", dir, sockname);
```

**Impact:** Misconfiguration (log cap silently wrong/disabled → either unbounded-ish growth defeated or no log when one was requested), and a crash on OOM. No memory-safety break (`snprintf` is bounded). LOW.

**Attack or failure scenario:** An operator sets `-C 99999999999999999m` expecting a huge cap; it wraps to a few bytes, and `rotate_log` trims the log to near-nothing every write, destroying the history the operator wanted.

**Recommendation:** Detect overflow in `parse_size` (compare against `SIZE_MAX / multiplier` before multiplying) and reject; cap to a sane max. Check `malloc` returns and fail cleanly. Detect `snprintf` truncation (`ret >= (int)sizeof(buf)`) for the `.log`/dir paths and error out rather than operating on a truncated path.

**Patch:**
```diff
--- a/atch.c
+++ b/atch.c
@@ parse_size
-	if (*end == 'k' || *end == 'K') {
-		v *= 1024;
-		end++;
-	} else if (*end == 'm' || *end == 'M') {
-		v *= 1024 * 1024;
-		end++;
-	}
+	unsigned long mul = 1;
+	if (*end == 'k' || *end == 'K') { mul = 1024UL; end++; }
+	else if (*end == 'm' || *end == 'M') { mul = 1024UL * 1024UL; end++; }
+	if (mul != 1 && v > (unsigned long)SIZE_MAX / mul)
+		return 1;               /* overflow */
+	v *= mul;
@@ expand_sockname
 	full = malloc(fulllen + 1);
+	if (!full) { perror("malloc"); exit(1); }
 	snprintf(full, fulllen + 1, "%s/%s", dir, sockname);
```

**Tests to add:**
- unit: `parse_size("99999999999999999m")` returns error (overflow) rather than a small value.
- integration: a session name long enough to truncate `log_path` is rejected, not silently mishandled.

**Residual risk / follow-up:** Path-truncation handling should be centralized in one helper that builds `<path>.log` so all four call sites (clear/tail/open/rm) stay consistent.

**Claude Issue Verification:** VALID - Confirmed: no overflow guard at atch.c:123-128; malloc unchecked at atch.c:277; multiple fixed-size path buffers truncate silently. Memory-safe but a correctness/robustness defect. LOW.
**Codex Issue Verification:** VALID - Confirmed: `parse_size` lacks `errno` and multiplication overflow checks, `expand_sockname` does not check `malloc`, and fixed-size `.log` buffers silently truncate over-long socket paths.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### 2026-06-25-1-8: Scrollback ring can be overwritten mid-replay, delivering corrupted/out-of-order history

**Status:** OPEN
**Severity:** LOW
**Confidence:** Likely
**Pass:** Logic Correctness (Pass 2)
**File:** [master.c:294-364](../../../master.c#L294-L364), [master.c:368-397](../../../master.c#L368-L397), [master.c:686-700](../../../master.c#L686-L700)

`replay_start`/`replay_drain` snapshot only a *position* into the shared ring (`p->replay_head` = `scrollback_head`, `p->replay_remaining` = `scrollback_len`) and stream from the live `scrollback_buf` across potentially many `select` iterations (master.c:354-364, 686-697). If a client's socket buffer fills (`EAGAIN`), `replay_drain` returns and the master proceeds to `pty_activity`, where `scrollback_append` can advance `scrollback_head` and **overwrite the very bytes the client has not yet read** (master.c:296-317). The client then resumes reading from a `replay_head` that now points at newer data, producing a corrupted/torn replay (old history interleaved with or replaced by newer bytes, out of order).

**Category**
- OWASP: A04 Insecure Design (shared mutable buffer without snapshot/copy)
- WSTG: WSTG-BUSL-07 (function misuse / integrity of replayed data)
- API Security: —
- NIST CSF 2.0: PR.DS-01 (data integrity)

**Evidence:**
```c
// master.c:361-363 — replay holds an index into the *live* ring
p->replay_head = scrollback_head;
p->replay_remaining = scrollback_len;
// master.c:306-315 — concurrent append advances head, overwriting unread bytes
size_t wp = (scrollback_head + scrollback_len) & (SCROLLBACK_SIZE - 1);
scrollback_buf[wp] = buf[i];
if (scrollback_len < SCROLLBACK_SIZE) scrollback_len++;
else scrollback_head = (scrollback_head + 1) & (SCROLLBACK_SIZE - 1);
```

**Impact:** Integrity of the replayed scrollback only — a client attaching to a busy session over a slow/backpressured link may see garbled history. No memory-safety issue (indices stay in-bounds via the power-of-two mask) and no cross-session leak. LOW; worse for large/busy sessions.

**Attack or failure scenario:** A high-output program (e.g. a build) is running; a user attaches over a constrained link so the socket buffers fill mid-replay; while `replay_drain` is parked on `EAGAIN`, the build's new output wraps the 128 KB ring and the user's replayed "history" is a corrupted mix instead of a coherent transcript.

**Recommendation:** Snapshot the replay region into a per-client buffer at `replay_start` (or freeze it by copying the `scrollback_len` bytes), so subsequent `scrollback_append` cannot mutate in-flight replay data. Alternatively, track a generation/wrap counter and abort+restart replay (or fall back to the on-disk log) if the ring wrapped past `replay_head` while draining.

**Patch:** (design — per-client snapshot)
```text
On replay_start: malloc replay_remaining bytes, memcpy the linearized ring
contents into it, and have replay_drain stream from that private copy; free
on completion/disconnect. Decouples replay from concurrent scrollback_append.
```

**Tests to add:**
- integration: attach a deliberately slow reader to a session emitting > SCROLLBACK_SIZE bytes during replay; assert the replayed prefix is a contiguous, in-order suffix of the produced output (no interleaving).

**Residual risk / follow-up:** A per-client copy adds up to `SCROLLBACK_SIZE` bytes of memory per attaching client; bound the number of concurrent replays or cap the snapshot size.

**Claude Issue Verification:** VALID - Confirmed by tracing replay_start/replay_drain against scrollback_append; the EAGAIN return path (master.c:335-336) leaves a live index that a later append (master.c:699-700) can overrun. Integrity-only, in-bounds, LOW.
**Codex Issue Verification:** VALID - Confirmed: replay state is an index into the shared mutable ring; a client paused on `EAGAIN` can resume after `scrollback_append` has wrapped over unread bytes, yielding an in-bounds but torn replay.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### Coverage and blind spots (Pass 2)
- **Reviewed:** option parsing, size parsing, path construction, env-chain self-attach prevention (`attach_main` ancestry scan — looks correct), scrollback ring math, attach/detach/suspend state.
- **Not verified:** behavior under genuine concurrent multi-client attach with backpressure (1-8 reasoned from code, not stress-tested).
- **Artifacts:** a multi-client stress harness; ASan/UBSan build to confirm no index issues under load.

---

## Pass 3 — Reliability, Resilience & Operational Safety

### Executive summary
- When the **sole** attached client (or **all** attached clients) stop reading (e.g. terminal flow-control / Ctrl-S, or a slow link), the master parks in `pty_activity`'s inner `select` and **freezes the session and blocks the child process** — head-of-line blocking with no per-client timeout or drop (2026-06-25-1-2; reconciled to the sole/all-clients condition). This is the most operationally significant resilience issue.
- A same-uid process can open unbounded connections; accepted fds are never capped or checked against `FD_SETSIZE` before being handed to `FD_SET`, so a connection flood drives an out-of-bounds `FD_SET` and crashes/corrupts the master (2026-06-25-1-11, Agent B / Codex).
- Protocol reads assume one packet per `read()`; a partial read disconnects a legitimate client (2026-06-25-1-4). Signal handlers call non-async-signal-safe `printf`/`exit` (2026-06-25-1-5).

### Prioritized findings

---

### 2026-06-25-1-2: One non-reading client freezes the whole session and stalls the child (head-of-line blocking DoS)

**Status:** OPEN
**Severity:** MEDIUM
**Confidence:** Confirmed
**Pass:** Reliability (Pass 3)
**File:** [master.c:368-464](../../../master.c#L368-L464)

`pty_activity` reads a chunk from the pty and must deliver it to **every** attached client before returning to the main loop. For a client whose socket send buffer is full (it has stopped reading), the write yields `EAGAIN`; the code neither counts it as done nor drops it (drop happens only on a non-`EAGAIN` error, master.c:451-458). With that client as the blocker, `nclients` stays 0, the control socket isn't readable, and the function `goto top`s back into a blocking `select` that waits for the stuck client to become writable (master.c:408-463). Until that client drains, the master **never returns to the main loop**: it stops reading new pty output (the pty buffer fills and the child blocks on write), stops accepting `MSG_DETACH`/new connections, and stops servicing every other attached client.

**Category**
- OWASP: A04 Insecure Design (no backpressure isolation) / availability
- WSTG: WSTG-BUSL-03 / DoS via resource starvation
- API Security: API4 Unrestricted Resource Consumption (analogous)
- NIST CSF 2.0: PR.IR-03 (resilience), DE.CM-01, RS.MI-02

**Evidence:**
```c
// master.c:439-463 — buffer must reach all clients; stuck client never dropped on EAGAIN
while (written < len) {
    ssize_t n = write(p->fd, buf + written, len - written);
    if (n > 0) { written += n; continue; }
    else if (n < 0 && errno == EINTR) continue;
    break;
}
if (written == len) nclients++;
else if (errno != EAGAIN) { /* drop client */ }
...
if (!FD_ISSET(s, &readfds) && nclients == 0)
    goto top;          /* re-block until the slow client drains */
```

**Impact:** Availability of the session and liveness of the hosted program. Trivially and often **accidentally** triggered: pressing Ctrl-S (XOFF) in one attached terminal, a wedged/backgrounded terminal, or a high-latency SSH link will stall the program for all clients and can wedge a long-running job. The stuck client cannot even detach (its `MSG_DETACH` is never read). Same-uid scope keeps it MEDIUM rather than HIGH, but it is a real, easy-to-hit denial of service in a multi-client tool whose headline feature is multi-client attach.

**Attack or failure scenario:** Two users (same account) attach to a shared `atch` session running a deploy. One presses Ctrl-S by habit; the deploy's stdout backs up, the pty fills, the deploy blocks mid-step, and neither user can detach or regain control until the first terminal is resumed/killed.

**Recommendation:** Give each client a bounded outbound buffer and a write deadline; on persistent `EAGAIN` past a threshold (bytes queued or time), **drop the slow client** (close + unlink from list) rather than blocking the session — exactly as `replay_drain` already drops on hard errors. Decouple pty reading from per-client writing: queue pty output per client and flush opportunistically in the main `select` loop (the main loop already handles `writefds`), so a slow client only loses its own data.

**Patch:** (design — non-trivial; sketch)
```text
- Maintain a per-client ring/queue of pending output bytes (cap e.g. 1 MB).
- pty_activity: append to each attached client's queue; never block on write.
- main loop: for clients with non-empty queues, set writefds; flush on
  writable; if a client's queue exceeds the cap, drop the client.
- Remove the goto-top re-select-until-delivered loop in pty_activity.
```

**Tests to add:**
- reliability/security: attach a reader that stops consuming (fill its socket buffer), keep emitting pty output, and assert (a) other clients keep receiving, (b) the child is not blocked indefinitely, (c) the slow client is dropped after the cap.

**Residual risk / follow-up:** Per-client queues add memory; the cap must be tuned. This is the same backpressure trade-off `tmux` solves with bounded client buffers — adopting that model is the durable fix.

**Claude Issue Verification:** PARTIALLY_VALID - Reconciled (was VALID; updated to agree with Codex after a fresh re-read). The core availability bug is real and stands: when the **sole** attached client stalls (or **all** attached clients stall), `pty_activity` reaches the `goto top` path with `nclients == 0` (master.c:462-463), the inner `select` (master.c:427) blocks with no timeout waiting for a stuck client to become writable or a new connection, the master never returns to read the pty, the pty buffer fills, and the hosted child blocks — a genuine MEDIUM DoS, trivially hit via Ctrl-S on a single-client attach (the common case for this dtach-style tool). However, the finding's headline framing and its two-user scenario are **inaccurate**: if a co-attached client is still draining, the write to it completes, `nclients > 0`, the `goto top` is not taken, the function returns, and the main loop keeps reading the pty — the stuck client merely loses that chunk rather than freezing everyone. So the precise condition is "sole/all attached clients stuck," not "any one client among several." Severity MEDIUM and the fix (per-client bounded queue + slow-client drop) are unchanged.
**Codex Issue Verification:** PARTIALLY_VALID - The availability bug is real for a sole attached client, or when all attached clients stop reading: `pty_activity` waits inside its inner `select` until a client socket is writable or a new connection arrives, so the master can stop reading the pty and stall the child. The report overstates the multi-client case: if at least one attached client remains writable, the loop writes to that client, returns, and the stuck client simply misses that chunk rather than freezing everyone.

**Agent A Reconciliation (1-2):** Final stance PARTIALLY_VALID — both agents now agree. The issue is a confirmed MEDIUM availability bug in the single-client / all-clients-stalled case; the multi-client "freezes the whole session for everyone" claim in the Problem/Impact/Scenario prose is an overstatement and should be read with the corrected condition above. Title retained for traceability but understood as the sole/all-clients case.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### 2026-06-25-1-4: Protocol read assumes one packet per `read()`; a short read disconnects a valid client

**Status:** OPEN
**Severity:** LOW
**Confidence:** Likely
**Pass:** Reliability (Pass 3)
**File:** [master.c:499-517](../../../master.c#L499-L517)

`client_activity` does a single `read(p->fd, &pkt, sizeof(struct packet))` and treats any result other than exactly `sizeof(struct packet)` (10 bytes) as a fatal error, closing and freeing the client (master.c:510-517). Stream (`SOCK_STREAM`) sockets do **not** guarantee that a full 10-byte packet arrives in one `read`; under fragmentation, signal interruption mid-read, or a sender that writes a partial packet, a legitimate client is silently dropped. Conversely two queued packets could be split mid-boundary. There is no length-prefard reassembly buffer.

**Category**
- OWASP: A04 Insecure Design (framing assumption on a byte stream)
- WSTG: WSTG-BUSL-09 / protocol robustness
- API Security: —
- NIST CSF 2.0: PR.IR-03, DE.CM-09

**Evidence:**
```c
// master.c:505-517
len = read(p->fd, &pkt, sizeof(struct packet));
if (len < 0 && (errno == EAGAIN || errno == EINTR)) return;
if (len != sizeof(struct packet)) {     /* any short read => disconnect */
    close(p->fd); ... free(p); return;
}
```

**Impact:** Spurious client disconnects (lost keystrokes / detach signals) under load or on slow links. In practice Unix-domain stream writes of 10 bytes are usually delivered atomically, so it rarely fires — hence Likely/LOW — but it is a latent correctness bug and a fragility that worsens under the very backpressure of 2026-06-25-1-2.

**Attack or failure scenario:** A client on a loaded system is interrupted by a signal after 6 of 10 bytes are read; the master sees `len == 6`, drops the client, and the user is unexpectedly detached.

**Recommendation:** Accumulate into a per-client buffer until a full `struct packet` is available, then dispatch; retain leftover bytes for the next read. Only close on `read` returning 0 (EOF) or a hard error.

**Patch:** (design)
```text
Add `unsigned char inbuf[sizeof(struct packet)]; size_t inlen;` to struct client.
On readable: read into inbuf+inlen; while (inlen >= sizeof(pkt)) { dispatch;
memmove remainder; }. Close only on EOF/hard error.
```

**Tests to add:**
- integration: feed packets split across two writes with a delay; assert the client stays connected and both packets are processed.

**Residual risk / follow-up:** Requires defining behavior for a peer that sends a partial packet and then stalls (idle timeout). Pairs naturally with the per-client buffering work in 2026-06-25-1-2.

**Claude Issue Verification:** VALID - Confirmed framing assumption at master.c:505-517; correct for the common case but not guaranteed for SOCK_STREAM. LOW given atomic small-write behavior in practice.
**Codex Issue Verification:** VALID - Confirmed: the master treats a stream socket as fixed-record transport and closes a client on any short read. The write side has the same fragility in `write_packet_or_fail`/`push_main`, so the issue is at least as broad as described.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### 2026-06-25-1-5: Signal handlers call async-signal-unsafe `printf`/`exit`/`system`

**Status:** OPEN
**Severity:** LOW
**Confidence:** Confirmed
**Pass:** Reliability (Pass 3)
**File:** [attach.c:153-166](../../../attach.c#L153-L166), [attach.c:85-95](../../../attach.c#L85-L95), [master.c:115-126](../../../master.c#L115-L126)

The attach-side signal handler `die` (installed for SIGHUP/SIGTERM/SIGINT/SIGQUIT, attach.c:350-353) calls `session_age` → `snprintf`, then `printf`, then `exit` (attach.c:155-165). `exit()` runs `atexit` handlers including `restore_term`, which calls `printf`, `fflush`, and even `system("tput init …")` (attach.c:86-95). None of `printf`/`exit`/`system`/`fflush` are async-signal-safe; calling them from a handler is undefined behavior (deadlock on the stdio lock if the signal interrupts a `printf`, heap corruption, or a re-entrant `system`/`fork`). `master_die` likewise calls `exit` (master.c:125).

**Category**
- OWASP: A04 Insecure Design (unsafe signal handling)
- WSTG: WSTG-BUSL / robustness
- API Security: —
- NIST CSF 2.0: PR.IR-03 (resilience), RS.MI-01

**Evidence:**
```c
// attach.c:155-165 — printf + exit from a signal handler
static RETSIGTYPE die(int sig) {
    char age[32]; session_age(age, sizeof(age));
    if (sig == SIGHUP || sig == SIGINT)
        printf("%s[%s: session '%s' detached after %s]\r\n", ...);
    ...
    exit(1);     /* -> atexit restore_term -> printf/fflush/system */
}
```

**Impact:** Rare but real hangs/crashes on signal delivery (e.g. SIGINT arriving while the main loop is inside `printf` flushing a large replay), and an unexpected `fork`+`system` from signal context. Reliability only. LOW.

**Attack or failure scenario:** A burst of output is being flushed when the user hits Ctrl-C; the signal interrupts `printf` holding the stdio lock, `die` re-enters `printf`, and the client deadlocks instead of detaching cleanly.

**Recommendation:** Make handlers minimal and async-signal-safe: set a `volatile sig_atomic_t` flag and handle exit/printing in the main loop, or restrict handlers to `write(2)` of a fixed message and `_exit`. Move terminal restoration out of the handler path or use `signalfd`/self-pipe.

**Patch:** (design)
```text
die(): set got_signal = sig; (no printf/exit). Main select loop checks the
flag, prints the detach/exit message with normal stdio, then exits. Replace
exit() in handlers with _exit() if an immediate stop is required.
```

**Tests to add:**
- reliability: deliver SIGINT repeatedly during a large replay and assert clean detach without hang (best-effort / stress).

**Residual risk / follow-up:** Hard to test deterministically; an ASan/TSan run and code-level guarantee (no stdio in handlers) is the practical bar.

**Claude Issue Verification:** VALID - Confirmed: die() (attach.c:153) and restore_term via atexit use printf/system; master_die uses exit. Standard async-signal-safety violation. LOW (rare).
**Codex Issue Verification:** VALID - Confirmed: attach-side signal handlers call `printf` and `exit`, and `exit` can run `restore_term` with `printf`/`fflush`/`system`; master-side handlers call `exit`, which can run `cleanup_session`. These are not async-signal-safe.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### Coverage and blind spots (Pass 3)
- **Reviewed:** the master `select` loop, `pty_activity` delivery loop, client lifecycle (accept/free), signal setup, `kill_main` grace/escalation timing, `tail -f` polling.
- **Not verified:** behavior under real backpressure and signal races at runtime (1-2/1-4/1-5 are code-derived). `tail -f`'s infinite `usleep` poll that never detects session end is noted but not separately filed (cosmetic).
- **Artifacts:** a load/backpressure harness; strace under Ctrl-S.

---

## Pass 4 — Test Coverage, Maintainability & Refactor Opportunities

### Executive summary
- The `tests/test.sh` integration suite is solid for CLI surface (help/version, start/list/kill/clear/current/push/tail/rm, legacy flags, env-var derivation) but has **zero security or concurrency tests** and no CI/SAST wiring — false confidence on exactly the areas this review flags.

### Prioritized findings

---

### 2026-06-25-1-9: No security/concurrency tests, no CI or SAST in-repo; security-relevant paths are untested

**Status:** OPEN
**Severity:** LOW
**Confidence:** Confirmed
**Pass:** Test Coverage (Pass 4)
**File:** [tests/test.sh:1-823](../../../tests/test.sh#L1-L823), [makefile:1-60](../../../makefile#L1-L60)

`tests/test.sh` (823 lines, single-process, sequential) covers the command surface and several edge cases (already-running, bad command, nested env chains, exited-session listing). Gaps relevant to this review: (1) **no permission/ownership assertions** — nothing checks the socket is `0600`, the dir `0700`, or that the log is not world-readable; (2) **no symlink/TOCTOU test** for the `/tmp` fallback (1-1); (3) **no FD-leak test** (1-6); (4) **no multi-client / backpressure / slow-reader test** (1-2, 1-4, 1-8) — every test attaches at most implicitly; (5) **no signal-race test** (1-5); (6) no CI workflow (`.github/`) and no Semgrep/ASan/UBSan in the build, so regressions in any of the above ship silently. `grep` for `symlink|perm|0600|cloexec|race` in the suite returns nothing security-relevant.

**Category**
- OWASP: A04 Insecure Design (verification gap) / A09-adjacent (no security testing)
- WSTG: WSTG-CONF-09 (no permission tests), test-coverage hygiene
- API Security: —
- NIST CSF 2.0: GV.SC-09 (assurance), DE.CM-09 (mechanisms tested), ID.RA-01

**Evidence:**
```text
tests/test.sh: 0 hits for symlink/nofollow/cloexec/0600/0700/race/inject
makefile: no CI target, no semgrep/asan/ubsan; `test` target only runs test.sh
repo: no .github/workflows, no CHANGELOG (so WONTFIX/ONHOLD history is unrecorded)
```

**Impact:** The control families most at risk (file permissions, fd hygiene, backpressure, signal safety) have no automated guard, so the fixes for 1-1/1-2/1-6 could regress undetected. Maintainability: there is also no `CHANGELOG.md`, which this review process references for WONTFIX/ONHOLD tracking. LOW (process/assurance).

**Attack or failure scenario:** A future refactor drops the `chmod(name, 0600)` in `create_socket` or the dir-creation `0700`; no test fails, and sessions become group/other-accessible on a host with a permissive umask.

**Recommendation:** Add a `security` test group asserting socket/dir/log modes, a symlink-refusal test for the fallback path, an FD-leak test (`ls /proc/self/fd` inside a session), and a slow-reader/multi-client test. Add a CI workflow that builds and runs `test.sh` on Linux (and macOS), runs Semgrep, and builds with `-fsanitize=address,undefined` for the test run. Add `CHANGELOG.md` to record fix/WONTFIX/ONHOLD decisions.

**Patch:** (test additions — see "Tests to add" across findings)
```text
tests/test.sh: + group "security": stat -c %a on socket==600, dir==700, log==600;
plant symlink in fallback dir and assert refusal; child-fd-leak assertion.
.github/workflows/ci.yml: build (gcc, musl-static), run test.sh, run semgrep,
asan/ubsan test build.
```

**Tests to add:**
- security: permission assertions (modes), symlink refusal (1-1), fd leak (1-6).
- integration: multi-client + slow reader (1-2/1-4), mid-replay overwrite (1-8).
- CI: Semgrep + ASan/UBSan gates.

**Residual risk / follow-up:** Some tests (signal races, backpressure) are inherently flaky; mark them best-effort and run under sanitizers where possible.

**Claude Issue Verification:** VALID - Confirmed by scanning tests/test.sh (no security/concurrency assertions) and makefile/repo (no CI/SAST, no CHANGELOG). Process/assurance gap. LOW.
**Codex Issue Verification:** VALID - Confirmed by reading `tests/test.sh`, `makefile`, and repo layout: the integration suite covers CLI behavior but lacks permission, symlink, fd-leak, multi-client/backpressure, signal-race, and sanitizer/CI coverage.

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

### Coverage and blind spots (Pass 4)
- **Reviewed:** full `tests/test.sh`, `makefile` targets, repo layout for CI/SAST/changelog.
- **Not verified:** test behavior on macOS; whether release CI exists outside the repo (e.g. in GitHub Actions not committed here).
- **Artifacts:** the actual CI configuration (if any) and coverage reports would refine this.

---

## Pass 5 — Compliance Mapping & Missing Controls

### Executive summary
- Weakest control families: **PR.DS / PR.PS (data & platform integrity)** — symlink-followable log creation, leaked log fd, mutable-during-replay ring; and **PR.IR (resilience)** — single-slow-client DoS, unsafe signal handling, fragile framing.
- Authentication/authorization is **filesystem-only** by design (`0600`/`0700`); acceptable within a uid but undocumented as the trust boundary and not enforced via peer-cred.
- No detection/logging of misuse (DE.* essentially absent) and no test/CI assurance for security controls.

### Coverage by NIST CSF 2.0

| Outcome | Status | Related findings |
|---|---|---|
| **GV.PO / GV.SC-09** Policy & assurance | weak | 2026-06-25-1-9 |
| **ID.RA-01** Vulnerability identification | partial | this review; no SAST in CI (1-9) |
| **PR.AA-01/05** Identity & access enforcement | partial | 2026-06-25-1-1, 2026-06-25-1-3 |
| **PR.DS-01/02** Data integrity (at rest / in transit-display) | weak | 2026-06-25-1-1, 2026-06-25-1-6, 2026-06-25-1-8, 2026-06-25-1-10 |
| **PR.PS-01/06** Platform/config hygiene | weak | 2026-06-25-1-6, 2026-06-25-1-7 |
| **PR.IR-03** Resilience under load | weak | 2026-06-25-1-2, 2026-06-25-1-4, 2026-06-25-1-5, 2026-06-25-1-11 |
| **DE.CM-01/09** Monitoring of misuse | failing | 2026-06-25-1-11 (no cap/audit of control-channel connections); otherwise none |
| **RS.MI-01/02** Mitigation | partial | 2026-06-25-1-2, 2026-06-25-1-5, 2026-06-25-1-11 |

### OWASP coverage

| OWASP Top 10 (2021) | Findings |
|---|---|
| A01 Broken Access Control | 2026-06-25-1-1, 2026-06-25-1-3 |
| A02 Cryptographic Failures | — (no crypto; N/A) |
| A03 Injection | 2026-06-25-1-10 (terminal-escape) |
| A04 Insecure Design | 2026-06-25-1-1, 2026-06-25-1-2, 2026-06-25-1-3, 2026-06-25-1-4, 2026-06-25-1-5, 2026-06-25-1-7, 2026-06-25-1-8, 2026-06-25-1-11 |
| A05 Security Misconfiguration | 2026-06-25-1-6, 2026-06-25-1-7 |
| A06 Vulnerable Components | — (no third-party deps; static musl) |
| A07 Auth Failures | 2026-06-25-1-3 (local IPC analog) |
| A08 Software/Data Integrity | 2026-06-25-1-1, 2026-06-25-1-6, 2026-06-25-1-8 |
| A09 Logging/Monitoring Failures | 2026-06-25-1-9 (no security tests/CI) |
| A10 SSRF | — (N/A) |

| OWASP API Top 10 (2023) | Findings |
|---|---|
| API2 Broken Authentication (local IPC analog) | 2026-06-25-1-3 |
| API4 Unrestricted Resource Consumption | 2026-06-25-1-2, 2026-06-25-1-11 |
| API8 Security Misconfiguration | 2026-06-25-1-6 |

### WSTG coverage

| WSTG family | Findings |
|---|---|
| ATHN | 2026-06-25-1-3 |
| ATHZ | 2026-06-25-1-1, 2026-06-25-1-3 |
| SESS | 2026-06-25-1-3 |
| INPV | 2026-06-25-1-7, 2026-06-25-1-10 |
| CRYP | — (N/A) |
| CONF | 2026-06-25-1-1, 2026-06-25-1-6, 2026-06-25-1-9 |
| BUSL | 2026-06-25-1-2, 2026-06-25-1-4, 2026-06-25-1-8, 2026-06-25-1-11 |
| CLNT | 2026-06-25-1-10 |

### Missing controls — recommended additions
1. Safe per-user runtime directory selection (`$XDG_RUNTIME_DIR`) + verified `0700` ownership and `O_NOFOLLOW`/`openat` on log/socket — closes 1-1 (and the entry vector for 1-10).
2. Per-client bounded output queue + slow-client drop, a hard `MAX_CLIENTS` cap, and migration from `select`/`fd_set` to `poll`/`epoll` — closes 1-2 and 1-11 and hardens 1-4.
3. `FD_CLOEXEC`/`O_CLOEXEC` on all long-lived fds — closes 1-6.
4. Async-signal-safe handlers (flag + main-loop handling) — closes 1-5.
5. Overflow-checked size parsing and centralized, truncation-checked path building — closes 1-7.
6. Per-client scrollback snapshot on replay — closes 1-8.
7. Security/concurrency test group + CI with Semgrep/ASan/UBSan + `CHANGELOG.md` — closes 1-9 and prevents regressions of all the above.
8. (defense-in-depth) `SO_PEERCRED` uid check on `accept` — hardens 1-3.

---

## Final Consolidated Output

### Top issues across all passes (11)

| Rank | ID | Severity | Title |
|---|---|---|---|
| 1 | 2026-06-25-1-1 | MEDIUM | Predictable `/tmp` fallback + symlink-following log/socket creation |
| 2 | 2026-06-25-1-11 | MEDIUM | Unbounded clients drive `FD_SET` past `FD_SETSIZE` → master crash/corruption |
| 3 | 2026-06-25-1-2 | MEDIUM | Sole/all-clients-stalled freezes the session and stalls the child (DoS) |
| 4 | 2026-06-25-1-6 | LOW | Session log fd leaked to executed program (no `FD_CLOEXEC`) |
| 5 | 2026-06-25-1-3 | LOW | Control socket has no message origin check (same-uid injection) |
| 6 | 2026-06-25-1-8 | LOW | Scrollback ring overwritten mid-replay → corrupted history |
| 7 | 2026-06-25-1-4 | LOW | Short read disconnects a valid client (framing assumption) |
| 8 | 2026-06-25-1-5 | LOW | Async-signal-unsafe `printf`/`exit`/`system` in handlers |
| 9 | 2026-06-25-1-7 | LOW | `parse_size` integer overflow + unchecked malloc/path truncation |
| 10 | 2026-06-25-1-10 | LOW | Verbatim log replay = terminal-escape injection vector |
| 11 | 2026-06-25-1-9 | LOW | No security/concurrency tests, no CI/SAST, no CHANGELOG |

### Patch plan

**Immediate (today):**
- 2026-06-25-1-6: add `O_CLOEXEC` to `open_log` (one line, no downside).
- 2026-06-25-1-7: overflow-check `parse_size`, check the `expand_sockname` malloc.
- 2026-06-25-1-11: reject `fd >= FD_SETSIZE` immediately after `accept` (and `close` it) as a stop-gap against the out-of-bounds `FD_SET`.

**Near-term (this sprint):**
- 2026-06-25-1-1: `$XDG_RUNTIME_DIR` + verified `0700` dir + `O_NOFOLLOW` on log.
- 2026-06-25-1-2 / 1-4 / 1-11: per-client bounded output queue with slow-client drop and read reassembly, plus a hard `MAX_CLIENTS` cap and idle-connection timeout — and migrate the `select`/`fd_set` loops to `poll`/`ppoll`/`epoll` so descriptor values are no longer bounded by `FD_SETSIZE` (durable fix for 1-11, and the natural home for the 1-2 per-client queues).
- 2026-06-25-1-5: move printing/exit out of signal handlers.
- 2026-06-25-1-9: security test group + CI (Semgrep/ASan/UBSan) + CHANGELOG.

**Structural (later):**
- 2026-06-25-1-8: per-client scrollback snapshot on replay.
- 2026-06-25-1-3: `SO_PEERCRED` uid check (defense-in-depth) + README trust-boundary doc.
- 2026-06-25-1-10: opt-in sanitized cold-replay mode + README residual-risk note.

### Test plan

**Existing tests to update:**
- `tests/test.sh` — add permission assertions (socket `0600`, dir `0700`, log `0600`).

**New tests to add first:**
- symlink-refusal in fallback dir (1-1); child fd-leak check (1-6); overflow `parse_size` unit (1-7); slow-reader/multi-client (1-2/1-4); mid-replay overwrite (1-8); client-flood / `FD_SETSIZE` test (1-11) — open more than `FD_SETSIZE` connections and assert the master refuses the excess and stays alive (run under ASan/UBSan).

**Security regression tests to automate in CI:**
- Semgrep (the C ruleset that produced the two leads), ASan + UBSan build for the test run, and a permission/symlink smoke test.

### Control matrix

| Finding | Severity | File/Area | OWASP | WSTG | API | NIST CSF 2.0 | Patch status |
|---|---|---|---|---|---|---|---|
| 2026-06-25-1-1 | MEDIUM | atch.c get_session_dir / master.c open_log | A01/A04 | CONF-09 | — | PR.AA-05, PR.DS-01 | proposed |
| 2026-06-25-1-2 | MEDIUM | master.c pty_activity | A04 | BUSL-03 | API4 | PR.IR-03 | proposed |
| 2026-06-25-1-11 | MEDIUM | master.c control_activity / FD_SET loops | A04 | BUSL-03 | API4 | PR.IR-03, DE.CM-09, RS.MI-02 | proposed |
| 2026-06-25-1-3 | LOW | master.c client_activity | A01/A07 | ATHZ-02 | API2 | PR.AA-05 | proposed |
| 2026-06-25-1-4 | LOW | master.c client_activity | A04 | BUSL-09 | — | PR.IR-03 | proposed |
| 2026-06-25-1-5 | LOW | attach.c die / master.c master_die | A04 | BUSL | — | PR.IR-03 | proposed |
| 2026-06-25-1-6 | LOW | master.c open_log | A05/A08 | CONF-09 | API8 | PR.PS-01 | proposed |
| 2026-06-25-1-7 | LOW | atch.c parse_size / expand_sockname | A04/A05 | INPV-11 | — | PR.PS-01 | proposed |
| 2026-06-25-1-8 | LOW | master.c scrollback replay | A04/A08 | BUSL-07 | — | PR.DS-01 | proposed |
| 2026-06-25-1-10 | LOW | attach.c/atch.c log replay | A03 | CLNT-04 | — | PR.DS-02 | proposed |
| 2026-06-25-1-9 | LOW | tests/test.sh, makefile | A04/A09 | CONF-09 | — | GV.SC-09, DE.CM-09 | proposed |

---

## Summary by Priority

### Critical (Fix Before Release)
- None.

### High (Fix This Sprint)
- None.

### Medium (Should Fix)
1. **2026-06-25-1-1**: Predictable `/tmp` fallback + symlink-following log/socket creation.
2. **2026-06-25-1-11**: Unbounded clients drive `FD_SET` past `FD_SETSIZE` → master crash/corruption.
3. **2026-06-25-1-2**: Sole/all-clients-stalled freezes the session and stalls the child (DoS).

### Low (Nice to Have)
4. **2026-06-25-1-3**: Control socket has no message origin check (same-uid injection).
5. **2026-06-25-1-4**: Short read disconnects a valid client.
6. **2026-06-25-1-5**: Async-signal-unsafe `printf`/`exit`/`system` in signal handlers.
7. **2026-06-25-1-6**: Session log fd leaked to executed program (no `FD_CLOEXEC`).
8. **2026-06-25-1-7**: `parse_size` integer overflow + unchecked malloc/path truncation.
9. **2026-06-25-1-8**: Scrollback ring overwritten mid-replay → corrupted history.
10. **2026-06-25-1-9**: No security/concurrency tests, no CI/SAST, no CHANGELOG.
11. **2026-06-25-1-10**: Verbatim log replay is a terminal-escape injection vector.

---

## Previously Marked WONTFIX/ONHOLD (Unchanged)

No `CHANGELOG.md` exists and no prior review or WONTFIX/ONHOLD records were found in the repository, so there are no previously deferred issues to carry forward.

**WONTFIX:**
- None.

**ONHOLD:**
- None.

---

## Items the agents could NOT verify (would improve confidence)
- Real multi-user-host exploitation of 2026-06-25-1-1 (reasoned from code; not executed in a shared-`/tmp` environment with `$HOME` unset).
- Runtime backpressure/signal-race behavior for 2026-06-25-1-2 / 1-4 / 1-5 (derived from the `select` loop logic; no load harness was run).
- Live reproduction of the `FD_SETSIZE` overflow in 2026-06-25-1-11 (reasoned from code; not crashed in a process with `RLIMIT_NOFILE > FD_SETSIZE`). The defect is unconditional in code; only the *crash trigger* depends on the master's soft fd limit exceeding `FD_SETSIZE`.
- macOS (`HAVE_UTIL_H`) build/behavior, including whether the bundled `openpty`/`forkpty` is compiled there.
- Terminal-emulator-specific impact of 2026-06-25-1-10 (varies by terminal).
- Whether an out-of-repo CI/SAST pipeline already exists (none is committed).

---

## Appendix: Files Touched by Findings

| File | Findings |
|------|----------|
| atch.c | 2026-06-25-1-1, 2026-06-25-1-7 |
| master.c | 2026-06-25-1-1, 2026-06-25-1-2, 2026-06-25-1-3, 2026-06-25-1-4, 2026-06-25-1-6, 2026-06-25-1-8, 2026-06-25-1-11 |
| attach.c | 2026-06-25-1-5, 2026-06-25-1-10 |
| tests/test.sh | 2026-06-25-1-9 |
| makefile | 2026-06-25-1-9 |

---

## Agent B (Codex) Cross-Verification and Independent Review Addendum

### Cross-verification summary
- Codex verified Agent A findings inline in each finding's `Codex Issue Verification` field.
- Codex marked 9 Agent A findings VALID and 1 finding PARTIALLY_VALID.
- `2026-06-25-1-2` is PARTIALLY_VALID because the slow-client stall is real for a sole attached client or when all attached clients are backpressured, but the report overstates the "one client freezes every multi-client session" case: if another attached client is writable, `pty_activity` writes to that client and returns, while the stuck client misses the chunk.
- Codex independently re-triaged both Semgrep leads and agrees with Agent A's dismissals: `atch.c:25` is bounded by construction, and `master.c:833` is unreachable in the supported Linux build and receives `name == NULL` from the local callers.

### Codex 5-pass independent review results
| Pass | Result |
|---|---|
| Pass 1 — Attack Surface & High-Risk Security Findings | No additional high-risk trust-boundary issue beyond Agent A's socket/log/path and same-uid IPC findings. Explicit `/` paths in shared directories share the same log-symlink weakness described in 1-1 and should be fixed by the same `openat`/`O_NOFOLLOW`/directory validation work. |
| Pass 2 — Logic Correctness, Abuse Cases & Edge Conditions | No new standalone finding beyond Agent A's numeric/path truncation and replay-integrity issues. Codex noted that colon characters in custom socket paths can confuse the colon-separated `ATCH_SESSION` ancestry chain, but this is a low-value self-attach/current-display correctness edge case rather than a security finding. |
| Pass 3 — Reliability, Resilience & Operational Safety | Added 2026-06-25-1-11 for unbounded accepted clients feeding `FD_SET` without an `FD_SETSIZE` guard, creating local same-uid memory corruption/crash risk in the master. |
| Pass 4 — Test Coverage, Maintainability & Refactor Opportunities | No additional process finding beyond 1-9. Tests should specifically cover the new `FD_SETSIZE` client-flood case and enforce a max-client/drop policy. |
| Pass 5 — Compliance Mapping & Missing Controls | Add 2026-06-25-1-11 to OWASP A04 Insecure Design / API4 Unrestricted Resource Consumption analog / WSTG-BUSL DoS / NIST PR.IR-03 and DE.CM-09. |

### 2026-06-25-1-11: Unbounded client connections can drive `FD_SET` past `FD_SETSIZE` and crash/corrupt the master

**Status:** OPEN
**Severity:** MEDIUM
**Confidence:** Confirmed
**Pass:** Reliability (Pass 3)
**File:** [master.c:472](../../../master.c#L472), [master.c:654](../../../master.c#L654), [master.c:417](../../../master.c#L417)

The master accepts every connection that can reach the Unix socket, allocates a `struct client`, and stores the accepted fd without any maximum-client limit or check that the fd is lower than `FD_SETSIZE`. Later, both the main loop and `pty_activity` pass every client fd to `FD_SET`. On systems using the traditional `select(2)` API, `FD_SET(fd, ...)` is only valid for `0 <= fd < FD_SETSIZE`; higher values are undefined behavior and commonly write past the end of the stack `fd_set`. A same-uid process can open many connections to a session socket and force the master to accept fd numbers above `FD_SETSIZE`, causing memory corruption or a deterministic crash of the session master.

**Category**
- OWASP: A04 Insecure Design / availability and unsafe resource handling
- WSTG: WSTG-BUSL-03 (resource exhaustion / DoS)
- API Security: API4 Unrestricted Resource Consumption (local IPC analog)
- NIST CSF 2.0: PR.IR-03, DE.CM-09, RS.MI-02

**Evidence:**
```c
// master.c:472-495 — no max-client limit and no fd >= FD_SETSIZE rejection
fd = accept(s, NULL, NULL);
...
p = malloc(sizeof(struct client));
...
p->fd = fd;
...
*(p->pprev) = p;

// master.c:654-666 — accepted fd is blindly placed into fd_set
for (p = clients; p; p = p->next) {
    FD_SET(p->fd, &readfds);
    if (p->fd > highest_fd)
        highest_fd = p->fd;
    ...
}

// master.c:417-423 — pty_activity repeats the same unchecked FD_SET pattern
for (p = clients, nclients = 0; p; p = p->next) {
    if (!p->attached)
        continue;
    FD_SET(p->fd, &writefds);
    ...
}
```

**Impact:** A same-uid local process can crash or corrupt the master process for any session it can connect to. This can terminate or wedge the hosted pty, lose live session availability, and undermine recovery/history expectations. The scope is same-uid because the socket permissions are the main boundary, so this is MEDIUM rather than HIGH, but it is a concrete memory-safety bug in a daemon-like process.

**Attack or failure scenario:** A malicious or buggy same-uid helper repeatedly opens Unix socket connections to `~/.cache/atch/work` and keeps them open. Once the accepted descriptor number in the master exceeds `FD_SETSIZE - 1` (often 1023), the next main-loop iteration executes `FD_SET(p->fd, &readfds)` out of bounds and the master crashes or corrupts stack state.

**Recommendation:** Prefer replacing `select`/`fd_set` with `poll`, `ppoll`, or `epoll` so descriptor values are not constrained by `FD_SETSIZE`. As an immediate mitigation, enforce a hard `MAX_CLIENTS`, reject `fd >= FD_SETSIZE` immediately after `accept`, and add idle/client lifetime limits so unattached connections cannot accumulate indefinitely.

**Patch:**
```diff
--- a/master.c
+++ b/master.c
@@ control_activity
 	fd = accept(s, NULL, NULL);
 	if (fd < 0)
 		return;
+	if (fd >= FD_SETSIZE) {
+		close(fd);
+		return;
+	}
 	else if (setnonblocking(fd) < 0) {
 		close(fd);
 		return;
 	}
```
```text
Structural fix: replace all fd_set/select loops with poll/ppoll and maintain a bounded client count; drop or refuse clients once the configured cap is reached.
```

**Tests to add:**
- security/reliability: start a session, open more than `FD_SETSIZE` client connections from a helper, and assert the master refuses excess connections without crashing and the child pty remains usable.
- unit/static: if `select` remains, assert `control_activity` closes accepted fds `>= FD_SETSIZE` and enforces a `MAX_CLIENTS` cap.
- CI: run the client-flood test under ASan/UBSan so any future out-of-bounds `FD_SET` regression is caught.

**Residual risk / follow-up:** A simple `fd >= FD_SETSIZE` guard prevents memory corruption but does not solve resource exhaustion below the fd limit. The durable fix is a bounded client model plus `poll`/`epoll`, ideally implemented together with the per-client output queues proposed for 2026-06-25-1-2.

**Claude Issue Verification:** VALID - Confirmed by reading `control_activity` (master.c:473, no `MAX_CLIENTS` cap and no `fd >= FD_SETSIZE` rejection after `accept`), the main loop (master.c:654-657, `FD_SET(p->fd, &readfds)` for every client), and `pty_activity` (master.c:417-422, same pattern for `writefds`). Idle accepted clients are never pruned (a client is removed only on read EOF/hard error or write error), so a same-uid process can hold open many connections and accumulate high fd numbers; once an accepted fd reaches `FD_SETSIZE` (typically 1023), `FD_SET` writes out of bounds — UB, in practice stack corruption/crash. One nuance affecting reachability (not validity): the master can only be handed an fd `>= FD_SETSIZE` if its own `RLIMIT_NOFILE` soft limit exceeds `FD_SETSIZE`; on a default 1024-fd limit `accept` fails with `EMFILE` before crossing the boundary, but on modern systemd/container defaults (where `RLIMIT_NOFILE` is commonly 524288+) the boundary is reachable and the crash is deterministic. Severity MEDIUM and Confidence Confirmed are appropriate (same-uid memory-safety/DoS in the long-lived master).

**Claude Fix Verification:** PENDING
**Codex Fix Verification:** PENDING

---

## Agent A Reconciliation Pass — Issue Verification Disagreements (2026-06-25)

Agent A re-examined every finding whose two Issue Verification fields are not identical, re-reading the cited source fresh and steelmanning Agent B before re-affirming, per Step 2.6 of the prompt. Stance is neutral: original Agent A verdicts get no extra weight.

Sweep result:
- **No INVALID verdicts exist** anywhere in the report, from either agent.
- **The only finding whose statuses ever differed is 2026-06-25-1-2** (Codex PARTIALLY_VALID vs Claude's original VALID). It is the sole disagreement and has already been reconciled to a common **PARTIALLY_VALID** (see the inline `Agent A Reconciliation (1-2)` block and both Issue Verification fields).
- **The other 10 findings (1-1, 1-3, 1-4, 1-5, 1-6, 1-7, 1-8, 1-9, 1-10, 1-11) are VALID / VALID** — no disagreement, nothing to reconcile.

**2026-06-25-1-2 — re-verified against source this pass (final stance: PARTIALLY_VALID, unchanged).** Re-reading `pty_activity` at master.c:368-464 confirms the reconciled position rather than either agent's original framing:
- The post-write counter `nclients` (master.c:449-450) is incremented only when a client receives the **full** chunk; a back-pressured client returns `EAGAIN` (master.c:451) and is neither counted nor dropped.
- The re-block guard is `if (!FD_ISSET(s, &readfds) && nclients == 0) goto top;` (master.c:462-463), and the inner `select` (master.c:427) is called with a `NULL` (infinite) timeout.
- **Sole attached client, or all attached clients, stalled:** `nclients == 0`, no new connection arrives, so the function loops back to a `NULL`-timeout `select` that waits for the stuck client to become writable. The master never returns to read the pty, the pty fills, and the hosted child blocks — a real MEDIUM availability bug, trivially hit by Ctrl-S on the common single-client attach.
- **Multiple attached clients with at least one still draining:** the draining client takes the full write, `nclients >= 1`, the `goto top` is not taken, the function returns, and the main loop keeps reading the pty. The stuck client only loses that chunk; the session is not frozen.

The original Problem/Impact/Scenario prose ("one client freezes the whole session" / the two-user deploy story) overstates the multi-client case, which is exactly why the verdict is PARTIALLY_VALID rather than VALID. The underlying single-client/all-stalled DoS, the MEDIUM severity, the Confirmed confidence, and the per-client-bounded-queue + slow-client-drop fix are all correct and unchanged. Both agents agree.

**Outcome:** No unresolved Issue Verification disagreements remain after this reconciliation pass. The reconciled set stands as recorded in the Executive Summary (10× VALID/VALID, 1× PARTIALLY_VALID/PARTIALLY_VALID). No Top 10 / Patch plan / Test plan / Control matrix updates are required, because no severity, confidence, or OWASP/WSTG/API/NIST mapping changed during reconciliation.
