# atch

`atch` is a small C utility that lets you attach and detach terminal sessions,
similar to the detach feature of `screen/tmux` — but without the terminal emulation,
multiple windows, or other overhead.

**The key property of `atch` is transparency.** It does not interpose a
terminal emulator between you and your program. The raw byte stream flows
directly from the pty to your terminal, exactly as if you had run the program
in a plain shell. This means:

- **Mouse works.** Mouse reporting, click events, and drag sequences pass
  through unmodified. No escape-sequence re-encoding, no `set -g mouse on`,
  no fighting with terminfo databases.
- **Scroll works.** Programs that use the alternate screen buffer, or that
  emit their own scroll sequences, behave identically inside and outside an
  `atch` session. There is nothing to configure.
- **Colors and graphics work.** True-color, sixel, kitty graphics, OSC
  sequences — all pass through untouched.
- **`$TERM` is unchanged.** `atch` does not set or override your terminal
  type. The program sees exactly the same `$TERM` your shell uses.

In contrast, `tmux` and `screen` implement their own terminal emulators.
They re-encode the output stream, which frequently breaks mouse support,
scroll behavior, and newer terminal features unless you find and apply the
right obscure configuration knob — and then remember it on every new machine.
With `atch` there is nothing to remember, because there is nothing in the way.

When a program runs inside an `atch` session it is protected from the
controlling terminal. You can detach from it, disconnect, and re-attach later
from the same or a different terminal, and the program keeps running
undisturbed.

**Session history survives everything.** Every byte written to the terminal
is appended to a persistent log file on disk. When you re-attach — whether
the session is still running, crashed, or you have rebooted the machine — the
full output history is replayed to your terminal first, so you can see exactly
what happened and pick up right where you left off. No plugins, no
configuration, no manual `script` wrappers. Other session managers keep
history only in memory: when the process dies or the machine reboots, the
output is gone. With `atch` it is on disk until you clear it.

## Features

- Attach and detach from running programs
- Multiple clients can attach to the same session simultaneously
- **No terminal emulation** — raw output stream is passed through unchanged
- Sessions persist across disconnects, crashes, and reboots
- **Full session history on disk** — every line ever written is saved and replayed on re-attach
- **History survives process exit** — re-opening a session shows the complete prior output before starting fresh
- Push stdin directly to a running session
- List sessions with liveness status; `list -a` also shows exited sessions that still have a log on disk
- **One-command cleanup** — `rm <session>` or `rm -a` removes stale/exited sessions and their logs
- Prevents accidental recursive self-attach
- Tiny and auditable

## Installation

