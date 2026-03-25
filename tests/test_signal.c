/*
 * test_signal.c — deterministic signal-safety tests for atch attach.
 *
 * Uses forkpty() to create a real PTY, execs atch attach in the child,
 * and sends signals to the exact child PID from the parent. No pkill,
 * no script, no heuristics.
 *
 * Build:  cc -o test_signal tests/test_signal.c -lutil
 * Usage:  ./test_signal <path-to-atch-binary>
 *
 * Requires a running atch session named "sig-test-session".
 * The wrapper script tests/test_signal.sh handles setup/teardown.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

static int pass_count = 0;
static int fail_count = 0;
static int test_num = 0;

static void ok(const char *desc)
{
	test_num++;
	pass_count++;
	printf("ok %d - %s\n", test_num, desc);
}

static void fail(const char *desc, const char *detail)
{
	test_num++;
	fail_count++;
	printf("not ok %d - %s\n", test_num, desc);
	if (detail)
		printf("  # %s\n", detail);
}

/* Wait for child to exit within timeout_ms, draining master_fd to prevent
 * the child from blocking on PTY writes (e.g. during session log replay).
 * Pass master_fd=-1 if the master is already closed.
 * Returns exit status or -1 on timeout. */
static int wait_exit(pid_t pid, int timeout_ms, int master_fd)
{
	int elapsed = 0;
	int status;
	char drain[4096];

	while (elapsed < timeout_ms) {
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid) {
			if (WIFEXITED(status))
				return WEXITSTATUS(status);
			if (WIFSIGNALED(status))
				return 128 + WTERMSIG(status);
			return -1;
		}
		/* Drain PTY output to prevent child blocking on write */
		if (master_fd >= 0) {
			struct pollfd pfd = { .fd = master_fd, .events = POLLIN };
			if (poll(&pfd, 1, 0) > 0)
				(void)read(master_fd, drain, sizeof(drain));
		}
		usleep(10000); /* 10ms */
		elapsed += 10;
	}
	return -1; /* timed out */
}

/* Check if pid is still alive. */
static int is_alive(pid_t pid)
{
	return kill(pid, 0) == 0;
}

/* Read available data from fd into buf (non-blocking). */
static ssize_t drain_fd(int fd, char *buf, size_t size, int timeout_ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	ssize_t total = 0;

	while (total < (ssize_t)size - 1) {
		int r = poll(&pfd, 1, timeout_ms);
		if (r <= 0)
			break;
		ssize_t n = read(fd, buf + total, size - 1 - total);
		if (n <= 0)
			break;
		total += n;
		timeout_ms = 50; /* short timeout for subsequent reads */
	}
	buf[total] = '\0';
	return total;
}

/*
 * Fork a child that execs atch attach <session>.
 * Returns child PID, sets *master_fd to the PTY master.
 */
static pid_t spawn_attach(const char *atch_bin, const char *session,
			   int *master_fd)
{
	int master;
	pid_t pid = forkpty(&master, NULL, NULL, NULL);

	if (pid < 0) {
		perror("forkpty");
		exit(1);
	}

	if (pid == 0) {
		/* child — exec atch attach */
		execl(atch_bin, atch_bin, "attach", session, (char *)NULL);
		perror("execl");
		_exit(127);
	}

	/* parent */
	*master_fd = master;

	/* Make master non-blocking for drain_fd */
	int flags = fcntl(master, F_GETFL);
	if (flags >= 0)
		fcntl(master, F_SETFL, flags | O_NONBLOCK);

	return pid;
}

/*
 * Test: SIGWINCH does not kill the attach process.
 * After receiving SIGWINCH, the child must still be alive.
 */
static void test_sigwinch_survives(const char *atch_bin, const char *session)
{
	int master;
	pid_t pid = spawn_attach(atch_bin, session, &master);

	/* Let attach settle */
	usleep(300000);

	if (!is_alive(pid)) {
		fail("sigwinch: child alive before signal", "child died during attach");
		close(master);
		return;
	}

	/* Send SIGWINCH */
	kill(pid, SIGWINCH);
	usleep(200000);

	if (is_alive(pid))
		ok("sigwinch: child survives SIGWINCH");
	else
		fail("sigwinch: child survives SIGWINCH", "child died after SIGWINCH");

	/* Send burst of SIGWINCH */
	for (int i = 0; i < 10; i++) {
		kill(pid, SIGWINCH);
		usleep(10000);
	}
	usleep(200000);

	if (is_alive(pid))
		ok("sigwinch: child survives SIGWINCH burst (10x)");
	else
		fail("sigwinch: child survives SIGWINCH burst", "child died during burst");

	/* Clean up: send SIGTERM to exit */
	kill(pid, SIGTERM);
	wait_exit(pid, 2000, master);
	close(master);
}

/*
 * Test: SIGTERM causes prompt exit (no deadlock).
 */
