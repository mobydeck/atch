#include "atch.h"

/* The pty struct - The pty information is stored here. */
struct pty {
	/* File descriptor of the pty */
	int fd;
#ifdef BROKEN_MASTER
	/* File descriptor of the slave side of the pty. For broken systems. */
	int slave;
#endif
	/* Process id of the child. */
	pid_t pid;
	/* The terminal parameters of the pty. Old and new for comparision
	 ** purposes. */
	struct termios term;
	/* The current window size of the pty. */
	struct winsize ws;
};

/* A connected client */
struct client {
	/* The next client in the linked list. */
	struct client *next;
	/* The previous client in the linked list. */
	struct client **pprev;
	/* File descriptor of the client. */
	int fd;
	/* Whether or not the client is attached. */
	int attached;
	/* Scrollback replay state: physical ring index and bytes remaining. */
	size_t replay_head;
	size_t replay_remaining;
};

/* The list of connected clients. */
static struct client *clients;
/* The pseudo-terminal created for the child process. */
static struct pty the_pty;

/* Persistent session log */
static int log_fd = -1;
static size_t log_written;
static time_t master_start_time;
size_t log_max_size = LOG_MAX_SIZE;

/* Scrollback ring buffer */
static unsigned char scrollback_buf[SCROLLBACK_SIZE];
static size_t scrollback_head;	/* physical index of the oldest byte */
static size_t scrollback_len;	/* number of valid bytes, 0..SCROLLBACK_SIZE */

/*
** Trim log_fd to its last LOG_MAX_SIZE bytes, then seek to the end.
** Called at startup and whenever log_written reaches LOG_MAX_SIZE.
*/
static void rotate_log(void)
{
	off_t size;
	char *buf;
	ssize_t n;

	size = lseek(log_fd, 0, SEEK_END);
	if (size > (off_t) log_max_size) {
		buf = malloc(log_max_size);
		if (buf) {
			lseek(log_fd, size - (off_t) log_max_size, SEEK_SET);
			n = read(log_fd, buf, log_max_size);
			if (n > 0) {
				ftruncate(log_fd, 0);
				lseek(log_fd, 0, SEEK_SET);
				write(log_fd, buf, (size_t)n);
			}
			free(buf);
		}
	}
	lseek(log_fd, 0, SEEK_END);
}

/*
** Open (or create) the session log, trimming it to LOG_MAX_SIZE if it has
** grown larger. Returns the fd positioned at the end, ready for appending.
*/
static int open_log(const char *path)
{
	int fd;

	fd = open(path, O_RDWR | O_CREAT, 0600);
	if (fd < 0)
		return -1;

	log_fd = fd;
	rotate_log();
	return fd;
}

