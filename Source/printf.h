/****************************************************************************
 * bfs                                                                      *
 * Copyright (C) 2017-2022 Tavian Barnes <tavianator@tavianator.com>        *
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
 * Implementation of -printf/-fprintf.
 */

#ifndef BFS_PRINTF_H
#define BFS_PRINTF_H

#include "color.h"

struct BFTW;
struct bfs_ctx;
struct bfs_expr;

/**
 * A printf command, the result of parsing a single format string.
 */
struct bfs_printf;

/**
 * Parse a -printf format string.
 *
 * @param ctx
 *         The bfs context.
 * @param expr
 *         The expression to fill in.
 * @param format
 *         The format string to parse.
 * @return
 *         0 on success, -1 on failure.
 */
int bfs_printf_parse(const struct bfs_ctx *ctx, struct bfs_expr *expr, const char *format);

/**
 * Evaluate a parsed format string.
 *
 * @param cfile
 *         The CFILE to print to.
 * @param format
 *         The parsed printf format.
 * @param ftwbuf
 *         The bftw() data for the current file.
 * @return
 *         0 on success, -1 on failure.
 */
int bfs_printf(CFILE *cfile, const struct bfs_printf *format, const struct BFTW *ftwbuf);

/**
 * Free a parsed format string.
 */
void bfs_printf_free(struct bfs_printf *format);

#endif // BFS_PRINTF_H
