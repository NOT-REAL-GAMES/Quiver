/* Portable, asynchronous child process used by the map editor. */
#ifndef EDITOR_PROCESS_H
#define EDITOR_PROCESS_H

#include <stddef.h>
#include <stdint.h>

typedef struct editor_process_s editor_process_t;

typedef enum
{
	EDITOR_PROCESS_ERROR = -1,
	EDITOR_PROCESS_RUNNING = 0,
	EDITOR_PROCESS_EXITED = 1
} editor_process_status_t;

/* argv is a NULL-terminated list of UTF-8 arguments. argv[0] names the
 * executable; no shell is involved. stdout and stderr are merged, stdin is
 * closed, and the returned object owns the child until destroyed. On error,
 * NULL is returned and a diagnostic is written when error_size is nonzero. */
editor_process_t *Editor_ProcessStart (const char *const *argv, char *error, size_t error_size);

/* Never waits for output or process termination. Reads up to capacity bytes
 * without appending a terminator. capacity must be nonzero. Keep polling while
 * RUNNING, even after cancellation, to drain output and collect the child.
 * EXITED is returned only after currently available output has been drained;
 * exit_code is the process status, or 128 + signal on POSIX. On ERROR, retain
 * the object and destroy it. Calls for one object must be serialized. */
editor_process_status_t Editor_ProcessPoll (
	editor_process_t *process, void *output, size_t capacity, size_t *output_size, uint32_t *exit_code, char *error, size_t error_size);

/* Forcefully stop the child and its process group/job where supported. This
 * call does not wait; an already exited child is left alone. */
void Editor_ProcessCancel (editor_process_t *process);

/* Cancels and reaps a running child, closes all handles and frees the object.
 * This may wait briefly for process termination; NULL is accepted. */
void Editor_ProcessDestroy (editor_process_t *process);

#endif