The easiest way to get `atch` is to download a pre-built binary from the
[GitHub releases page](https://github.com/mobydeck/atch/releases). The release
binaries are statically linked against musl libc — they run on any Linux
distribution with no dependencies. Just download and extract:

```sh
arch=$(uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/')
curl -Lo atch.tgz https://github.com/mobydeck/atch/releases/latest/download/atch-linux-${arch}.tgz
tar -xzf atch.tgz atch
sudo mv atch /usr/local/bin/
rm -f atch.tgz
```

Or download the `.tgz` from the releases page and extract it manually.

## Why C and a static binary?

`atch` sits between your keyboard and your running program at the lowest
level of the Unix process model: it opens pseudo-terminals, manages Unix
domain sockets, and forwards raw byte streams. These are system-level
operations that map directly to C system calls. There is no meaningful
runtime overhead to hide, no framework to bring in, and no interpreter to
start — the binary does exactly what it advertises and nothing else.

The release binaries are statically linked against
[musl libc](https://musl.libc.org/). A static musl binary is a
**single self-contained file** that carries everything it needs. You copy
it to any Linux machine — Ubuntu, Debian, Alpine, RHEL, a bare container
image, a two-year-old distro — and it runs. There are no shared library
version mismatches, no `apt install`, no `LD_LIBRARY_PATH` gymnastics. For
a small tool you want to drop onto servers and forget about, a static binary
is the right default.

## Building from source

```sh
make
```

Build just the internal static library artifact:

```sh
make libatch.a
```

Note: `libatch.a` is currently an internal build artifact used to modularize
`atch` internals. It is not yet a documented/stable public API.

## Usage

```
atch [<session> [command...]]   Attach to session or create it (default)
atch <command> [options] ...
```

Sessions are identified by name. A bare name (no `/`) is stored as a socket
under `~/.cache/atch/`. A name containing `/` is used as-is as a filesystem path.

If no command is given, `$SHELL` is used.

## Commands

| Command | Description |
|---------|-------------|
| `atch [<session> [cmd...]]` | Attach to a session, or create it if it doesn't exist (default behavior). Prints a confirmation when a new session is created. |
| `attach <session>` | Strict attach — fail if the session does not exist. |
| `new <session> [cmd...]` | Create a new session and attach to it. Prints a confirmation before attaching. |
| `start <session> [cmd...]` | Create a new session, detached (atch exits immediately). Prints a confirmation on success. |
| `run <session> [cmd...]` | Like `start`, but atch stays in the foreground instead of daemonizing. |
| `push <session>` | Copy stdin verbatim to the session. |
| `kill [-f] <session>` | Gracefully stop a session (SIGTERM, then SIGKILL after 5 s if needed). With `-f` / `--force`, skip the grace period and send SIGKILL immediately. |
| `clear [<session>]` | Truncate the on-disk session log. Defaults to the current session when run inside one. |
| `tail [-f] [-n N] <session>` | Print the last N lines of the session log (default: 10). Works for both running and exited sessions. With `-f`, follow new output as it is written (useful for monitoring a running session without attaching). |
| `list [-a]` | List sessions. Shows `[attached]` when a client is connected, `[stale]` for leftover sockets with no running master. With `-a`, also shows `[exited]` sessions that have a log file but are no longer running. Prints `(no sessions)` when the list is empty. |
| `rm [-a] [<session>]` | Remove a session and its log. Refuses if the session is currently running — use `kill` first. Works on stale (socket exists but process is dead) and exited (log only) sessions. With `-a`, sweeps all stale and exited sessions at once. |
| `current` | Print the current session name and exit 0 if inside a session; exit 1 silently if not. |

Short aliases: `a` → `attach`, `n` → `new`, `s` → `start`, `p` → `push`,
`k` → `kill`, `l` / `ls` → `list`.

## Options

Options can appear before the subcommand, before the session name, or after the session name.

| Flag | Description |
|------|-------------|
| `-e <char>` | Set the detach character. Accepts `^X` notation. Default: `^\`. |
| `-E` | Disable the detach character entirely. |
| `-r <method>` | Redraw method on attach: `none`, `ctrl_l`, or `winch` (default). |
| `-R <method>` | Clear method on attach: `none` or `move`. |
| `-z` | Disable suspend-key (`^Z`) processing (pass it to the program instead). |
| `-q` | Suppress informational messages. |
| `-t` | Disable VT100 assumptions. |
| `-C <size>` | Set the on-disk log cap for the session being created. Accepts a bare number (bytes), or a number with `k`/`K` (KiB) or `m`/`M` (MiB) suffix. `0` disables the log entirely. Default: `1m`. |

Use `--` to separate atch options from command arguments that start with `-`:

```sh
atch new mysession -- grep -r foo /var/log
```

## Examples

**Start a shell session named `work` and attach to it:**
```sh
atch work
```

**Start a specific command in a named session:**
```sh
atch new build -- make -j4
```

**Attach to an existing session, creating it if needed:**
```sh
atch work
```

**Strict attach — fail if the session is not running:**
```sh
atch attach work
```

**Detach** from a running session: press `^\` (Ctrl-\\). The session and its
program keep running.

**Re-attach** later:
```sh
atch work
```

**Run a command fully detached (no terminal needed):**
```sh
atch start daemon myserver
# atch: session 'daemon' started
```

Use `-q` to suppress confirmation messages in scripts:
```sh
atch start -q daemon myserver
```

**Send keystrokes to a running session:**
```sh
printf 'ls -la\n' | atch push work
```

**Use a custom detach character:**
```sh
atch -e '^A' attach work
```

**List all sessions:**
```sh
atch list
```

**List sessions including those that have exited but still have a log:**
```sh
atch list -a
```

**Inspect the last 20 lines of a session log:**
```sh
atch tail -n 20 work
```

**Follow a session's output without attaching:**
```sh
atch tail -f work
```

**Kill a session:**
```sh
atch kill work
```

**Remove a stale or exited session (socket + log):**
```sh
atch rm crashed-job
```

**Sweep all stale and exited sessions in one go:**
```sh
atch rm -a
```

## Session storage

By default, session sockets are stored in `~/.cache/atch/`. The directory is
created automatically, including `~/.cache` if it does not yet exist.

When `$HOME` is unset or empty, `atch` looks up the home directory from the
system user database (`/etc/passwd`). If that also yields nothing useful (or
points to `/`), sockets fall back to `/tmp/.atch-<uid>/`.

To use a custom path, include a `/` in the session name:

```sh
atch new /tmp/mysession
```

`atch` sets the `ATCH_SESSION` environment variable inside each session to a
colon-separated ancestry chain of socket paths, outermost first. A
non-nested session has a single path; nested sessions accumulate:

```
outer session:   ATCH_SESSION=/home/user/.cache/atch/outer
inner session:   ATCH_SESSION=/home/user/.cache/atch/outer:/home/user/.cache/atch/inner
```

This serves two purposes: self-attach prevention (any ancestor in the chain
is rejected) and session detection from scripts. Use `atch current` to get
the human-readable session name — it prints just the basenames separated by
` > `:

```sh
# exit code: 0 inside a session, 1 outside
atch current && echo "inside session: $(atch current)"

# nested session example:
# outer > inner

# shell prompt example (bash/zsh PS1)
PS1='$(atch current 2>/dev/null && echo "[$(atch current)] ")$ '
```

To test whether you are inside any `atch` session:

```sh
[ -n "$ATCH_SESSION" ] && echo "inside a session"
```

## Session history

`atch` keeps two complementary history stores, both replayed automatically
whenever you attach — no configuration required.

### On-disk log (persistent)

Every byte written to the pty is appended to a log file on disk
(`~/.cache/atch/<session>.log`). The log persists across everything:

- **Detach / re-attach** — re-attaching to a running session replays the
  complete history before the live stream begins.
- **Session exit** — once the program exits, the full output remains on disk.
  Running `atch mysession` again starts a fresh session but first shows
  everything from the previous one, so you know exactly what it did. Use
  `atch list -a` to see all sessions that still have a log, including exited ones.
- **Machine reboot** — the log file survives a reboot. The next time you
  open the session you see the complete prior output before the new shell
  starts.
- **Crash recovery** — if the session process is killed unexpectedly, the
  log is intact. Nothing is lost.

This is fundamentally different from `tmux`, `screen`, and `dtach`: they hold
history only in memory. When the process exits or the machine restarts, the
output is gone. With `atch` the raw byte stream is on disk until you
explicitly clear it with `atch clear` (or `atch clear <session>`).

To inspect the log without attaching to the session, use `atch tail`:

```sh
atch tail mysession          # last 10 lines
atch tail -n 50 mysession    # last 50 lines
atch tail -f mysession       # follow live output
```

This works whether the session is running, exited, or from a previous boot.

The log is capped at 1 MB by default; once it exceeds that, only the most
recent 1 MB is kept. You can change the cap per session with `-C`:

```sh
atch -C 4m new mysession       # 4 MB cap
atch -C 128k start daemon      # 128 KB cap
atch -C 0 start daemon         # no log at all
```

When the log is disabled with `-C 0`, re-attaching to a **running** session
still replays recent output from the in-memory ring buffer. Only cold replay
of a dead session (after the master has exited) is unavailable.

To change the compiled-in default, build with:

```sh
make CFLAGS="-DLOG_MAX_SIZE=$((4*1024*1024))"
```

### In-memory ring buffer

While the session is running, `atch` maintains a 128 KB ring buffer in the
master process. It is the primary replay source when you re-attach: the ring
replays the most recent output instantly so your display is current. When the
on-disk log is also present it covers the full history; when logging is
disabled (`-C 0`) the ring is the only replay source available while the
session is live.

The ring is lost when the master exits; the on-disk log covers that case.

To adjust the ring size, build with:

```sh
make CFLAGS="-DSCROLLBACK_SIZE=$((256*1024))"
```

The value must be a power of two.

### Clearing history

To wipe the on-disk log and start clean on the next attach:

```sh
atch clear            # inside a session — clears the current session's log
atch clear mysession  # from outside — clear a named session's log
```

To completely remove a session that is no longer running (deletes both the socket file and the log):

```sh
atch rm mysession     # remove one stale or exited session
atch rm -a            # remove all stale and exited sessions at once
```

`rm` refuses to touch a running session — use `atch kill` first if you want to stop and remove it.

To clear the log automatically whenever you run `clear`, add a shell function
to your `.bashrc` / `.zshrc`:

```sh
clear() { command clear; [ -n "$ATCH_SESSION" ] && atch clear 2>/dev/null; }
```

This only fires for the literal `clear` command, not for full-screen programs
like `vim` or `htop` that also erase the terminal.

## Backward compatibility

The original flag-based syntax is still supported:

```
atch -a <session>              # same as: atch attach <session>
atch -A <session> [cmd...]     # same as: atch [<session> [cmd...]]
atch -c <session> [cmd...]     # same as: atch new <session> [cmd...]
atch -n <session> [cmd...]     # same as: atch start <session> [cmd...]
atch -N <session> [cmd...]     # same as: atch run <session> [cmd...]
atch -p <session>              # same as: atch push <session>
atch -k <session>              # same as: atch kill <session>
atch -l                        # same as: atch list
atch -i                        # same as: atch current
```

Existing scripts do not need to be updated.

## License

GPL. Based on dtach by Ned T. Crigler.
