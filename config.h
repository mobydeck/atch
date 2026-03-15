#define HAVE_LIBUTIL 1
#ifdef __APPLE__
#define HAVE_UTIL_H 1
#else
#define HAVE_PTY_H 1
#endif

#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_UNISTD_H 1

#define PACKAGE_NAME "atch"
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "dev"
#endif
#define PACKAGE_URL "https://github.com/mobydeck/atch"
#define RETSIGTYPE void


/* In-memory scrollback ring buffer size — must be a power of two */
#ifndef SCROLLBACK_SIZE
#define SCROLLBACK_SIZE (128 * 1024)
#endif
_Static_assert(SCROLLBACK_SIZE > 0 && (SCROLLBACK_SIZE & (SCROLLBACK_SIZE - 1)) == 0,
	"SCROLLBACK_SIZE must be a positive power of two");

/* Maximum size of the on-disk session log; older bytes are trimmed on open */
#ifndef LOG_MAX_SIZE
#define LOG_MAX_SIZE (1024 * 1024)
#endif
