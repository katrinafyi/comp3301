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

#include <sys/types.h>
#include <sys/wait.h>

#undef _KERNEL
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <err.h>
#include <errno.h>
#include <assert.h>
#include <zones.h>

#ifndef nitems
#define nitems(_a) (sizeof(_a) / sizeof(_a[0]))
#endif

struct task {
	const char *name;
	int (*task)(int, char *[]);
	const char *usage;
};

static const char zcreate_usage[] = "create zonename";
static int	zcreate(int, char *[]);

static const char zdestroy_usage[] = "destroy zonename";
static int	zdestroy(int, char *[]);

static const char zexec_usage[] = "exec zonename command ...";
static int	zexec(int, char *[]);

static const char zid_usage[] = "id [zonename]";
static int	zid(int, char *[]);

static const char zlist_usage[] = "list";
static int	zlist(int, char *[]);

static const char zstats_usage[] = "stats [-H] [-o property[,...]] [-s property] [zonename ...]";
static int	zstats(int, char *[]);

static const struct task tasks[] = {
	{ "create",	zcreate,	zcreate_usage },
	{ "destroy",	zdestroy,	zdestroy_usage },
	{ "exec",	zexec,		zexec_usage },
	{ "list",	zlist,		zlist_usage },
	{ "id",		zid,		zid_usage },
	{ "stats",	zstats,		zstats_usage },
};

static const struct task *
task_lookup(const char *arg)
{
	const struct task *t;
	size_t i;

	for (i = 0; i < nitems(tasks); i++) {
		t = &tasks[i];
		if (strcmp(arg, t->name) == 0)
			return (t);
	}

	return (NULL);
}

__dead void
usage(void)
{
	extern char *__progname;
	const struct task *t;
	size_t i;

	fprintf(stderr, "usage:");
	for (i = 0; i < nitems(tasks); i++) {
		t = &tasks[i];
		fprintf(stderr, "\t%s %s\n", __progname, t->usage);
	}

	exit(1);
}

__dead void
zusage(const char *str)
{
	extern char *__progname;

	fprintf(stderr, "usage: %s %s\n", __progname, str);

	exit(1);
}

int
main(int argc, char *argv[])
{
	const struct task *t;

	if (argc < 2)
		usage();

	t = task_lookup(argv[1]);
	if (t == NULL)
		usage();

	argc -= 2;
	argv += 2;

	return (t->task(argc, argv));
}

static zoneid_t
getzoneid(const char *zone)
{
	const char *errstr;
	zoneid_t z;

	z = zone_id(zone);
	if (z == -1) {
		if (errno != ESRCH)
			err(1, "zone lookup");

		z = strtonum(zone, 0, MAXZONEIDS, &errstr);
		if (errstr != NULL)
			errx(1, "unknown zone \"%s\"", zone);
	}

	return (z);
}

static int
zcreate(int argc, char *argv[])
{
	if (argc != 1)
		zusage(zcreate_usage);

	if (zone_create(argv[0]) == -1)
		err(1, "create");

	return (0);
}

static int
zdestroy(int argc, char *argv[])
{
	zoneid_t z;

	if (argc != 1)
		zusage(zdestroy_usage);

	z = getzoneid(argv[0]);

	if (zone_destroy(z) == -1)
		err(1, "destroy");

	return (0);
}

static int
zexec(int argc, char *argv[])
{
	zoneid_t z;

	if (argc < 2)
		zusage(zexec_usage);

	z = getzoneid(argv[0]);

	argc -= 1;
	argv += 1;

	if (zone_enter(z) == -1)
		err(1, "enter");

	execvp(argv[0], argv);

	err(1, "exec %s", argv[0]);
	/* NOTREACHED */
}

static int
zid(int argc, char *argv[])
{
	const char *zonename;
	zoneid_t z;

	switch (argc) {
	case 0:
		zonename = NULL;
		break;
	case 1:
		zonename = argv[0];
		break;
	default:
		zusage(zid_usage);
	}

	z = zone_id(zonename);
	if (z == -1)
		err(1, "id");

	printf("%d\n", z);

	return (0);
}

static int
zlist(int argc, char *argv[])
{
	char zonename[MAXZONENAMELEN];
	zoneid_t *zs = NULL;
	size_t nzs, i = 8;
	zoneid_t z;

	if (argc != 0)
		zusage(zlist_usage);

	for (;;) {
		nzs = i;

		zs = reallocarray(zs, nzs, sizeof(*zs));
		if (zs == NULL)
			err(1, "lookup");

		if (zone_list(zs, &nzs) == 0)
			break;

		if (errno != EFAULT)
			err(1, "list");

		i <<= 1;
	}

	printf("%8s %s\n", "ID", "NAME");

	for (i = 0; i < nzs; i++) {
		z = zs[i];
		if (zone_name(z, zonename, sizeof(zonename)) == -1)
			err(1, "name");
		printf("%8d %s\n", z, zonename);
	}

	free(zs);

	return (0);
}

static int
zstats(int argc, char *argv[])
{
	char zonename[MAXZONENAMELEN];
	zoneid_t *zs = NULL;
	size_t nzs, i = 8;
	zoneid_t z;
	struct zusage zu;

	if (argc != 0)
		zusage(zstats_usage);

	for (;;) {
		nzs = i;

		zs = reallocarray(zs, nzs, sizeof(*zs));
		if (zs == NULL)
			err(1, "lookup");

		if (zone_list(zs, &nzs) == 0)
			break;

		if (errno != EFAULT)
			err(1, "list");

		i <<= 1;
	}

	printf("%8s %s\n", "ID", "NAME");

	for (i = 0; i < nzs; i++) {
		z = zs[i];
		if (zone_name(z, zonename, sizeof(zonename)) == -1)
			err(1, "name");
		if (zone_stats(z, &zu, NULL))
			err(1, "stats");
		printf("%8d %s %llu %llu\n", z, zonename, zu.zu_nprocs, zu.zu_utime.tv_sec);
	}

	free(zs);

	return (0);
}
