#ifndef ATCH_CLI_RUNTIME_H
#define ATCH_CLI_RUNTIME_H

int expand_sockname(void);
char **use_shell_if_no_cmd(int argc, char **argv);
void save_term(void);
int require_tty(void);
int consume_session(int *argc, char ***argv);

int cmd_list(int argc, char **argv);
int cmd_current(void);
int cmd_attach(int argc, char **argv);
int cmd_new(int argc, char **argv);
int cmd_start(int argc, char **argv);
int cmd_run(int argc, char **argv);
int cmd_push(int argc, char **argv);
int cmd_kill(int argc, char **argv);
int cmd_clear(int argc, char **argv);
int cmd_tail(int argc, char **argv);
int cmd_rm(int argc, char **argv);
int cmd_open(char *session, int argc, char **argv);

void usage(void);

#endif
