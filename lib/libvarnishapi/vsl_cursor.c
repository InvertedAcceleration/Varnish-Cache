/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2013 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 * Author: Martin Blix Grydeland <martin@varnish-software.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "vas.h"
#include "vdef.h"
#include "miniobj.h"
#include "vapi/vsm.h"
#include "vsm_api.h"
#include "vapi/vsl.h"
#include "vsl_api.h"

struct vslc_vsm {
	struct vslc			c;
	unsigned			magic;
#define VSLC_VSM_MAGIC			0x4D3903A6

	struct VSM_data			*vsm;
	struct VSM_fantom		vf;

	const struct VSL_head		*head;
	const uint32_t			*end;
	ssize_t				segsize;
	struct VSLC_ptr			next;
};

static void
vslc_vsm_delete(void *cursor)
{
	struct vslc_vsm *c;

	CAST_OBJ_NOTNULL(c, (void *)cursor, VSLC_VSM_MAGIC);
	FREE_OBJ(c);
}

static int
vslc_vsm_check(const void *cursor, const struct VSLC_ptr *ptr)
{
	const struct vslc_vsm *c;
	unsigned seqdiff, segment, segdiff;

	CAST_OBJ_NOTNULL(c, cursor, VSLC_VSM_MAGIC);

	if (ptr->ptr == NULL)
		return (0);

	/* Check sequence number */
	seqdiff = c->head->seq - ptr->priv;
	if (c->head->seq < ptr->priv)
		/* Wrap around skips 0 */
		seqdiff -= 1;
	if (seqdiff > 1)
		/* Too late */
		return (0);

	/* Check overrun */
	segment = (ptr->ptr - c->head->log) / c->segsize;
	if (segment >= VSL_SEGMENTS)
		/* Rounding error spills to last segment */
		segment = VSL_SEGMENTS - 1;
	segdiff = (segment - c->head->segment) % VSL_SEGMENTS;
	if (segdiff == 0 && seqdiff == 0)
		/* In same segment, but close to tail */
		return (2);
	if (segdiff <= 2)
		/* Too close to continue */
		return (0);
	if (segdiff <= 4)
		/* Warning level */
		return (1);
	/* Safe */
	return (2);
}

static int
vslc_vsm_next(void *cursor)
{
	struct vslc_vsm *c;
	int i;
	uint32_t t;

	CAST_OBJ_NOTNULL(c, cursor, VSLC_VSM_MAGIC);
	CHECK_OBJ_NOTNULL(c->vsm, VSM_MAGIC);

	/* Assert pointers */
	AN(c->next.ptr);
	assert(c->next.ptr >= c->head->log);
	assert(c->next.ptr < c->end);

	i = vslc_vsm_check(c, &c->next);
	if (i <= 0)
		/* Overrun */
		return (-3);

	/* Check VSL fantom and abandonment */
	if (*(volatile const uint32_t *)c->next.ptr == VSL_ENDMARKER) {
		if (VSM_invalid == VSM_StillValid(c->vsm, &c->vf) ||
		    VSM_Abandoned(c->vsm))
			return (-2);
	}

	while (1) {
		assert(c->next.ptr >= c->head->log);
		assert(c->next.ptr < c->end);
		AN(c->head->seq);
		t = *(volatile const uint32_t *)c->next.ptr;
		AN(t);

		if (t == VSL_WRAPMARKER) {
			/* Wrap around not possible at front */
			assert(c->next.ptr != c->head->log);
			c->next.ptr = c->head->log;
			continue;
		}

		if (t == VSL_ENDMARKER) {
			if (c->next.ptr != c->head->log &&
			    c->next.priv != c->head->seq) {
				/* ENDMARKER not at front and seq wrapped */
				/* XXX: assert on this? */
				c->next.ptr = c->head->log;
				continue;
			}
			return (0);
		}

		if (c->next.ptr == c->head->log)
			c->next.priv = c->head->seq;

		c->c.c.rec = c->next;
		c->next.ptr = VSL_NEXT(c->next.ptr);
		return (1);
	}
}