static void test_sigterm_exits(const char *atch_bin, const char *session)
{
	int master;
	pid_t pid = spawn_attach(atch_bin, session, &master);

	usleep(300000);

	if (!is_alive(pid)) {
		fail("sigterm: child alive before signal", "child died during attach");
		close(master);
		return;
	}

	kill(pid, SIGTERM);

	/* Wait with PTY master still open — child must exit from the signal
	 * handler, not from EOF on the terminal. Parent drains the PTY to
	 * prevent the child blocking on replay writes. */
	int status = wait_exit(pid, 3000, master);
	close(master);

	if (status == -1) {
		fail("sigterm: child exits within 3s (master still open)",
		     "child hung (possible deadlock)");
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
	} else {
		ok("sigterm: child exits promptly after SIGTERM");
	}

	if (!is_alive(pid))
		ok("sigterm: child is dead after SIGTERM");
	else
		fail("sigterm: child is dead after SIGTERM", "child still alive");
}

/*
 * Test: SIGHUP causes prompt exit (simulates SSH disconnect).
 */
static void test_sighup_exits(const char *atch_bin, const char *session)
{
	int master;
	pid_t pid = spawn_attach(atch_bin, session, &master);

	usleep(300000);

	if (!is_alive(pid)) {
		fail("sighup: child alive before signal", "child died during attach");
		close(master);
		return;
	}

	kill(pid, SIGHUP);

	/* Wait with PTY master still open — child must exit from the signal
	 * handler, not from EOF on the terminal. Parent drains the PTY to
	 * prevent the child blocking on replay writes. */
	int status = wait_exit(pid, 3000, master);
	close(master);

	if (status == -1) {
		fail("sighup: child exits within 3s (master still open)",
		     "child hung (possible deadlock)");
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
	} else {
		ok("sighup: child exits promptly after SIGHUP");
	}

	if (!is_alive(pid))
		ok("sighup: child is dead after SIGHUP");
	else
		fail("sighup: child is dead after SIGHUP", "child still alive");
}

/*
 * Test: SIGTERM still exits promptly when attach is blocked writing to
 * its own PTY. The parent intentionally does not drain the PTY master.
 */
static void test_sigterm_exits_while_stdout_blocked(const char *atch_bin,
						    const char *session)
{
	int master;
	pid_t pid = spawn_attach(atch_bin, session, &master);

	/* Give the noisy session time to fill the PTY and block the child. */
	usleep(1000000);

	if (!is_alive(pid)) {
		fail("sigterm: child alive before blocked-write signal",
		     "child died during noisy attach");
		close(master);
		return;
	}

	kill(pid, SIGTERM);

	/* Keep the PTY master open but undrained. If attach retries EINTR
	 * forever in write_all(), this wait will time out. */
	int status = wait_exit(pid, 3000, -1);
	close(master);

	if (status == -1) {
		fail("sigterm: child exits within 3s while stdout blocked",
		     "child hung in blocked write");
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
	} else {
		ok("sigterm: child exits promptly while stdout blocked");
	}

	if (!is_alive(pid))
		ok("sigterm: child is dead after blocked-write SIGTERM");
	else
		fail("sigterm: child is dead after blocked-write SIGTERM",
		     "child still alive");
}

/*
 * Test: detach character (^\, 0x1c) causes clean detach.
 * The child should exit, and the session should still be running.
 */
static void test_detach_char(const char *atch_bin, const char *session)
{
	int master;
	pid_t pid = spawn_attach(atch_bin, session, &master);

	usleep(300000);

	if (!is_alive(pid)) {
		fail("detach: child alive before detach char", "child died during attach");
		close(master);
		return;
	}

	/* Send detach character: ^\ (0x1c) */
	char detach = 0x1c;
	if (write(master, &detach, 1) < 0) {
		fail("detach: write detach char to PTY", strerror(errno));
		close(master);
		return;
	}

	/* Give atch time to process the detach and exit */
	usleep(500000);

	/* Read any output from PTY before closing */
	char buf[4096];
	drain_fd(master, buf, sizeof(buf), 200);
	close(master);

	int status = wait_exit(pid, 3000, -1);
	if (status == -1) {
		fail("detach: child exits after detach char", "child hung");
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
	} else {
		ok("detach: child exits after detach char");
	}

	if (!is_alive(pid))
		ok("detach: child is dead after detach");
	else
		fail("detach: child is dead after detach", "child still alive");
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr,
			"Usage: %s <path-to-atch> [session-name] [noisy-session]\n",
			argv[0]);
		return 1;
	}

	const char *atch_bin = argv[1];
	const char *session = argc > 2 ? argv[2] : "sig-test-session";
	const char *noisy_session = argc > 3 ? argv[3] : "sig-noisy-session";

	printf("TAP version 13\n");

	test_sigwinch_survives(atch_bin, session);
	test_sigterm_exits(atch_bin, session);
	test_sighup_exits(atch_bin, session);
	test_sigterm_exits_while_stdout_blocked(atch_bin, noisy_session);
	test_detach_char(atch_bin, session);

	printf("\n1..%d\n", test_num);
	printf("# %d passed, %d failed\n", pass_count, fail_count);

	return fail_count > 0 ? 1 : 0;
}
