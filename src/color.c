/****************************************************************************
 * bfs                                                                      *
 * Copyright (C) 2015-2022 Tavian Barnes <tavianator@tavianator.com>        *
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

#include "color.h"
#include "bftw.h"
#include "dir.h"
#include "dstring.h"
#include "expr.h"
#include "fsade.h"
#include "stat.h"
#include "trie.h"
#include "util.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct colors {
	char *reset;
	char *leftcode;
	char *rightcode;
	char *endcode;
	char *clear_to_eol;

	char *bold;
	char *gray;
	char *red;
	char *green;
	char *yellow;
	char *blue;
	char *magenta;
	char *cyan;
	char *white;

	char *warning;
	char *error;

	char *normal;

	char *file;
	char *multi_hard;
	char *executable;
	char *capable;
	char *setgid;
	char *setuid;

	char *directory;
	char *sticky;
	char *other_writable;
	char *sticky_other_writable;

	char *link;
	char *orphan;
	char *missing;
	bool link_as_target;

	char *blockdev;
	char *chardev;
	char *door;
	char *pipe;
	char *socket;

	/** A mapping from color names (fi, di, ln, etc.) to struct fields. */
	struct trie names;

	/** A mapping from file extensions to colors. */
	struct trie ext_colors;
};

/** Initialize a color in the table. */
static int init_color(struct colors *colors, const char *name, const char *value, char **field) {
	if (value) {
		*field = dstrdup(value);
		if (!*field) {
			return -1;
		}
	} else {
		*field = NULL;
	}

	struct trie_leaf *leaf = trie_insert_str(&colors->names, name);
	if (leaf) {
		leaf->value = field;
		return 0;
	} else {
		return -1;
	}
}

/** Get a color from the table. */
static char **get_color(const struct colors *colors, const char *name) {
	const struct trie_leaf *leaf = trie_find_str(&colors->names, name);
	if (leaf) {
		return (char **)leaf->value;
	} else {
		return NULL;
	}
}

/** Set the value of a color. */
static int set_color(struct colors *colors, const char *name, char *value) {
	char **color = get_color(colors, name);
	if (color) {
		dstrfree(*color);
		*color = value;
		return 0;
	} else {
		return -1;
	}
}

/**
 * Transform a file extension for fast lookups, by reversing and lowercasing it.
 */
static void extxfrm(char *ext) {
	size_t len = strlen(ext);
	for (size_t i = 0; i < len - i; ++i) {
		char a = ext[i];
		char b = ext[len - i - 1];

		// What's internationalization?  Doesn't matter, this is what
		// GNU ls does.  Luckily, since there's no standard C way to
		// casefold.  Not using tolower() here since it respects the
		// current locale, which GNU ls doesn't do.
		if (a >= 'A' && a <= 'Z') {
			a += 'a' - 'A';
		}
		if (b >= 'A' && b <= 'Z') {
			b += 'a' - 'A';
		}

		ext[i] = b;
		ext[len - i - 1] = a;
	}
}

/**
 * Set the color for an extension.
 */
static int set_ext_color(struct colors *colors, char *key, const char *value) {
	extxfrm(key);

	// A later *.x should override any earlier *.x, *.y.x, etc.
	struct trie_leaf *match;
	while ((match = trie_find_postfix(&colors->ext_colors, key))) {
		dstrfree(match->value);
		trie_remove(&colors->ext_colors, match);
	}

	struct trie_leaf *leaf = trie_insert_str(&colors->ext_colors, key);
	if (leaf) {
		leaf->value = (char *)value;
		return 0;
	} else {
		return -1;
	}
}

/**
 * Find a color by an extension.
 */
static const char *get_ext_color(const struct colors *colors, const char *filename) {
	char *xfrm = strdup(filename);
	if (!xfrm) {
		return NULL;
	}
	extxfrm(xfrm);

	const struct trie_leaf *leaf = trie_find_prefix(&colors->ext_colors, xfrm);
	free(xfrm);
	if (leaf) {
		return leaf->value;
	} else {
		return NULL;
	}
}