static int
vslc_vsm_reset(void *cursor)
{
	struct vslc_vsm *c;
	unsigned segment;

	CAST_OBJ_NOTNULL(c, cursor, VSLC_VSM_MAGIC);

	/*
	 * Starting (VSL_SEGMENTS - 3) behind varnishd. This way
	 * even if varnishd wraps immediately, we'll still have a
	 * full segment worth of log before the general constraint
	 * of at least 2 segments apart will be broken
	 */
	segment = (c->head->segment + 3) % VSL_SEGMENTS;
	if (c->head->segments[segment] < 0)
		segment = 0;
	assert(c->head->segments[segment] >= 0);
	c->next.ptr = c->head->log + c->head->segments[segment];
	c->next.priv = c->head->seq;
	c->c.c.rec.ptr = NULL;

	return (0);
}

static int
vslc_vsm_skip(void *cursor, ssize_t words)
{
	struct vslc_vsm *c;

	CAST_OBJ_NOTNULL(c, cursor, VSLC_VSM_MAGIC);
	if (words < 0)
		return (-1);

	c->next.ptr += words;
	assert(c->next.ptr >= c->head->log);
	assert(c->next.ptr < c->end);
	c->c.c.rec.ptr = NULL;

	return (0);
}

static struct vslc_tbl vslc_vsm_tbl = {
	.delete		= vslc_vsm_delete,
	.next		= vslc_vsm_next,
	.reset		= vslc_vsm_reset,
	.skip		= vslc_vsm_skip,
	.check		= vslc_vsm_check,
};

struct VSL_cursor *
VSL_CursorVSM(struct VSL_data *vsl, struct VSM_data *vsm, int tail)
{
	struct vslc_vsm *c;
	struct VSM_fantom vf;
	struct VSL_head *head;

	CHECK_OBJ_NOTNULL(vsl, VSL_MAGIC);
	CHECK_OBJ_NOTNULL(vsm, VSM_MAGIC);

	if (!VSM_Get(vsm, &vf, VSL_CLASS, "", "")) {
		vsl_diag(vsl, "No VSL chunk found (child not started ?)\n");
		return (NULL);
	}

	head = vf.b;
	if (memcmp(head->marker, VSL_HEAD_MARKER, sizeof head->marker)) {
		vsl_diag(vsl, "Not a VSL chunk\n");
		return (NULL);
	}
	if (head->seq == 0) {
		vsl_diag(vsl, "VSL chunk not initialized\n");
		return (NULL);
	}

	ALLOC_OBJ(c, VSLC_VSM_MAGIC);
	if (c == NULL) {
		vsl_diag(vsl, "Out of memory\n");
		return (NULL);
	}
	c->c.magic = VSLC_MAGIC;
	c->c.tbl = & vslc_vsm_tbl;

	c->vsm = vsm;
	c->vf = vf;
	c->head = head;
	c->end = vf.e;
	c->segsize = (c->end - c->head->log) / VSL_SEGMENTS;

	if (tail) {
		/* Locate tail of log */
		c->next.ptr = c->head->log +
		    c->head->segments[c->head->segment];
		while (c->next.ptr < c->end &&
		    *(volatile const uint32_t *)c->next.ptr != VSL_ENDMARKER)
			c->next.ptr = VSL_NEXT(c->next.ptr);
		c->next.priv = c->head->seq;
	} else
		AZ(vslc_vsm_reset(&c->c));

	return (&c->c.c);
}

struct vslc_file {
	struct vslc			c;
	unsigned			magic;
#define VSLC_FILE_MAGIC			0x1D65FFEF

	int				error;
	int				fd;
	ssize_t				buflen;
	uint32_t			*buf;
};

static void
vslc_file_delete(void *cursor)
{
	struct vslc_file *c;

	CAST_OBJ_NOTNULL(c, cursor, VSLC_FILE_MAGIC);
	if (c->fd > STDIN_FILENO)
		(void)close(c->fd);
	if (c->buf != NULL)
		free(c->buf);
	FREE_OBJ(c);
}

static ssize_t
vslc_file_readn(int fd, void *buf, size_t n)
{
	size_t t = 0;
	ssize_t l;

	while (t < n) {
		l = read(fd, (char *)buf + t, n - t);
		if (l <= 0)
			return (l);
		t += l;
	}
	return (t);
}

