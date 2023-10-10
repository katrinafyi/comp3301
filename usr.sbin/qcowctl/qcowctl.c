/* */

/*
 * Copyright (c) 2023 The University of Queensland
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

#include <sys/ioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <err.h>

#include <dev/qcowvar.h>

#ifndef nitems
#define nitems(_a) ((sizeof((_a)) / sizeof((_a)[0])))
#endif

#define QCOW_PREFIX	"qcow"
#define QCOW_PREFIXLEN	(sizeof(QCOW_PREFIX) - 1)

struct subcmd {
	int (*cmd)(const struct subcmd *, int, char *[]);
	const char *name;
	const char *usage;
};

static int	qcow_attach(const struct subcmd *, int, char *[]);
static int	qcow_detach(const struct subcmd *, int, char *[]);
static int	qcow_image(const struct subcmd *, int, char *[]);
static int	qcow_stat(const struct subcmd *, int, char *[]);

static const struct subcmd subcmds[] = {
	{ qcow_attach,	"attach",
	    "[-Dr] [-s secsize|-S secbits] qcowX image.qcow2" },
	{ qcow_detach,	"detach",
	    "[-Df] qcowX" },
	{ qcow_image,	"image",
	    "[-D] qcowX" },
	{ qcow_stat,	"stat",
	    "[-D] qcowX" },
};

static const struct subcmd *
		qcow_subcmd_lookup(const char *);

__dead static void
usage(void)
{
	extern char *__progname;
	const struct subcmd *c = &subcmds[0];
	size_t i;

	fprintf(stderr, "usage:\t%s %s", __progname, c->name);
	for (i = 1; i < nitems(subcmds); i++) {
		c = &subcmds[i];
		printf("|%s", c->name);
	}
	printf(" ...\n");

	for (i = 0; i < nitems(subcmds); i++) {
		c = &subcmds[i];
		fprintf(stderr, "\t%s %s %s\n", __progname,
		    c->name, c->usage);
	}

	exit(1);
}

__dead static void
qcow_usage(const struct subcmd *c)
{
	extern char *__progname;

	fprintf(stderr, "\t%s qcowX %s %s\n", __progname,
	    c->name, c->usage);

	exit(1);
}

int
main(int argc, char *argv[])
{
	const struct subcmd *c;

	if (argc < 2)
		usage();

	argc -= 1;
	argv += 1;

	c = qcow_subcmd_lookup(argv[0]);
	if (c == NULL) {
		warnx("unknown command %s", argv[0]);
		usage();
	}

	return (*c->cmd)(c, argc, argv);
}

static const struct subcmd *
qcow_subcmd_lookup(const char *name)
{
	size_t i;

	for (i = 0; i < nitems(subcmds); i++) {
		const struct subcmd *c = &subcmds[i];
		if (strcmp(c->name, name) == 0)
			return (c);
	}

	return (NULL);
}

static int
openqcow(int Dflag, const char *name, int flags)
{
	char dpath[PATH_MAX];
	size_t namelen;
	const char *errstr;
	int rv;
	int dfd;

	if (!Dflag) {
		namelen = strlen(name);
		if (namelen < QCOW_PREFIXLEN)
			errx(1, "%s: short name", name);
		if (memcmp(name, QCOW_PREFIX, QCOW_PREFIXLEN) != 0) {
			errx(1, "%s: invalid %s device name prefix", name,
			    QCOW_PREFIX);
		}

		(void)strtonum(name + QCOW_PREFIXLEN, 0, 0xffffffff, &errstr);
		if (errstr != NULL)
			errx(1, "%s: unit number: %s", name, errstr);

		rv = snprintf(dpath, sizeof(dpath), "/dev/r%sc", name);
		if (rv == -1 || (size_t)rv >= sizeof(dpath))
			errx(1, "devpath snprintf");

		name = dpath;
	}

	dfd = open(name, flags);
	if (dfd == -1)
		err(1, "%s", name);

	return dfd;
}

static int
qcow_secbits(const char *arg, const char **errstr)
{
	int bytes, i;

	bytes = strtonum(arg, 1 << QCOW_SECBITS_MIN, 1 << QCOW_SECBITS_MAX,
	    errstr);
	if (errstr != NULL)
		return (-1);

	/* brute force is fine... */
	for (i = QCOW_SECBITS_MIN; i <= QCOW_SECBITS_MAX; i++) {
		if (bytes == (1 << i))
			return (i);
	}

	*errstr = "invalid";
	return (-1);
}