/* Write the pty-child PID to <sockname>.ppid for ancestry verification. */
static void write_session_ppid(pid_t pid)
{
	char ppid_path[600];
	int fd;
	char buf[32];
	int len;

	snprintf(ppid_path, sizeof(ppid_path), "%s.ppid", sockname);
	fd = open(ppid_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return;
	len = snprintf(buf, sizeof(buf), "%d\n", (int)pid);
	write(fd, buf, (size_t)len);
	close(fd);
}

/* Write end marker to log, close it, and unlink the socket. */
static void cleanup_session(void)
{
	char ppid_path[600];

	if (log_fd >= 0) {
		time_t age = time(NULL) - master_start_time;
		char agebuf[32];
		char marker[160];

		format_age(age, agebuf, sizeof(agebuf));
		snprintf(marker, sizeof(marker),
			 "\r\n[%s: session '%s' ended after %s]\r\n", progname,
			 session_shortname(), agebuf);
		write(log_fd, marker, strlen(marker));
		close(log_fd);
		log_fd = -1;
	}
	unlink(sockname);
	snprintf(ppid_path, sizeof(ppid_path), "%s.ppid", sockname);
	unlink(ppid_path);
}

/* Signal */
static RETSIGTYPE master_die(int sig)
{
	/* Well, the child died. */
	if (sig == SIGCHLD) {
#ifdef BROKEN_MASTER
		/* Damn you Solaris! */
		close(the_pty.fd);
#endif
		return;
	}
	exit(1);
}

/* Sets a file descriptor to non-blocking mode. */
static int setnonblocking(int fd)
{
	int flags;

#if defined(O_NONBLOCK)
	flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;
	return 0;
#elif defined(FIONBIO)
	flags = 1;
	if (ioctl(fd, FIONBIO, &flags) < 0)
		return -1;
	return 0;
#else
#warning Do not know how to set non-blocking mode.
	return 0;
#endif
}

/* Initialize the pty structure. */
static int init_pty(char **argv, int statusfd)
{
	/* Use the original terminal's settings. We don't have to set the
	 ** window size here, because the attacher will send it in a packet. */
	the_pty.term = orig_term;
	memset(&the_pty.ws, 0, sizeof(struct winsize));

	/* Create the pty process */
	if (!dont_have_tty)
		the_pty.pid = forkpty(&the_pty.fd, NULL, &the_pty.term, NULL);
	else
		the_pty.pid = forkpty(&the_pty.fd, NULL, NULL, NULL);
	if (the_pty.pid < 0)
		return -1;
	else if (the_pty.pid == 0) {
		/* Child.. Execute the program. */
		/* SESSION_ENVVAR holds the colon-separated ancestry chain,
		 ** outermost first, ending with the current session's socket.
		 ** A single (non-nested) session has no colon. */
		{
			const char *prev = getenv(SESSION_ENVVAR);
			if (prev && *prev) {
				size_t len =
				    strlen(prev) + 1 + strlen(sockname) + 1;
				char *chain = malloc(len);
				if (chain) {
					snprintf(chain, len, "%s:%s", prev,
						 sockname);
					setenv(SESSION_ENVVAR, chain, 1);
					free(chain);
				}
			} else {
				setenv(SESSION_ENVVAR, sockname, 1);
			}
		}
		execvp(*argv, argv);

		/* Report the error to statusfd if we can, or stdout if we
		 ** can't. */
		if (statusfd != -1)
			dup2(statusfd, 1);
		else
			printf("%s", clear_csi_data());

		printf("%s: could not execute %s: %s\r\n", progname,
		       *argv, strerror(errno));
		fflush(stdout);
		_exit(127);
	}
	/* Parent.. Finish up and return */
#ifdef BROKEN_MASTER
	{
		char *buf;

		buf = ptsname(the_pty.fd);
		the_pty.slave = open(buf, O_RDWR | O_NOCTTY);
	}
#endif
	return 0;
}

/* Send a signal to the slave side of a pseudo-terminal. */
static void killpty(struct pty *pty, int sig)
{
	pid_t pgrp = -1;

#ifdef TIOCSIGNAL
	if (ioctl(pty->fd, TIOCSIGNAL, sig) >= 0)
		return;
#endif
#ifdef TIOCSIG
	if (ioctl(pty->fd, TIOCSIG, sig) >= 0)
		return;
#endif
#ifdef TIOCGPGRP
#ifdef BROKEN_MASTER
	if (ioctl(pty->slave, TIOCGPGRP, &pgrp) >= 0 && pgrp != -1 &&
	    kill(-pgrp, sig) >= 0)
		return;
#endif
	if (ioctl(pty->fd, TIOCGPGRP, &pgrp) >= 0 && pgrp != -1 &&
	    kill(-pgrp, sig) >= 0)
		return;
#endif

	/* Fallback using the child's pid. */
	kill(-pty->pid, sig);
}

/* Creates a new unix domain socket. */
static int create_socket(char *name)
{
	int s;
	struct sockaddr_un sockun;
	mode_t omask;

	if (strlen(name) > sizeof(sockun.sun_path) - 1)
		return socket_with_chdir(name, create_socket);

	omask = umask(077);
	s = socket(PF_UNIX, SOCK_STREAM, 0);
	umask(omask);		/* umask always succeeds, errno is untouched. */
	if (s < 0)
		return -1;
	sockun.sun_family = AF_UNIX;
	memcpy(sockun.sun_path, name, strlen(name) + 1);
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
	}
	return s;
}

/* Update the modes on the socket. */
static void update_socket_modes(int exec)
{
	struct stat st;
	mode_t newmode;

	if (stat(sockname, &st) < 0)
		return;

	if (exec)
		newmode = st.st_mode | S_IXUSR;
	else
		newmode = st.st_mode & ~S_IXUSR;

	if (st.st_mode != newmode)
		chmod(sockname, newmode);
}

