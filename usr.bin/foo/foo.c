#include <sys/ioctl.h>
#include <sys/ioccom.h>
#include <sys/types.h>

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <assert.h>

#include <dev/pci/p6statsvar.h>

int main(int argc, char** argv) {
	int fd = open("/dev/p6stats", O_RDWR);
	assert(fd);


	uint64_t ints[] = {0, 1, 2, 3, 4, 5, 6};
	struct p6stats_calc s;
	s.pc_inputs = ints;
	s.pc_ninputs = 1;

	assert(ioctl(fd, P6STATS_IOC_CALC, &s));



	return 1;
}
