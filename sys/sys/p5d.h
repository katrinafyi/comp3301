#if !defined(_SYS_P5D_H)
#define _SYS_P5D_H

#include <sys/ioctl.h>
#include <sys/ioccom.h>
#include <sys/types.h>

struct p5d_status_params {
	uint	psp_is_num_waiting;
};

#define	P5D_IOC_STATUS	_IOR('5', 1, struct p5d_status_params)

#endif /* _SYS_P5D_H */
