/* Portable, asynchronous child process used by the map editor. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "editor_process.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ProcessError (char *error, size_t size, const char *message, unsigned long code)
{
	if (error && size)
		snprintf (error, size, "%s (%lu)", message, code);
}

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>

struct editor_process_s
{
	HANDLE child, output, job;
	int exited;
	uint32_t exit_code;
};

/* Quote every argument using the inverse of CommandLineToArgvW/MSVCRT rules.
 * In particular, backslashes immediately preceding quotes (including the
 * closing quote) must be doubled. */
static int AppendChar (wchar_t **buffer, size_t *used, size_t *capacity, wchar_t ch)
{
	if (*used + 1 >= *capacity)
	{
		size_t next = *capacity ? *capacity * 2 : 128;
		if (next > 32768)
			next = 32768;
		if (next <= *used + 1)
			return 0;
		wchar_t *p = (wchar_t *)realloc (*buffer, next * sizeof (*p));
		if (!p)
			return 0;
		*buffer = p;
		*capacity = next;
	}
	(*buffer)[(*used)++] = ch;
	(*buffer)[*used] = L'\0';
	return 1;
}

static wchar_t *BuildCommandLine (const char *const *argv)
{
	wchar_t *buffer = NULL;
	size_t used = 0, capacity = 0;
	for (size_t i = 0; argv[i]; ++i)
	{
		int n = MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, argv[i], -1, NULL, 0);
		if (!n)
			goto fail;
		wchar_t *arg = (wchar_t *)malloc ((size_t)n * sizeof (*arg));
		if (!arg)
			goto fail;
		if (!MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, argv[i], -1, arg, n))
		{
			free (arg);
			goto fail;
		}
		int ok = (!i || AppendChar (&buffer, &used, &capacity, L' ')) && AppendChar (&buffer, &used, &capacity, L'"');
		const wchar_t *p = arg;
		while (ok && *p)
		{
			size_t slashes = 0;
			while (*p == L'\\')
			{
				++slashes;
				++p;
			}
			size_t count = (*p == L'"' || !*p) ? 2 * slashes : slashes;
			while (ok && count--)
				ok = AppendChar (&buffer, &used, &capacity, L'\\');
			if (*p == L'"')
				ok = ok && AppendChar (&buffer, &used, &capacity, L'\\');
			if (*p)
				ok = ok && AppendChar (&buffer, &used, &capacity, *p++);
		}
		ok = ok && AppendChar (&buffer, &used, &capacity, L'"');
		free (arg);
		if (!ok)
			goto fail;
	}
	return buffer;
fail:
	free (buffer);
	return NULL;
}

