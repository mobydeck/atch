#include "atch.h"
#include "atch_cmd.h"
#include "atch_cli_opts.h"
#include "atch_cli_runtime.h"
#include "atch_paths.h"
#include "atch_session.h"

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

int atch_cli_main(int argc, char **argv)
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
		if (atch_parse_options(&argc, &argv))
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
			return cmd_list(argc, argv);
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
		if (expand_sockname())
		return 1;

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

		if (atch_parse_options(&argc, &argv))
			return 1;
		if (mode != 'a')
			argv = use_shell_if_no_cmd(argc, argv);
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

	switch (atch_resolve_command(cmd)) {
	case ATCH_CMD_LIST:
		return cmd_list(argc, argv);
	case ATCH_CMD_CURRENT:
		return cmd_current();
	case ATCH_CMD_ATTACH:
		return cmd_attach(argc, argv);
	case ATCH_CMD_NEW:
		return cmd_new(argc, argv);
	case ATCH_CMD_START:
		return cmd_start(argc, argv);
	case ATCH_CMD_RUN:
		return cmd_run(argc, argv);
	case ATCH_CMD_PUSH:
		return cmd_push(argc, argv);
	case ATCH_CMD_KILL:
		return cmd_kill(argc, argv);
	case ATCH_CMD_CLEAR:
		return cmd_clear(argc, argv);
	case ATCH_CMD_TAIL:
		return cmd_tail(argc, argv);
	case ATCH_CMD_RM:
		return cmd_rm(argc, argv);
	case ATCH_CMD_OPEN:
	default:
		/* Smart default: treat first arg as session name → attach-or-create */
		return cmd_open((char *)cmd, argc, argv);
	}
}
