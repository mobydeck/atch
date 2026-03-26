#include "atch.h"
#include "atch_cli_opts.h"
#include "atch_session.h"

/* Expand session name to full socket path in-place. */
int expand_sockname(void)
{
	char *full = atch_expand_session_name_dup(sockname);
	if (!full) {
		printf("%s: out of memory\n", progname);
		return 1;
	}
	sockname = full;
	return 0;
}

/* Return argv unchanged if argc > 0; otherwise return a {shell, NULL} argv. */
char **use_shell_if_no_cmd(int argc, char **argv)
{
	static char *shell_argv[2];
	const char *shell;
	struct passwd *pw;

	if (argc > 0)
		return argv;
	shell = getenv("SHELL");
	if (!shell || !*shell) {
		pw = getpwuid(getuid());
		if (pw && pw->pw_shell && *pw->pw_shell)
			shell = pw->pw_shell;
	}
	if (!shell || !*shell)
		shell = "/bin/sh";
	shell_argv[0] = (char *)shell;
	shell_argv[1] = NULL;
	return shell_argv;
}

/* Snapshot terminal settings; sets dont_have_tty if not a tty. */
void save_term(void)
{
	if (tcgetattr(0, &orig_term) < 0) {
		memset(&orig_term, 0, sizeof(struct termios));
		dont_have_tty = 1;
	}
}

/* Print error and return 1 if no tty is available. */
int require_tty(void)
{
	if (dont_have_tty) {
		printf("%s: attaching to a session requires a terminal.\n",
		       progname);
		return 1;
	}
	return 0;
}

/* Consume first arg as session name, expand it, advance argc/argv. */
int consume_session(int *argc, char ***argv)
{
	if (*argc < 1) {
		printf("%s: No session was specified.\n", progname);
		printf("Try '%s --help' for more information.\n", progname);
		return 1;
	}
	sockname = **argv;
	++(*argv);
	--(*argc);
	if (expand_sockname())
		return 1;
	return 0;
}

/* atch list [-a] */
int cmd_list(int argc, char **argv)
{
	int show_all = 0;

	while (argc >= 1 && strcmp(argv[0], "-a") == 0) {
		show_all = 1;
		argc--;
		argv++;
	}
	return list_main(show_all);
}

/* atch current
** SESSION_ENVVAR holds the colon-separated ancestry chain, outermost first.
** A single (non-nested) session has no colon. */
int cmd_current(void)
{
	const char *chain = getenv(SESSION_ENVVAR);
	char *copy, *seg, *colon;
	const char *name;
	int first;

	if (!chain || !*chain)
		return 1;

	copy = strdup(chain);
	if (!copy)
		return 1;

	first = 1;
	seg = copy;
	for (;;) {
		colon = strchr(seg, ':');
		if (colon)
			*colon = '\0';
		name = strrchr(seg, '/');
		if (!first)
			printf(" > ");
		printf("%s", name ? name + 1 : seg);
		first = 0;
		if (!colon)
			break;
		seg = colon + 1;
	}
	printf("\n");
	free(copy);
	return 0;
}

/* atch attach <session> — strict attach, fail if missing */
int cmd_attach(int argc, char **argv)
{
	if (atch_parse_options(&argc, &argv))
		return 1;
	if (consume_session(&argc, &argv))
		return 1;
	if (atch_parse_options(&argc, &argv))
		return 1;
	if (argc > 0) {
		printf("%s: Invalid number of arguments.\n", progname);
		printf("Try '%s --help' for more information.\n", progname);
		return 1;
	}
	save_term();
	if (require_tty())
		return 1;
	return attach_main(0);
}

/* atch new <session> [cmd...] — create session and attach */
int cmd_new(int argc, char **argv)
{
	if (atch_parse_options(&argc, &argv))
		return 1;
	if (consume_session(&argc, &argv))
		return 1;
	if (atch_parse_options(&argc, &argv))
		return 1;
	argv = use_shell_if_no_cmd(argc, argv);
	save_term();
	if (require_tty())
		return 1;
	if (master_main(argv, 1, 0) != 0)
		return 1;
	if (!quiet)
		printf("%s: session '%s' created\n", progname,
		       session_shortname());
	return attach_main(0);
}

/* atch start <session> [cmd...] — create detached */
int cmd_start(int argc, char **argv)
{
	if (atch_parse_options(&argc, &argv))
		return 1;
	if (consume_session(&argc, &argv))
		return 1;
	if (atch_parse_options(&argc, &argv))
		return 1;
	argv = use_shell_if_no_cmd(argc, argv);
	save_term();
	if (master_main(argv, 0, 0) != 0)
		return 1;
	if (!quiet)
		printf("%s: session '%s' started\n", progname,
		       session_shortname());
	return 0;
}

