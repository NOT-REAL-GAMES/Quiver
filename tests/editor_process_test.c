#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "../Quake/editor_process.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static void nap (void) { Sleep (1); }
#else
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
static void nap (void) { struct timespec t = {0, 1000000}; nanosleep (&t, NULL); }
#endif

int main (int argc, char **argv)
{
	if (argc > 1 && !strcmp (argv[1], "child"))
	{
		if (argc != 7 || strcmp (argv[2], "space here") || strcmp (argv[3], "quote\" slash\\") || strcmp (argv[4], "$(not-a-shell)") ||
			strcmp (argv[5], "") || strcmp (argv[6], "backslash\\\"quote"))
			return 9;
		fputs ("stdout marker\n", stdout);
		fputs ("stderr marker\n", stderr);
		for (int i = 0; i < 150000; ++i)
			putchar ('x');
		return 37;
	}
	if (argc > 1 && !strcmp (argv[1], "slow"))
	{
		for (;;) nap ();
	}
	char error[256] = {0}, output[257];
	size_t n, total = 0;
	uint32_t exit_code = 0;
	const char *args[] = {argv[0], "child", "space here", "quote\" slash\\", "$(not-a-shell)", "", "backslash\\\"quote", NULL};
	editor_process_t *p = Editor_ProcessStart (args, error, sizeof (error));
	if (!p) { fprintf (stderr, "%s\n", error); return 1; }
	int saw_out = 0, saw_err = 0, complete = 0;
	/* Retain a prefix because reads may split messages arbitrarily. */
	char prefix[128] = {0}; size_t prefix_size = 0;
	for (int i = 0; i < 30000; ++i)
	{
		editor_process_status_t state = Editor_ProcessPoll (p, output, sizeof (output), &n, &exit_code, error, sizeof (error));
		if (state == EDITOR_PROCESS_ERROR) { fprintf (stderr, "%s\n", error); return 2; }
		if (prefix_size < sizeof (prefix) - 1)
		{
			size_t copy = n < sizeof (prefix) - 1 - prefix_size ? n : sizeof (prefix) - 1 - prefix_size;
			memcpy (prefix + prefix_size, output, copy); prefix_size += copy;
		}
		total += n;
		if (state == EDITOR_PROCESS_EXITED) { complete = 1; break; }
		nap ();
	}
	saw_out = strstr (prefix, "stdout marker") != NULL;
	saw_err = strstr (prefix, "stderr marker") != NULL;
	Editor_ProcessDestroy (p);
	if (!complete || exit_code != 37 || total != 150000 + strlen ("stdout marker\n") + strlen ("stderr marker\n") || !saw_out || !saw_err)
	{
		fprintf (stderr, "output/exit mismatch: complete=%d code=%u bytes=%zu out=%d err=%d\n", complete, exit_code, total, saw_out, saw_err);
		return 3;
	}
	const char *slow[] = {argv[0], "slow", NULL};
	p = Editor_ProcessStart (slow, error, sizeof (error));
	if (!p) { fprintf (stderr, "%s\n", error); return 4; }
	Editor_ProcessCancel (p);
	complete = 0;
	for (int i = 0; i < 3000; ++i)
	{
		editor_process_status_t state = Editor_ProcessPoll (p, output, sizeof (output), &n, &exit_code, error, sizeof (error));
		if (state == EDITOR_PROCESS_ERROR) { fprintf (stderr, "%s\n", error); return 5; }
		if (state == EDITOR_PROCESS_EXITED) { complete = 1; break; }
		nap ();
	}
	Editor_ProcessDestroy (p);
	if (!complete || exit_code == 0) return 6;
	const char *missing[] = {"quiver-definitely-missing-map-compiler", NULL};
	p = Editor_ProcessStart (missing, error, sizeof (error));
	if (p) { Editor_ProcessDestroy (p); return 7; }
	if (!error[0]) return 8;
#ifndef _WIN32
	/* Standard streams may be closed when the game is started by a launcher. */
	pid_t test = fork ();
	if (test == 0)
	{
		close (0); close (1); close (2);
		p = Editor_ProcessStart (args, error, sizeof (error));
		if (!p) _exit (10);
		complete = 0;
		total = 0;
		for (int i = 0; i < 30000; ++i)
		{
			editor_process_status_t state = Editor_ProcessPoll (p, output, sizeof (output), &n, &exit_code, error, sizeof (error));
			if (state == EDITOR_PROCESS_ERROR) _exit (11);
			total += n;
			if (state == EDITOR_PROCESS_EXITED) { complete = 1; break; }
			nap ();
		}
		Editor_ProcessDestroy (p);
		_exit (complete && exit_code == 37 && total == 150000 + strlen ("stdout marker\n") + strlen ("stderr marker\n") ? 0 : 12);
	}
	int test_status;
	if (test < 0 || waitpid (test, &test_status, 0) != test || !WIFEXITED (test_status) || WEXITSTATUS (test_status)) return 9;
#endif
	puts ("editor process tests passed");
	return 0;
}
