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

struct	zusage {
	struct timeval	zu_utime;	/* user time used */
	struct timeval	zu_stime;	/* system time used */
#define	zu_first	zu_minflt
	uint64_t	zu_minflt;	/* page reclaims */
	uint64_t	zu_majflt;	/* page faults */
	uint64_t	zu_nswaps;	/* swaps */
	uint64_t	zu_inblock;	/* block input operations */
	uint64_t	zu_oublock;	/* block output operations */
	uint64_t	zu_msgsnd;	/* messages sent */
	uint64_t	zu_msgrcv;	/* messages received */
	uint64_t	zu_nvcsw;	/* voluntary context switches */
	uint64_t	zu_nivcsw;	/* involuntary context switches */
	uint64_t	zu_enters;	/* zone_enter()s into the zone */
	uint64_t	zu_forks;	/* processes forked in the zone */
	uint64_t	zu_nprocs;	/* number of running processes */
#define	zu_last		zu_nprocs
};

#ifdef _KERNEL
void		zone_boot(void);
int		zone_visible(struct process *, struct process *);
struct zone *	zone_ref(struct zone *);
void		zone_addfork(struct zone *);
void		zone_addsubrusage(struct zone *, const struct rusage *, const struct rusage *);
void		zone_unref(struct zone *);
zoneid_t	zone_id(const struct zone *);
int		zone_stats(zoneid_t z, struct zusage *zu, size_t *zulen);
#endif

#endif /* _SYS_ZONES_H_ */
