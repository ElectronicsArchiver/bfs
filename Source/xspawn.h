/****************************************************************************
 * bfs                                                                      *
 * Copyright (C) 2018-2022 Tavian Barnes <tavianator@tavianator.com>        *
 *                                                                          *
 * Permission to use, copy, modify, and/or distribute this software for any *
 * purpose with or without fee is hereby granted.                           *
 *                                                                          *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES *
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF         *
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR  *
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES   *
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN    *
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF  *
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.           *
 ****************************************************************************/

/**
 * A process-spawning library inspired by posix_spawn().
 */

#ifndef BFS_XSPAWN_H
#define BFS_XSPAWN_H

#include <sys/resource.h>
#include <sys/types.h>

/**
 * bfs_spawn() flags.
 */
enum bfs_spawn_flags {
	/** Use the PATH variable to resolve the executable (like execvp()). */
	BFS_SPAWN_USEPATH = 1 << 0,
};

/**
 * bfs_spawn() attributes, controlling the context of the new process.
 */
struct bfs_spawn {
	enum bfs_spawn_flags flags;
	struct bfs_spawn_action *actions;
	struct bfs_spawn_action **tail;
};

/**
 * Create a new bfs_spawn() context.
 *
 * @return 0 on success, -1 on failure.
 */
int bfs_spawn_init(struct bfs_spawn *ctx);

/**
 * Destroy a bfs_spawn() context.
 *
 * @return 0 on success, -1 on failure.
 */
int bfs_spawn_destroy(struct bfs_spawn *ctx);

/**
 * Set the flags for a bfs_spawn() context.
 *
 * @return 0 on success, -1 on failure.
 */
int bfs_spawn_setflags(struct bfs_spawn *ctx, enum bfs_spawn_flags flags);

/**
 * Add a close() action to a bfs_spawn() context.
 *
 * @return 0 on success, -1 on failure.
 */
int bfs_spawn_addclose(struct bfs_spawn *ctx, int fd);

/**
 * Add a dup2() action to a bfs_spawn() context.
 *
 * @return 0 on success, -1 on failure.
 */
int bfs_spawn_adddup2(struct bfs_spawn *ctx, int oldfd, int newfd);

/**
 * Add an fchdir() action to a bfs_spawn() context.
 *
 * @return 0 on success, -1 on failure.
 */
int bfs_spawn_addfchdir(struct bfs_spawn *ctx, int fd);

/**
 * Add a setrlimit() action to a bfs_spawn() context.
 *
 * @return 0 on success, -1 on failure.
 */
int bfs_spawn_addsetrlimit(struct bfs_spawn *ctx, int resource, const struct rlimit *rl);

/**
 * Spawn a new process.
 *
 * @param exe
 *         The executable to run.
 * @param ctx
 *         The context for the new process.
 * @param argv
 *         The arguments for the new process.
 * @param envp
 *         The environment variables for the new process (NULL for the current
 *         environment).
 * @return
 *         The PID of the new process, or -1 on error.
 */
pid_t bfs_spawn(const char *exe, const struct bfs_spawn *ctx, char **argv, char **envp);

/**
 * Look up an executable in the current PATH, as BFS_SPAWN_USEPATH or execvp()
 * would do.
 *
 * @param exe
 *         The name of the binary to execute.  Bare names without a '/' will be
 *         searched on the provided PATH.
 * @return
 *         The full path to the executable, which should be free()'d, or NULL on
 *         failure.
 */
char *bfs_spawn_resolve(const char *exe);

#endif // BFS_XSPAWN_H
