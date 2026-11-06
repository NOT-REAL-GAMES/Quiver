#include "editor_tools.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int called;
int Quiver_QBSPMain (int argc, const char **argv)
{
	assert (argc == 3 && !strcmp (argv[0], "qbsp") && !strcmp (argv[1], "map with spaces.map") && !strcmp (argv[2], "result.bsp"));
	called = 1; return 7;
}
int Quiver_VISMain (int argc, const char **argv) { assert (argc == 1 && !strcmp (argv[0], "vis")); called = 2; return 8; }
int Quiver_LIGHTMain (int argc, const char **argv) { assert (argc == 1 && !strcmp (argv[0], "light")); called = 3; return 9; }

int main (int argc, char **argv)
{
	int status = 0;
	char *ordinary[] = {"game", "-window", NULL};
	char *qbsp[] = {"game", EDITOR_TOOLS_WORKER_OPTION, "qbsp", "map with spaces.map", "result.bsp", NULL};
	char *vis[] = {"game", EDITOR_TOOLS_WORKER_OPTION, "vis", NULL};
	char *light[] = {"game", EDITOR_TOOLS_WORKER_OPTION, "light", NULL};
	char *unknown[] = {"game", EDITOR_TOOLS_WORKER_OPTION, "unknown", NULL};
	char path[32768];
	assert (argc && EditorTools_GetSelfPath (argv[0], path, sizeof (path)) && *path);
	assert (!EditorTools_Dispatch (2, ordinary, &status));
	assert (EditorTools_Available ());
	assert (EditorTools_Dispatch (5, qbsp, &status) && status == 7 && called == 1);
	assert (EditorTools_Dispatch (3, vis, &status) && status == 8 && called == 2);
	assert (EditorTools_Dispatch (3, light, &status) && status == 9 && called == 3);
	assert (EditorTools_Dispatch (3, unknown, &status) && status == 2);
	puts ("editor_tools_test: OK");
	return 0;
}
