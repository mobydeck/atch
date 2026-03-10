#include "atch.h"

/* Env-var name string, computed from progname at startup. */
const char *session_envvar;

/* Build "NAME_SESSION" from the basename of progname.
** Non-alphanumeric characters are replaced with '_'.
** Uses static storage — called once before any fork. */
static void init_envvar_names(void)
{
	static char envname[128];
	const char *base = strrchr(progname, '/');
	const char *p;
	char *d;
	size_t max;

	base = base ? base + 1 : progname;

	d = envname;
	max = sizeof(envname) - sizeof("_SESSION");
	for (p = base; *p && (size_t)(d - envname) < max; p++)
		*d++ = (*p >= 'a' && *p <= 'z') ? (char)(*p - 'a' + 'A') :
		    ((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) ?
		    *p : '_';
	strcpy(d, "_SESSION");
	session_envvar = envname;
}

/* Returns the basename of the current session socket path. */
const char *session_shortname(void)
{
	const char *p = strrchr(sockname, '/');
	return p ? p + 1 : sockname;
}

/* Returns the directory where session sockets are stored. */
void get_session_dir(char *buf, size_t size)
{
	const char *home = getenv("HOME");
	const char *base = strrchr(progname, '/');
	struct passwd *pw;

	base = base ? base + 1 : progname;

	/* If $HOME is unset or empty, try the passwd database. */
	if (!home || !*home) {
		pw = getpwuid(getuid());
		if (pw && pw->pw_dir && *pw->pw_dir)
			home = pw->pw_dir;
	}

	/* Use $HOME only if it is set and not the root directory. */
	if (home && *home && strcmp(home, "/") != 0)
		snprintf(buf, size, "%s/.cache/%s", home, base);
	else
		snprintf(buf, size, "/tmp/.%s-%d", base, (int)getuid());
}

/*
** Long-path helper for Unix socket operations: saves cwd, chdirs into the
** directory part of path, calls fn(basename), then restores cwd.
** Used by connect_socket and create_socket to handle paths > sun_path limit.
*/
int socket_with_chdir(char *path, int (*fn)(char *))
{
	char *slash = strrchr(path, '/');
	int dirfd, s;

	if (!slash) {
		errno = ENAMETOOLONG;
		return -1;
	}
	dirfd = open(".", O_RDONLY);
	if (dirfd < 0)
		return -1;
	*slash = '\0';
	s = chdir(path) >= 0 ? fn(slash + 1) : -1;
	*slash = '/';
	if (s >= 0 && fchdir(dirfd) < 0) {
		close(s);
		s = -1;
	}
	close(dirfd);
	return s;
}

/* argv[0] from the program */
char *progname;
/* The name of the passed in socket. */
char *sockname;
/* The character used for detaching. Defaults to '^\' */
int detach_char = '\\' - 64;
/* 1 if we should not interpret the suspend character. */
int no_suspend;
/* The default redraw method. REDRAW_UNSPEC = auto: winch if tty, none otherwise. */
int redraw_method = REDRAW_UNSPEC;
/* Clear method. CLEAR_UNSPEC = auto: move if tty, none otherwise. */
int clear_method = CLEAR_UNSPEC;
int quiet = 0;
/* 1 if we should not send ansi sequences to the terminal */
int no_ansiterm = 0;

/*
** The original terminal settings. Shared between the master and attach
** processes. The master uses it to initialize the pty, and the attacher uses
** it to restore the original settings.
*/
struct termios orig_term;
int dont_have_tty;

/* Parse a size string: bare number, or number with k/K (×1024) or m/M (×1048576).
** Writes result to *out. Returns 0 on success, 1 on error. */
static int parse_size(const char *s, size_t *out)
{
	char *end;
	unsigned long v;

	if (!s || !*s)
		return 1;
	v = strtoul(s, &end, 10);
	if (end == s)
		return 1;
	if (*end == 'k' || *end == 'K') {
		v *= 1024;
		end++;
	} else if (*end == 'm' || *end == 'M') {
		v *= 1024 * 1024;
		end++;
	}
	if (*end != '\0')
		return 1;
	*out = (size_t)v;
	return 0;
}

/*
** Parse option flags from argv/argc. Stops at '--' or a non-option argument.
** Returns 0 on success, 1 on error (message already printed).
*/
static int parse_options(int *argc, char ***argv)
{
	while (*argc >= 1 && ***argv == '-') {
		char *p;

		if (strcmp((*argv)[0], "--") == 0) {
			++(*argv);
			--(*argc);
			break;
		}

		for (p = (*argv)[0] + 1; *p; ++p) {
			if (*p == 'E')
				detach_char = -1;
			else if (*p == 'z')
				no_suspend = 1;
			else if (*p == 'q')
				quiet = 1;
			else if (*p == 't')
				no_ansiterm = 1;
			else if (*p == 'e') {
				++(*argv);
				--(*argc);
				if (*argc < 1) {
					printf("%s: No escape character "
					       "specified.\n", progname);
					printf("Try '%s --help' for more "
					       "information.\n", progname);
					return 1;
				}
				if ((*argv)[0][0] == '^' && (*argv)[0][1]) {
					if ((*argv)[0][1] == '?')
						detach_char = '\177';
					else
						detach_char =
						    (*argv)[0][1] & 037;
				} else
					detach_char = (*argv)[0][0];
				break;
			} else if (*p == 'r') {
				++(*argv);
				--(*argc);
				if (*argc < 1) {
					printf("%s: No redraw method "
					       "specified.\n", progname);
					printf("Try '%s --help' for more "
					       "information.\n", progname);
					return 1;
				}
				if (strcmp((*argv)[0], "none") == 0)
					redraw_method = REDRAW_NONE;
				else if (strcmp((*argv)[0], "ctrl_l") == 0)
					redraw_method = REDRAW_CTRL_L;
				else if (strcmp((*argv)[0], "winch") == 0)
					redraw_method = REDRAW_WINCH;
				else {
					printf("%s: Invalid redraw method "
					       "specified.\n", progname);
					printf("Try '%s --help' for more "
					       "information.\n", progname);
					return 1;
				}
				break;
			} else if (*p == 'R') {
				++(*argv);
				--(*argc);
				if (*argc < 1) {
					printf("%s: No clear method "
					       "specified.\n", progname);
					printf("Try '%s --help' for more "
					       "information.\n", progname);
					return 1;
				}
				if (strcmp((*argv)[0], "none") == 0)
					clear_method = CLEAR_NONE;
				else if (strcmp((*argv)[0], "move") == 0)
					clear_method = CLEAR_MOVE;
				else {
					printf("%s: Invalid clear method "
					       "specified.\n", progname);
					printf("Try '%s --help' for more "
					       "information.\n", progname);
					return 1;
				}
				break;
			} else if (*p == 'C') {
				++(*argv);
				--(*argc);
				if (*argc < 1) {
					printf("%s: No log size "
					       "specified.\n", progname);
					printf("Try '%s --help' for more "
					       "information.\n", progname);
					return 1;
				}
				if (parse_size((*argv)[0], &log_max_size)) {
					printf("%s: Invalid log size "
					       "'%s'.\n", progname, (*argv)[0]);
					printf("Try '%s --help' for more "
					       "information.\n", progname);
					return 1;
				}
				break;
			} else {
				printf("%s: Invalid option '-%c'\n",
				       progname, *p);
				printf("Try '%s --help' for more "
				       "information.\n", progname);
				return 1;
			}
		}
		++(*argv);
		--(*argc);
	}
	return 0;
}

/* Expand a bare session name to its full socket path in-place. */
static void expand_sockname(void)
{
	char dir[512];
	size_t fulllen;
	char *full;
	char *slash;

	if (strchr(sockname, '/') != NULL)
		return;

	get_session_dir(dir, sizeof(dir));
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir(dir, 0700);
		*slash = '/';
	}
	mkdir(dir, 0700);
	fulllen = strlen(dir) + 1 + strlen(sockname);
	full = malloc(fulllen + 1);
	snprintf(full, fulllen + 1, "%s/%s", dir, sockname);
	sockname = full;
}