/**
 * Parse a chunk of $LS_COLORS that may have escape sequences.  The supported
 * escapes are:
 *
 * \a, \b, \f, \n, \r, \t, \v:
 *     As in C
 * \e:
 *     ESC (\033)
 * \?:
 *     DEL (\177)
 * \_:
 *     ' ' (space)
 * \NNN:
 *     Octal
 * \xNN:
 *     Hex
 * ^C:
 *     Control character.
 *
 * See man dir_colors.
 *
 * @param value
 *         The value to parse.
 * @param end
 *         The character that marks the end of the chunk.
 * @param[out] next
 *         Will be set to the next chunk.
 * @return
 *         The parsed chunk as a dstring.
 */
static char *unescape(const char *value, char end, const char **next) {
	if (!value) {
		goto fail;
	}

	char *str = dstralloc(0);
	if (!str) {
		goto fail_str;
	}

	const char *i;
	for (i = value; *i && *i != end; ++i) {
		unsigned char c = 0;

		switch (*i) {
		case '\\':
			switch (*++i) {
			case 'a':
				c = '\a';
				break;
			case 'b':
				c = '\b';
				break;
			case 'e':
				c = '\033';
				break;
			case 'f':
				c = '\f';
				break;
			case 'n':
				c = '\n';
				break;
			case 'r':
				c = '\r';
				break;
			case 't':
				c = '\t';
				break;
			case 'v':
				c = '\v';
				break;
			case '?':
				c = '\177';
				break;
			case '_':
				c = ' ';
				break;

			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
				while (i[1] >= '0' && i[1] <= '7') {
					c <<= 3;
					c |= *i++ - '0';
				}
				c <<= 3;
				c |= *i - '0';
				break;

			case 'X':
			case 'x':
				while (true) {
					if (i[1] >= '0' && i[1] <= '9') {
						c <<= 4;
						c |= i[1] - '0';
					} else if (i[1] >= 'A' && i[1] <= 'F') {
						c <<= 4;
						c |= i[1] - 'A' + 0xA;
					} else if (i[1] >= 'a' && i[1] <= 'f') {
						c <<= 4;
						c |= i[1] - 'a' + 0xA;
					} else {
						break;
					}
					++i;
				}
				break;

			case '\0':
				goto fail_str;

			default:
				c = *i;
				break;
			}
			break;

		case '^':
			switch (*++i) {
			case '?':
				c = '\177';
				break;
			case '\0':
				goto fail_str;
			default:
				// CTRL masks bits 6 and 7
				c = *i & 0x1F;
				break;
			}
			break;

		default:
			c = *i;
			break;
		}

		if (dstrapp(&str, c) != 0) {
			goto fail_str;
		}
	}

	if (*i) {
		*next = i + 1;
	} else {
		*next = NULL;
	}

	return str;

fail_str:
	dstrfree(str);
fail:
	*next = NULL;
	return NULL;
}

/** Parse the GNU $LS_COLORS format. */
static void parse_gnu_ls_colors(struct colors *colors, const char *ls_colors) {
	for (const char *chunk = ls_colors, *next; chunk; chunk = next) {
		if (chunk[0] == '*') {
			char *key = unescape(chunk + 1, '=', &next);
			if (!key) {
				continue;
			}

			char *value = unescape(next, ':', &next);
			if (value) {
				if (set_ext_color(colors, key, value) != 0) {
					dstrfree(value);
				}
			}

			dstrfree(key);
		} else {
			const char *equals = strchr(chunk, '=');
			if (!equals) {
				break;
			}

			char *value = unescape(equals + 1, ':', &next);
			if (!value) {
				continue;
			}

			char *key = strndup(chunk, equals - chunk);
			if (!key) {
				dstrfree(value);
				continue;
			}

			// All-zero values should be treated like NULL, to fall
			// back on any other relevant coloring for that file
			if (strspn(value, "0") == strlen(value)
			    && strcmp(key, "rs") != 0
			    && strcmp(key, "lc") != 0
			    && strcmp(key, "rc") != 0
			    && strcmp(key, "ec") != 0) {
				dstrfree(value);
				value = NULL;
			}

			if (set_color(colors, key, value) != 0) {
				dstrfree(value);
			}
			free(key);
		}
	}

	if (colors->link && strcmp(colors->link, "target") == 0) {
		colors->link_as_target = true;
		dstrfree(colors->link);
		colors->link = NULL;
	}
}

