#include "atch.h"

#include <limits.h>

/*
** Generic DEC private mode state tracker.
**
** Scans pty output for ESC[?Nh and ESC[?Nl sequences, records the last-seen
** state per mode number, and persists a preamble file of non-default states.
** On re-attach the preamble is replayed before the session log, restoring
** terminal state that may have been lost to log rotation.
**
** Three configuration layers (each overrides the previous):
**   1. Compiled-in defaults (below)
**   2. Global config: ~/.config/atch/tstate.conf
**   3. Per-session config: sockname.tstate
*/

#define MAX_MODES 64

struct tstate_mode {
	int mode;		/* DEC private mode number */
	char name[48];		/* human-readable name */
	const char *tiname_set;	/* terminfo name for 'h', or NULL */
	const char *tiname_reset; /* terminfo name for 'l', or NULL */
	int default_on;		/* 1 if 'h' is normal, 0 if 'l' is normal */
	int tracked;		/* 1 = track, 0 = skip */
	int state;		/* -1 = unseen, 0 = 'l', 1 = 'h' */
};

static struct tstate_mode modes[MAX_MODES];
static int nmodes;
static int dirty;

/* Compiled-in defaults. */
static const struct {
	int mode;
	const char *name;
	const char *tiname_set;
	const char *tiname_reset;
	int default_on;
} builtin_modes[] = {
	{ 25,   "cursor_visibility", "civis",  "cnorm",  1 },
	{ 1049, "alt_screen",        "smcup",  "rmcup",  0 },
	{ 2004, "bracketed_paste",   NULL,     NULL,     0 },
	{ 1004, "focus_events",      NULL,     NULL,     0 },
	{ 1000, "mouse_tracking",    NULL,     NULL,     0 },
	{ 1006, "mouse_sgr",         NULL,     NULL,     0 },
};

#define NBUILTINS (sizeof(builtin_modes) / sizeof(builtin_modes[0]))

static void init_defaults(void)
{
	size_t i;
	if (nmodes > 0)
		return;
	for (i = 0; i < NBUILTINS && nmodes < MAX_MODES; i++) {
		modes[nmodes].mode = builtin_modes[i].mode;
		snprintf(modes[nmodes].name, sizeof(modes[nmodes].name),
			 "%s", builtin_modes[i].name);
		modes[nmodes].tiname_set = builtin_modes[i].tiname_set;
		modes[nmodes].tiname_reset = builtin_modes[i].tiname_reset;
		modes[nmodes].default_on = builtin_modes[i].default_on;
		modes[nmodes].tracked = 1;
		modes[nmodes].state = -1;
		nmodes++;
	}
}

static struct tstate_mode *find_mode(int mode)
{
	int i;
	for (i = 0; i < nmodes; i++)
		if (modes[i].mode == mode)
			return &modes[i];
	return NULL;
}

static struct tstate_mode *ensure_mode(int mode, const char *name)
{
	struct tstate_mode *m = find_mode(mode);
	if (m)
		return m;
	if (nmodes >= MAX_MODES)
		return NULL;
	m = &modes[nmodes++];
	m->mode = mode;
	if (name)
		snprintf(m->name, sizeof(m->name), "%s", name);
	else
		snprintf(m->name, sizeof(m->name), "mode_%d", mode);
	m->tiname_set = NULL;
	m->tiname_reset = NULL;
	m->default_on = 0;
	m->tracked = 1;
	m->state = -1;
	return m;
}

/* ------------------------------------------------------------------ */
/* Scanner                                                            */
/* ------------------------------------------------------------------ */

void tstate_scan(const unsigned char *buf, size_t len)
{
	size_t i;

	init_defaults();

	for (i = 0; i < len; i++) {
		struct tstate_mode *m;
		int mode_num, new_state;
		size_t j;

		if (buf[i] != 0x1b)
			continue;
		/* Need at least ESC [ ? <digit> <h|l> = 5 bytes */
		if (i + 4 >= len)
			continue;
		if (buf[i + 1] != '[' || buf[i + 2] != '?')
			continue;

		/* Parse decimal mode number */
		j = i + 3;
		mode_num = 0;
		while (j < len && buf[j] >= '0' && buf[j] <= '9') {
			mode_num = mode_num * 10 + (buf[j] - '0');
			j++;
		}
		if (j == i + 3 || j >= len)
			continue;
		if (buf[j] != 'h' && buf[j] != 'l')
			continue;
		new_state = (buf[j] == 'h') ? 1 : 0;

		m = find_mode(mode_num);
		if (m && m->tracked && m->state != new_state) {
			m->state = new_state;
			dirty = 1;
		}
		i = j; /* skip past the sequence */
	}
}

int tstate_is_dirty(void)
{
	return dirty;
}

/* ------------------------------------------------------------------ */
/* Preamble                                                           */
/* ------------------------------------------------------------------ */