/* Append len bytes from buf to the scrollback ring buffer.
** If the buffer is full, the oldest bytes are overwritten. */
static void scrollback_append(const unsigned char *buf, size_t len)
{
	size_t i;

	if (len == 0)
		return;
	if (len >= SCROLLBACK_SIZE) {
		buf += len - SCROLLBACK_SIZE;
		len = SCROLLBACK_SIZE;
	}
	for (i = 0; i < len; i++) {
		size_t wp =
		    (scrollback_head + scrollback_len) & (SCROLLBACK_SIZE - 1);
		scrollback_buf[wp] = buf[i];
		if (scrollback_len < SCROLLBACK_SIZE) {
			scrollback_len++;
		} else {
			scrollback_head =
			    (scrollback_head + 1) & (SCROLLBACK_SIZE - 1);
		}
	}
}

/* Drain pending scrollback data to client p's non-blocking socket.
** Sets p->attached = 1 when replay is complete. */
static void replay_drain(struct client *p)
{
	while (p->replay_remaining > 0) {
		size_t contiguous = SCROLLBACK_SIZE - p->replay_head;
		ssize_t n;

		if (contiguous > p->replay_remaining)
			contiguous = p->replay_remaining;

		n = write(p->fd, scrollback_buf + p->replay_head, contiguous);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				return;
			/* Write error: remove this client */
			close(p->fd);
			if (p->next)
				p->next->pprev = p->pprev;
			*(p->pprev) = p->next;
			free(p);
			return;
		}
		p->replay_head =
		    (p->replay_head + (size_t)n) & (SCROLLBACK_SIZE - 1);
		p->replay_remaining -= (size_t)n;
	}
	p->attached = 1;
}

/* Begin scrollback replay to client p.
** Called from the MSG_ATTACH handler in client_activity. */
static void replay_start(struct client *p)
{
	if (scrollback_len == 0) {
		p->replay_remaining = 0;
		p->attached = 1;
		return;
	}
	p->replay_head = scrollback_head;
	p->replay_remaining = scrollback_len;
	replay_drain(p);
}

/* Process activity on the pty - Input and terminal changes are sent out to
** the attached clients. If the pty goes away, we die. */
static void pty_activity(int s)
{
	unsigned char buf[BUFSIZE];
	ssize_t len;
	struct client *p, *next;
	fd_set readfds, writefds;
	int highest_fd, nclients;

	/* Read the pty activity */
	len = read(the_pty.fd, buf, sizeof(buf));

	/* Error -> die */
	if (len <= 0) {
		int status;

		if (wait(&status) >= 0) {
			if (WIFEXITED(status))
				exit(WEXITSTATUS(status));
		}
		exit(1);
	}
	scrollback_append(buf, (size_t)len);
	if (log_fd >= 0) {
		write(log_fd, buf, (size_t)len);
		log_written += (size_t)len;
		if (log_written >= log_max_size) {
			rotate_log();
			log_written = 0;
		}
	}
#ifdef BROKEN_MASTER
	/* Get the current terminal settings. */
	if (tcgetattr(the_pty.slave, &the_pty.term) < 0)
		exit(1);
#else
	/* Get the current terminal settings. */
	if (tcgetattr(the_pty.fd, &the_pty.term) < 0)
		exit(1);
#endif

 top:
	/*
	 ** Wait until at least one client is writable. Also wait on the control
	 ** socket in case a new client tries to connect.
	 */
	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	FD_SET(s, &readfds);
	highest_fd = s;
	for (p = clients, nclients = 0; p; p = p->next) {
		if (!p->attached)
			continue;
		FD_SET(p->fd, &writefds);
		if (p->fd > highest_fd)
			highest_fd = p->fd;
		nclients++;
	}
	if (nclients == 0)
		return;
	if (select(highest_fd + 1, &readfds, &writefds, NULL, NULL) < 0)
		return;

	/* Send the data out to the clients. */
	for (p = clients, nclients = 0; p; p = next) {
		ssize_t written;

		next = p->next;
		if (!FD_ISSET(p->fd, &writefds))
			continue;

		written = 0;
		while (written < len) {
			ssize_t n = write(p->fd, buf + written, len - written);

			if (n > 0) {
				written += n;
				continue;
			} else if (n < 0 && errno == EINTR)
				continue;
			break;
		}
		if (written == len) {
			nclients++;
		} else if (errno != EAGAIN) {
			/* Write error: drop this client */
			close(p->fd);
			if (next)
				next->pprev = p->pprev;
			*(p->pprev) = next;
			free(p);
		}
	}

	/* Try again if nothing happened. */
	if (!FD_ISSET(s, &readfds) && nclients == 0)
		goto top;
}

