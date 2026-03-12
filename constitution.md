# atch — Fork Constitution

## 1. Identity

This repository is a macOS-compatible fork of [mobydeck/atch](https://github.com/mobydeck/atch)
(GPL licence). The upstream project targets Linux exclusively and links with
`-static`; this fork lifts that constraint and adds macOS-specific headers so
the binary builds and runs natively on Darwin.

The fork exists for two reasons:

1. **macOS build support** — upstream does not handle `util.h` (Darwin) vs
   `pty.h` / `libutil.h` (Linux) and links with `-static` which is unsupported
   on macOS.
2. **UX evolutions** — session management improvements that may or may not be
   suitable for upstream (see § Upstream policy).

Fork: <https://github.com/DonaldoDes/atch>
Upstream: <https://github.com/mobydeck/atch>

---

## 2. Architectural Principles

### Raw PTY passthrough — no terminal emulation

atch multiplexes a PTY session over a Unix socket.  The master process owns the
PTY and forwards raw bytes to every attached client; clients write raw bytes back
to the master.  There is **no terminal emulation layer**, no VT100/ANSI parser,
no screen buffer reconstruction.  Sequences reach the real terminal of each
attaching client unchanged.

### Minimalism

- Pure C, no external runtime dependencies beyond the system C library and
  `openpty(3)` / `forkpty(3)` (provided by `-lutil` on Linux, `util.h` on
  Darwin).
- No autoconf, no cmake, no pkg-config.  A single `makefile` drives the build.
- No third-party libraries.  If a feature requires a dependency, reconsider the
  feature.

### Source layout

| File | Role |
|------|------|
| `atch.c` | Main entry point, command dispatch, shared utilities |
| `atch.h` | Shared declarations, includes, protocol constants |
| `config.h` | Compile-time feature flags and tunables |
| `master.c` | PTY master process (session owner) |
| `attach.c` | Attaching client process |

---

## 3. C Style

Observe and match the conventions already present in the codebase:

- **Indentation**: tabs (1 tab = 1 level).
- **Brace placement**: opening brace on the same line for control structures;
  on a new line for function definitions.
- **Comment style**: `/* single-line */` and the `**`-prefixed block form for
  multi-line explanations (`/* \n** text\n*/`).
- **Function length**: keep functions short and focused; extract helpers rather
  than nesting logic.
- **Naming**: `snake_case` for functions and variables; `UPPER_CASE` for
  macros and `enum` constants.
- **Error handling**: check every syscall return value; use `errno` for
  diagnostics; prefer early-return on error over deep nesting.
- **String safety**: `snprintf` instead of `sprintf`; explicit size arguments
  on all buffer operations.
- **Compiler warnings**: code must compile cleanly under `-W -Wall`.

---

## 4. Upstream Policy

| Change type | Action |
|-------------|--------|
| Generic bug fix (Linux + macOS) | Open a PR upstream; cherry-pick the fix here once merged or if upstream is slow to respond |
| macOS-specific fix (e.g. `util.h`, no `-static`) | Keep in this fork; do not send upstream |
| UX feature (session history, log rotation, kill `--force`, …) | Open a PR upstream if the change is general-purpose; keep here otherwise |
| Breaking protocol change | Discuss upstream before implementing |

The guiding principle: upstream is the source of truth for the protocol and the
core PTY loop.  This fork adds a compatibility shim and UX polish; it does not
diverge architecturally.

---

## 5. Build

### Prerequisites

- macOS: Xcode Command Line Tools (`xcode-select --install`).
- Linux: `gcc`, `make`, `libutil` (or `libbsd`).

### Local build

```sh
make clean && make
```

The `makefile` detects the platform via `uname -s` and omits `-static` on
Darwin automatically.

### Docker / cross-compile (Linux release binary)

```sh
make build-docker          # build Linux binary via Docker
make release               # build amd64 + arm64 tarballs in ./release/
```

### Relevant makefile variables

| Variable | Default | Purpose |
|----------|---------|---------|
| `VERSION` | `dev` | Embedded in the binary via `-DPACKAGE_VERSION` |
| `arch` | host arch | Target architecture for Docker build |
| `BUILDDIR` | `.` | Output directory for the binary |

---

## 6. Tests

The test suite is a POSIX shell script (`tests/test.sh`) that emits TAP output.
It requires the compiled `atch` binary as its only argument and runs in an
isolated `$HOME` under `/tmp`.

### Run on Linux (or via Docker)

```sh
make test          # builds Docker image + runs tests inside the container
```

### Run directly (if atch is already compiled locally)

```sh
sh tests/test.sh ./atch
```

The tests cover: session create/attach/detach/kill, `push`, `list`, `current`,
`clear`, the `-q` quiet flag, log-cap (`-C`), kill `--force`, and `start`.

There are currently no unit tests for individual C functions; all tests are
integration tests at the CLI level.
