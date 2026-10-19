#pragma once

#if defined(__APPLE__)

#include <cstddef>
#include <cstdint>

/* A macOS replacement for libobs' os_process_pipe on macOS 26.
 *
 * OBS 32's os_process_pipe_create() adds a duplicate close() file action to
 * posix_spawn. macOS 26 rejects that with EBADF, so every call returns null
 * before the child is launched. This helper builds the same stdout/stderr
 * pipes without the duplicate action and also keeps the child pid so Stop()
 * can terminate a stalled connector instead of blocking forever in fread().
 */
struct zc_mac_process_pipe;

zc_mac_process_pipe *zc_mac_process_pipe_create(const char *command,
						const char *mode, char *error,
						size_t error_size);
size_t zc_mac_process_pipe_read(zc_mac_process_pipe *pipe, uint8_t *dst,
				size_t size);
size_t zc_mac_process_pipe_read_err(zc_mac_process_pipe *pipe, uint8_t *dst,
				    size_t size);
int zc_mac_process_pipe_destroy(zc_mac_process_pipe *pipe);
void zc_mac_process_pipe_terminate(zc_mac_process_pipe *pipe);

#endif
