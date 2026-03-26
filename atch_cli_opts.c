#include "atch.h"
#include "atch_cli_opts.h"

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

/* Parse option flags from argv/argc. Stops at '--' or a non-option argument.
** Returns 0 on success, 1 on error (message already printed). */
int atch_parse_options(int *argc, char ***argv)
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
					printf("%s: No escape character specified.\n", progname);
					printf("Try '%s --help' for more information.\n",
					       progname);
					return 1;
				}
				if ((*argv)[0][0] == '^' && (*argv)[0][1]) {
					if ((*argv)[0][1] == '?')
						detach_char = '\177';
					else
						detach_char = (*argv)[0][1] & 037;
				} else
					detach_char = (*argv)[0][0];
				break;
			} else if (*p == 'r') {
				++(*argv);
				--(*argc);
				if (*argc < 1) {
					printf("%s: No redraw method specified.\n", progname);
					printf("Try '%s --help' for more information.\n",
					       progname);
					return 1;
				}
				if (strcmp((*argv)[0], "none") == 0)
					redraw_method = REDRAW_NONE;
				else if (strcmp((*argv)[0], "ctrl_l") == 0)
					redraw_method = REDRAW_CTRL_L;
				else if (strcmp((*argv)[0], "winch") == 0)
					redraw_method = REDRAW_WINCH;
				else {
					printf("%s: Invalid redraw method specified.\n", progname);
					printf("Try '%s --help' for more information.\n",
					       progname);
					return 1;
				}
				break;
			} else if (*p == 'R') {
				++(*argv);
				--(*argc);
				if (*argc < 1) {
					printf("%s: No clear method specified.\n", progname);
					printf("Try '%s --help' for more information.\n",
					       progname);
					return 1;
				}
				if (strcmp((*argv)[0], "none") == 0)
					clear_method = CLEAR_NONE;
				else if (strcmp((*argv)[0], "move") == 0)
					clear_method = CLEAR_MOVE;
				else {
					printf("%s: Invalid clear method specified.\n", progname);
					printf("Try '%s --help' for more information.\n",
					       progname);
					return 1;
				}
				break;
			} else if (*p == 'C') {
				++(*argv);
				--(*argc);
				if (*argc < 1) {
					printf("%s: No log size specified.\n", progname);
					printf("Try '%s --help' for more information.\n",
					       progname);
					return 1;
				}
				if (parse_size((*argv)[0], &log_max_size)) {
					printf("%s: Invalid log size '%s'.\n", progname,
					       (*argv)[0]);
					printf("Try '%s --help' for more information.\n",
					       progname);
					return 1;
				}
				break;
			} else {
				printf("%s: Invalid option '-%c'\n", progname, *p);
				printf("Try '%s --help' for more information.\n", progname);
				return 1;
			}

			if (*argc < 2)
				break;
		}
		++(*argv);
		--(*argc);
	}
	return 0;
}
