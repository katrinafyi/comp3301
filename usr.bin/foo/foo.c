#undef _KERNEL
#include <sys/ioctl.h>
#include <sys/ioccom.h>
#include <sys/types.h>

#include <err.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <assert.h>

#include <dev/pci/p6statsvar.h>

int main(int argc, char** argv) {
	assert(0 == system("sync"));

	int fd = open(argv[1], O_RDWR);
	perror("open");
	printf("fd %d\n", fd);
	assert(fd >= 0);

	uint64_t ints[] = {0, 1, 2, 3, 4, 5, 6};
	struct p6stats_calc s;
	struct p6stats_output o;
	s.pc_inputs = ints;
	s.pc_ninputs = 1;
	s.pc_output = &o;

	ioctl(fd, P6STATS_IOC_CALC, &s);
	perror("ioctl");

	for (unsigned i = 0; i < 4; i++) {
		printf("output %u = %llu\n", i, ((uint64_t *)s.pc_output)[i]);
	}

	return 0;
}
