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
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <err.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <zones.h>

#ifndef nitem
#define nitems(_a) (sizeof(_a) / sizeof(_a[0]))
#endif

#define COL_MAX_WIDTH 32

enum column {
	ID,
	NAME,

	UTIME,
	STIME,
	MINFLT,
	MAJFLT,
	NSWAPS,
	INBLOCK,
	OUBLOCK,
	MSGSND,
	MSGRCV,
	NVCSW,
	NIVCSW,
	ENTERS,
	FORKS,
	NPROCS,

	COL_MAX_COLUMNS,
	COL_INVALID,
};

/* argument dispatch functions. */

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

static const char zname_usage[] = "name [id]";
static int	zname(int, char *[]);

static const char zlist_usage[] = "list";
static int	zlist(int, char *[]);

static const char zstats_usage[] =
	"stats [-H] [-o property[,...]] [-s property] [zonename ...]";
static int	zstats(int, char *[]);

static const struct task tasks[] = {
	{ "create",	zcreate,	zcreate_usage },
	{ "destroy",	zdestroy,	zdestroy_usage },
	{ "exec",	zexec,		zexec_usage },
	{ "list",	zlist,		zlist_usage },
	{ "id",		zid,		zid_usage },
	{ "name",	zname,		zname_usage },
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
		if (0 != zone_name(z, NULL, 0) && errno != EFAULT)
			errx(1, "unknown zone id \"%s\"", zone);
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
zname(int argc, char *argv[])
{
	char zonename[MAXZONENAMELEN];
	const char *errstr;
	zoneid_t z;

	if (argc == 0) {
		z = zone_id(NULL);
		if (z == -1)
			err(1, "id");
	} else if (argc == 1) {
		z = strtonum(argv[0], 0, MAXZONEIDS, &errstr);
		if (errstr != NULL)
			errx(1, "name: id %s", errstr);
	} else {
		zusage(zname_usage);
	}

	if (0 != zone_name(z, zonename, nitems(zonename)))
		err(1, "name");

	printf("%s\n", zonename);

	return 0;
}

/*
 * Reads the list of all visible zones, returning a newly-allocated array of
 * the returned length. Exits on errors.
 */
static void
zlist_get(zoneid_t **zs, size_t *nzs)
{
	*zs = NULL;
	*nzs = 0;
	size_t i = 8;
	for (;;) {
		*nzs = i;

		*zs = reallocarray(*zs, *nzs, sizeof(**zs));
		if (zs == NULL)
			err(1, "lookup");

		if (zone_list(*zs, nzs) == 0)
			break;

		if (errno != EFAULT)
			err(1, "list");

		i *= 2;
	}
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

	zlist_get(&zs, &nzs);

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

static const char *
zstats_colname(enum column col)
{
	switch (col) {
	case ID:
		return "ID";
	case NAME:
		return "Name";
	case UTIME:
		return "UTime";
	case STIME:
		return "STime";
	case MINFLT:
		return "MinFlt";
	case MAJFLT:
		return "MajFlt";
	case NSWAPS:
		return "Swaps";
	case INBLOCK:
		return "IBlk";
	case OUBLOCK:
		return "OBlk";
	case MSGSND:
		return "MsgSnd";
	case MSGRCV:
		return "MsgRcv";
	case NVCSW:
		return "VCSw";
	case NIVCSW:
		return "ICSw";
	case FORKS:
		return "Forks";
	case ENTERS:
		return "Enters";
	case NPROCS:
		return "NProcs";
	case COL_MAX_COLUMNS:
	case COL_INVALID:
		break;
	}
	assert(0 <= col && col < COL_MAX_COLUMNS &&
	    "invalid enum zusage_field value.");
	return "(invalid)";
}

static void
zstats_colval(zoneid_t id, const char *name, const struct zstats *zu,
    enum column col, char *str, size_t len)
{
	if (col == ID) {
		snprintf(str, len, "%u", id);
	} else if (col == NAME) {
		snprintf(str, len, "%s", name);
	} else if (col == UTIME || col == STIME) {
		const struct timeval *tv =
		    col == UTIME ? &zu->zu_utime : &zu->zu_stime;
		/* t is initially in microseconds. */
		unsigned long long t = 1000000 * tv->tv_sec + tv->tv_usec;
		unsigned long long secs = t / 1000000;
		t %= 1000000;
		unsigned msecs = t / 1000;
		snprintf(str, len, "%01llu.%03u", secs, msecs);
	} else if (col == MINFLT) {
		snprintf(str, len, "%llu", zu->zu_minflt);
	} else if (col == MAJFLT) {
		snprintf(str, len, "%llu", zu->zu_majflt);
	} else if (col == NSWAPS) {
		snprintf(str, len, "%llu", zu->zu_nswaps);
	} else if (col == INBLOCK) {
		snprintf(str, len, "%llu", zu->zu_inblock);
	} else if (col == OUBLOCK) {
		snprintf(str, len, "%llu", zu->zu_oublock);
	} else if (col == MSGSND) {
		snprintf(str, len, "%llu", zu->zu_msgsnd);
	} else if (col == MSGRCV) {
		snprintf(str, len, "%llu", zu->zu_msgrcv);
	} else if (col == NVCSW) {
		snprintf(str, len, "%llu", zu->zu_nvcsw);
	} else if (col == NIVCSW) {
		snprintf(str, len, "%llu", zu->zu_nivcsw);
	} else if (col == FORKS) {
		snprintf(str, len, "%llu", zu->zu_forks);
	} else if (col == ENTERS) {
		snprintf(str, len, "%llu", zu->zu_enters);
	} else if (col == NPROCS) {
		snprintf(str, len, "%llu", zu->zu_nprocs);
	} else {
		assert(0 <= col && col < COL_MAX_COLUMNS &&
		    "invalid enum zusage_field value.");
	}
}

static bool
strislower(const char *str)
{
	for (; str[0]; str++) {
		if (!islower(str[0]))
			return false;
	}
	return true;
}

static void
zstats_getcols(char arg[], enum column *columns)
{
	char *s, *last;
	/* column index. equivalently, number of columns already given. */
	unsigned int i;

	for (i = 0; i < COL_MAX_COLUMNS; i++)
		columns[i] = COL_INVALID;

	s = strtok_r(arg, ",", &last);
	for (i = 0; s; i++, s = strtok_r(NULL, ",", &last)) {
		if (i >= COL_MAX_COLUMNS)
			errx(1, "column spec exceeds maximum length of %u",
			    COL_MAX_COLUMNS);

		for (enum column c = 0; c < COL_MAX_COLUMNS; c++) {
			const char *cname = zstats_colname(c);
			if (strislower(s) && strcasecmp(s, cname) == 0) {
				columns[i] = c;
			}
		}

		if (columns[i] == COL_INVALID)
			errx(1, "invalid column name: \"%s\"", s);
	}
}

static void
zstats_getopt(int *argc, char ***argv,
    bool *headings, enum column *columns)
{
	/*
	 * unlike in main(), arguments start at 0 here. however,
	 * getopt(3) seems to require that arguments start at index 0.
	 * as such, decrement the argc and argv counts to fake this. :(
	 */

	*argc += 1;
	*argv -= 1;

	int ch;
	*headings = true;
	while ((ch = getopt(*argc, *argv, "Ho:")) != -1) {
		switch (ch) {
		case 'H':
			*headings = false;
			break;
		case 'o':
			zstats_getcols(optarg, columns);
			break;
		default:
			zusage(zstats_usage);
		}
	}
	*argc -= optind;
	*argv += optind;
}

static int
zstats(int argc, char *argv[])
{
	size_t nzs, i;
	enum column c;
	zoneid_t z, *zs = NULL;
	struct zstats zu;
	char zonename[MAXZONENAMELEN];

	bool hasheader = true;
	enum column columns[COL_MAX_COLUMNS] = {
		ID, NAME, UTIME, STIME, MINFLT, MAJFLT, NSWAPS, INBLOCK,
		OUBLOCK, MSGSND, MSGRCV, NVCSW, NIVCSW, FORKS, ENTERS, NPROCS
	};
	char colbuffer[COL_MAX_WIDTH];

	zstats_getopt(&argc, &argv, &hasheader, columns);

	if (argc == 0) {
		zlist_get(&zs, &nzs);
	} else {
		nzs = argc;
		zs = calloc(nzs, sizeof(*zs));
		for (i = 0; i < argc; i++) {
			zs[i] = getzoneid(argv[i]);
		}
	}

	if (hasheader) {
		for (i = 0; i < COL_MAX_COLUMNS; i++) {
			if (columns[i] == COL_INVALID)
				continue;
			printf("%10s", zstats_colname(columns[i]));
		}
		putchar('\n');
	}

	for (i = 0; i < nzs; i++) {
		z = zs[i];
		if (zone_name(z, zonename, sizeof(zonename)) == -1)
			err(1, "name");
		if (zone_stats(z, &zu))
			err(1, "stats");
		for (c = 0; c < COL_MAX_COLUMNS; c++) {
			if (columns[c] == COL_INVALID)
				continue;
			zstats_colval(z, zonename, &zu, columns[c],
			    colbuffer, COL_MAX_WIDTH);
			colbuffer[COL_MAX_WIDTH - 1] = 0; /* just in case :) */
			printf("%10s", colbuffer);
		}
		putchar('\n');
	}

	free(zs);

	return (0);
}
