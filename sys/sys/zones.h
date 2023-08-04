/*	$OpenBSD$ */

/*
 * Copyright (c) 2015 David Gwynne <dlg@uq.edu.au>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _SYS_ZONES_H_
#define _SYS_ZONES_H_

#define MAXZONENAMELEN  256             /* max zone name length w/ NUL */
#define MAXZONES	1024
#define MAXZONEIDS	99999

#ifdef _KERNEL
void		zone_boot(void);
int		zone_visible(struct process *, struct process *);
struct zone *	zone_ref(struct zone *);
void		zone_unref(struct zone *);
zoneid_t	zone_id(const struct zone *);
#endif

#endif /* _SYS_ZONES_H_ */
