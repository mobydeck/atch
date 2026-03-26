#include <string.h>

#include "atch_cmd.h"

static int is_cmd(const char *arg, const char *a, const char *b, const char *c)
{
	return (a && strcmp(arg, a) == 0) || (b && strcmp(arg, b) == 0) ||
	    (c && strcmp(arg, c) == 0);
}

enum atch_command atch_resolve_command(const char *cmd)
{
	if (is_cmd(cmd, "list", "l", "ls"))
		return ATCH_CMD_LIST;
	if (is_cmd(cmd, "current", NULL, NULL))
		return ATCH_CMD_CURRENT;
	if (is_cmd(cmd, "attach", "a", NULL))
		return ATCH_CMD_ATTACH;
	if (is_cmd(cmd, "new", "n", NULL))
		return ATCH_CMD_NEW;
	if (is_cmd(cmd, "start", "s", NULL))
		return ATCH_CMD_START;
	if (is_cmd(cmd, "run", NULL, NULL))
		return ATCH_CMD_RUN;
	if (is_cmd(cmd, "push", "p", NULL))
		return ATCH_CMD_PUSH;
	if (is_cmd(cmd, "kill", "k", NULL))
		return ATCH_CMD_KILL;
	if (is_cmd(cmd, "clear", NULL, NULL))
		return ATCH_CMD_CLEAR;
	if (is_cmd(cmd, "tail", NULL, NULL))
		return ATCH_CMD_TAIL;
	if (is_cmd(cmd, "rm", NULL, NULL))
		return ATCH_CMD_RM;
	return ATCH_CMD_OPEN;
}