/* Return argv unchanged if argc > 0; otherwise return a {shell, NULL} argv. */
static char **use_shell_if_no_cmd(int argc, char **argv)
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
static void save_term(void)
{
	if (tcgetattr(0, &orig_term) < 0) {
		memset(&orig_term, 0, sizeof(struct termios));
		dont_have_tty = 1;
	}
}

/* Print error and return 1 if no tty is available. */
static int require_tty(void)
{
	if (dont_have_tty) {
		printf("%s: attaching to a session requires a terminal.\n",
		       progname);
		return 1;
	}
	return 0;
}

/* Consume first arg as session name, expand it, advance argc/argv. */
static int consume_session(int *argc, char ***argv)
{
	if (*argc < 1) {
		printf("%s: No session was specified.\n", progname);
		printf("Try '%s --help' for more information.\n", progname);
		return 1;
	}
	sockname = **argv;
	++(*argv);
	--(*argc);
	expand_sockname();
	return 0;
}

/* True if arg matches any of the given names (NULL slots are ignored). */
static int is_cmd(const char *arg, const char *a, const char *b, const char *c)
{
	return strcmp(arg, a) == 0 ||
	    (b && strcmp(arg, b) == 0) || (c && strcmp(arg, c) == 0);
}

/* atch list */
static int cmd_list(void)
{
	return list_main();
}

