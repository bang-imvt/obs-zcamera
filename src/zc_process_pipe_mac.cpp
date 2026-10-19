#include "zc_process_pipe_mac.h"

#if defined(__APPLE__)

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

struct zc_mac_process_pipe {
	pid_t pid = -1;
	FILE *out = nullptr;
	FILE *err = nullptr;
	bool reaped = false;
};

static void set_error(char *error, size_t error_size, const char *what,
		      int code)
{
	if (!error || error_size == 0)
		return;
	if (code)
		snprintf(error, error_size, "%s: %s", what, strerror(code));
	else
		snprintf(error, error_size, "%s", what);
}

static void close_pair(int fds[2])
{
	if (fds[0] >= 0)
		close(fds[0]);
	if (fds[1] >= 0)
		close(fds[1]);
	fds[0] = fds[1] = -1;
}

zc_mac_process_pipe *zc_mac_process_pipe_create(const char *command,
						const char *mode, char *error,
						size_t error_size)
{
	if (error && error_size)
		error[0] = '\0';
	if (!command || !mode || mode[0] != 'r') {
		set_error(error, error_size, "only read-mode pipes are supported",
			  0);
		return nullptr;
	}

	int out[2] = {-1, -1};
	int err[2] = {-1, -1};
	if (pipe(out) != 0) {
		set_error(error, error_size, "stdout pipe()", errno);
		return nullptr;
	}
	if (pipe(err) != 0) {
		const int saved = errno;
		close_pair(out);
		set_error(error, error_size, "stderr pipe()", saved);
		return nullptr;
	}

	posix_spawn_file_actions_t actions;
	int rc = posix_spawn_file_actions_init(&actions);
	if (rc != 0) {
		close_pair(out);
		close_pair(err);
		set_error(error, error_size, "posix_spawn_file_actions_init", rc);
		return nullptr;
	}

	/* Unlike libobs' implementation, close each parent-side endpoint only
	   once. A duplicate close() action makes macOS 26's posix_spawn return
	   EBADF before it even reaches /bin/sh. */
	rc = posix_spawn_file_actions_addclose(&actions, out[0]);
	if (rc == 0 && out[1] != STDOUT_FILENO)
		rc = posix_spawn_file_actions_adddup2(&actions, out[1],
						      STDOUT_FILENO);
	if (rc == 0 && out[1] != STDOUT_FILENO)
		rc = posix_spawn_file_actions_addclose(&actions, out[1]);
	if (rc == 0)
		rc = posix_spawn_file_actions_addclose(&actions, err[0]);
	if (rc == 0 && err[1] != STDERR_FILENO)
		rc = posix_spawn_file_actions_adddup2(&actions, err[1],
						      STDERR_FILENO);
	if (rc == 0 && err[1] != STDERR_FILENO)
		rc = posix_spawn_file_actions_addclose(&actions, err[1]);
	if (rc != 0) {
		posix_spawn_file_actions_destroy(&actions);
		close_pair(out);
		close_pair(err);
		set_error(error, error_size, "posix_spawn file actions", rc);
		return nullptr;
	}

	posix_spawnattr_t attr;
	rc = posix_spawnattr_init(&attr);
	if (rc != 0) {
		posix_spawn_file_actions_destroy(&actions);
		close_pair(out);
		close_pair(err);
		set_error(error, error_size, "posix_spawnattr_init", rc);
		return nullptr;
	}
	/* Put the shell in its own process group. Stop() can then terminate the
	   shell and any descendant that did not get replaced by exec. */
	short flags = POSIX_SPAWN_SETPGROUP;
	rc = posix_spawnattr_setflags(&attr, flags);
	if (rc == 0)
		rc = posix_spawnattr_setpgroup(&attr, 0);
	if (rc != 0) {
		posix_spawnattr_destroy(&attr);
		posix_spawn_file_actions_destroy(&actions);
		close_pair(out);
		close_pair(err);
		set_error(error, error_size, "posix_spawnattr_setpgroup", rc);
		return nullptr;
	}

	char *argv[] = {const_cast<char *>("/bin/sh"),
			const_cast<char *>("-c"),
			const_cast<char *>(command), nullptr};
	pid_t pid = -1;
	rc = posix_spawn(&pid, "/bin/sh", &actions, &attr, argv, environ);
	posix_spawnattr_destroy(&attr);
	posix_spawn_file_actions_destroy(&actions);
	if (rc != 0) {
		close_pair(out);
		close_pair(err);
		set_error(error, error_size, "posix_spawn(/bin/sh)", rc);
		return nullptr;
	}

	/* The child owns the write ends now. */
	close(out[1]);
	close(err[1]);
	out[1] = err[1] = -1;

	FILE *out_file = fdopen(out[0], "r");
	if (!out_file) {
		const int saved = errno;
		kill(-pid, SIGKILL);
		int status = 0;
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		}
		close(out[0]);
		close(err[0]);
		set_error(error, error_size, "fdopen(stdout)", saved);
		return nullptr;
	}
	FILE *err_file = fdopen(err[0], "r");
	if (!err_file) {
		const int saved = errno;
		kill(-pid, SIGKILL);
		int status = 0;
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		}
		fclose(out_file);
		close(err[0]);
		set_error(error, error_size, "fdopen(stderr)", saved);
		return nullptr;
	}

	auto *pipe = new (std::nothrow) zc_mac_process_pipe;
	if (!pipe) {
		kill(-pid, SIGKILL);
		int status = 0;
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		}
		fclose(out_file);
		fclose(err_file);
		set_error(error, error_size, "out of memory", 0);
		return nullptr;
	}
	pipe->pid = pid;
	pipe->out = out_file;
	pipe->err = err_file;
	return pipe;
}

size_t zc_mac_process_pipe_read(zc_mac_process_pipe *pipe, uint8_t *dst,
				size_t size)
{
	if (!pipe || !pipe->out || !dst || size == 0)
		return 0;
	return fread(dst, 1, size, pipe->out);
}

size_t zc_mac_process_pipe_read_err(zc_mac_process_pipe *pipe, uint8_t *dst,
				    size_t size)
{
	if (!pipe || !pipe->err || !dst || size == 0)
		return 0;
	return fread(dst, 1, size, pipe->err);
}

void zc_mac_process_pipe_terminate(zc_mac_process_pipe *pipe)
{
	if (!pipe || pipe->reaped || pipe->pid <= 0)
		return;

	kill(-pipe->pid, SIGTERM);
	kill(pipe->pid, SIGTERM);
	for (int i = 0; i < 20; i++) {
		int status = 0;
		pid_t rc = waitpid(pipe->pid, &status, WNOHANG);
		if (rc == pipe->pid || (rc < 0 && errno == ECHILD)) {
			pipe->reaped = true;
			return;
		}
		struct timespec delay = {0, 50 * 1000 * 1000};
		nanosleep(&delay, nullptr);
	}

	kill(-pipe->pid, SIGKILL);
	kill(pipe->pid, SIGKILL);
	int status = 0;
	while (waitpid(pipe->pid, &status, 0) < 0 && errno == EINTR) {
	}
	pipe->reaped = true;
}

int zc_mac_process_pipe_destroy(zc_mac_process_pipe *pipe)
{
	if (!pipe)
		return 0;
	if (pipe->out)
		fclose(pipe->out);
	if (pipe->err)
		fclose(pipe->err);

	int status = 0;
	if (!pipe->reaped && pipe->pid > 0) {
		while (waitpid(pipe->pid, &status, 0) < 0 && errno == EINTR) {
		}
	}
	delete pipe;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 0;
}

#endif