void tstate_write_preamble(const char *base)
{
	char path[PATH_MAX], tmp[PATH_MAX];
	unsigned char buf[MAX_MODES * 16];
	size_t pos = 0;
	int i, any = 0;

	init_defaults();

	snprintf(path, sizeof(path), "%s.tpreamble", base);
	snprintf(tmp, sizeof(tmp), "%s.tpreamble.tmp", base);

	for (i = 0; i < nmodes; i++) {
		int n;
		if (!modes[i].tracked || modes[i].state < 0)
			continue;
		/* Skip if state matches the default */
		if (modes[i].state == modes[i].default_on)
			continue;
		n = snprintf((char *)buf + pos, sizeof(buf) - pos,
			     "\033[?%d%c", modes[i].mode,
			     modes[i].state ? 'h' : 'l');
		if (n < 0 || pos + (size_t)n >= sizeof(buf))
			break;
		pos += (size_t)n;
		any = 1;
	}

	if (!any) {
		unlink(path);
		unlink(tmp);
		dirty = 0;
		return;
	}

	{
		int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd < 0)
			return;
		write(fd, buf, pos);
		close(fd);
		rename(tmp, path);
	}
	dirty = 0;
}

void tstate_cleanup(const char *base)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s.tpreamble", base);
	unlink(path);
	snprintf(path, sizeof(path), "%s.tpreamble.tmp", base);
	unlink(path);
}

int tstate_replay_preamble(const char *base)
{
	char path[PATH_MAX];
	unsigned char buf[512];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "%s.tpreamble", base);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		write(1, buf, (size_t)n);
	close(fd);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Config loading/saving                                              */
/* ------------------------------------------------------------------ */

static void load_config_file(const char *path)
{
	char line[128];
	FILE *f;

	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		int tracked, mode_num;
		char name_buf[48];
		struct tstate_mode *m;

		/* Skip comments and blank lines */
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == '\n' || *p == '\0')
			continue;

		if (*p != '+' && *p != '-')
			continue;
		tracked = (*p == '+') ? 1 : 0;
		p++;

		/* Parse mode number */
		mode_num = 0;
		if (*p < '0' || *p > '9')
			continue;
		while (*p >= '0' && *p <= '9') {
			mode_num = mode_num * 10 + (*p - '0');
			p++;
		}

		/* Optional name */
		while (*p == ' ' || *p == '\t') p++;
		name_buf[0] = '\0';
		if (*p && *p != '\n') {
			size_t nlen;
			char *end = p;
			while (*end && *end != '\n') end++;
			nlen = (size_t)(end - p);
			if (nlen >= sizeof(name_buf))
				nlen = sizeof(name_buf) - 1;
			memcpy(name_buf, p, nlen);
			name_buf[nlen] = '\0';
		}

		m = ensure_mode(mode_num,
				name_buf[0] ? name_buf : NULL);
		if (m)
			m->tracked = tracked;
	}
	fclose(f);
}

static void get_global_config_path(char *buf, size_t size)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");

	if (xdg && *xdg)
		snprintf(buf, size, "%s/atch/tstate.conf", xdg);
	else if (home && *home)
		snprintf(buf, size, "%s/.config/atch/tstate.conf", home);
	else
		snprintf(buf, size, "/tmp/.atch-tstate.conf");
}

void tstate_load_global_config(void)
{
	char path[PATH_MAX];

	init_defaults();
	get_global_config_path(path, sizeof(path));
	load_config_file(path);
}

void tstate_load_config(const char *base)
{
	char path[PATH_MAX];

	init_defaults();
	snprintf(path, sizeof(path), "%s.tstate", base);
	load_config_file(path);
}

static void save_config_to(const char *path)
{
	char tmp[PATH_MAX];
	FILE *f;
	int i;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	for (i = 0; i < nmodes; i++)
		fprintf(f, "%c%d %s\n", modes[i].tracked ? '+' : '-',
			modes[i].mode, modes[i].name);
	fclose(f);
	rename(tmp, path);
}

void tstate_save_config(const char *base)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s.tstate", base);
	save_config_to(path);
}

void tstate_save_global_config(void)
{
	char path[PATH_MAX], dir[PATH_MAX];
	char *slash;

	get_global_config_path(path, sizeof(path));

	/* Ensure parent directory exists */
	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir(dir, 0700);
	}

	save_config_to(path);
}

/* ------------------------------------------------------------------ */
/* Public query/mutate API                                            */
/* ------------------------------------------------------------------ */

int tstate_find_mode(const char *name_or_number)
{
	int i;

	init_defaults();

	/* Try as a number first — return the number even if not yet tracked */
	if (name_or_number[0] >= '0' && name_or_number[0] <= '9')
		return atoi(name_or_number);
	/* Try as a name */
	for (i = 0; i < nmodes; i++)
		if (strcmp(name_or_number, modes[i].name) == 0)
			return modes[i].mode;
	/* Try as a tiname */
	for (i = 0; i < nmodes; i++) {
		if (modes[i].tiname_set &&
		    strcmp(name_or_number, modes[i].tiname_set) == 0)
			return modes[i].mode;
		if (modes[i].tiname_reset &&
		    strcmp(name_or_number, modes[i].tiname_reset) == 0)
			return modes[i].mode;
	}
	return -1;
}