/* atch run <session> [cmd...] — create, master stays in foreground */
int cmd_run(int argc, char **argv)
{
	if (atch_parse_options(&argc, &argv))
		return 1;
	if (consume_session(&argc, &argv))
		return 1;
	if (atch_parse_options(&argc, &argv))
		return 1;
	argv = use_shell_if_no_cmd(argc, argv);
	save_term();
	return master_main(argv, 0, 1);
}

/* atch push <session> — pipe stdin into session */
int cmd_push(int argc, char **argv)
{
	if (consume_session(&argc, &argv))
		return 1;
	if (argc > 0) {
		printf("%s: Invalid number of arguments.\n", progname);
		printf("Try '%s --help' for more information.\n", progname);
		return 1;
	}
	return push_main();
}

/* atch kill <session> — stop session */
int cmd_kill(int argc, char **argv)
{
	int force = 0;

	/* Accept -f / --force before or after the session name. */
	while (argc >= 1 && (strcmp(argv[0], "-f") == 0 ||
			     strcmp(argv[0], "--force") == 0)) {
		force = 1;
		argc--;
		argv++;
	}
	if (consume_session(&argc, &argv))
		return 1;
	while (argc >= 1 && (strcmp(argv[0], "-f") == 0 ||
			     strcmp(argv[0], "--force") == 0)) {
		force = 1;
		argc--;
		argv++;
	}
	if (argc > 0) {
		printf("%s: Invalid number of arguments.\n", progname);
		printf("Try '%s --help' for more information.\n", progname);
		return 1;
	}
	return kill_main(force);
}

/* atch clear <session> — truncate the on-disk session log */
int cmd_clear(int argc, char **argv)
{
	char log_path[600];
	int fd;

	if (argc > 0) {
		if (consume_session(&argc, &argv))
			return 1;
	} else {
		const char *chain = getenv(SESSION_ENVVAR);
		const char *last;

		if (!chain || !*chain) {
			printf("%s: No session was specified.\n", progname);
			printf("Try '%s --help' for more information.\n",
			       progname);
			return 1;
		}
		last = strrchr(chain, ':');
		sockname = (char *)(last ? last + 1 : chain);
	}
	if (argc > 0) {
		printf("%s: Invalid number of arguments.\n", progname);
		printf("Try '%s --help' for more information.\n", progname);
		return 1;
	}
	snprintf(log_path, sizeof(log_path), "%s.log", sockname);
	fd = open(log_path, O_WRONLY | O_TRUNC);
	if (fd >= 0) {
		close(fd);
		if (!quiet)
			printf("%s: session '%s' log cleared\n",
			       progname, session_shortname());
	} else if (errno != ENOENT) {
		printf("%s: %s: %s\n", progname, log_path, strerror(errno));
		return 1;
	}
	return 0;
}

/* Scan log fd backward and return the byte offset to seek to before
** printing the last nlines lines of output. */
static off_t find_tail_start(int fd, off_t size, int nlines)
{
	char buf[BUFSIZE];
	int count = 0;
	off_t pos = size;

	while (pos > 0 && count <= nlines) {
		off_t chunk = pos > (off_t)sizeof(buf) ? (off_t)sizeof(buf) : pos;
		ssize_t n;
		ssize_t i;

		pos -= chunk;
		lseek(fd, pos, SEEK_SET);
		n = read(fd, buf, (size_t)chunk);
		if (n <= 0)
			break;
		for (i = n - 1; i >= 0; i--) {
			if (buf[i] == '\n') {
				if (++count > nlines)
					return pos + i + 1;
			}
		}
	}
	return 0;
}

/* atch tail [-f] [-n N] <session> — print last N lines of session log */
int cmd_tail(int argc, char **argv)
{
	int follow = 0, nlines = 10;
	char log_path[600];
	unsigned char rbuf[BUFSIZE];
	off_t size, start;
	ssize_t n;
	int fd;

	/* Parse -f and -n N (also -nN) before the session name */
	while (argc >= 1 && argv[0][0] == '-' && argv[0][1] != '\0') {
		if (strcmp(argv[0], "-f") == 0) {
			follow = 1;
			argc--;
			argv++;
		} else if (strcmp(argv[0], "-n") == 0) {
			if (argc < 2) {
				printf("%s: -n requires an argument\n", progname);
				printf("Try '%s --help' for more information.\n",
				       progname);
				return 1;
			}
			nlines = atoi(argv[1]);
			argc -= 2;
			argv += 2;
		} else if (strncmp(argv[0], "-n", 2) == 0 &&
			   argv[0][2] != '\0') {
			nlines = atoi(argv[0] + 2);
			argc--;
			argv++;
		} else {
			printf("%s: Invalid option '%s'\n", progname, argv[0]);
			printf("Try '%s --help' for more information.\n",
			       progname);
			return 1;
		}
	}
	if (nlines < 1)
		nlines = 1;

	if (consume_session(&argc, &argv))
		return 1;
	if (argc > 0) {
		printf("%s: Invalid number of arguments.\n", progname);
		printf("Try '%s --help' for more information.\n", progname);
		return 1;
	}

	snprintf(log_path, sizeof(log_path), "%s.log", sockname);
	fd = open(log_path, O_RDONLY);
	if (fd < 0) {
		if (errno == ENOENT)
			printf("%s: no log for session '%s'\n", progname,
			       session_shortname());
		else
			printf("%s: %s: %s\n", progname, log_path,
			       strerror(errno));
		return 1;
	}

	size = lseek(fd, 0, SEEK_END);
	if (size > 0) {
		start = find_tail_start(fd, size, nlines);
		lseek(fd, start, SEEK_SET);
		while ((n = read(fd, rbuf, sizeof(rbuf))) > 0)
			write(1, rbuf, (size_t)n);
	}

	if (follow) {
		signal(SIGPIPE, SIG_IGN);
		for (;;) {
			usleep(250000);
			while ((n = read(fd, rbuf, sizeof(rbuf))) > 0)
				write(1, rbuf, (size_t)n);
		}
	}

	close(fd);
	return 0;
}