static int
vslc_file_next(void *cursor)
{
	struct vslc_file *c;
	ssize_t i, l;

	CAST_OBJ_NOTNULL(c, cursor, VSLC_FILE_MAGIC);

	if (c->error)
		return (c->error);

	do {
		c->c.c.rec.ptr = NULL;
		assert(c->buflen >= VSL_BYTES(2));
		i = vslc_file_readn(c->fd, c->buf, VSL_BYTES(2));
		if (i < 0)
			return (-4);	/* I/O error */
		if (i == 0)
			return (-1);	/* EOF */
		assert(i == VSL_BYTES(2));
		l = VSL_BYTES(2 + VSL_WORDS(VSL_LEN(c->buf)));
		if (c->buflen < l) {
			c->buf = realloc(c->buf, 2 * l);
			AN(c->buf);
			c->buflen = 2 * l;
		}
		if (l > VSL_BYTES(2)) {
			i = vslc_file_readn(c->fd, c->buf + 2,
			    l - VSL_BYTES(2));
			if (i < 0)
				return (-4);	/* I/O error */
			if (i == 0)
				return (-1);	/* EOF */
			assert(i == l - VSL_BYTES(2));
		}
		c->c.c.rec.ptr = c->buf;
	} while (VSL_TAG(c->c.c.rec.ptr) == SLT__Batch);
	return (1);
}

static int
vslc_file_reset(void *cursor)
{
	(void)cursor;
	/* XXX: Implement me */
	return (-1);
}

static struct vslc_tbl vslc_file_tbl = {
	.delete		= vslc_file_delete,
	.next		= vslc_file_next,
	.reset		= vslc_file_reset,
	.skip		= NULL,
	.check		= NULL,
};

struct VSL_cursor *
VSL_CursorFile(struct VSL_data *vsl, const char *name)
{
	struct vslc_file *c;
	int fd;
	char buf[] = VSL_FILE_ID;
	ssize_t i;

	if (!strcmp(name, "-"))
		fd = STDIN_FILENO;
	else {
		fd = open(name, O_RDONLY);
		if (fd < 0) {
			vsl_diag(vsl, "Could not open %s: %s\n", name,
			    strerror(errno));
			return (NULL);
		}
	}

	i = vslc_file_readn(fd, buf, sizeof buf);
	if (i <= 0) {
		if (fd > STDIN_FILENO)
			(void)close(fd);
		vsl_diag(vsl, "VSL file read error: %s\n",
		    i < 0 ? strerror(errno) : "EOF");
		return (NULL);
	}
	assert(i == sizeof buf);
	if (memcmp(buf, VSL_FILE_ID, sizeof buf)) {
		if (fd > STDIN_FILENO)
			(void)close(fd);
		vsl_diag(vsl, "Not a VSL file: %s\n", name);
		return (NULL);
	}

	ALLOC_OBJ(c, VSLC_FILE_MAGIC);
	if (c == NULL) {
		if (fd > STDIN_FILENO)
			(void)close(fd);
		vsl_diag(vsl, "Out of memory\n");
		return (NULL);
	}
	c->c.magic = VSLC_MAGIC;
	c->c.tbl = &vslc_file_tbl;

	c->fd = fd;
	c->buflen = BUFSIZ;
	c->buf = malloc(c->buflen);
	AN(c->buf);

	return (&c->c.c);
}

void
VSL_DeleteCursor(struct VSL_cursor *cursor)
{
	struct vslc *c;

	CAST_OBJ_NOTNULL(c, (void *)cursor, VSLC_MAGIC);
	if (c->tbl->delete == NULL)
		return;
	(c->tbl->delete)(c);
}

int
VSL_ResetCursor(struct VSL_cursor *cursor)
{
	struct vslc *c;

	CAST_OBJ_NOTNULL(c, (void *)cursor, VSLC_MAGIC);
	if (c->tbl->reset == NULL)
		return (-1);
	return ((c->tbl->reset)(c));
}

int
VSL_Next(struct VSL_cursor *cursor)
{
	struct vslc *c;

	CAST_OBJ_NOTNULL(c, (void *)cursor, VSLC_MAGIC);
	AN(c->tbl->next);
	return ((c->tbl->next)(c));
}

int
vsl_skip(struct VSL_cursor *cursor, ssize_t words)
{
	struct vslc *c;

	CAST_OBJ_NOTNULL(c, (void *)cursor, VSLC_MAGIC);
	if (c->tbl->skip == NULL)
		return (-1);
	return ((c->tbl->skip)(c, words));
}

int
VSL_Check(const struct VSL_cursor *cursor, const struct VSLC_ptr *ptr)
{
	const struct vslc *c;

	CAST_OBJ_NOTNULL(c, (const void *)cursor, VSLC_MAGIC);
	if (c->tbl->check == NULL)
		return (-1);
	return ((c->tbl->check)(c, ptr));
}
