#ifndef ATCH_PATHS_H
#define ATCH_PATHS_H

const char *session_shortname(void);
void get_session_dir(char *buf, size_t size);
int socket_with_chdir(char *path, int (*fn)(char *));

#endif
