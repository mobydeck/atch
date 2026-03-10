#include "atch.h"

#ifndef VDISABLE
#ifdef _POSIX_VDISABLE
#define VDISABLE _POSIX_VDISABLE
#else
#define VDISABLE 0377
#endif
#endif

/*
** The current terminal settings. After coming back from a suspend, we
** restore this.
*/
static struct termios cur_term;
/* 1 if the window size changed */
static int win_changed;
/* Socket creation time, used to compute session age in messages. */
time_t session_start;

char const *clear_csi_data(void)
{
	if (no_ansiterm || clear_method == CLEAR_NONE ||
	    (clear_method == CLEAR_UNSPEC && dont_have_tty))
		return "\r\n";
	/* CLEAR_MOVE, or CLEAR_UNSPEC with a real tty: move to bottom */
	return "\033[999H\r\n";
}

/* Write buf to fd handling partial writes. Exit on failure. */
void write_buf_or_fail(int fd, const void *buf, size_t count)
{
	while (count != 0) {
		ssize_t ret = write(fd, buf, count);

		if (ret >= 0) {
			buf = (const char *)buf + ret;
			count -= ret;
		} else if (ret < 0 && errno == EINTR)
			continue;
		else {
			if (session_start) {
				char age[32];
				session_age(age, sizeof(age));
				printf
				    ("%s[%s: session '%s' write failed after %s]\r\n",
				     clear_csi_data(), progname,
				     session_shortname(), age);
			} else {
				printf("%s[%s: write failed]\r\n",
				       clear_csi_data(), progname);
			}
			exit(1);
		}
	}
}

/* Write pkt to fd. Exit on failure. */
void write_packet_or_fail(int fd, const struct packet *pkt)
{
	while (1) {
		ssize_t ret = write(fd, pkt, sizeof(struct packet));

		if (ret == sizeof(struct packet))
			return;
		else if (ret < 0 && errno == EINTR)
			continue;
		else {
			if (session_start) {
				char age[32];
				session_age(age, sizeof(age));
				printf
				    ("%s[%s: session '%s' write failed after %s]\r\n",
				     clear_csi_data(), progname,
				     session_shortname(), age);
			} else {
				printf("%s[%s: write failed]\r\n",
				       clear_csi_data(), progname);
			}
			exit(1);
		}
	}
}

/* Restores the original terminal settings. */
static void restore_term(void)
{
	tcsetattr(0, TCSADRAIN, &orig_term);
	if (!no_ansiterm) {
		printf("\033[0m\033[?25h");
	}
	fflush(stdout);
	if (no_ansiterm)
		(void)system("tput init 2>/dev/null");
}

/* Connects to a unix domain socket */
static int connect_socket(char *name)
{
	int s;
	struct sockaddr_un sockun;

	if (strlen(name) > sizeof(sockun.sun_path) - 1)
		return socket_with_chdir(name, connect_socket);

	s = socket(PF_UNIX, SOCK_STREAM, 0);
	if (s < 0)
		return -1;
	sockun.sun_family = AF_UNIX;
	memcpy(sockun.sun_path, name, strlen(name) + 1);
	if (connect(s, (struct sockaddr *)&sockun, sizeof(sockun)) < 0) {
		close(s);

		/* ECONNREFUSED is also returned for regular files, so make
		 ** sure we are trying to connect to a socket. */
		if (errno == ECONNREFUSED) {
			struct stat st;

			if (stat(name, &st) < 0)
				return -1;
			else if (!S_ISSOCK(st.st_mode))
				errno = ENOTSOCK;
		}
		return -1;
	}
	return s;
}

void format_age(time_t secs, char *buf, size_t size)
{
	int d = (int)(secs / 86400);
	int h = (int)((secs % 86400) / 3600);
	int m = (int)((secs % 3600) / 60);
	int s = (int)(secs % 60);

	if (d > 0)
		snprintf(buf, size, "%dd %dh %dm %ds", d, h, m, s);
	else if (h > 0)
		snprintf(buf, size, "%dh %dm %ds", h, m, s);
	else if (m > 0)
		snprintf(buf, size, "%dm %ds", m, s);
	else
		snprintf(buf, size, "%ds", s);
}

