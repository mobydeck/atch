#include "atch.h"
#include "atch_session.h"

char *atch_expand_session_name_dup(const char *name)
{
	char dir[512];
	size_t fulllen;
	char *full;
	char *slash;

	if (!name || !*name)
		return NULL;
	if (strchr(name, '/') != NULL)
		return strdup(name);

	get_session_dir(dir, sizeof(dir));
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir(dir, 0700);
		*slash = '/';
	}
	mkdir(dir, 0700);
	fulllen = strlen(dir) + 1 + strlen(name);
	full = malloc(fulllen + 1);
	if (!full)
		return NULL;
	snprintf(full, fulllen + 1, "%s/%s", dir, name);
	return full;
}
