#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <err.h>

#include <sys/add2.h>

static void
usage(void)
{
	fprintf(stderr, "usage: add2 <a> <b>\n");
	exit(1);
}

int
main(int argc, char *argv[])
{
	unsigned int a, b, sum;
	const char *errstr;
	int rc;

	if (argc < 2) {
		warnx("not enough arguments");
		usage();
	}

	a = strtonum(argv[1], 0, INT64_MAX, &errstr);
	if (errstr != NULL) {
		warnx("first number is %s: %s", errstr, argv[1]);
		usage();
	}

	b = strtonum(argv[2], 0, INT64_MAX, &errstr);
	if (errstr != NULL) {
		warnx("second number is %s: %s", errstr, argv[2]);
		usage();
	}
	
	rc = add2(ADD2_MODE_ADD, a, b, &sum);
	if (rc)
		err(1, "add2");

	printf("%u\n", sum);
	return (0);
}
