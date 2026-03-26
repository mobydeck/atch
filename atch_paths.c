#include "atch.h"
#include "atch_paths.h"

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