void session_age(char *buf, size_t size)
{
	time_t now = time(NULL);
	format_age(now > session_start ? now - session_start : 0, buf, size);
}

/* Signal */
static RETSIGTYPE die(int sig)
{
	char age[32];
	session_age(age, sizeof(age));
	/* Print a nice pretty message for some things. */
	if (sig == SIGHUP || sig == SIGINT)
		printf("%s[%s: session '%s' detached after %s]\r\n",
		       clear_csi_data(), progname, session_shortname(), age);
	else
		printf
		    ("%s[%s: session '%s' got signal %d - exiting after %s]\r\n",
		     clear_csi_data(), progname, session_shortname(), sig, age);
	exit(1);
}

/* Window size change. */
static RETSIGTYPE win_change(ATTRIBUTE_UNUSED int sig)
{
	signal(SIGWINCH, win_change);
	win_changed = 1;
}

/* Handles input from the keyboard. */
static void process_kbd(int s, struct packet *pkt)
{
	/* Suspend? */
	if (!no_suspend && (pkt->u.buf[0] == cur_term.c_cc[VSUSP])) {
		/* Tell the master that we are suspending. */
		pkt->type = MSG_DETACH;
		write_packet_or_fail(s, pkt);

		/* And suspend... */
		tcsetattr(0, TCSADRAIN, &orig_term);
		printf("%s", clear_csi_data());
		kill(getpid(), SIGTSTP);
		tcsetattr(0, TCSADRAIN, &cur_term);

		/* Tell the master that we are returning. */
		pkt->type = MSG_ATTACH;
		pkt->len = 0;	/* normal ring replay on resume */
		write_packet_or_fail(s, pkt);

		/* We would like a redraw, too. */
		pkt->type = MSG_REDRAW;
		pkt->len = redraw_method;
		ioctl(0, TIOCGWINSZ, &pkt->u.ws);
		write_packet_or_fail(s, pkt);
		return;
	}
	/* Detach char? */
	else if (pkt->u.buf[0] == detach_char) {
		char age[32];
		session_age(age, sizeof(age));
		printf("%s[%s: session '%s' detached after %s]\r\n",
		       clear_csi_data(), progname, session_shortname(), age);
		exit(0);
	}
	/* Just in case something pukes out. */
	else if (pkt->u.buf[0] == '\f')
		win_changed = 1;

	/* Push it out */
	write_packet_or_fail(s, pkt);
}

/* Set to 1 once we have replayed the on-disk log, so attach_main
** knows not to replay it a second time. */
static int log_already_replayed;

/* Replay sockname+".log" to stdout, if it exists.
** saved_errno is from the failed connect: ECONNREFUSED means the session was
** killed/crashed (socket still on disk), ENOENT means clean exit (socket was
** unlinked; end marker is already in the log).
** Pass 0 when replaying for a running session (no end message printed).
** Returns 1 if a log was found and replayed, 0 if no log exists.
**
** Only the last SCROLLBACK_SIZE bytes of the log are replayed to avoid
** overwhelming the terminal when attaching to a session with a large log
** (e.g. a long-running build).  This matches the in-memory ring-buffer cap
** used when replaying a live session's scrollback. */
int replay_session_log(int saved_errno)
{
	char log_path[600];
	int logfd;
	const char *name;

	snprintf(log_path, sizeof(log_path), "%s.log", sockname);
	logfd = open(log_path, O_RDONLY);
	if (logfd < 0)
		return 0;

	{
		unsigned char rbuf[BUFSIZE];
		ssize_t n;
		off_t log_size;

		/* Seek to the last SCROLLBACK_SIZE bytes so that a very large
		 * log (e.g. from a long build session) does not flood the
		 * terminal.  If the log is smaller than SCROLLBACK_SIZE, start
		 * from the beginning. */
		log_size = lseek(logfd, 0, SEEK_END);
		if (log_size > (off_t)SCROLLBACK_SIZE)
			lseek(logfd, log_size - (off_t)SCROLLBACK_SIZE,
			      SEEK_SET);
		else
			lseek(logfd, 0, SEEK_SET);

		while ((n = read(logfd, rbuf, sizeof(rbuf))) > 0)
			write(1, rbuf, (size_t)n);
		close(logfd);
	}

	/* Socket still on disk = killed/crashed; clean exit already wrote its
	 * end marker into the log, so no extra message needed. */
	if (saved_errno == ECONNREFUSED) {
		name = session_shortname();
		printf("%s[%s: session '%s' ended unexpectedly]\r\n",
		       clear_csi_data(), progname, name);
	}
	log_already_replayed = 1;
	return 1;
}