struct colors *parse_colors() {
	struct colors *colors = malloc(sizeof(struct colors));
	if (!colors) {
		return NULL;
	}

	trie_init(&colors->names);
	trie_init(&colors->ext_colors);

	int ret = 0;

	// From man console_codes

	ret |= init_color(colors, "rs", "0",      &colors->reset);
	ret |= init_color(colors, "lc", "\033[",  &colors->leftcode);
	ret |= init_color(colors, "rc", "m",      &colors->rightcode);
	ret |= init_color(colors, "ec", NULL,     &colors->endcode);
	ret |= init_color(colors, "cl", "\033[K", &colors->clear_to_eol);

	ret |= init_color(colors, "bld", "01;39", &colors->bold);
	ret |= init_color(colors, "gry", "01;30", &colors->gray);
	ret |= init_color(colors, "red", "01;31", &colors->red);
	ret |= init_color(colors, "grn", "01;32", &colors->green);
	ret |= init_color(colors, "ylw", "01;33", &colors->yellow);
	ret |= init_color(colors, "blu", "01;34", &colors->blue);
	ret |= init_color(colors, "mag", "01;35", &colors->magenta);
	ret |= init_color(colors, "cyn", "01;36", &colors->cyan);
	ret |= init_color(colors, "wht", "01;37", &colors->white);

	ret |= init_color(colors, "wrn", "01;33", &colors->warning);
	ret |= init_color(colors, "err", "01;31", &colors->error);

	// Defaults from man dir_colors

	ret |= init_color(colors, "no", NULL, &colors->normal);

	ret |= init_color(colors, "fi", NULL,    &colors->file);
	ret |= init_color(colors, "mh", NULL,    &colors->multi_hard);
	ret |= init_color(colors, "ex", "01;32", &colors->executable);
	ret |= init_color(colors, "ca", "30;41", &colors->capable);
	ret |= init_color(colors, "sg", "30;43", &colors->setgid);
	ret |= init_color(colors, "su", "37;41", &colors->setuid);

	ret |= init_color(colors, "di", "01;34", &colors->directory);
	ret |= init_color(colors, "st", "37;44", &colors->sticky);
	ret |= init_color(colors, "ow", "34;42", &colors->other_writable);
	ret |= init_color(colors, "tw", "30;42", &colors->sticky_other_writable);

	ret |= init_color(colors, "ln", "01;36", &colors->link);
	ret |= init_color(colors, "or", NULL,    &colors->orphan);
	ret |= init_color(colors, "mi", NULL,    &colors->missing);
	colors->link_as_target = false;

	ret |= init_color(colors, "bd", "01;33", &colors->blockdev);
	ret |= init_color(colors, "cd", "01;33", &colors->chardev);
	ret |= init_color(colors, "do", "01;35", &colors->door);
	ret |= init_color(colors, "pi", "33",    &colors->pipe);
	ret |= init_color(colors, "so", "01;35", &colors->socket);

	if (ret) {
		free_colors(colors);
		return NULL;
	}

	parse_gnu_ls_colors(colors, getenv("LS_COLORS"));
	parse_gnu_ls_colors(colors, getenv("BFS_COLORS"));

	return colors;
}

void free_colors(struct colors *colors) {
	if (colors) {
		struct trie_leaf *leaf;
		while ((leaf = trie_first_leaf(&colors->ext_colors))) {
			dstrfree(leaf->value);
			trie_remove(&colors->ext_colors, leaf);
		}
		trie_destroy(&colors->ext_colors);

		while ((leaf = trie_first_leaf(&colors->names))) {
			char **field = leaf->value;
			dstrfree(*field);
			trie_remove(&colors->names, leaf);
		}
		trie_destroy(&colors->names);

		free(colors);
	}
}

