#ifndef ATCH_SESSION_H
#define ATCH_SESSION_H

/* Resolve a user-provided session name to full socket path.
 * - If name contains '/', returns strdup(name)
 * - Otherwise returns "<session-dir>/<name>"
 * Caller owns returned memory.
 */
char *atch_expand_session_name_dup(const char *name);

#endif
