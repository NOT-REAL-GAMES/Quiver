/* Map tools live in this executable but run as isolated child processes. */
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif
#include "editor_tools.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <stdint.h>
#else
#include <unistd.h>
#endif

#ifdef HAVE_EMBEDDED_MAPTOOLS
extern int Quiver_QBSPMain (int argc, const char **argv);
extern int Quiver_VISMain (int argc, const char **argv);
extern int Quiver_LIGHTMain (int argc, const char **argv);
#endif

int EditorTools_Available (void)
{
#ifdef HAVE_EMBEDDED_MAPTOOLS
	return 1;
#else
	return 0;
#endif
}

int EditorTools_Dispatch (int argc, char **argv, int *result)
{
	if (argc < 2 || strcmp (argv[1], EDITOR_TOOLS_WORKER_OPTION))
		return 0;
	if (result) *result = 2;
	if (argc < 3)
	{
		fprintf (stderr, "Missing embedded map tool name\n");
		return 1;
	}
#ifdef HAVE_EMBEDDED_MAPTOOLS
	/* Pass the tool name as argv[0], as its normal command-line entry expects. */
	int status;
	if (!strcmp (argv[2], "qbsp")) status = Quiver_QBSPMain (argc - 2, (const char **)(argv + 2));
	else if (!strcmp (argv[2], "vis")) status = Quiver_VISMain (argc - 2, (const char **)(argv + 2));
	else if (!strcmp (argv[2], "light")) status = Quiver_LIGHTMain (argc - 2, (const char **)(argv + 2));
	else { fprintf (stderr, "Unknown embedded map tool: %s\n", argv[2]); status = 2; }
	if (result) *result = status;
#else
	fprintf (stderr, "Embedded map tools were not built; configure with a working C++ compiler\n");
#endif
	return 1;
}

int EditorTools_GetSelfPath (const char *argv0, char *path, size_t size)
{
	if (!path || !size) return 0;
#ifdef _WIN32
	wchar_t wide[32768];
	DWORD n = GetModuleFileNameW (NULL, wide, (DWORD)(sizeof (wide) / sizeof (*wide)));
	if (n && n < sizeof (wide) / sizeof (*wide) &&
		WideCharToMultiByte (CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, path, (int)size, NULL, NULL)) return 1;
#elif defined(__APPLE__)
	uint32_t capacity = size > UINT32_MAX ? UINT32_MAX : (uint32_t)size;
	if (_NSGetExecutablePath (path, &capacity) == 0) return 1;
#elif defined(__linux__)
	ssize_t n = readlink ("/proc/self/exe", path, size - 1);
	if (n >= 0 && (size_t)n < size - 1) { path[n] = '\0'; return 1; }
#endif
	if (!argv0 || !*argv0 || strlen (argv0) >= size) return 0;
	memcpy (path, argv0, strlen (argv0) + 1);
	return 1;
}
