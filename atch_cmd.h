#ifndef ATCH_CMD_H
#define ATCH_CMD_H

enum atch_command {
	ATCH_CMD_OPEN = 0,
	ATCH_CMD_LIST,
	ATCH_CMD_CURRENT,
	ATCH_CMD_ATTACH,
	ATCH_CMD_NEW,
	ATCH_CMD_START,
	ATCH_CMD_RUN,
	ATCH_CMD_PUSH,
	ATCH_CMD_KILL,
	ATCH_CMD_CLEAR,
	ATCH_CMD_TAIL,
	ATCH_CMD_RM,
};

enum atch_command atch_resolve_command(const char *cmd);

#endif
