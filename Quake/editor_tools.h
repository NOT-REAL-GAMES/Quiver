/* Private map-compiler worker dispatch.  No renderer initialization is needed. */
#ifndef QUAKE_EDITOR_TOOLS_H
#define QUAKE_EDITOR_TOOLS_H

#include <stddef.h>

#define EDITOR_TOOLS_WORKER_OPTION "--quiver-map-tool"

/* Returns nonzero when the argument list selects worker mode; result receives
 * the tool's exit status. Unknown or unavailable tools return status 2. */
int EditorTools_Dispatch (int argc, char **argv, int *result);
int EditorTools_Available (void);

/* Resolve the running executable for spawning a private worker. argv0 is the
 * original process argument, used only when no platform path is available. */
int EditorTools_GetSelfPath (const char *argv0, char *path, size_t size);

#endif