CFILE *cfwrap(FILE *file, const struct colors *colors, bool close) {
	CFILE *cfile = malloc(sizeof(*cfile));
	if (!cfile) {
		return NULL;
	}

	cfile->buffer = dstralloc(128);
	if (!cfile->buffer) {
		free(cfile);
		return NULL;
	}

	cfile->file = file;
	cfile->close = close;

	if (isatty(fileno(file))) {
		cfile->colors = colors;
	} else {
		cfile->colors = NULL;
	}

	return cfile;
}

int cfclose(CFILE *cfile) {
	int ret = 0;

	if (cfile) {
		dstrfree(cfile->buffer);

		if (cfile->close) {
			ret = fclose(cfile->file);
		}

		free(cfile);
	}

	return ret;
}

/** Check if a symlink is broken. */
static bool is_link_broken(const struct BFTW *ftwbuf) {
	if (ftwbuf->stat_flags & BFS_STAT_NOFOLLOW) {
		return xfaccessat(ftwbuf->at_fd, ftwbuf->at_path, F_OK) != 0;
	} else {
		return true;
	}
}

/** Get the color for a file. */
static const char *file_color(const struct colors *colors, const char *filename, const struct BFTW *ftwbuf, enum bfs_stat_flags flags) {
	enum bfs_type type = bftw_type(ftwbuf, flags);
	if (type == BFS_ERROR) {
		goto error;
	}

	const struct bfs_stat *statbuf = NULL;
	const char *color = NULL;

	switch (type) {
	case BFS_REG:
		if (colors->setuid || colors->setgid || colors->executable || colors->multi_hard) {
			statbuf = bftw_stat(ftwbuf, flags);
			if (!statbuf) {
				goto error;
			}
		}

		if (colors->setuid && (statbuf->mode & 04000)) {
			color = colors->setuid;
		} else if (colors->setgid && (statbuf->mode & 02000)) {
			color = colors->setgid;
		} else if (colors->capable && bfs_check_capabilities(ftwbuf) > 0) {
			color = colors->capable;
		} else if (colors->executable && (statbuf->mode & 00111)) {
			color = colors->executable;
		} else if (colors->multi_hard && statbuf->nlink > 1) {
			color = colors->multi_hard;
		}

		if (!color) {
			color = get_ext_color(colors, filename);
		}

		if (!color) {
			color = colors->file;
		}

		break;

	case BFS_DIR:
		if (colors->sticky_other_writable || colors->other_writable || colors->sticky) {
			statbuf = bftw_stat(ftwbuf, flags);
			if (!statbuf) {
				goto error;
			}
		}

		if (colors->sticky_other_writable && (statbuf->mode & 01002) == 01002) {
			color = colors->sticky_other_writable;
		} else if (colors->other_writable && (statbuf->mode & 00002)) {
			color = colors->other_writable;
		} else if (colors->sticky && (statbuf->mode & 01000)) {
			color = colors->sticky;
		} else {
			color = colors->directory;
		}

		break;

	case BFS_LNK:
		if (colors->orphan && is_link_broken(ftwbuf)) {
			color = colors->orphan;
		} else {
			color = colors->link;
		}
		break;

	case BFS_BLK:
		color = colors->blockdev;
		break;
	case BFS_CHR:
		color = colors->chardev;
		break;
	case BFS_FIFO:
		color = colors->pipe;
		break;
	case BFS_SOCK:
		color = colors->socket;
		break;
	case BFS_DOOR:
		color = colors->door;
		break;

	default:
		break;
	}

	if (!color) {
		color = colors->normal;
	}

	return color;

error:
	if (colors->missing) {
		return colors->missing;
	} else {
		return colors->orphan;
	}
}

/** Print an ANSI escape sequence. */
static int print_esc(CFILE *cfile, const char *esc) {
	const struct colors *colors = cfile->colors;

	if (dstrdcat(&cfile->buffer, colors->leftcode) != 0) {
		return -1;
	}
	if (dstrdcat(&cfile->buffer, esc) != 0) {
		return -1;
	}
	if (dstrdcat(&cfile->buffer, colors->rightcode) != 0) {
		return -1;
	}

	return 0;
}

