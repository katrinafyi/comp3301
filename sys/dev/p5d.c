#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/device.h>
#include <sys/vnode.h>
#include <sys/poll.h>

void
p5dattach(int n)
{
}

int
p5dopen(dev_t dev, int mode, int flags, struct proc *p)
{
	printf("hello p5d world\n");
	return (0);
}

int
p5dclose(dev_t dev, int flag, int mode, struct proc *p)
{
	return (0);
}

int
p5dwrite(dev_t dev, struct uio *uio, int flags)
{
	return (EOPNOTSUPP);
}

int
p5dread(dev_t dev, struct uio *uio, int flags)
{
	return (EOPNOTSUPP);
}

int
p5dioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	return (ENXIO);
}
