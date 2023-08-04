#include <err.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(char c) {
  errx(-1, "usage: foobar [-s] A B");
}

int main(int argc, char *argv[]) {
  int ch;
  bool subtract = false;
  long long a, b;
  while ((ch = getopt(argc, argv, "s")) != -1) {
    switch (ch) {
    case 's':
      subtract = true;
      break;
    default:
      usage(optopt);
      break;
    }
  }

  argc -= optind;
  argv += optind;

  if (argc != 2) {
    usage(0);
    return 0;
  }

  const char *errstr;
  a = strtonum(argv[0], LLONG_MIN, LLONG_MAX, &errstr);
  if (errstr) {
    errx(1, "first number is %s: %s", errstr, argv[0]);
  }
  b = strtonum(argv[1], LLONG_MIN, LLONG_MAX, &errstr);
  if (errstr) {
    errx(1, "second number is %s: %s", errstr, argv[1]);
  }

  printf("%lli\n", a + (subtract ? -b : b));
  return (0);
}