/* Process activity on the control socket */
static void control_activity(int s)
{
	int fd;
	struct client *p;

	/* Accept the new client and link it in. */
	fd = accept(s, NULL, NULL);
	if (fd < 0)
		return;
	else if (setnonblocking(fd) < 0) {
		close(fd);
		return;
	}

	/* Link it in. */
	p = malloc(sizeof(struct client));
	if (!p) {
		close(fd);
		return;
	}
	p->fd = fd;
	p->attached = 0;
	p->replay_head = 0;
	p->replay_remaining = 0;
	p->pprev = &clients;
	p->next = *(p->pprev);
	if (p->next)
		p->next->pprev = &p->next;
	*(p->pprev) = p;
}

/* Process activity from a client. */
static void client_activity(struct client *p)
{
	ssize_t len;
	struct packet pkt;

	/* Read the activity. */
	len = read(p->fd, &pkt, sizeof(struct packet));
	if (len < 0 && (errno == EAGAIN || errno == EINTR))
		return;

	/* Close the client on an error. */
	if (len != sizeof(struct packet)) {
		close(p->fd);
		if (p->next)
			p->next->pprev = p->pprev;
		*(p->pprev) = p->next;
		free(p);
		return;
	}

	/* Push out data to the program. */
	if (pkt.type == MSG_PUSH) {
		if (pkt.len <= sizeof(pkt.u.buf))
			write_buf_or_fail(the_pty.fd, pkt.u.buf, pkt.len);
	}

	/* Attach or detach from the program. */
	else if (pkt.type == MSG_ATTACH) {
		if (pkt.len)
			p->attached = 1;	/* client loaded log; skip ring */
		else
			replay_start(p);
	} else if (pkt.type == MSG_DETACH)
		p->attached = 0;

	/* Window size change request, without a forced redraw. */
	else if (pkt.type == MSG_WINCH) {
		the_pty.ws = pkt.u.ws;
		ioctl(the_pty.fd, TIOCSWINSZ, &the_pty.ws);
	}

	/* Force a redraw using a particular method. */
	else if (pkt.type == MSG_REDRAW) {
		int method = pkt.len;

		/* If the client didn't specify a particular method, use
		 ** whatever we had on startup. */
		if (method == REDRAW_UNSPEC)
			method = redraw_method;
		if (method == REDRAW_NONE)
			return;

		/* Set the window size. */
		the_pty.ws = pkt.u.ws;
		ioctl(the_pty.fd, TIOCSWINSZ, &the_pty.ws);

		/* Send a ^L character if the terminal is in no-echo and
		 ** character-at-a-time mode. */
		if (method == REDRAW_CTRL_L) {
			char c = '\f';

			if (((the_pty.term.c_lflag & (ECHO | ICANON)) == 0) &&
			    (the_pty.term.c_cc[VMIN] == 1)) {
				write_buf_or_fail(the_pty.fd, &c, 1);
			}
		}
		/* Send a WINCH signal to the program. */
		else if (method == REDRAW_WINCH) {
			killpty(&the_pty, SIGWINCH);
		}
	}

	/* Send a signal to the child process. */
	else if (pkt.type == MSG_KILL) {
		int sig = pkt.len ? (int)(unsigned char)pkt.len : SIGTERM;
		killpty(&the_pty, sig);
	}
}