/** Reset after an ANSI escape sequence. */
static int print_reset(CFILE *cfile) {
	const struct colors *colors = cfile->colors;

	if (colors->endcode) {
		return dstrdcat(&cfile->buffer, colors->endcode);
	} else {
		return print_esc(cfile, colors->reset);
	}
}

/** Print a string with an optional color. */
static int print_colored(CFILE *cfile, const char *esc, const char *str, size_t len) {
	if (esc) {
		if (print_esc(cfile, esc) != 0) {
			return -1;
		}
	}
	if (dstrncat(&cfile->buffer, str, len) != 0) {
		return -1;
	}
	if (esc) {
		if (print_reset(cfile) != 0) {
			return -1;
		}
	}

	return 0;
}

/** Find the offset of the first broken path component. */
static ssize_t first_broken_offset(const char *path, const struct BFTW *ftwbuf, enum bfs_stat_flags flags, size_t max) {
	ssize_t ret = max;
	assert(ret >= 0);

	if (bftw_type(ftwbuf, flags) != BFS_ERROR) {
		goto out;
	}

	char *at_path;
	int at_fd;
	if (path == ftwbuf->path) {
		if (ftwbuf->depth == 0) {
			at_fd = AT_FDCWD;
			at_path = dstrndup(path, max);
		} else {
			// The parent must have existed to get here
			goto out;
		}
	} else {
		// We're in print_link_target(), so resolve relative to the link's parent directory
		at_fd = ftwbuf->at_fd;
		if (at_fd == AT_FDCWD && path[0] != '/') {
			at_path = dstrndup(ftwbuf->path, ftwbuf->nameoff);
			if (at_path && dstrncat(&at_path, path, max) != 0) {
				ret = -1;
				goto out_path;
			}
		} else {
			at_path = dstrndup(path, max);
		}
	}

	if (!at_path) {
		ret = -1;
		goto out;
	}

	while (ret > 0) {
		if (xfaccessat(at_fd, at_path, F_OK) == 0) {
			break;
		}

		size_t len = dstrlen(at_path);
		while (ret && at_path[len - 1] == '/') {
			--len, --ret;
		}
		while (ret && at_path[len - 1] != '/') {
			--len, --ret;
		}

		dstresize(&at_path, len);
	}

out_path:
	dstrfree(at_path);
out:
	return ret;
}

/** Print the directories leading up to a file. */
static int print_dirs_colored(CFILE *cfile, const char *path, const struct BFTW *ftwbuf, enum bfs_stat_flags flags, size_t nameoff) {
	const struct colors *colors = cfile->colors;

	ssize_t broken = first_broken_offset(path, ftwbuf, flags, nameoff);
	if (broken < 0) {
		return -1;
	}

	if (broken > 0) {
		if (print_colored(cfile, colors->directory, path, broken) != 0) {
			return -1;
		}
	}

	if ((size_t)broken < nameoff) {
		const char *color = colors->missing;
		if (!color) {
			color = colors->orphan;
		}
		if (print_colored(cfile, color, path + broken, nameoff - broken) != 0) {
			return -1;
		}
	}

	return 0;
}

/** Print a file name with colors. */
static int print_name_colored(CFILE *cfile, const char *name, const struct BFTW *ftwbuf, enum bfs_stat_flags flags) {
	const char *color = file_color(cfile->colors, name, ftwbuf, flags);
	return print_colored(cfile, color, name, strlen(name));
}

/** Print a path with colors. */
static int print_path_colored(CFILE *cfile, const char *path, const struct BFTW *ftwbuf, enum bfs_stat_flags flags) {
	size_t nameoff;
	if (path == ftwbuf->path) {
		nameoff = ftwbuf->nameoff;
	} else {
		nameoff = xbasename(path) - path;
	}

	if (print_dirs_colored(cfile, path, ftwbuf, flags, nameoff) != 0) {
		return -1;
	}

	return print_name_colored(cfile, path + nameoff, ftwbuf, flags);
}

