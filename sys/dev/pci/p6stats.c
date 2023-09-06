#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/device.h>
#include <sys/vnode.h>
#include <sys/atomic.h>

#include <machine/bus.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcidevs.h>

struct p6stats_softc {
	struct device		sc_dev;
};

static int	p6stats_match(struct device *, void *, void *);
static void	p6stats_attach(struct device *, struct device *, void *);

const struct cfattach p6stats_ca = {
	.ca_devsize	= sizeof(struct p6stats_softc),
	.ca_match	= p6stats_match,
	.ca_attach	= p6stats_attach
};

struct cfdriver p6stats_cd = {
	.cd_devs	= NULL,
	.cd_name	= "p6stats",
	.cd_class	= DV_DULL
};

const struct pci_matchid p6stats_devices[] = {
	{ 0x3301, 0x0002 }
};

static int
p6stats_match(struct device *parent, void *match, void *aux)
{
        struct pci_attach_args *pa = aux;
        return (pci_matchbyid(pa, p6stats_devices, 1));
}

static void
p6stats_attach(struct device *parent, struct device *self, void *aux)
{
        struct p6stats_softc *sc = (struct p6stats_softc *)self;

	/* your code here */
}

int
p6statsopen(dev_t dev, int mode, int flags, struct proc *p)
{
	/* replace with your code */
	return (ENXIO);
}

int
p6statsclose(dev_t dev, int flag, int mode, struct proc *p)
{
	/* replace with your code */
	return (ENXIO);
}

int
p6statsioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	/* replace with your code */
	return (ENXIO);
}