/* The master process - It watches over the pty process and the attached */
/* clients. */
static void master_process(int s, char **argv, int waitattach, int statusfd)
{
	struct client *p, *next;
	fd_set readfds;
	fd_set writefds;
	int highest_fd;
	int nullfd;

	int has_attached_client = 0;

	/* Okay, disassociate ourselves from the original terminal, as we
	 ** don't care what happens to it. */
	setsid();

	/* Set a trap to write the end marker and unlink the socket when we die. */
	atexit(cleanup_session);

	/* Create a pty in which the process is running. */
	signal(SIGCHLD, master_die);
	if (init_pty(argv, statusfd) < 0) {
		if (statusfd != -1)
			dup2(statusfd, 1);
		if (errno == ENOENT)
			printf("%s: Could not find a pty.\n", progname);
		else
			printf("%s: init_pty: %s\n", progname, strerror(errno));
		exit(1);
	}

	/* Record the pty-child PID for ancestry verification in attach_main.
	 * attach_main reads <sockname>.ppid to confirm that a process trying
	 * to attach is genuinely running inside this session before blocking
	 * a re-attach based on a potentially stale ATCH_SESSION value. */
	write_session_ppid(the_pty.pid);

	/* Set up some signals. */
	signal(SIGPIPE, SIG_IGN);
	signal(SIGXFSZ, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);
	signal(SIGINT, master_die);
	signal(SIGTERM, master_die);

	/* Close statusfd, since we don't need it anymore. */
	if (statusfd != -1)
		close(statusfd);

	/* Make sure stdin/stdout/stderr point to /dev/null. We are now a
	 ** daemon. */
	nullfd = open("/dev/null", O_RDWR);
	dup2(nullfd, 0);
	dup2(nullfd, 1);
	dup2(nullfd, 2);
	if (nullfd > 2)
		close(nullfd);

	/* Loop forever. */
	while (1) {
		int new_has_attached_client = 0;

		/* Re-initialize the file descriptor sets for select. */
		FD_ZERO(&readfds);
		FD_ZERO(&writefds);
		FD_SET(s, &readfds);
		highest_fd = s;

		/*
		 ** When waitattach is set, wait until the client attaches
		 ** before trying to read from the pty.
		 */
		if (waitattach) {
			if (clients && clients->attached)
				waitattach = 0;
		} else {
			FD_SET(the_pty.fd, &readfds);
			if (the_pty.fd > highest_fd)
				highest_fd = the_pty.fd;
		}

		for (p = clients; p; p = p->next) {
			FD_SET(p->fd, &readfds);
			if (p->fd > highest_fd)
				highest_fd = p->fd;

			if (p->attached)
				new_has_attached_client = 1;

			if (p->replay_remaining > 0) {
				FD_SET(p->fd, &writefds);
				if (p->fd > highest_fd)
					highest_fd = p->fd;
			}
		}

		/* chmod the socket if necessary. */
		if (has_attached_client != new_has_attached_client) {
			update_socket_modes(new_has_attached_client);
			has_attached_client = new_has_attached_client;
		}

		/* Wait for something to happen. */
		if (select(highest_fd + 1, &readfds, &writefds, NULL, NULL) < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			exit(1);
		}

		/* New client? */
		if (FD_ISSET(s, &readfds))
			control_activity(s);
		/* Activity on a client? */
		for (p = clients; p; p = next) {
			next = p->next;
			if (FD_ISSET(p->fd, &readfds))
				client_activity(p);
		}
		/* Drain pending scrollback replay for writable clients. */
		for (p = clients; p; p = next) {
			next = p->next;
			if (p->replay_remaining > 0
			    && FD_ISSET(p->fd, &writefds))
				replay_drain(p);
		}
		/* pty activity? */
		if (FD_ISSET(the_pty.fd, &readfds))
			pty_activity(s);
	}
}