int attach_main(int noerror)
{
	struct packet pkt;
	unsigned char buf[BUFSIZE];
	fd_set readfds;
	int s;

	/* Refuse to attach to any session in our ancestry chain (catches both
	 * direct self-attach and indirect loops like A -> B -> A).
	 * SESSION_ENVVAR is the colon-separated chain, so scanning it covers
	 * all ancestors. */
	{
		const char *tosearch = getenv(SESSION_ENVVAR);

		if (tosearch && *tosearch) {
			size_t slen = strlen(sockname);
			const char *p = tosearch;

			while (*p) {
				const char *colon = strchr(p, ':');
				size_t tlen =
				    colon ? (size_t)(colon - p) : strlen(p);

				if (tlen == slen
				    && strncmp(p, sockname, tlen) == 0) {
					if (!noerror)
						printf
						    ("%s: cannot attach to session '%s' from within itself\n",
						     progname,
						     session_shortname());
					return 1;
				}
				if (!colon)
					break;
				p = colon + 1;
			}
		}
	}

	/* Attempt to open the socket. Don't display an error if noerror is
	 ** set. */
	s = connect_socket(sockname);
	if (s < 0) {
		int saved_errno = errno;
		const char *name = session_shortname();

		if (!noerror) {
			if (!replay_session_log(saved_errno)) {
				if (saved_errno == ENOENT)
					printf
					    ("%s: session '%s' does not exist\n",
					     progname, name);
				else if (saved_errno == ECONNREFUSED)
					printf
					    ("%s: session '%s' is not running\n",
					     progname, name);
				else if (saved_errno == ENOTSOCK)
					printf
					    ("%s: '%s' is not a valid session\n",
					     progname, name);
				else
					printf("%s: %s: %s\n", progname,
					       sockname, strerror(saved_errno));
			}
		}
		return 1;
	}

	/* Replay the on-disk log so the user sees full session history.
	 ** Skip if already replayed by the error path (exited-session case). */
	int skip_ring = 0;
	if (!log_already_replayed && replay_session_log(0))
		skip_ring = 1;

	/* Record session start time from the socket file's ctime. */
	{
		struct stat st;
		session_start =
		    (stat(sockname, &st) == 0) ? st.st_mtime : time(NULL);
	}

	/* The current terminal settings are equal to the original terminal
	 ** settings at this point. */
	cur_term = orig_term;

	/* Set a trap to restore the terminal when we die. */
	atexit(restore_term);

	/* Set some signals. */
	signal(SIGPIPE, SIG_IGN);
	signal(SIGXFSZ, SIG_IGN);
	signal(SIGHUP, die);
	signal(SIGTERM, die);
	signal(SIGINT, die);
	signal(SIGQUIT, die);
	signal(SIGWINCH, win_change);

	/* Set raw mode. */
	cur_term.c_iflag &=
	    ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
	cur_term.c_iflag &= ~(IXON | IXOFF);
	cur_term.c_oflag &= ~(OPOST);
	cur_term.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	cur_term.c_cflag &= ~(CSIZE | PARENB);
	cur_term.c_cflag |= CS8;
	cur_term.c_cc[VLNEXT] = VDISABLE;
	cur_term.c_cc[VMIN] = 1;
	cur_term.c_cc[VTIME] = 0;
	tcsetattr(0, TCSADRAIN, &cur_term);

	/* Clear the screen on attach. Only do a full reset when explicitly
	 ** requested (CLEAR_MOVE); default/unspec just emits a blank line so
	 ** any preceding log replay remains visible.
	 ** When log replay was done for a running session (skip_ring=1), skip
	 ** the separator: the log ends at the exact pty cursor position, so
	 ** the prompt is already visible and correctly placed. */
	if (clear_method == CLEAR_MOVE && !no_ansiterm) {
		write_buf_or_fail(1, "\033c", 2);
	} else if (!quiet && !skip_ring) {
		write_buf_or_fail(1, "\r\n", 2);
	}

	/* Tell the master that we want to attach.
	 ** pkt.len=1 means the client already loaded the log; master skips
	 ** the in-memory ring replay to avoid showing history twice. */
	memset(&pkt, 0, sizeof(struct packet));
	pkt.type = MSG_ATTACH;
	pkt.len = skip_ring;
	write_packet_or_fail(s, &pkt);

	/* We would like a redraw, too. */
	pkt.type = MSG_REDRAW;
	pkt.len = redraw_method;
	ioctl(0, TIOCGWINSZ, &pkt.u.ws);
	write_packet_or_fail(s, &pkt);

	/* Wait for things to happen */
	while (1) {
		int n;

		FD_ZERO(&readfds);
		FD_SET(0, &readfds);
		FD_SET(s, &readfds);
		n = select(s + 1, &readfds, NULL, NULL, NULL);
		if (n < 0 && errno != EINTR && errno != EAGAIN) {
			char age[32];
			session_age(age, sizeof(age));
			printf
			    ("%s[%s: session '%s' select failed after %s]\r\n",
			     clear_csi_data(), progname, session_shortname(),
			     age);
			exit(1);
		}

		/* Pty activity */
		if (n > 0 && FD_ISSET(s, &readfds)) {
			ssize_t len = read(s, buf, sizeof(buf));

			if (len == 0) {
				if (!quiet) {
					char age[32];
					session_age(age, sizeof(age));
					printf
					    ("%s[%s: session '%s' exited after %s]\r\n",
					     clear_csi_data(), progname,
					     session_shortname(), age);
				}
				exit(0);
			} else if (len < 0) {
				char age[32];
				session_age(age, sizeof(age));
				printf
				    ("%s[%s: session '%s' read error after %s]\r\n",
				     clear_csi_data(), progname,
				     session_shortname(), age);
				exit(1);
			}
			/* Send the data to the terminal. */
			write_buf_or_fail(1, buf, len);
			n--;
		}
		/* stdin activity */
		if (n > 0 && FD_ISSET(0, &readfds)) {
			ssize_t len;

			pkt.type = MSG_PUSH;
			memset(pkt.u.buf, 0, sizeof(pkt.u.buf));
			len = read(0, pkt.u.buf, sizeof(pkt.u.buf));

			if (len <= 0)
				exit(1);

			pkt.len = len;
			process_kbd(s, &pkt);
			n--;
		}

		/* Window size changed? */
		if (win_changed) {
			win_changed = 0;

			pkt.type = MSG_WINCH;
			ioctl(0, TIOCGWINSZ, &pkt.u.ws);
			write_packet_or_fail(s, &pkt);
		}
	}
	return 0;
}