/* atch current
** SESSION_ENVVAR holds the colon-separated ancestry chain, outermost first.
** A single (non-nested) session has no colon. */
static int cmd_current(void)
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
static int cmd_attach(int argc, char **argv)
{
	if (parse_options(&argc, &argv))
		return 1;
	if (consume_session(&argc, &argv))
		return 1;
	if (parse_options(&argc, &argv))
		return 1;
	if (argc > 0) {
		printf("%s: Invalid number of arguments.\n", progname);
		printf("Try '%s --help' for more information.\n", progname);
		return 1;
	}
	/* Check ancestry before TTY so the correct error is shown first. */
	if (check_attach_ancestry())
		return 1;
	save_term();
	if (require_tty())
		return 1;
	return attach_main(0);
}

/* atch new <session> [cmd...] — create session and attach */
static int cmd_new(int argc, char **argv)
{
	if (parse_options(&argc, &argv))
		return 1;
	if (consume_session(&argc, &argv))
		return 1;
	if (parse_options(&argc, &argv))
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
static int cmd_start(int argc, char **argv)
{
	if (parse_options(&argc, &argv))
		return 1;
	if (consume_session(&argc, &argv))
		return 1;
	if (parse_options(&argc, &argv))
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
static int cmd_run(int argc, char **argv)
{
	if (parse_options(&argc, &argv))
		return 1;
	if (consume_session(&argc, &argv))
		return 1;
	if (parse_options(&argc, &argv))
		return 1;
	argv = use_shell_if_no_cmd(argc, argv);
	save_term();
	return master_main(argv, 0, 1);
}

/* atch push <session> — pipe stdin into session */
static int cmd_push(int argc, char **argv)
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
static int cmd_kill(int argc, char **argv)
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
static int cmd_clear(int argc, char **argv)
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

/* Default: atch <session> [cmd...] — attach-or-create */
static int cmd_open(char *session, int argc, char **argv)
{
	sockname = session;
	expand_sockname();
	if (parse_options(&argc, &argv))
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

static void usage(void)
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
	       "  list\t\t\t\t\tList all sessions\n"
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

int main(int argc, char **argv)
{
	const char *cmd;

	progname = argv[0];
	init_envvar_names();
	++argv;
	--argc;

	if (argc < 1)
		usage();

	/* --help / --version / -h */
	if (strcmp(*argv, "--help") == 0 || strcmp(*argv, "-h") == 0 ||
	    strcmp(*argv, "?") == 0)
		usage();
	if (strcmp(*argv, "--version") == 0) {
		printf(PACKAGE_NAME " - version %s, compiled on %s at %s.\n",
		       PACKAGE_VERSION, __DATE__, __TIME__);
		return 0;
	}

	/*
	 ** Pre-pass: consume any global options (-q, -e, -E, -r, -R, -z, -t)
	 ** that appear before the subcommand or legacy mode letter.  We stop
	 ** (without error) as soon as we see a flag that is not a known global
	 ** option, so legacy mode letters like -a/-n still reach the dispatcher
	 ** below unchanged.
	 */
	while (argc >= 1 && argv[0][0] == '-' && argv[0][1] != '\0' &&
	       argv[0][1] != '-') {
		char c = argv[0][1];

		if (c != 'e' && c != 'E' && c != 'r' && c != 'R' &&
		    c != 'z' && c != 'q' && c != 't' && c != 'C')
			break;
		if (parse_options(&argc, &argv))
			return 1;
	}
	if (argc < 1)
		usage();

	/*
	 ** Legacy backward-compat: flag-based syntax (-a, -c, -n, -N, etc.).
	 ** Detected when the first argument is a single-dash flag.
	 */
	if (argc >= 1 && argv[0][0] == '-' && argv[0][1] != '\0' &&
	    argv[0][1] != '-') {
		int mode = (*argv)[1];

		++argv;
		--argc;
		if (mode == '?' || mode == 'h')
			usage();
		if (mode == 'l')
			return cmd_list();
		if (mode == 'i')
			return cmd_current();
		if (mode != 'a' && mode != 'A' && mode != 'c' &&
		    mode != 'n' && mode != 'N' && mode != 'p' && mode != 'k') {
			printf("%s: Invalid mode '-%c'\n", progname, mode);
			printf("Try '%s --help' for more information.\n",
			       progname);
			return 1;
		}

		if (argc < 1) {
			printf("%s: No session was specified.\n", progname);
			printf("Try '%s --help' for more information.\n",
			       progname);
			return 1;
		}
		sockname = *argv;
		++argv;
		--argc;
		expand_sockname();

		if (mode == 'p') {
			if (argc > 0) {
				printf("%s: Invalid number of arguments.\n",
				       progname);
				printf("Try '%s --help' for more "
				       "information.\n", progname);
				return 1;
			}
			return push_main();
		}
		if (mode == 'k') {
			if (argc > 0) {
				printf("%s: Invalid number of arguments.\n",
				       progname);
				printf("Try '%s --help' for more "
				       "information.\n", progname);
				return 1;
			}
			return kill_main(0);
		}

		if (parse_options(&argc, &argv))
			return 1;
		if (mode != 'a')
			argv = use_shell_if_no_cmd(argc, argv);
		/* Check ancestry before TTY so the correct error fires first. */
		if (mode == 'a' || mode == 'A' || mode == 'c') {
			if (check_attach_ancestry())
				return 1;
		}
		save_term();
		if (dont_have_tty && mode != 'n' && mode != 'N') {
			printf("%s: attaching to a session requires a "
			       "terminal.\n", progname);
			return 1;
		}

		if (mode == 'a') {
			if (argc > 0) {
				printf("%s: Invalid number of arguments.\n",
				       progname);
				printf("Try '%s --help' for more "
				       "information.\n", progname);
				return 1;
			}
			return attach_main(0);
		}
		if (mode == 'n')
			return master_main(argv, 0, 0);
		if (mode == 'N')
			return master_main(argv, 0, 1);
		if (mode == 'c') {
			if (master_main(argv, 1, 0) != 0)
				return 1;
			return attach_main(0);
		}
		/* mode == 'A' */
		if (attach_main(1) != 0) {
			if (errno == ECONNREFUSED || errno == ENOENT) {
				int saved_errno = errno;

				replay_session_log(saved_errno);
				if (saved_errno == ECONNREFUSED)
					unlink(sockname);
				if (master_main(argv, 1, 0) != 0)
					return 1;
			}
			return attach_main(0);
		}
		return 0;
	}

	/* New command-based dispatch */
	cmd = *argv;
	++argv;
	--argc;

	if (is_cmd(cmd, "list", "l", "ls"))
		return cmd_list();
	if (is_cmd(cmd, "current", NULL, NULL))
		return cmd_current();
	if (is_cmd(cmd, "attach", "a", NULL))
		return cmd_attach(argc, argv);
	if (is_cmd(cmd, "new", "n", NULL))
		return cmd_new(argc, argv);
	if (is_cmd(cmd, "start", "s", NULL))
		return cmd_start(argc, argv);
	if (is_cmd(cmd, "run", NULL, NULL))
		return cmd_run(argc, argv);
	if (is_cmd(cmd, "push", "p", NULL))
		return cmd_push(argc, argv);
	if (is_cmd(cmd, "kill", "k", NULL))
		return cmd_kill(argc, argv);
	if (is_cmd(cmd, "clear", NULL, NULL))
		return cmd_clear(argc, argv);

	/* Smart default: treat first arg as session name → attach-or-create */
	return cmd_open((char *)cmd, argc, argv);
}
