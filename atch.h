#ifndef atch_h
#define atch_h

#if defined(__has_attribute)
#if __has_attribute(unused)
#define ATTRIBUTE_UNUSED __attribute__((__unused__))
#else
#define ATTRIBUTE_UNUSED
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define ATTRIBUTE_UNUSED __attribute__((__unused__))
#else
#define ATTRIBUTE_UNUSED
#endif

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#ifdef HAVE_PTY_H
#include <pty.h>
#endif

#ifdef HAVE_LIBUTIL_H
#include <libutil.h>
#endif

#ifdef HAVE_STROPTS_H
#include <stropts.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

#include <pwd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <dirent.h>

#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

#ifndef S_ISSOCK
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#endif

extern char *progname, *sockname;
extern int detach_char, no_suspend, redraw_method, clear_method, no_ansiterm, quiet;
extern size_t log_max_size;
extern struct termios orig_term;
extern int dont_have_tty;
extern time_t session_start;
void format_age(time_t secs, char *buf, size_t size);
void session_age(char *buf, size_t size);
const char *session_shortname(void);

enum
{
	MSG_PUSH	= 0,
	MSG_ATTACH	= 1,
	MSG_DETACH	= 2,
	MSG_WINCH	= 3,
	MSG_REDRAW	= 4,
	MSG_KILL	= 5,
};

enum
{
	REDRAW_UNSPEC	= 0,
	REDRAW_NONE	= 1,
	REDRAW_CTRL_L	= 2,
	REDRAW_WINCH	= 3,
};

enum
{
	CLEAR_UNSPEC	= 0,
	CLEAR_NONE	= 1,
	CLEAR_MOVE	= 2,
};

/* The client to master protocol. */
struct packet
{
	unsigned char type;
	unsigned char len;
	union
	{
		unsigned char buf[sizeof(struct winsize)];
		struct winsize ws;
	} u;
};

/*
** The master sends a simple stream of text to the attaching clients, without
** any protocol. This might change back to the packet based protocol in the
** future. In the meantime, however, we minimize the amount of data sent back
** and forth between the client and the master. BUFSIZE is the size of the
** buffer used for the text stream.
*/
#define BUFSIZE 4096

/* Computed at startup from progname so the binary can be renamed freely. */
extern const char *session_envvar;
#define SESSION_ENVVAR session_envvar

void write_buf_or_fail(int fd, const void *buf, size_t count);
void write_packet_or_fail(int fd, const struct packet *pkt);

void get_session_dir(char *buf, size_t size);
int socket_with_chdir(char *path, int (*fn)(char *));

int replay_session_log(int saved_errno);
int check_attach_ancestry(void);
int attach_main(int noerror);
int master_main(char **argv, int waitattach, int dontfork);
int push_main(void);
int list_main(void);
int kill_main(int force);

char const * clear_csi_data(void);

#ifdef sun
#define BROKEN_MASTER
#endif
#endif