int master_main(char **argv, int waitattach, int dontfork)
{
	int fd[2] = { -1, -1 };
	int s;
	pid_t pid;

	/* Use a default redraw method if one hasn't been specified yet. */
	if (redraw_method == REDRAW_UNSPEC)
		redraw_method = dont_have_tty ? REDRAW_NONE : REDRAW_WINCH;

	/* Create the unix domain socket. */
	s = create_socket(sockname);
	if (s < 0) {
		if (errno == EADDRINUSE)
			printf("%s: session '%s' is already running\n",
			       progname, session_shortname());
		else
			printf("%s: %s: %s\n", progname, sockname,
			       strerror(errno));
		return 1;
	}

	/* Open the persistent session log (best-effort; ignore failures). */
	if (log_max_size > 0) {
		char log_path[600];

		snprintf(log_path, sizeof(log_path), "%s.log", sockname);
		log_fd = open_log(log_path);
	}
	master_start_time = time(NULL);
#if defined(F_SETFD) && defined(FD_CLOEXEC)
	fcntl(s, F_SETFD, FD_CLOEXEC);

	/* If FD_CLOEXEC works, create a pipe and use it to report any errors
	 ** that occur while trying to execute the program. */
	if (dontfork) {
		fd[1] = dup(2);
		if (fcntl(fd[1], F_SETFD, FD_CLOEXEC) < 0) {
			close(fd[1]);
			fd[1] = -1;
		}
	} else if (pipe(fd) >= 0) {
		if (fcntl(fd[0], F_SETFD, FD_CLOEXEC) < 0 ||
		    fcntl(fd[1], F_SETFD, FD_CLOEXEC) < 0) {
			close(fd[0]);
			close(fd[1]);
			fd[0] = fd[1] = -1;
		}
	}
#endif

	if (dontfork) {
		master_process(s, argv, waitattach, fd[1]);
		return 0;
	}

	/* Fork off so we can daemonize and such */
	pid = fork();
	if (pid < 0) {
		printf("%s: fork: %s\n", progname, strerror(errno));
		cleanup_session();
		return 1;
	} else if (pid == 0) {
		/* Child - this becomes the master */
		if (fd[0] != -1)
			close(fd[0]);
		master_process(s, argv, waitattach, fd[1]);
		return 0;
	}
	/* Parent - just return. */

#if defined(F_SETFD) && defined(FD_CLOEXEC)
	/* Check if an error occurred while trying to execute the program. */
	if (fd[0] != -1) {
		char buf[1024];
		ssize_t len;

		close(fd[1]);
		len = read(fd[0], buf, sizeof(buf));
		if (len > 0) {
			do {
				write_buf_or_fail(2, buf, len);
				len = read(fd[0], buf, sizeof(buf));
			} while (len > 0);

			kill(pid, SIGTERM);
			return 1;
		}
		close(fd[0]);
	}
#endif
	close(s);
	return 0;
}

#ifndef HAVE_PTY_H
/* openpty: Use /dev/ptmx and Unix98 if we have it. */
int
openpty(int *amaster, int *aslave, char *name, struct termios *termp,
	struct winsize *winp)
{
	int master, slave;
	char *buf;

	master = open("/dev/ptmx", O_RDWR);
	if (master < 0)
		return -1;
	if (grantpt(master) < 0)
		return -1;
	if (unlockpt(master) < 0)
		return -1;
	buf = ptsname(master);
	if (!buf)
		return -1;

	slave = open(buf, O_RDWR | O_NOCTTY);
	if (slave < 0)
		return -1;

#ifdef I_PUSH
	if (ioctl(slave, I_PUSH, "ptem") < 0)
		return -1;
	if (ioctl(slave, I_PUSH, "ldterm") < 0)
		return -1;
#endif

	*amaster = master;
	*aslave = slave;
	if (name)
		strcpy(name, buf);
	if (termp)
		tcsetattr(slave, TCSAFLUSH, termp);
	if (winp)
		ioctl(slave, TIOCSWINSZ, winp);
	return 0;
}

pid_t
forkpty(int *amaster, char *name, struct termios *termp, struct winsize *winp)
{
	pid_t pid;
	int master, slave;

	if (openpty(&master, &slave, name, termp, winp) < 0)
		return -1;
	*amaster = master;

	/* Fork off... */
	pid = fork();
	if (pid < 0)
		return -1;
	else if (pid == 0) {
		char *buf;
		int fd;

		setsid();
#ifdef TIOCSCTTY
		buf = NULL;
		if (ioctl(slave, TIOCSCTTY, NULL) < 0)
			_exit(1);
#else
		buf = ptsname(master);
		fd = open(buf, O_RDWR);
		close(fd);
#endif
		dup2(slave, 0);
		dup2(slave, 1);
		dup2(slave, 2);

		if (slave > 2)
			close(slave);
		close(master);
		return 0;
	} else {
		close(slave);
		return pid;
	}
}
#endif				/* !HAVE_PTY_H */