int push_main()
{
	struct packet pkt;
	int s;

	/* Attempt to open the socket. */
	s = connect_socket(sockname);
	if (s < 0) {
		printf("%s: %s: %s\n", progname, sockname, strerror(errno));
		return 1;
	}

	/* Set some signals. */
	signal(SIGPIPE, SIG_IGN);

	/* Push the contents of standard input to the socket. */
	pkt.type = MSG_PUSH;
	for (;;) {
		ssize_t len;

		memset(pkt.u.buf, 0, sizeof(pkt.u.buf));
		len = read(0, pkt.u.buf, sizeof(pkt.u.buf));

		if (len == 0)
			return 0;
		else if (len < 0) {
			printf("%s: %s: %s\n", progname, sockname,
			       strerror(errno));
			return 1;
		}

		pkt.len = len;
		len = write(s, &pkt, sizeof(struct packet));
		if (len != sizeof(struct packet)) {
			if (len >= 0)
				errno = EPIPE;

			printf("%s: %s: %s\n", progname, sockname,
			       strerror(errno));
			return 1;
		}
	}
}

static int send_kill(int sig)
{
	struct packet pkt;
	int s;
	ssize_t ret;

	s = connect_socket(sockname);
	if (s < 0)
		return -1;
	memset(&pkt, 0, sizeof(pkt));
	pkt.type = MSG_KILL;
	pkt.len = (unsigned char)sig;
	ret = write(s, &pkt, sizeof(pkt));
	close(s);
	return (ret == sizeof(pkt)) ? 0 : -1;
}

