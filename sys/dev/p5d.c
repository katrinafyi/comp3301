#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/device.h>
#include <sys/vnode.h>
#include <sys/poll.h>
#include <sys/malloc.h>

#include <sys/p5d.h>

enum p5d_flags {
	SEND_WAITING	= (1<<0),
};

struct p5d_softc {
	struct mutex	sc_mtx;
	uint32_t	sc_flags;
	int		sc_num;
};

static struct p5d_softc *sc = NULL;

void
p5dattach(int n)
{
	sc = malloc(sizeof(struct p5d_softc), M_DEVBUF, M_WAITOK|M_ZERO);
	mtx_init(&sc->sc_mtx, IPL_NONE);
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
p5dioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	struct p5d_status_params *psp;

	switch (cmd) {
	case P5D_IOC_STATUS:
		psp = (void *)data;
		mtx_enter(&sc->sc_mtx);
		psp->psp_is_num_waiting = sc->sc_flags & SEND_WAITING;
		mtx_leave(&sc->sc_mtx);
		return 0;
	}

	return (ENXIO);
}

int
p5dwrite(dev_t dev, struct uio *uio, int flags)
{
	int eno;

	if (uio->uio_offset < 0)
		return (EINVAL);

	if (uio->uio_resid != sizeof(sc->sc_num))
		return (EINVAL);

	mtx_enter(&sc->sc_mtx);
	if (sc->sc_flags & SEND_WAITING) {
		eno = EBUSY;
		goto out;
	}
	sc->sc_num = 0;
	eno = uiomove(&sc->sc_num, sizeof(sc->sc_num), uio);
	if (eno == 0) {
		sc->sc_flags |= SEND_WAITING;
		wakeup_one(&sc->sc_flags);
	}

out:
	mtx_leave(&sc->sc_mtx);
	return (eno);
}

int
p5dread(dev_t dev, struct uio *uio, int flags)
{
	int eno;

	if (uio->uio_offset < 0)
		return (EINVAL);

	if (uio->uio_resid != sizeof(sc->sc_num))
		return (EINVAL);

	mtx_enter(&sc->sc_mtx);
	while (!(sc->sc_flags & SEND_WAITING)) {
		eno = msleep(&sc->sc_flags, &sc->sc_mtx, PCATCH, "p5d", 0);
		if (eno != 0)
			goto out;
	}
	eno = uiomove(&sc->sc_num, sizeof(sc->sc_num), uio);
	if (eno == 0)
		sc->sc_flags &= ~SEND_WAITING;

out:
	mtx_leave(&sc->sc_mtx);
	return (eno);
}