/* Resolve a state value string to 0 or 1. Accepts "h", "l", "on", "off",
** or tiname aliases like "civis", "cnorm", "smcup", "rmcup". */
int tstate_resolve_state(int mode, const char *val)
{
	struct tstate_mode *m;
	int i;

	if (strcmp(val, "h") == 0 || strcmp(val, "on") == 0 ||
	    strcmp(val, "1") == 0)
		return 1;
	if (strcmp(val, "l") == 0 || strcmp(val, "off") == 0 ||
	    strcmp(val, "0") == 0)
		return 0;

	/* Check tiname match */
	m = find_mode(mode);
	if (m) {
		if (m->tiname_set && strcmp(val, m->tiname_set) == 0)
			return 1;
		if (m->tiname_reset && strcmp(val, m->tiname_reset) == 0)
			return 0;
	}

	/* Check all modes for tiname (mode may be specified by number) */
	for (i = 0; i < nmodes; i++) {
		if (modes[i].tiname_set &&
		    strcmp(val, modes[i].tiname_set) == 0)
			return 1;
		if (modes[i].tiname_reset &&
		    strcmp(val, modes[i].tiname_reset) == 0)
			return 0;
	}
	return -1;
}

int tstate_set_mode(int mode, int state)
{
	struct tstate_mode *m;

	init_defaults();
	m = find_mode(mode);
	if (!m)
		return -1;
	if (m->state != state) {
		m->state = state;
		dirty = 1;
	}
	return 0;
}

int tstate_toggle_mode(int mode)
{
	struct tstate_mode *m;

	init_defaults();
	m = find_mode(mode);
	if (!m)
		return -1;
	if (m->state < 0)
		m->state = m->default_on ? 0 : 1; /* flip from default */
	else
		m->state = m->state ? 0 : 1;
	dirty = 1;
	return 0;
}

int tstate_track_mode(int mode, const char *name)
{
	struct tstate_mode *m;

	init_defaults();
	m = ensure_mode(mode, name);
	if (!m)
		return -1;
	m->tracked = 1;
	return 0;
}

int tstate_notrack_mode(int mode)
{
	struct tstate_mode *m;

	init_defaults();
	m = find_mode(mode);
	if (!m)
		return -1;
	m->tracked = 0;
	return 0;
}

int tstate_get_mode(int mode)
{
	struct tstate_mode *m;

	init_defaults();
	m = find_mode(mode);
	return m ? m->state : -1;
}

int tstate_reset_all(unsigned char *buf, size_t buflen)
{
	size_t pos = 0;
	int i;

	init_defaults();
	for (i = 0; i < nmodes; i++) {
		int n;
		int def_state = modes[i].default_on;
		n = snprintf((char *)buf + pos, buflen - pos,
			     "\033[?%d%c", modes[i].mode,
			     def_state ? 'h' : 'l');
		if (n < 0 || pos + (size_t)n >= buflen)
			break;
		pos += (size_t)n;
	}
	return (int)pos;
}

int tstate_mode_seq(int mode, int state, unsigned char *buf, size_t buflen)
{
	int n = snprintf((char *)buf, buflen, "\033[?%d%c",
			 mode, state ? 'h' : 'l');
	return (n > 0 && (size_t)n < buflen) ? n : -1;
}

/* ------------------------------------------------------------------ */
/* Display                                                            */
/* ------------------------------------------------------------------ */

void tstate_show(const char *base)
{
	int i;
	char path[PATH_MAX];
	unsigned char buf[512];
	ssize_t n;
	int fd;

	tstate_load_global_config();
	tstate_load_config(base);

	/* Read preamble to populate active state */
	snprintf(path, sizeof(path), "%s.tpreamble", base);
	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		n = read(fd, buf, sizeof(buf));
		close(fd);
		if (n > 0)
			tstate_scan(buf, (size_t)n);
	}

	printf("  %-6s %-20s %-8s %-8s %s\n",
	       "Mode", "Name", "State", "Default", "Tracked");
	printf("  %-6s %-20s %-8s %-8s %s\n",
	       "----", "----", "-----", "-------", "-------");
	for (i = 0; i < nmodes; i++) {
		const char *state_str;
		const char *default_str;

		if (modes[i].state < 0)
			state_str = "-";
		else if (modes[i].state == 1) {
			state_str = modes[i].tiname_set
				    ? modes[i].tiname_set : "h";
		} else {
			state_str = modes[i].tiname_reset
				    ? modes[i].tiname_reset : "l";
		}

		default_str = modes[i].default_on ? "h" : "l";

		printf("  %-6d %-20s %-8s %-8s %s\n",
		       modes[i].mode, modes[i].name,
		       state_str, default_str,
		       modes[i].tracked ? "yes" : "no");
	}
}