int cmd_rm(int argc, char **argv)
{
	int all = 0;

	if (argc >= 1 && strcmp(argv[0], "-a") == 0) {
		all = 1;
		argc--;
		argv++;
	}

	if (all) {
		if (argc > 0) {
			printf("%s: Invalid number of arguments.\n", progname);
			printf("Try '%s --help' for more information.\n",
			       progname);
			return 1;
		}
		return rm_main(1);
	}

	if (consume_session(&argc, &argv))
		return 1;
	if (argc > 0) {
		printf("%s: Invalid number of arguments.\n", progname);
		printf("Try '%s --help' for more information.\n", progname);
		return 1;
	}
	return rm_main(0);
}

/* Default: atch <session> [cmd...] — attach-or-create */
int cmd_open(char *session, int argc, char **argv)
{
	sockname = session;
	if (expand_sockname())
		return 1;
	if (atch_parse_options(&argc, &argv))
		return 1;
	argv = use_shell_if_no_cmd(argc, argv);
	save_term();
	if (require_tty())
		return 1;
	if (attach_main(1) != 0) {
		if (errno == ECONNREFUSED || errno == ENOENT) {
			int saved_errno = errno;

			replay_session_log(saved_errno);
			if (saved_errno == ECONNREFUSED)
				unlink(sockname);
			if (master_main(argv, 1, 0) != 0)
				return 1;
			if (!quiet)
				printf("%s: session '%s' created\n", progname,
				       session_shortname());
		}
		return attach_main(0);
	}
	return 0;
}

void usage(void)
{
	printf(PACKAGE_NAME " - version %s, compiled on %s at %s.\n"
	       "Usage:\n"
	       "  " PACKAGE_NAME " [<session> [command...]]"
	       "\t\tAttach to session or create it\n"
	       "  " PACKAGE_NAME " <command> [options] ...\n"
	       "\n"
	       "Commands:\n"
	       "  attach  <session>"
	       "\t\t\tStrict attach (fail if session missing)\n"
	       "  new     <session> [command...]"
	       "\tCreate session and attach\n"
	       "  start   <session> [command...]"
	       "\tCreate session, detached\n"
	       "  run     <session> [command...]"
	       "\tCreate session, master in foreground\n"
	       "  push    <session>"
	       "\t\t\tPipe stdin into session\n"
	       "  kill    [-f] <session>"
	       "\t\tStop session (SIGTERM then SIGKILL)\n"
	       "    -f, --force\t\t\tSkip grace period, send SIGKILL immediately\n"
	       "  clear   [<session>]"
	       "\t\t\tTruncate the session log\n"
	       "  tail    [-f] [-n N] <session>"
	       "\tPrint last N lines of session log\n"
	       "    -f\t\t\t\tFollow log output\n"
	       "    -n <lines>\t\t\tNumber of lines (default 10)\n"
	       "  list    [-a]\t\t\t\tList sessions (-a includes exited)\n"
	       "  rm      [-a] [<session>]"
	       "\t\tRemove stale/exited session(s)\n"
	       "    -a\t\t\t\tRemove all stale and exited sessions\n"
	       "  current\t\t\t\tPrint current session name\n"
	       "\n"
	       "Options:\n"
	       "  -e <char>\tSet detach character (default: ^\\)\n"
	       "  -E\t\tDisable detach character\n"
	       "  -r <method>\tRedraw method: none | ctrl_l | winch\n"
	       "  -R <method>\tClear method:  none | move\n"
	       "  -z\t\tDisable suspend key\n"
	       "  -q\t\tSuppress messages\n"
	       "  -t\t\tDisable VT100 assumptions\n"
	       "  -C <size>\tLog cap: 0=disable, e.g. 128k, 4m (default 1m)\n"
	       "\nURL: " PACKAGE_URL "\n\n",
	       PACKAGE_VERSION, __DATE__, __TIME__);
	exit(0);
}