static int session_gone(void)
{
	struct stat st;
	return stat(sockname, &st) < 0 && errno == ENOENT;
}

int kill_main(int force)
{
	const char *name = session_shortname();
	int i;

	signal(SIGPIPE, SIG_IGN);

	if (force) {
		/* Skip the grace period — send SIGKILL immediately. */
		if (send_kill(SIGKILL) < 0) {
			if (errno == ENOENT)
				printf("%s: session '%s' does not exist\n",
				       progname, name);
			else if (errno == ECONNREFUSED)
				printf("%s: session '%s' is not running\n",
				       progname, name);
			else
				printf("%s: %s: %s\n", progname, sockname,
				       strerror(errno));
			return 1;
		}
		for (i = 0; i < 20; i++) {
			usleep(100000);
			if (session_gone()) {
				if (!quiet)
					printf("%s: session '%s' killed\n",
					       progname, name);
				return 0;
			}
		}
		printf("%s: session '%s' did not stop\n", progname, name);
		return 1;
	}

	if (send_kill(SIGTERM) < 0) {
		if (errno == ENOENT)
			printf("%s: session '%s' does not exist\n",
			       progname, name);
		else if (errno == ECONNREFUSED)
			printf("%s: session '%s' is not running\n",
			       progname, name);
		else
			printf("%s: %s: %s\n", progname, sockname,
			       strerror(errno));
		return 1;
	}

	/* Wait up to 5 seconds for graceful exit. */
	for (i = 0; i < 50; i++) {
		usleep(100000);
		if (session_gone()) {
			printf("%s: session '%s' stopped\n", progname, name);
			return 0;
		}
	}

	/* Still alive — escalate to SIGKILL. */
	send_kill(SIGKILL);

	for (i = 0; i < 20; i++) {
		usleep(100000);
		if (session_gone()) {
			printf("%s: session '%s' killed\n", progname, name);
			return 0;
		}
	}

	printf("%s: session '%s' did not stop\n", progname, name);
	return 1;
}

int list_main(void)
{
	char dir[512];
	char path[768];		/* sizeof(dir) + '/' + NAME_MAX */
	DIR *d;
	struct dirent *ent;
	time_t now = time(NULL);
	int count = 0;

	get_session_dir(dir, sizeof(dir));

	d = opendir(dir);
	if (!d) {
		if (errno == ENOENT)
			goto empty;
		printf("%s: %s: %s\n", progname, dir, strerror(errno));
		return 1;
	}

	while ((ent = readdir(d)) != NULL) {
		struct stat st;
		char age[32];
		int s;

		if (ent->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
		if (stat(path, &st) < 0 || !S_ISSOCK(st.st_mode))
			continue;

		format_age(now > st.st_mtime ? now - st.st_mtime : 0,
			   age, sizeof(age));

		s = connect_socket(path);
		if (s >= 0) {
			int attached = st.st_mode & S_IXUSR;
			close(s);
			if (attached)
				printf("%-24s since %s ago [attached]\n",
				       ent->d_name, age);
			else
				printf("%-24s since %s ago\n",
				       ent->d_name, age);
			count++;
		} else if (errno == ECONNREFUSED) {
			printf("%-24s since %s ago [stale]\n", ent->d_name,
			       age);
			count++;
		}
	}

	closedir(d);

 empty:
	if (count == 0 && !quiet)
		printf("(no sessions)\n");
	return 0;
}