static int
qcow_attach(const struct subcmd *c, int argc, char *argv[])
{
	struct qcow_attach qc = {
		.qc_readonly = 0,
		.qc_secbits = QCOW_SECBITS_DEFAULT,
	};
	int Dflag = 0;
	int secbits;
	const char *errstr;
	int ch;
	int dfd;

	while ((ch = getopt(argc, argv, "Drs:S:")) != -1) {
		switch (ch) {
		case 'D':
			Dflag = 1;
			break;
		case 'r':
			qc.qc_readonly = 1;
			break;
		case 's':
			secbits = qcow_secbits(optarg, &errstr);
			if (secbits == -1)
				errx(1, "sector size %s is %s", optarg, errstr);
			qc.qc_secbits = secbits;
			break;
		case 'S':
			qc.qc_secbits = strtonum(optarg,
			    QCOW_SECBITS_MIN, QCOW_SECBITS_MAX, &errstr);
			if (errstr != NULL)
				errx(1, "sector bits %s is %s", optarg, errstr);
			break;
		default:
			qcow_usage(c);
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 2)
		qcow_usage(c);

	dfd = openqcow(Dflag, argv[0], O_RDWR);
	qc.qc_file = argv[1];

	if (ioctl(dfd, QCOWIOCATTACH, &qc) == -1)
		err(1, "%s %s %s", c->name, argv[0], argv[1]);

	return (0);
}

static int
qcow_detach(const struct subcmd *c, int argc, char *argv[])
{
	int force = 0;
	int Dflag = 0;
	int ch;
	int dfd;

	while ((ch = getopt(argc, argv, "Df")) != -1) {
		switch (ch) {
		case 'D':
			Dflag = 1;
			break;
		case 'f':
			force = 1;
			break;
		default:
			qcow_usage(c);
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1)
		qcow_usage(c);

	dfd = openqcow(Dflag, argv[0], O_RDWR);

	if (ioctl(dfd, QCOWIOCDETACH, &force) == -1)
		err(1, "%s%s %s", force ? "force " : "", c->name, argv[0]);

	return (0);
}

static int
qcow_image(const struct subcmd *c, int argc, char *argv[])
{
	struct qcow_fname name;
	int Dflag = 0;
	int ch;
	int dfd;

	while ((ch = getopt(argc, argv, "D")) != -1) {
		switch (ch) {
		case 'D':
			Dflag = 1;
			break;
		default:
			qcow_usage(c);
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1)
		qcow_usage(c);

	dfd = openqcow(Dflag, argv[0], O_RDONLY);

	if (ioctl(dfd, QCOWIOCFNAME, &name) == -1)
		err(1, "%s %s", c->name, argv[0]);

	printf("%s\n", name.qc_name);

	return (0);
}

#include <sys/stat.h>
#include <sys/disklabel.h>

static int
qcow_stat(const struct subcmd *c, int argc, char *argv[])
{
	struct stat st;
	int Dflag = 0;
	int ch;
	int dfd;
	dev_t dev;

	while ((ch = getopt(argc, argv, "D")) != -1) {
		switch (ch) {
		case 'D':
			Dflag = 1;
			break;
		default:
			qcow_usage(c);
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1)
		qcow_usage(c);

	dfd = openqcow(Dflag, argv[0], O_RDONLY);

	if (ioctl(dfd, QCOWIOCSTAT, &st) == -1)
		err(1, "%s %s", c->name, argv[0]);

	/* could statfs i guess */
	dev = st.st_dev;
	printf("device %u, major %d, minor %d, disk unit %d, disk part %d\n",
	    dev, major(dev), minor(dev), DISKUNIT(dev), DISKPART(dev));

	/* what are the types of these things? */
	printf("inode %llu\n", (uint64_t)st.st_ino);
	printf("size %llu\n", (uint64_t)st.st_size);

	return (0);
}