/** Print the name of a file with the appropriate colors. */
static int print_name(CFILE *cfile, const struct BFTW *ftwbuf) {
	const char *name = ftwbuf->path + ftwbuf->nameoff;

	const struct colors *colors = cfile->colors;
	if (!colors) {
		return dstrcat(&cfile->buffer, name);
	}

	enum bfs_stat_flags flags = ftwbuf->stat_flags;
	if (colors->link_as_target && ftwbuf->type == BFS_LNK) {
		flags = BFS_STAT_TRYFOLLOW;
	}

	return print_name_colored(cfile, name, ftwbuf, flags);
}

/** Print the path to a file with the appropriate colors. */
static int print_path(CFILE *cfile, const struct BFTW *ftwbuf) {
	const struct colors *colors = cfile->colors;
	if (!colors) {
		return dstrcat(&cfile->buffer, ftwbuf->path);
	}

	enum bfs_stat_flags flags = ftwbuf->stat_flags;
	if (colors->link_as_target && ftwbuf->type == BFS_LNK) {
		flags = BFS_STAT_TRYFOLLOW;
	}

	return print_path_colored(cfile, ftwbuf->path, ftwbuf, flags);
}

/** Print a link target with the appropriate colors. */
static int print_link_target(CFILE *cfile, const struct BFTW *ftwbuf) {
	const struct bfs_stat *statbuf = bftw_cached_stat(ftwbuf, BFS_STAT_NOFOLLOW);
	size_t len = statbuf ? statbuf->size : 0;

	char *target = xreadlinkat(ftwbuf->at_fd, ftwbuf->at_path, len);
	if (!target) {
		return -1;
	}

	int ret;
	if (cfile->colors) {
		ret = print_path_colored(cfile, target, ftwbuf, BFS_STAT_FOLLOW);
	} else {
		ret = dstrcat(&cfile->buffer, target);
	}

	free(target);
	return ret;
}

/** Format some colored output to the buffer. */
BFS_FORMATTER(2, 3)
static int cbuff(CFILE *cfile, const char *format, ...);

/** Dump a parsed expression tree, for debugging. */
static int print_expr(CFILE *cfile, const struct bfs_expr *expr, bool verbose) {
	if (dstrcat(&cfile->buffer, "(") != 0) {
		return -1;
	}

	const struct bfs_expr *lhs = NULL;
	const struct bfs_expr *rhs = NULL;

	if (bfs_expr_has_children(expr)) {
		lhs = expr->lhs;
		rhs = expr->rhs;

		if (cbuff(cfile, "${red}%s${rs}", expr->argv[0]) < 0) {
			return -1;
		}
	} else {
		if (cbuff(cfile, "${blu}%s${rs}", expr->argv[0]) < 0) {
			return -1;
		}
	}

	for (size_t i = 1; i < expr->argc; ++i) {
		if (cbuff(cfile, " ${bld}%s${rs}", expr->argv[i]) < 0) {
			return -1;
		}
	}

	if (verbose) {
		double rate = 0.0, time = 0.0;
		if (expr->evaluations) {
			rate = 100.0*expr->successes/expr->evaluations;
			time = (1.0e9*expr->elapsed.tv_sec + expr->elapsed.tv_nsec)/expr->evaluations;
		}
		if (cbuff(cfile, " [${ylw}%zu${rs}/${ylw}%zu${rs}=${ylw}%g%%${rs}; ${ylw}%gns${rs}]",
		          expr->successes, expr->evaluations, rate, time)) {
			return -1;
		}
	}

	if (lhs) {
		if (dstrcat(&cfile->buffer, " ") != 0) {
			return -1;
		}
		if (print_expr(cfile, lhs, verbose) != 0) {
			return -1;
		}
	}

	if (rhs) {
		if (dstrcat(&cfile->buffer, " ") != 0) {
			return -1;
		}
		if (print_expr(cfile, rhs, verbose) != 0) {
			return -1;
		}
	}

	if (dstrcat(&cfile->buffer, ")") != 0) {
		return -1;
	}

	return 0;
}