editor_process_t *Editor_ProcessStart (const char *const *argv, char *error, size_t error_size)
{
	if (!argv || !argv[0] || !argv[0][0])
	{
		ProcessError (error, error_size, "Invalid executable", 0);
		return NULL;
	}
	wchar_t *command = BuildCommandLine (argv);
	if (!command)
	{
		ProcessError (error, error_size, "Invalid or oversized UTF-8 argument list", GetLastError ());
		return NULL;
	}
	editor_process_t *p = (editor_process_t *)calloc (1, sizeof (*p));
	HANDLE write_pipe = NULL, input = INVALID_HANDLE_VALUE;
	PPROC_THREAD_ATTRIBUTE_LIST attributes = NULL;
	SIZE_T attr_size = 0;
	DWORD code = ERROR_NOT_ENOUGH_MEMORY;
	if (!p)
		goto fail;
	SECURITY_ATTRIBUTES security = {sizeof (security), NULL, TRUE};
	if (!CreatePipe (&p->output, &write_pipe, &security, 0) || !SetHandleInformation (p->output, HANDLE_FLAG_INHERIT, 0))
		goto win_fail;
	input = CreateFileW (L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (input == INVALID_HANDLE_VALUE)
		goto win_fail;
	InitializeProcThreadAttributeList (NULL, 1, 0, &attr_size);
	attributes = (PPROC_THREAD_ATTRIBUTE_LIST)malloc (attr_size);
	if (!attributes)
		goto fail;
	if (!InitializeProcThreadAttributeList (attributes, 1, 0, &attr_size))
	{
		free (attributes);
		attributes = NULL;
		goto win_fail;
	}
	HANDLE inherited[] = {write_pipe, input};
	if (!UpdateProcThreadAttribute (attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof (inherited), NULL, NULL))
		goto win_fail;
	p->job = CreateJobObjectW (NULL, NULL);
	if (p->job)
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (!SetInformationJobObject (p->job, JobObjectExtendedLimitInformation, &limits, sizeof (limits)))
		{
			CloseHandle (p->job);
			p->job = NULL;
		}
	}
	STARTUPINFOEXW startup = {0};
	startup.StartupInfo.cb = sizeof (startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = input;
	startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = write_pipe;
	startup.lpAttributeList = attributes;
	PROCESS_INFORMATION child = {0};
	if (!CreateProcessW (NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
			NULL, NULL, &startup.StartupInfo, &child))
		goto win_fail;
	p->child = child.hProcess;
	if (p->job && !AssignProcessToJobObject (p->job, p->child))
	{
		CloseHandle (p->job);
		p->job = NULL;
	}
	if (ResumeThread (child.hThread) == (DWORD)-1)
	{
		code = GetLastError ();
		TerminateProcess (p->child, 1);
		WaitForSingleObject (p->child, INFINITE);
		CloseHandle (child.hThread);
		goto fail;
	}
	CloseHandle (child.hThread);
	DeleteProcThreadAttributeList (attributes);
	free (attributes);
	CloseHandle (input);
	CloseHandle (write_pipe);
	free (command);
	return p;
win_fail:
	code = GetLastError ();
fail:
	if (attributes)
	{
		DeleteProcThreadAttributeList (attributes);
		free (attributes);
	}
	if (input != INVALID_HANDLE_VALUE)
		CloseHandle (input);
	if (write_pipe)
		CloseHandle (write_pipe);
	if (p)
	{
		if (p->output) CloseHandle (p->output);
		if (p->child) CloseHandle (p->child);
		if (p->job) CloseHandle (p->job);
		free (p);
	}
	free (command);
	ProcessError (error, error_size, "Could not start compiler", code);
	return NULL;
}

editor_process_status_t Editor_ProcessPoll (
	editor_process_t *p, void *output, size_t capacity, size_t *output_size, uint32_t *exit_code, char *error, size_t error_size)
{
	if (output_size) *output_size = 0;
	if (!p || !output || !capacity || !output_size || !exit_code)
	{
		ProcessError (error, error_size, "Invalid process poll arguments", 0);
		return EDITOR_PROCESS_ERROR;
	}
	if (!p->exited)
	{
		DWORD state = WaitForSingleObject (p->child, 0);
		if (state == WAIT_OBJECT_0)
		{
			DWORD code;
			if (!GetExitCodeProcess (p->child, &code))
				goto failed;
			p->exited = 1;
			p->exit_code = code;
		}
		else if (state == WAIT_FAILED)
			goto failed;
	}
	DWORD available = 0;
	if (!PeekNamedPipe (p->output, NULL, 0, NULL, &available, NULL))
	{
		if (GetLastError () != ERROR_BROKEN_PIPE)
			goto failed;
		available = 0;
	}
	if (available)
	{
		DWORD count = available;
		if ((size_t)count > capacity) count = (DWORD)capacity;
		if (!ReadFile (p->output, output, count, &count, NULL))
			goto failed;
		*output_size = count;
		/* A full buffer may not contain all output; poll once more. */
		if (count == capacity)
			return EDITOR_PROCESS_RUNNING;
	}
	if (!p->exited)
		return EDITOR_PROCESS_RUNNING;
	*exit_code = p->exit_code;
	return EDITOR_PROCESS_EXITED;
failed:
	ProcessError (error, error_size, "Could not poll compiler", GetLastError ());
	return EDITOR_PROCESS_ERROR;
}

void Editor_ProcessCancel (editor_process_t *p)
{
	if (!p || p->exited)
		return;
	if (p->job)
		TerminateJobObject (p->job, 1);
	else
		TerminateProcess (p->child, 1);
}

void Editor_ProcessDestroy (editor_process_t *p)
{
	if (!p) return;
	Editor_ProcessCancel (p);
	if (!p->exited) WaitForSingleObject (p->child, INFINITE);
	CloseHandle (p->output);
	CloseHandle (p->child);
	if (p->job) CloseHandle (p->job);
	free (p);
}

#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
struct editor_process_s
{
	pid_t child;
	int output, exited;
	uint32_t exit_code;
};

editor_process_t *Editor_ProcessStart (const char *const *argv, char *error, size_t error_size)
{
	if (!argv || !argv[0] || !argv[0][0])
	{
		ProcessError (error, error_size, "Invalid executable", 0);
		return NULL;
	}
	editor_process_t *p = (editor_process_t *)calloc (1, sizeof (*p));
	if (!p)
	{
		ProcessError (error, error_size, "Out of memory", ENOMEM);
		return NULL;
	}
	int fds[2] = {-1, -1}, code;
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attr;
	int actions_ready = 0, attr_ready = 0;
#if defined(__linux__)
	/* Avoid exposing the pipe to other threads' concurrent child launches. */
	if (pipe2 (fds, O_CLOEXEC) < 0 && (errno != ENOSYS || pipe (fds) < 0))
#else
	if (pipe (fds) < 0)
#endif
	{
		code = errno;
		goto fail;
	}
	/* A host may have closed a standard descriptor. Keep pipe handles above
	 * stderr so spawn file actions never close one of their own targets. */
	for (int i = 0; i < 2; ++i)
	{
		if (fds[i] <= STDERR_FILENO)
		{
#if defined(F_DUPFD_CLOEXEC)
			int replacement = fcntl (fds[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
#else
			int replacement = fcntl (fds[i], F_DUPFD, STDERR_FILENO + 1);
#endif
			if (replacement < 0)
			{
				code = errno;
				goto fail;
			}
			close (fds[i]);
			fds[i] = replacement;
		}
	}
	if (fcntl (fds[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl (fds[1], F_SETFD, FD_CLOEXEC) < 0 ||
		fcntl (fds[0], F_SETFL, O_NONBLOCK) < 0)
	{
		code = errno;
		goto fail;
	}
	if ((code = posix_spawn_file_actions_init (&actions)) != 0) goto fail;
	actions_ready = 1;
	if ((code = posix_spawn_file_actions_addopen (&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0)) != 0 ||
		(code = posix_spawn_file_actions_adddup2 (&actions, fds[1], STDOUT_FILENO)) != 0 ||
		(code = posix_spawn_file_actions_adddup2 (&actions, fds[1], STDERR_FILENO)) != 0 ||
		(code = posix_spawn_file_actions_addclose (&actions, fds[0])) != 0 ||
		(code = posix_spawn_file_actions_addclose (&actions, fds[1])) != 0)
		goto fail;
	if ((code = posix_spawnattr_init (&attr)) != 0) goto fail;
	attr_ready = 1;
	if ((code = posix_spawnattr_setpgroup (&attr, 0)) != 0 || (code = posix_spawnattr_setflags (&attr, POSIX_SPAWN_SETPGROUP)) != 0)
		goto fail;
	code = posix_spawnp (&p->child, argv[0], &actions, &attr, (char *const *)argv, environ);
	if (code)
		goto fail;
	posix_spawnattr_destroy (&attr);
	posix_spawn_file_actions_destroy (&actions);
	close (fds[1]);
	p->output = fds[0];
	return p;
fail:
	if (attr_ready) posix_spawnattr_destroy (&attr);
	if (actions_ready) posix_spawn_file_actions_destroy (&actions);
	if (fds[0] >= 0) close (fds[0]);
	if (fds[1] >= 0) close (fds[1]);
	free (p);
	ProcessError (error, error_size, "Could not start compiler", (unsigned long)code);
	return NULL;
}

editor_process_status_t Editor_ProcessPoll (
	editor_process_t *p, void *output, size_t capacity, size_t *output_size, uint32_t *exit_code, char *error, size_t error_size)
{
	if (output_size) *output_size = 0;
	if (!p || !output || !capacity || !output_size || !exit_code)
	{
		ProcessError (error, error_size, "Invalid process poll arguments", 0);
		return EDITOR_PROCESS_ERROR;
	}
	if (!p->exited)
	{
		int status;
		pid_t result;
		do { result = waitpid (p->child, &status, WNOHANG); } while (result < 0 && errno == EINTR);
		if (result < 0)
		{
			ProcessError (error, error_size, "Could not poll compiler", (unsigned long)errno);
			return EDITOR_PROCESS_ERROR;
		}
		if (result == p->child)
		{
			p->exited = 1;
			p->exit_code = WIFEXITED (status) ? (uint32_t)WEXITSTATUS (status) : 128u + (uint32_t)WTERMSIG (status);
		}
	}
	ssize_t count;
	do { count = read (p->output, output, capacity); } while (count < 0 && errno == EINTR);
	if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
	{
		ProcessError (error, error_size, "Could not read compiler output", (unsigned long)errno);
		return EDITOR_PROCESS_ERROR;
	}
	if (count > 0)
	{
		*output_size = (size_t)count;
		if ((size_t)count == capacity)
			return EDITOR_PROCESS_RUNNING;
	}
	if (!p->exited)
		return EDITOR_PROCESS_RUNNING;
	*exit_code = p->exit_code;
	return EDITOR_PROCESS_EXITED;
}

void Editor_ProcessCancel (editor_process_t *p)
{
	if (!p || p->exited)
		return;
	/* Spawned with a dedicated process group so compiler children are stopped. */
	if (kill (-p->child, SIGKILL) < 0 && errno == ESRCH)
		kill (p->child, SIGKILL);
}

void Editor_ProcessDestroy (editor_process_t *p)
{
	if (!p) return;
	Editor_ProcessCancel (p);
	if (!p->exited)
		while (waitpid (p->child, NULL, 0) < 0 && errno == EINTR) {}
	close (p->output);
	free (p);
}
#endif