static int cvbuff(CFILE *cfile, const char *format, va_list args) {
	const struct colors *colors = cfile->colors;
	int error = errno;

	for (const char *i = format; *i; ++i) {
		size_t verbatim = strcspn(i, "%$");
		if (dstrncat(&cfile->buffer, i, verbatim) != 0) {
			return -1;
		}

		i += verbatim;
		switch (*i) {
		case '%':
			switch (*++i) {
			case '%':
				if (dstrapp(&cfile->buffer, '%') != 0) {
					return -1;
				}
				break;

			case 'c':
				if (dstrapp(&cfile->buffer, va_arg(args, int)) != 0) {
					return -1;
				}
				break;

			case 'd':
				if (dstrcatf(&cfile->buffer, "%d", va_arg(args, int)) != 0) {
					return -1;
				}
				break;

			case 'g':
				if (dstrcatf(&cfile->buffer, "%g", va_arg(args, double)) != 0) {
					return -1;
				}
				break;

			case 's':
				if (dstrcat(&cfile->buffer, va_arg(args, const char *)) != 0) {
					return -1;
				}
				break;

			case 'z':
				++i;
				if (*i != 'u') {
					goto invalid;
				}
				if (dstrcatf(&cfile->buffer, "%zu", va_arg(args, size_t)) != 0) {
					return -1;
				}
				break;

			case 'm':
				if (dstrcat(&cfile->buffer, strerror(error)) != 0) {
					return -1;
				}
				break;

			case 'p':
				switch (*++i) {
				case 'F':
					if (print_name(cfile, va_arg(args, const struct BFTW *)) != 0) {
						return -1;
					}
					break;

				case 'P':
					if (print_path(cfile, va_arg(args, const struct BFTW *)) != 0) {
						return -1;
					}
					break;

				case 'L':
					if (print_link_target(cfile, va_arg(args, const struct BFTW *)) != 0) {
						return -1;
					}
					break;

				case 'e':
					if (print_expr(cfile, va_arg(args, const struct bfs_expr *), false) != 0) {
						return -1;
					}
					break;
				case 'E':
					if (print_expr(cfile, va_arg(args, const struct bfs_expr *), true) != 0) {
						return -1;
					}
					break;

				default:
					goto invalid;
				}

				break;

			default:
				goto invalid;
			}
			break;

		case '$':
			switch (*++i) {
			case '$':
				if (dstrapp(&cfile->buffer, '$') != 0) {
					return -1;
				}
				break;

			case '{': {
				++i;
				const char *end = strchr(i, '}');
				if (!end) {
					goto invalid;
				}
				if (!colors) {
					i = end;
					break;
				}

				size_t len = end - i;
				char name[len + 1];
				memcpy(name, i, len);
				name[len] = '\0';

				char **esc = get_color(colors, name);
				if (!esc) {
					goto invalid;
				}
				if (*esc) {
					if (print_esc(cfile, *esc) != 0) {
						return -1;
					}
				}

				i = end;
				break;
			}

			default:
				goto invalid;
			}
			break;

		default:
			return 0;
		}
	}

	return 0;

invalid:
	assert(!"Invalid format string");
	errno = EINVAL;
	return -1;
}

static int cbuff(CFILE *cfile, const char *format, ...) {
	va_list args;
	va_start(args, format);
	int ret = cvbuff(cfile, format, args);
	va_end(args);
	return ret;
}

int cvfprintf(CFILE *cfile, const char *format, va_list args) {
	assert(dstrlen(cfile->buffer) == 0);

	int ret = -1;
	if (cvbuff(cfile, format, args) == 0) {
		size_t len = dstrlen(cfile->buffer);
		if (fwrite(cfile->buffer, 1, len, cfile->file) == len) {
			ret = 0;
		}
	}

	dstresize(&cfile->buffer, 0);
	return ret;
}

int cfprintf(CFILE *cfile, const char *format, ...) {
	va_list args;
	va_start(args, format);
	int ret = cvfprintf(cfile, format, args);
	va_end(args);
	return ret;
}
